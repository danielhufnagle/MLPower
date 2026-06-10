/*
 * pmu_profiler.c - x86 (AMD Zen 4) PMU-based workload profiler
 * Adapted from ARM implementation for Jetson Orin Nano.
 */

#include <linux/module.h>
#include <linux/io.h>
#include <asm/msr.h>
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
 * AMD Zen 4 PMU Event Definitions
 * ============================================================================ */

#define AMD_EVENT_INST_RETIRED          0x00C0
#define AMD_EVENT_STALL_FRONTEND        0x00D0   /* Decoder empty */
#define AMD_EVENT_STALL_BACKEND         0x01A0   /* Dispatch stalls (UMASK 0x1E) */
#define AMD_EVENT_L2_MISSES             0x007E   /* Unified L2 misses */
#define AMD_EVENT_BR_MIS_PRED           0x00C3   /* Retired branch instructions mispredicted */
#define AMD_EVENT_DTLB_MISS             0x0045   /* L1 DTLB miss */

#define AMD_MSR_PERF_CTL0               0xc0010200
#define AMD_MSR_PERF_CTR0               0xc0010201

/* Control bits */
#define AMD_CTL_ENABLE                  (1ULL << 22)
#define AMD_CTL_USER                    (1ULL << 16)
#define AMD_CTL_OS                      (1ULL << 17)

/* ============================================================================
 * Events Configuration
 * ============================================================================ */

static const u64 profiler_events[] = {
    AMD_EVENT_INST_RETIRED,
    AMD_EVENT_STALL_BACKEND,  /* We will handle the UMASK and EventHigh specifically */
    AMD_EVENT_STALL_FRONTEND,
    AMD_EVENT_L2_MISSES,
    AMD_EVENT_BR_MIS_PRED,
    AMD_EVENT_DTLB_MISS,
};
#define NUM_PROFILER_EVENTS (sizeof(profiler_events) / sizeof(profiler_events[0]))

/* ============================================================================
 * Per-CPU Data Structures
 * ============================================================================ */

#define MAX_CPUS 16

struct cpu_pmu_snapshot {
    u64 evt[NUM_PROFILER_EVENTS];
    u64 cyc;
};

struct cpu_pmu_last {
    u64 evt[NUM_PROFILER_EVENTS];
    u64 cyc;
};

static struct cpu_pmu_snapshot snapshots[MAX_CPUS];
static struct cpu_pmu_last     cpu_last[MAX_CPUS];

static struct {
    int          num_cpus;
    int          num_slots;
    struct file *csv_file;
    loff_t       csv_pos;
    int          sample_count;
} profiler;

static void pmu_profiler_sample_workfn(struct work_struct *work);
static DECLARE_DELAYED_WORK(profiler_sample_work, pmu_profiler_sample_workfn);

/* ============================================================================
 * Per-CPU Functions
 * ============================================================================ */

static void setup_pmu_on_cpu(void *unused)
{
    int i;
    for (i = 0; i < profiler.num_slots; i++) {
        u64 event = profiler_events[i];
        u64 val = AMD_CTL_ENABLE | AMD_CTL_USER | AMD_CTL_OS;
        
        /* Encode EventSelect[7:0] */
        val |= (event & 0xFF);
        
        /* Encode EventSelect[11:8] into bits [35:32] */
        val |= ((event >> 8) & 0xF) << 32;
        
        /* Specific UMASK for Backend Stalls (event 0x1A0) */
        if (event == AMD_EVENT_STALL_BACKEND) {
            val |= (0x1E << 8); 
        }

        wrmsrl(AMD_MSR_PERF_CTL0 + (i * 2), val);
        wrmsrl(AMD_MSR_PERF_CTR0 + (i * 2), 0);
    }
}

static void snapshot_pmu_on_cpu(void *unused)
{
    int cpu = smp_processor_id();
    int i;

    if (cpu >= MAX_CPUS) return;

    for (i = 0; i < profiler.num_slots; i++) {
        rdmsrl(AMD_MSR_PERF_CTR0 + (i * 2), snapshots[cpu].evt[i]);
    }
    /* Use TSC as cycles equivalent or read a fixed counter if available */
    snapshots[cpu].cyc = rdtsc();
}

/* ============================================================================
 * CSV Writing
 * ============================================================================ */

static void write_csv_header(void)
{
    char buf[2048];
    int pos = 0;
    int cpu, e;
    static const char * const names[] = {
        "inst_retired", "stall_backend", "stall_frontend",
        "l2_cache_miss", "br_mis_pred", "dtlb_miss"
    };

    pos += snprintf(buf + pos, sizeof(buf) - pos,
                    "timestamp_ns,freq_khz_p0,freq_khz_p6");

    for (cpu = 0; cpu < profiler.num_cpus; cpu++) {
        for (e = 0; e < profiler.num_slots; e++)
            pos += snprintf(buf + pos, sizeof(buf) - pos,
                            ",%s_c%d", names[e], cpu);
        pos += snprintf(buf + pos, sizeof(buf) - pos, ",cycles_c%d", cpu);
    }
    pos += snprintf(buf + pos, sizeof(buf) - pos, "\n");
    kernel_write(profiler.csv_file, buf, pos, &profiler.csv_pos);
}

static void write_csv_line(u64 ts, u32 freq_p0, u32 freq_p6,
                            s64 deltas[][NUM_PROFILER_EVENTS + 1])
{
    char *buf = kmalloc(4096, GFP_KERNEL);
    int pos = 0;
    int cpu, e;

    if (!buf) return;

    pos += snprintf(buf + pos, 4096 - pos,
                    "%llu,%u,%u", (unsigned long long)ts, freq_p0, freq_p6);

    for (cpu = 0; cpu < profiler.num_cpus; cpu++) {
        for (e = 0; e <= profiler.num_slots; e++)
            pos += snprintf(buf + pos, 4096 - pos,
                            ",%lld", (long long)deltas[cpu][e]);
    }
    pos += snprintf(buf + pos, 4096 - pos, "\n");
    kernel_write(profiler.csv_file, buf, pos, &profiler.csv_pos);
    kfree(buf);
}

/* ============================================================================
 * Sampling Work Function
 * ============================================================================ */

static void pmu_profiler_sample_workfn(struct work_struct *work)
{
    u64 ts       = ktime_get_real_ns();
    u32 freq_p0  = cpufreq_get(0);
    u32 freq_p6  = cpufreq_get(6);
    s64 deltas[MAX_CPUS][NUM_PROFILER_EVENTS + 1];
    int cpu, e;

    on_each_cpu(snapshot_pmu_on_cpu, NULL, 1);

    for (cpu = 0; cpu < profiler.num_cpus; cpu++) {
        for (e = 0; e < profiler.num_slots; e++) {
            s64 d = (s64)(snapshots[cpu].evt[e] - cpu_last[cpu].evt[e]);
            if (d < 0) d = 0; /* Simpler guard for now */
            deltas[cpu][e] = d;
        }
        deltas[cpu][profiler.num_slots] =
            (s64)(snapshots[cpu].cyc - cpu_last[cpu].cyc);
    }

    if (profiler.sample_count >= 1)
        write_csv_line(ts, freq_p0, freq_p6, deltas);

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
    struct file *csv_file;
    char path[256];

    profiler.num_slots = NUM_PROFILER_EVENTS;
    profiler.num_cpus  = min_t(int, num_online_cpus(), MAX_CPUS);
    profiler.sample_count = 0;
    profiler.csv_pos      = 0;

    pr_info("x86 PMU Profiler: %d CPUs, %d event slots\n",
            profiler.num_cpus, profiler.num_slots);

    on_each_cpu(setup_pmu_on_cpu, NULL, 1);

    memset(snapshots, 0, sizeof(snapshots));
    memset(cpu_last,  0, sizeof(cpu_last));

    /* Adjust path to current workspace */
    snprintf(path, sizeof(path), "%s/pmu_data.csv", "/home/danielhufnagle/Documents/CS446/MLPower/src/x86");

    csv_file = filp_open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (IS_ERR(csv_file)) {
        pr_err("x86 PMU Profiler: failed to open CSV file %s\n", path);
        return PTR_ERR(csv_file);
    }
    profiler.csv_file = csv_file;
    write_csv_header();

    schedule_delayed_work_on(0, &profiler_sample_work, msecs_to_jiffies(10));
    return 0;
}

static void __exit pmu_profiler_exit(void)
{
    int i, cpu;
    cancel_delayed_work_sync(&profiler_sample_work);

    /* Disable counters on exit */
    for_each_online_cpu(cpu) {
        for (i = 0; i < profiler.num_slots; i++) {
            /* We can't use wrmsrl directly here easily across CPUs without smp_call_function */
        }
    }

    if (profiler.csv_file && !IS_ERR(profiler.csv_file))
        filp_close(profiler.csv_file, NULL);

    pr_info("x86 PMU Profiler: unloaded\n");
}

module_init(pmu_profiler_init);
module_exit(pmu_profiler_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Daniel Hufnagle");
MODULE_DESCRIPTION("x86 (AMD) PMU profiler");
