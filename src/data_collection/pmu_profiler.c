/*
 * pmu_profiler.c - ARM PMU-based workload profiler for ML-driven frequency scaling
 *
 * Samples all online CPUs simultaneously every 10ms using on_each_cpu().
 * Each CSV row contains per-core PMU event deltas + cycle counts for all cores,
 * plus both cpufreq policy frequencies (policy0: cores 0-3, policy4: cores 4-5).
 *
 * Authors: Novel Alam, Daniel Hufnagle, William McGarry
 * Platform: Jetson Orin Nano (aarch64)
 *
 * Reference: ARM DDI 0487 - Performance Monitors Extension
 * https://developer.arm.com/documentation/ddi0487/mb/
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
#include <linux/smp.h>
#include <linux/cpufreq.h>

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

static inline void isb_barrier(void) { asm volatile("isb" ::: "memory"); }

/* ============================================================================
 * ARM Register Read/Write Operations (AArch64 System Registers)
 * ============================================================================ */

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

static inline u64 read_pmccntr_el0(void) {
    u64 v;
    asm volatile("mrs %0, pmccntr_el0" : "=r"(v));
    return v;
}

static inline void select_evt_counter(u32 n) {
    asm volatile("msr pmselr_el0, %0" :: "r"((u64)n));
    isb_barrier();
}

static inline void write_pmxevtyper_el0(u64 v) {
    asm volatile("msr pmxevtyper_el0, %0" :: "r"(v));
    isb_barrier();
}

static inline u64 read_pmxevcntr_el0(void) {
    u64 v;
    asm volatile("mrs %0, pmxevcntr_el0" : "=r"(v));
    return v;
}

static inline void write_pmcntenset_el0(u64 v) {
    asm volatile("msr pmcntenset_el0, %0" :: "r"(v));
    isb_barrier();
}

static inline void write_pmovsclr_el0(u64 v) {
    asm volatile("msr pmovsclr_el0, %0" :: "r"(v));
    isb_barrier();
}

/* ============================================================================
 * PMU Control Functions
 * ============================================================================ */

static u32 get_num_hw_counters(void) {
    u64 pmcr = read_pmcr_el0();
    return ((pmcr >> 11) & 0x1f) + 1;
}

static void pmu_init_global(void) {
    u64 pmcr = read_pmcr_el0();
    pmcr |= (1ULL << 0);
    write_pmcr_el0(pmcr);
    isb_barrier();
}

static void pmu_start_counter(u32 n, u32 event_id) {
    select_evt_counter(n);
    write_pmxevtyper_el0((u64)event_id);
    isb_barrier();
}

static void pmu_enable_counter(u32 n) {
    write_pmcntenset_el0((1ULL << n) | (1ULL << 31));
    isb_barrier();
}

/* ============================================================================
 * Events Configuration
 * ============================================================================ */

/*
 * 6 events chosen for ML-based CPU frequency scaling signal quality.
 * Only 6 hardware event counter slots available on Jetson Orin Nano.
 */
static const u32 profiler_events[] = {
    INST_RETIRED,      /* 0x08 - instructions retired: baseline throughput */
    STALL_BACKEND,     /* 0x24 - cycles stalled on data: key scaling signal */
    STALL_FRONTEND,    /* 0x23 - cycles stalled on instructions */
    LL_CACHE_MISS_RD,  /* 0x37 - demand reads reaching DRAM */
    BR_MIS_PRED,       /* 0x10 - branch mispredictions */
    DTLB_WALK,         /* 0x34 - data TLB page-table walks */
};
#define NUM_PROFILER_EVENTS (sizeof(profiler_events) / sizeof(profiler_events[0]))

/* ============================================================================
 * Per-CPU Data Structures
 * ============================================================================ */

#define MAX_CPUS 8

/* Raw PMU counter values read from one CPU */
struct cpu_pmu_snapshot {
    u64 evt[NUM_PROFILER_EVENTS];
    u64 cyc;
};

/* Previous snapshot values for delta computation */
struct cpu_pmu_last {
    u64 evt[NUM_PROFILER_EVENTS];
    u64 cyc;
};

static struct cpu_pmu_snapshot snapshots[MAX_CPUS];
static struct cpu_pmu_last     cpu_last[MAX_CPUS];

/* Global profiler state */
static struct {
    int          num_cpus;
    int          num_slots;
    struct file *csv_file;
    loff_t       csv_pos;
    int          sample_count;
} profiler;

/* ============================================================================
 * Delayed Work Declaration
 * ============================================================================ */

static void pmu_profiler_sample_workfn(struct work_struct *work);
static DECLARE_DELAYED_WORK(profiler_sample_work, pmu_profiler_sample_workfn);

/* ============================================================================
 * Per-CPU Functions (called via on_each_cpu)
 * ============================================================================ */

/* Runs on each CPU: initialize PMU registers for this core */
static void setup_pmu_on_cpu(void *unused)
{
    int i;
    pmu_init_global();
    for (i = 0; i < profiler.num_slots; i++) {
        pmu_start_counter(i, profiler_events[i]);
        pmu_enable_counter(i);
    }
    write_pmovsclr_el0(0xffffffffULL);  /* clear overflow flags */
}

/* Runs on each CPU: snapshot PMU counters into snapshots[cpu] */
static void snapshot_pmu_on_cpu(void *unused)
{
    int cpu = smp_processor_id();
    int i;

    for (i = 0; i < profiler.num_slots; i++) {
        select_evt_counter(i);
        snapshots[cpu].evt[i] = read_pmxevcntr_el0();
    }
    snapshots[cpu].cyc = read_pmccntr_el0();
}

/* ============================================================================
 * CSV Writing
 * ============================================================================ */

static void write_csv_header(void)
{
    char buf[1024];
    int pos = 0;
    int cpu, e;
    static const char * const names[] = {
        "inst_retired", "stall_backend", "stall_frontend",
        "ll_cache_miss_rd", "br_mis_pred", "dtlb_walk"
    };

    pos += snprintf(buf + pos, sizeof(buf) - pos,
                    "timestamp_ns,freq_khz_p0,freq_khz_p4");

    for (cpu = 0; cpu < profiler.num_cpus; cpu++) {
        for (e = 0; e < profiler.num_slots; e++)
            pos += snprintf(buf + pos, sizeof(buf) - pos,
                            ",%s_c%d", names[e], cpu);
        pos += snprintf(buf + pos, sizeof(buf) - pos, ",cycles_c%d", cpu);
    }
    pos += snprintf(buf + pos, sizeof(buf) - pos, "\n");
    kernel_write(profiler.csv_file, buf, pos, &profiler.csv_pos);
}

static void write_csv_line(u64 ts, u32 freq_p0, u32 freq_p4,
                            s64 deltas[][NUM_PROFILER_EVENTS + 1])
{
    char buf[1024];
    int pos = 0;
    int cpu, e;

    pos += snprintf(buf + pos, sizeof(buf) - pos,
                    "%llu,%u,%u", (unsigned long long)ts, freq_p0, freq_p4);

    for (cpu = 0; cpu < profiler.num_cpus; cpu++) {
        for (e = 0; e <= profiler.num_slots; e++)
            pos += snprintf(buf + pos, sizeof(buf) - pos,
                            ",%lld", (long long)deltas[cpu][e]);
    }
    pos += snprintf(buf + pos, sizeof(buf) - pos, "\n");
    kernel_write(profiler.csv_file, buf, pos, &profiler.csv_pos);
}

/* ============================================================================
 * Sampling Work Function (runs on cpu0 every 10ms)
 * ============================================================================ */

static void pmu_profiler_sample_workfn(struct work_struct *work)
{
    u64 ts       = ktime_get_ns();
    u32 freq_p0  = cpufreq_get(0);
    u32 freq_p4  = cpufreq_get(4);
    s64 deltas[MAX_CPUS][NUM_PROFILER_EVENTS + 1];  /* +1 for cycles */
    int cpu, e;

    /* Snapshot PMU registers on all CPUs simultaneously via IPI */
    on_each_cpu(snapshot_pmu_on_cpu, NULL, 1);

    /* Compute per-core deltas from previous snapshot */
    for (cpu = 0; cpu < profiler.num_cpus; cpu++) {
        for (e = 0; e < profiler.num_slots; e++) {
            s64 d = (s64)(snapshots[cpu].evt[e] - cpu_last[cpu].evt[e]);
            if (d < -1000000LL) d = 0;  /* counter reset / wraparound guard */
            deltas[cpu][e] = d;
        }
        /* Cycles delta is last column per core */
        deltas[cpu][profiler.num_slots] =
            (s64)(snapshots[cpu].cyc - cpu_last[cpu].cyc);
    }

    /* Skip first invocation — deltas vs zero baseline are not meaningful */
    if (profiler.sample_count >= 1)
        write_csv_line(ts, freq_p0, freq_p4, deltas);

    /* Advance last values for next delta computation */
    for (cpu = 0; cpu < profiler.num_cpus; cpu++) {
        for (e = 0; e < profiler.num_slots; e++)
            cpu_last[cpu].evt[e] = snapshots[cpu].evt[e];
        cpu_last[cpu].cyc = snapshots[cpu].cyc;
    }

    profiler.sample_count++;
    schedule_delayed_work_on(0, &profiler_sample_work, msecs_to_jiffies(10));
}

/* ============================================================================
 * Module Initialization and Exit
 * ============================================================================ */

static int __init pmu_profiler_init(void)
{
    u32 hw_slots;
    struct file *csv_file;
    int i;

    hw_slots           = get_num_hw_counters();
    profiler.num_slots = (int)min_t(u32, NUM_PROFILER_EVENTS, hw_slots);
    profiler.num_cpus  = min_t(int, num_online_cpus(), MAX_CPUS);
    profiler.sample_count = 0;
    profiler.csv_pos      = 0;

    pr_info("PMU Profiler: %d CPUs, %d event slots (hw_max=%u)\n",
            profiler.num_cpus, profiler.num_slots, hw_slots);

    /* Initialize PMU on every CPU */
    on_each_cpu(setup_pmu_on_cpu, NULL, 1);

    memset(snapshots, 0, sizeof(snapshots));
    memset(cpu_last,  0, sizeof(cpu_last));

    /* Open CSV */
    csv_file = filp_open("/home/mlpower/MLPower/src/data_collection/pmu_data.csv",
                         O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (IS_ERR(csv_file)) {
        pr_err("PMU Profiler: failed to open CSV file\n");
        return PTR_ERR(csv_file);
    }
    profiler.csv_file = csv_file;
    write_csv_header();

    pr_info("PMU Profiler: CSV opened — columns: timestamp + 2 freqs + %d events x %d cores\n",
            profiler.num_slots + 1, profiler.num_cpus);
    for (i = 0; i < profiler.num_slots; i++)
        pr_info("  slot[%d] = 0x%02x\n", i, profiler_events[i]);

    schedule_delayed_work_on(0, &profiler_sample_work, msecs_to_jiffies(10));
    return 0;
}

static void __exit pmu_profiler_exit(void)
{
    cancel_delayed_work_sync(&profiler_sample_work);

    if (profiler.csv_file && !IS_ERR(profiler.csv_file))
        filp_close(profiler.csv_file, NULL);

    pr_info("PMU Profiler: unloaded — %d samples, %d CPUs x %d events\n",
            profiler.sample_count, profiler.num_cpus, profiler.num_slots);
}

module_init(pmu_profiler_init);
module_exit(pmu_profiler_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Novel Alam, Daniel Hufnagle, William McGarry");
MODULE_DESCRIPTION("ARM PMU profiler — all-core sampling for ML frequency scaling");
MODULE_VERSION("2.0");
