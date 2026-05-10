/*
 * pmu_profiler.c - ARM PMU-based workload profiler for ML-driven frequency scaling
 * 
 * Collects real-time performance metrics from Jetson Orin Nano's ARM Performance 
 * Monitoring Unit (PMU) for intelligent CPU frequency scaling decisions.
 * 
 * Authors: Novel Alam, Daniel Hufnagle, William McGarry
 * Platform: Jetson Orin Nano (aarch64)
 * 
 * Reference: ARM DDI 0487 - Performance Monitors Extension
 * https://developer.arm.com/documentation/ddi0487/mb/-Part-D-The-AArch64-System-Level-Architecture/-Chapter-D13-The-Performance-Monitors-Extension
 */

#include <linux/module.h>
#include <linux/io.h>
#include <asm/sysreg.h>
#include <linux/kernel.h>
#include <linux/preempt.h>
#include <linux/irqflags.h>
#include <linux/delay.h>
#include <linux/workqueue.h>
#include <linux/string.h>
#include <linux/fs.h>
#include <linux/ktime.h>
#include <linux/fs.h>

/* ============================================================================
 * ARM PMU Event Definitions (ARMv8 Cortex-A78)
 * ============================================================================ */

#define BR_MIS_PRED                      (0x10)
#define BR_MIS_PRED_RETIRED              (0x22)
#define BR_PRED                          (0x12)
#define BR_RETIRED                       (0x21)
#define BUS_ACCESS                       (0x19)
#define BUS_CYCLES                       (0x1D)
#define CID_WRITE_RETIRED                (0x0B)
#define CNT_CYCLES                       (0x4004)
#define CPU_CYCLES                       (0x11)
#define DTLB_WALK                        (0x34)
#define EXC_RETURN                       (0x0A)
#define EXC_TAKEN                        (0x09)
#define INST_RETIRED                     (0x08)
#define INST_SPEC                        (0x1B)
#define ITLB_WALK                        (0x35)
#define L1D_CACHE                        (0x04)
#define L1D_CACHE_LMISS_RD               (0x39)
#define L1D_CACHE_REFILL                 (0x03)
#define L1D_CACHE_WB                     (0x15)
#define L1D_TLB                          (0x25)
#define L1D_TLB_REFILL                   (0x05)
#define L1I_CACHE                        (0x14)
#define L1I_CACHE_LMISS                  (0x4006)
#define L1I_CACHE_REFILL                 (0x01)
#define L1I_TLB                          (0x26)
#define L1I_TLB_REFILL                   (0x02)
#define L2D_CACHE                        (0x16)
#define L2D_CACHE_ALLOCATE               (0x20)
#define L2D_CACHE_LMISS_RD               (0x4009)
#define L2D_CACHE_REFILL                 (0x17)
#define L2D_CACHE_WB                     (0x18)
#define L2D_TLB                          (0x2F)
#define L2D_TLB_REFILL                   (0x2D)
#define L3D_CACHE                        (0x2B)
#define L3D_CACHE_ALLOCATE               (0x29)
#define L3D_CACHE_LMISS_RD               (0x400B)
#define L3D_CACHE_REFILL                 (0x2A)
#define LL_CACHE_MISS_RD                 (0x37)
#define LL_CACHE_RD                      (0x36)
#define MEM_ACCESS                       (0x13)
#define MEMORY_ERROR                     (0x1A)
#define OP_RETIRED                       (0x3A)
#define OP_SPEC                          (0x3B)
#define REMOTE_ACCESS                    (0x31)
#define SAMPLE_COLLISION                 (0x4003)
#define SAMPLE_FEED                      (0x4001)
#define SAMPLE_FILTRATE                  (0x4002)
#define SAMPLE_POP                       (0x4000)
#define STALL                            (0x3C)
#define STALL_BACKEND                    (0x24)
#define STALL_BACKEND_MEM                (0x4005)
#define STALL_FRONTEND                   (0x23)
#define STALL_SLOT                       (0x3F)
#define STALL_SLOT_BACKEND               (0x3D)
#define STALL_SLOT_FRONTEND              (0x3E)
#define TTBR_WRITE_RETIRED               (0x1C)

/* ============================================================================
 * Inline Utilities
 * ============================================================================ */

/** Instruction synchronization barrier for register updates */
static inline void isb_barrier(void) { asm volatile("isb" ::: "memory"); }

/* ============================================================================
 * ARM Register Read/Write Operations (AArch64 System Registers)
 * ============================================================================ */

/**
 * PMCR_EL0: Global PMU control register.
 *
 * This register controls the overall behavior of the Performance Monitoring
 * Unit. Bit 0 (E) turns counting on and off for the whole PMU. Bit 1 (P)
 * resets the event counters (it does not touch the cycle counter) and bit 2
 * (C) resets the cycle counter (PMCCNTR_EL0). Other bits adjust divider and
 * export behavior or enable "long" 64-bit modes. Bits 15:11 expose how many
 * event counters the hardware implements (value = implemented - 1). The high
 * bits are reserved on most platforms.
 */

static inline u64 read_pmcr_el0(void) {
    u64 v;
    asm volatile("mrs %0, pmcr_el0" : "=r"(v));
    isb_barrier();
    return v;
}

static inline void write_pmcr_el0(u64 v) {
    asm volatile("msr pmcr_el0, %0" :: "r"(v));
    isb_barrier();
}

/**
 * PMCCNTR_EL0: Cycle counter (absolute).
 *
 * A dedicated 64-bit cycle counter used as a time/reference source. This is
 * independent of the per-event counters and is useful for timestamps and
 * measuring total cycles between samples.
 */

static inline u64 read_pmccntr_el0(void) {
    u64 v;
    asm volatile("mrs %0, pmccntr_el0" : "=r"(v));
    return v;
}

/**
 * PMSELR_EL0: Select which event counter slot subsequent accesses refer to.
 *
 * Writing a slot number here makes subsequent PMXEV* register reads/writes
 * operate on that selected counter. Think of it as choosing which "bank"
 * of counters you want to talk to.
 */

static inline void select_evt_counter(u32 n) {
    asm volatile("msr pmselr_el0, %0" :: "r"((u64)n));
    isb_barrier();
}

/**
 * PMXEVTYPER_EL0: Event type register for the selected slot.
 *
 * Write an event ID here (for the currently-selected slot via PMSELR_EL0)
 * to tell that counter what to measure (instructions retired, L1 misses,
 * branch misses, etc.). Only the low bits contain the event code; the rest
 * of the register is reserved for platform-specific features.
 */

static inline void write_pmxevtyper_el0(u64 v) {
    asm volatile("msr pmxevtyper_el0, %0" :: "r"(v));
    isb_barrier();
}

static inline u64 read_pmxevtyper_el0(void) {
    u64 v;
    asm volatile("mrs %0, pmxevtyper_el0" : "=r"(v));
    isb_barrier();
    return v;
}

/**
 * PMXEVCNTR_EL0: Current value for the selected event counter.
 *
 * After selecting a slot with PMSELR_EL0, read this register to obtain the
 * 64-bit counter value for that event. Writing allows resetting or seeding
 * the counter on some implementations.
 */

static inline u64 read_pmxevcntr_el0(void) {
    u64 v;
    asm volatile("mrs %0, pmxevcntr_el0" : "=r"(v));
    return v;
}

static inline void write_pmxevcntr_el0(u64 v) {
    asm volatile("msr pmxevcntr_el0, %0" :: "r"(v));
    isb_barrier();
}

/**
 * PMCNTENSET_EL0: Enable counters by setting bits.
 *
 * Each bit corresponds to a hardware counter: bits 0..30 enable the event
 * counters and bit 31 enables the cycle counter (PMCCNTR_EL0). Writing a 1
 * to a bit turns that counter on; use the clear register to disable.
 */

static inline u64 read_pmcntenset_el0(void) {
    u64 v;
    asm volatile("mrs %0, pmcntenset_el0" : "=r"(v));
    isb_barrier();
    return v;
}

static inline void write_pmcntenset_el0(u64 v) {
    asm volatile("msr pmcntenset_el0, %0" :: "r"(v));
    isb_barrier();
}

/**
 * PMCNTENCLR_EL0: Disable counters by writing bits.
 *
 * Complement to PMCNTENSET_EL0: write a 1 to a bit to turn that counter off.
 */

static inline void write_pmcntenclr_el0(u64 v) {
    asm volatile("msr pmcntenclr_el0, %0" :: "r"(v));
    isb_barrier();
}

/**
 * PMOVSCLR_EL0: Clear overflow status bits.
 *
 * If a counter overflowed (wrapped) the PMU sets status flags; write 1s
 * to this register to clear those flags for the event counters or the cycle
 * counter.
 */

static inline void write_pmovsclr_el0(u64 v) {
    asm volatile("msr pmovsclr_el0, %0" :: "r"(v));
    isb_barrier();
}

/* ============================================================================
 * PMU Control Functions
 * ============================================================================ */

/**
 * Get the number of implemented hardware event counter slots from PMCR_EL0.N
 * 
 * Returns: number of available counter slots (e.g., 6 on Jetson Orin Nano)
 */
static u32 get_num_hw_counters(void) {
    u64 pmcr = read_pmcr_el0();
    u32 n_field = (pmcr >> 11) & 0x1f;  /* bits 15:11 */
    return n_field + 1;  /* N field = implemented - 1 */
}

/**
 * Initialize PMU globally (call once at module load)
 * 
 * Enables global PMU counting without touching reset bits.
 */
static void pmu_init_global(void) {
    u64 pmcr = read_pmcr_el0();
    pmcr |= (1ULL << 0);  /* E bit: global enable */
    write_pmcr_el0(pmcr);
    isb_barrier();
}

/**
 * Configure a specific counter slot to measure a given event
 * 
 * @n:        Counter slot number (0-30)
 * @event_id: Event ID to measure (e.g., 0x08 for INST_RETIRED)
 */
static void pmu_start_counter(u32 n, u32 event_id) {
    select_evt_counter(n);
    write_pmxevtyper_el0((u64)event_id);
    isb_barrier();
}

/**
 * Enable a specific counter slot for counting
 * 
 * @n: Counter slot number to enable
 * Also enables the cycle counter (bit 31) for reference timestamps
 */
static void pmu_enable_counter(u32 n) {
    write_pmcntenset_el0((1ULL << n) | (1ULL << 31));
    isb_barrier();
}

/**
 * Disable a specific counter slot
 * 
 * @n: Counter slot number to disable
 */
static void pmu_disable_counter(u32 n) {
    write_pmcntenclr_el0((1ULL << n) | (1ULL << 31));
}

/**
 * Read the current value of a counter slot
 * 
 * @n: Counter slot number to read
 * Returns: Current 64-bit counter value
 */
static u64 pmu_read_counter(u32 n) {
    select_evt_counter(n);
    return read_pmxevcntr_el0();
}

/* ============================================================================
 * Profiler State and Configuration
 * ============================================================================ */

/** Profiler sampling state for tracking delta values between samples */
struct pmu_profiler_state {
    u32 num_slots;              /* Number of active counter slots */
    int cpu;                    /* CPU this profiler runs on */
    u64 last_evt[8];            /* Previous value per slot (max 6 used) */
    u64 last_cyc;               /* Previous cycle counter */
    u64 event_counts[8];        /* Accumulated delta counts (max 6 used) */
    struct file *csv_file;      /* File handle for CSV output */
    s64 prev_cycles;            /* Previous cycles delta for pairing with current events */
    s64 prev_evt[8];            /* Previous event deltas for feature-target pairing */
    int sample_count;           /* Number of samples collected */
    loff_t csv_file_pos;        /* Track file position for append */
    u64 prev_time_ns;           /* Timestamp of previous sample (ns) */
    u64 prev_abs_cycles;        /* Absolute PMCCNTR_EL0 value of previous sample */
};

/** 
 * Best 6 PMU events for ML-based frequency scaling on Jetson Orin Nano.
 * Only 6 hardware event counter slots are available on this platform.
 * 
 * These events provide comprehensive workload profiling:
 *   - INST_RETIRED:       Instruction throughput (IPC indicator)
 *   - L1D_CACHE_REFILL:   L1 data cache efficiency
 *   - L1I_CACHE_REFILL:   L1 instruction cache efficiency
 *   - L2D_CACHE_REFILL:   L2 cache efficiency
 *   - LL_CACHE_MISS_RD:   Memory access efficiency
 *   - BR_MIS_PRED:        Branch prediction accuracy
 * 
 * PMCCNTR_EL0 is read separately for cycle counter (not a slot).
 */
static const u32 profiler_events[] = {
    INST_RETIRED,             /* 0x08 - slot 0: instruction retirement rate */
    L1D_CACHE_REFILL,         /* 0x03 - slot 1: L1D cache misses */
    L1I_CACHE_REFILL,         /* 0x01 - slot 2: L1I cache misses */
    L2D_CACHE_REFILL,         /* 0x17 - slot 3: L2 cache misses */
    LL_CACHE_MISS_RD,         /* 0x37 - slot 4: last-level cache miss reads */
    BR_MIS_PRED,              /* 0x10 - slot 5: branch mispredictions */
};
#define NUM_PROFILER_EVENTS (sizeof(profiler_events) / sizeof(profiler_events[0]))

static struct pmu_profiler_state profiler_state;

/* Forward declaration for delayed work */
static void pmu_profiler_sample_workfn(struct work_struct *work);
static DECLARE_DELAYED_WORK(profiler_sample_work, pmu_profiler_sample_workfn);

/* ============================================================================
 * Profiler Sampling Function (10ms interval)
 * ============================================================================ */

/**
 * Periodic sampling function collecting PMU metrics every 10ms
 * 
 * Reads all configured counter slots, calculates deltas from last sample,
 * and accumulates metrics. Uses signed 64-bit arithmetic to handle wraparound.
 */
/**
 * Write CSV line to file. Format:
 * evt0_delta,evt1_delta,evt2_delta,evt3_delta,evt4_delta,evt5_delta,target_cycles_delta
 * 
 * Features are event deltas from sample N.
 * Target is cycles delta from sample N+1 (paired on next sample).
 */
static void write_csv_line(void)
{
    char buffer[512];
    int len;
    
    if (!profiler_state.csv_file || profiler_state.sample_count < 1)
        return;  /* Need at least one previous sample to form a pair */

    /* Format:
     * evt0,evt1,evt2,evt3,evt4,evt5,feat_time_ns,feat_abs_cycles,
     * target_cycles,target_time_ns,target_abs_cycles
     */
    len = snprintf(buffer, sizeof(buffer),
                   "%lld,%lld,%lld,%lld,%lld,%lld,%llu,%llu,%lld,%llu,%llu\n",
                   profiler_state.prev_evt[0], profiler_state.prev_evt[1],
                   profiler_state.prev_evt[2], profiler_state.prev_evt[3],
                   profiler_state.prev_evt[4], profiler_state.prev_evt[5],
                   (unsigned long long)profiler_state.prev_time_ns,
                   (unsigned long long)profiler_state.prev_abs_cycles,
                   (long long)profiler_state.prev_cycles,
                   (unsigned long long)ktime_get_ns(),
                   (unsigned long long)read_pmccntr_el0());
    
    if (len > 0) {
        kernel_write(profiler_state.csv_file, buffer, len, &profiler_state.csv_file_pos);
    }
}

static void pmu_profiler_sample_workfn(struct work_struct *work)
{
    u64 evt;
    u64 cyc;
    s64 delta_evt;
    s64 delta_cyc;
    int i;
    int all_zero = 1;

    /* Read cycle counter first */
    cyc = read_pmccntr_el0();
    delta_cyc = (s64)(cyc - profiler_state.last_cyc);
    profiler_state.last_cyc = cyc;
    /* Wall-clock time for this sample to detect delayed work gaps */
    u64 ts = ktime_get_ns();

    /* Read all configured event counters */
    s64 cur_evt[8] = {0};
    for (i = 0; i < profiler_state.num_slots; i++) {
        select_evt_counter(i);
        evt = read_pmxevcntr_el0();
        delta_evt = (s64)(evt - profiler_state.last_evt[i]);
        
        /* Skip corrupted samples (counter wraparound / reset) */
        if (delta_evt < 0 && delta_evt < -1000000) {
            pr_warn("profiler: evt[%d] counter wrapped (delta=%lld)\n", i, delta_evt);
            delta_evt = 0;
        }
        
        if (delta_evt != 0)
            all_zero = 0;
        
        profiler_state.event_counts[i] += delta_evt;
        profiler_state.last_evt[i] = evt;
        cur_evt[i] = delta_evt;  /* Store current delta locally */

        /* Log event delta */
        pr_info("evt[%d] (0x%02x): delta=%lld\n",
                i, profiler_events[i], delta_evt);
    }

    pr_info("cycles: delta=%lld time_ns=%llu abs_cycles=%llu\n", delta_cyc, ts, cyc);

    /* Write CSV line with previous events + current cycles as target */
    write_csv_line();

    /* Update prev_* fields for next pairing: features = current deltas, target = next sample */
    for (i = 0; i < profiler_state.num_slots; i++)
        profiler_state.prev_evt[i] = cur_evt[i];
    profiler_state.prev_cycles = delta_cyc;  /* Save cycles delta to be used as target for previous features */
    profiler_state.prev_time_ns = ts;
    profiler_state.prev_abs_cycles = cyc;
    profiler_state.sample_count++;

    /* Log diagnostics if all event deltas are zero */
    if (all_zero && delta_cyc == 0) {
        u64 pmcr = read_pmcr_el0();
        u64 pmcntenset = read_pmcntenset_el0();
        u32 pmu_e_bit = (pmcr & 0x1);

        pr_info("profiler: ZERO SAMPLE - cpu=%u, time_ns=%llu, pmccntr_abs=%llu, pmcr_e_bit=%u (0x%llx), pmcntenset=0x%llx\n",
                smp_processor_id(), ts, cyc, pmu_e_bit, pmcr, pmcntenset);
    }

    /* Schedule next sample in 10ms */
    schedule_delayed_work_on(profiler_state.cpu, &profiler_sample_work,
                            msecs_to_jiffies(10));
}

/* ============================================================================
 * Module Initialization and Exit
 * ============================================================================ */

static int __init pmu_profiler_init(void) {
    int i;
    u32 hw_slots;
    struct file *csv_file;
    
    /* Detect actual hardware counter slots */
    hw_slots = get_num_hw_counters();
    pr_info("PMU Profiler: hardware supports %u counter slots\n", hw_slots);
    pr_info("PMU Profiler: tracking %zu events\n", NUM_PROFILER_EVENTS);
    
    if (NUM_PROFILER_EVENTS > hw_slots) {
        pr_warn("PMU Profiler: requesting %zu events but hardware only has %u\n",
                NUM_PROFILER_EVENTS, hw_slots);
    }

    /* Enable global PMU */
    pmu_init_global();

    /* Configure each event on corresponding slot */
    for (i = 0; i < NUM_PROFILER_EVENTS && i < hw_slots; i++) {
        pmu_start_counter(i, profiler_events[i]);
    }

    /* Enable all slots */
    for (i = 0; i < NUM_PROFILER_EVENTS && i < hw_slots; i++) {
        pmu_enable_counter(i);
    }

    /* Initialize profiler state */
    profiler_state.num_slots = (NUM_PROFILER_EVENTS < hw_slots)
                                ? NUM_PROFILER_EVENTS : hw_slots;
    profiler_state.cpu = get_cpu();
    profiler_state.sample_count = 0;
    profiler_state.prev_cycles = 0;
        profiler_state.csv_file_pos = 0;
    
    /* Open CSV file for writing */
    csv_file = filp_open("/home/mlpower/MLPower/src/pmu_profiler/pmu_data.csv",
                         O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (IS_ERR(csv_file)) {
        pr_err("PMU Profiler: failed to open CSV file\n");
        return PTR_ERR(csv_file);
    }
    profiler_state.csv_file = csv_file;
    
    /* Write CSV header */
    {
        char header[] = "evt0_inst_ret,evt1_l1d_refill,evt2_l1i_refill,evt3_l2_refill,evt4_ll_miss_rd,evt5_br_mispred,feat_time_ns,feat_abs_cycles,target_cycles,target_time_ns,target_abs_cycles\n";
        kernel_write(profiler_state.csv_file, header, sizeof(header)-1, &profiler_state.csv_file_pos);
    }
    pr_info("PMU Profiler: CSV file opened at /home/mlpower/MLPower/src/pmu_profiler/pmu_data.csv\n");
    profiler_state.last_cyc = 0;
    memset(profiler_state.last_evt, 0, sizeof(profiler_state.last_evt));
    memset(profiler_state.event_counts, 0, sizeof(profiler_state.event_counts));
    put_cpu();

    pr_info("PMU Profiler: started on slots 0-%d (cycle counter: PMCCNTR_EL0)\n",
            profiler_state.num_slots - 1);
    
    /* Print configuration */
    for (i = 0; i < profiler_state.num_slots; i++) {
        pr_info("  slot[%d] = 0x%02x (%s)\n", i, profiler_events[i],
                (i == 0) ? "INST_RETIRED" :
                (i == 1) ? "L1D_CACHE_REFILL" :
                (i == 2) ? "L1I_CACHE_REFILL" :
                (i == 3) ? "L2D_CACHE_REFILL" :
                (i == 4) ? "LL_CACHE_MISS_RD" :
                (i == 5) ? "BR_MIS_PRED" : "???");
    }

    /* Start periodic sampling */
    schedule_delayed_work_on(profiler_state.cpu, &profiler_sample_work,
                            msecs_to_jiffies(10));

    return 0;
}

static void __exit pmu_profiler_exit(void) {
    int i;
    
    /* Stop sampling */
    cancel_delayed_work_sync(&profiler_sample_work);
    
    /* Collect final measurements */
    for (i = 0; i < profiler_state.num_slots; i++) {
        u64 evt = pmu_read_counter(i);
        profiler_state.event_counts[i] += (evt - profiler_state.last_evt[i]);
    }
    
    /* Close CSV file */
    if (profiler_state.csv_file && !IS_ERR(profiler_state.csv_file)) {
        filp_close(profiler_state.csv_file, NULL);
    }
    
    /* Print final report */
    pr_info("=== PMU Profiler Exit Report ===\n");
    pr_info("Total samples collected: %u\n", profiler_state.sample_count);
    pr_info("CSV training pairs: %u (sample N events → sample N+1 cycles)\n", 
            profiler_state.sample_count - 1);
    pr_info("CSV file saved: /home/mlpower/MLPower/src/pmu_profiler/pmu_data.csv\n");
    pr_info("Total events collected across %u slots:\n", profiler_state.num_slots);
    for (i = 0; i < profiler_state.num_slots; i++) {
        pr_info("  evt[%d] (0x%02x): %llu\n", i, profiler_events[i], 
                (unsigned long long)profiler_state.event_counts[i]);
    }
    pr_info("PMU Profiler unloaded\n");
}

module_init(pmu_profiler_init);
module_exit(pmu_profiler_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Novel Alam, Daniel Hufnagle, William McGarry");
MODULE_DESCRIPTION("ARM PMU profiler for ML-driven CPU frequency scaling on Jetson Orin Nano");
MODULE_VERSION("1.0");
