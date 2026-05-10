#include <linux/module.h>
#include <linux/io.h>
#include <asm/sysreg.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/preempt.h>
#include <linux/irqflags.h>
#include <linux/delay.h>
#include <linux/workqueue.h>
#include <linux/string.h>

#define MS_DELAY (1)

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

/*
    https://developer.arm.com/documentation/ddi0487/mb/-Part-D-The-AArch64-System-Level-Architecture/-Chapter-D13-The-Performance-Monitors-Extension
*/


/* choose a counter slot (usually start with n = 0). */
/* program that slot with an event id (example: 0x03 for L1D refill). */
/* reset the selected counter to zero before the test. */
/* turn on global PMU counting with PMCR_EL0.E = 1. */
/* enable your chosen slot in PMCNTENSET_EL0. */
/* run the workload, then read PMEVCNTR0_EL0 to get the final count. */



static inline void isb_barrier(void) { asm volatile("isb" ::: "memory"); }


/* PMCR_EL0 bitmap (Performance Monitors Control Register). */
/* [0] E  : enable all counters. */
/* [1] P  : reset event counters (not PMCCNTR). */
/* [2] C  : reset cycle counter PMCCNTR. */
/* [3] D  : cycle counter divider control. */
/* [4] X  : event export control. */
/* [5] DP : cycle counter disable in prohibited/debug states. */
/* [6] LC : long cycle counter enable (64-bit when implemented). */
/* [7] LP : long event counter enable. */
/* [15:11] N : number of implemented event counters minus 1 (read-only). */
/* other bits are reserved or implementation-defined. */
static inline u64 read_pmcr_el0(void) {
    u64 v;

    /* read the full 64-bit PMCR_EL0 value. */
    /* PMCR_EL0 controls global PMU behavior. */
    asm volatile("mrs %0, pmcr_el0" : "=r"(v));
    isb_barrier();

    return v;
}

/* PMCR_EL0 bitmap (write-relevant bits). */
/* [0] E, [1] P, [2] C, [3] D, [4] X, [5] DP, [6] LC, [7] LP. */
/* use read-modify-write so unrelated control bits are preserved. */
/* helper to write PMCR_EL0 (global PMU control register). */
static inline void write_pmcr_el0(u64 v) {
    /* write a 64-bit value into PMCR_EL0. */
    asm volatile("msr pmcr_el0, %0" :: "r"(v));
    isb_barrier();
}


/* PMSELR_EL0 bitmap (event counter selector). */
/* [4:0] SEL : selected event counter slot for PMXEV* access. */
/* [63:5] reserved. */
/* PMSelr chooses which event counter slot we are talking to. */
static inline void select_evt_counter(u32 n) {
    asm volatile("msr pmselr_el0, %0" :: "r"((u64)n));
    isb_barrier();
}

/* PMXEVTYPER_EL0 bitmap (selected counter event type/filter). */
/* [15:0] event field (architected event id mask in kernel headers). */
/* [31], [30], [27] are commonly used privilege-level filter bits. */
/* PMXEVTYPER sets the event type for the currently selected slot. */
static inline void write_pmxevtyper_el0(u64 v) {
    asm volatile("msr pmxevtyper_el0, %0" :: "r"(v));
    isb_barrier();
}

/* PMXEVCNTR_EL0 bitmap (selected event counter value register). */
/* [31:0] low counter bits (architecturally required). */
/* [63:32] high counter bits when long event counters are implemented. */
/* PMXEVCNTR lets us set the selected counter value directly. */
static inline void write_pmxevcntr_el0(u64 v) {
    asm volatile("msr pmxevcntr_el0, %0" :: "r"(v));
    isb_barrier();
}

/* PMXEVCNTR_EL0 bitmap (selected event counter value register). */
/* [31:0] low counter bits (architecturally required). */
/* [63:32] high counter bits when long event counters are implemented. */
/* read back the current value of the selected event counter. */
static inline u64 read_pmxevcntr_el0(void) {
    u64 v;
    asm volatile("mrs %0, pmxevcntr_el0" : "=r"(v));
    return v;
}

/* read PMCNTENSET_EL0 to see which counters are enabled */
static inline u64 read_pmcntenset_el0(void) {
    u64 v;
    asm volatile("mrs %0, pmcntenset_el0" : "=r"(v));
    isb_barrier();
    return v;
}

/* read PMXEVTYPER_EL0 for the currently selected slot */
static inline u64 read_pmxevtyper_el0(void) {
    u64 v;
    asm volatile("mrs %0, pmxevtyper_el0" : "=r"(v));
    isb_barrier();
    return v;
}

/* PMCNTENSET_EL0 bitmap (counter enable set). */
/* [30:0] Pm : write 1 to enable event counter m. */
/* [31]   C  : write 1 to enable PMCCNTR. */
/* [63:32] reserved. */
/* enable counters with a bitmask via PMCNTENSET. */
static inline void write_pmcntenset_el0(u64 v) {
    asm volatile("msr pmcntenset_el0, %0" :: "r"(v));
    isb_barrier();
}

/* PMCNTENCLR_EL0 bitmap (counter enable clear). */
/* [30:0] Pm : write 1 to disable event counter m. */
/* [31]   C  : write 1 to disable PMCCNTR. */
/* [63:32] reserved. */
/* disable counters with a bitmask via PMCNTENCLR. */
static inline void write_pmcntenclr_el0(u64 v) {
    asm volatile("msr pmcntenclr_el0, %0" :: "r"(v));
    isb_barrier();
}

/* PMOVSCLR_EL0 bitmap (overflow status clear). */
/* [30:0] Pm : write 1 to clear overflow for event counter m. */
/* [31]   C  : write 1 to clear PMCCNTR overflow. */
/* [63:32] reserved. */
/* clear overflow flags in PMOVSCLR. */
static inline void write_pmovsclr_el0(u64 v) {
    asm volatile("msr pmovsclr_el0, %0" :: "r"(v));
    isb_barrier();
}

/* PMCCNTR_EL0 bitmap (cycle counter register). */
/* [31:0] low cycle-count bits. */
/* [63:32] high bits when long cycle counting is enabled/implemented. */
/* reads the current cpu cycle counter. */
static inline u64 read_pmccntr_el0(void) {
    u64 v;
    asm volatile("mrs %0, pmccntr_el0" : "=r"(v));
    return v;
}


static void pmu_start_counter(u32 n, u32 event_id) {
    /* select slot n, then set which event it should count. */
    select_evt_counter(n);

    /* set event type (full event_id, not just low byte) */
    write_pmxevtyper_el0((u64)event_id);
    isb_barrier();
}

/* Enable counter slot */
static void pmu_enable_counter(u32 n) {
    write_pmcntenset_el0((1ULL << n) | (1ULL << 31));
    isb_barrier();
}

/* Get number of implemented event counters from PMCR_EL0.N (bits 15:11) */
static u32 get_num_hw_counters(void) {
    u64 pmcr = read_pmcr_el0();
    u32 n_field = (pmcr >> 11) & 0x1f;  /* bits 15:11 */
    return n_field + 1;  /* N field = implemented - 1 */
}

/* Initialize PMU globally (only call once) */
static void pmu_init_global(void) {
    u64 pmcr;

    pmcr = read_pmcr_el0();
    /* Just enable, don't touch reset bits */
    pmcr |= (1ULL << 0); /* E bit: global enable. */
    write_pmcr_el0(pmcr);
    isb_barrier();
}

static void pmu_stop_counter(u32 n, u64 *evt, u64 *cyc) {
    select_evt_counter(n);

    *evt = read_pmxevcntr_el0();

    *cyc = read_pmccntr_el0();
    
    write_pmcntenclr_el0((1ULL << n) | (1ULL << 31));
}

struct pmu_sample_state {
    u32 num_slots;
    int cpu;
    u64 last_evt[32];
    u64 last_cyc;
    u64 event_counts[32];
};

/* 
 * Best 6 PMU events for ML-based frequency scaling on Jetson Orin Nano.
 * Only 6 hardware event counter slots are available on this platform.
 * These events provide comprehensive workload profiling:
 *   - INST_RETIRED: instruction throughput
 *   - L1D/L1I_CACHE_REFILL: L1 cache efficiency
 *   - L2D_CACHE_REFILL: L2 cache misses
 *   - LL_CACHE_MISS_RD: last-level cache misses
 * PMCCNTR_EL0 is read separately for cycle counter (not a slot).
 */
static const u32 ml_pmu_events[] = {
    INST_RETIRED,             /* 0x08 - slot 0: instruction retirement rate */
    L1D_CACHE_REFILL,         /* 0x03 - slot 1: L1D cache misses */
    L1I_CACHE_REFILL,         /* 0x01 - slot 2: L1I cache misses */
    L2D_CACHE_REFILL,         /* 0x17 - slot 3: L2 cache misses */
    LL_CACHE_MISS_RD,         /* 0x37 - slot 4: last-level cache miss reads */
    BR_MIS_PRED,              /* 0x10 - slot 5: branch mispredictions */
};
#define NUM_EVENTS 6
#define NUM_EVENTS_ACTUAL (sizeof(ml_pmu_events) / sizeof(ml_pmu_events[0]))

static struct pmu_sample_state sample_state;

static void pmu_sample_workfn(struct work_struct *work);
static DECLARE_DELAYED_WORK(pmu_sample_work, pmu_sample_workfn);

static void pmu_sample_workfn(struct work_struct *work)
{
    u64 evt;
    u64 cyc;
    s64 delta_evt;
    s64 delta_cyc;
    int i;

    cyc = read_pmccntr_el0();
    delta_cyc = (s64)(cyc - sample_state.last_cyc);
    sample_state.last_cyc = cyc;

    /* read all configured event counters in parallel */
    for (i = 0; i < sample_state.num_slots; i++) {
        select_evt_counter(i);
        evt = read_pmxevcntr_el0();
        delta_evt = (s64)(evt - sample_state.last_evt[i]);
        
        /* handle counter overflow (delta should not be huge negative) */
        if (delta_evt < 0 && delta_evt < -1000000) {
            pr_warn("event[%d]: counter wrapped or reset (delta=%lld)\n", i, delta_evt);
            delta_evt = 0; /* skip this sample if counter was reset */
        }
        
        sample_state.event_counts[i] += delta_evt;
        sample_state.last_evt[i] = evt;

        /* always print all events */
        pr_info("evt[%2d] (0x%02x): delta=%lld\n",
                i, ml_pmu_events[i], delta_evt);
    }

    pr_info("cycles: delta=%lld\n", delta_cyc);

    /* queue the next sample on the same CPU in about 10 ms */
    schedule_delayed_work_on(sample_state.cpu, &pmu_sample_work, msecs_to_jiffies(MS_DELAY));
}

static int __init cache_miss_init(void) {
    int i;
    u32 hw_slots;
    
    /* Detect actual hardware counter slots */
    hw_slots = get_num_hw_counters();
    pr_info("Module init - hardware supports %u event counter slots\n", hw_slots);
    pr_info("Module init - tracking %u PMU events in parallel\n", NUM_EVENTS_ACTUAL);
    
    if (NUM_EVENTS_ACTUAL > hw_slots) {
        pr_warn("Warning: requesting %u events but hardware only has %u slots\n",
                NUM_EVENTS_ACTUAL, hw_slots);
    }

    /* global PMU enable (just enable, don't reset) */
    pmu_init_global();

    /* configure events in parallel on available slots */
    for (i = 0; i < NUM_EVENTS_ACTUAL && i < hw_slots; i++) {
        pmu_start_counter(i, ml_pmu_events[i]);
    }

    /* now enable all configured slots at once */
    for (i = 0; i < NUM_EVENTS_ACTUAL && i < hw_slots; i++) {
        pmu_enable_counter(i);
    }

    /* initialize the sample state */
    sample_state.num_slots = (NUM_EVENTS_ACTUAL < hw_slots) ? NUM_EVENTS_ACTUAL : hw_slots;
    sample_state.cpu = get_cpu();
    sample_state.last_cyc = 0;
    memset(sample_state.last_evt, 0, sizeof(sample_state.last_evt));
    memset(sample_state.event_counts, 0, sizeof(sample_state.event_counts));
    put_cpu();

    pr_info("PMU counters started on slots 0-%d (cycle counter: PMCCNTR_EL0)\n", sample_state.num_slots - 1);
    
    /* print enabled event configuration */
    for (i = 0; i < sample_state.num_slots; i++) {
        pr_info("  slot[%2d] = event 0x%02x\n", i, ml_pmu_events[i]);
    }

    /* kick off the repeating sampler every 10ms */
    schedule_delayed_work_on(sample_state.cpu, &pmu_sample_work, msecs_to_jiffies(10));

    return 0;
}

static void __exit cache_miss_exit(void) {
    int i;
    
    /* stop the sampler before disabling the counters */
    cancel_delayed_work_sync(&pmu_sample_work);
    
    /* read final values */
    for (i = 0; i < sample_state.num_slots; i++) {
        u64 evt;
        select_evt_counter(i);
        evt = read_pmxevcntr_el0();
        sample_state.event_counts[i] += (evt - sample_state.last_evt[i]);
    }
    
    /* print final accumulated counts */
    pr_info("=== Final Event Counts (%u slots) ===\n", sample_state.num_slots);
    for (i = 0; i < sample_state.num_slots; i++) {
        pr_info("evt[%2d] (0x%02x): %llu\n", i, ml_pmu_events[i], 
                (unsigned long long)sample_state.event_counts[i]);
    }
    pr_info("Module exit\n");
}

module_init(cache_miss_init);
module_exit(cache_miss_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("NOVEL ALAM, DANIEL HUFNAGLE, WILLIAM MCGARRY");
MODULE_DESCRIPTION("Testing kernel module functionality on Nvidia Jetson");
MODULE_VERSION("0.0");

