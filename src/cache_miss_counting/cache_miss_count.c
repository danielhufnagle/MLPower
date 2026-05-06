#include <linux/module.h>
#include <linux/io.h>
#include <asm/sysreg.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/preempt.h>
#include <linux/irqflags.h>
#include <linux/delay.h>


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

/* minimal helper removed: binary dump omitted in cleaned module */

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
    u64 pmcr;

    /* stop this counter while we reconfigure it. */

    write_pmcntenclr_el0(1ULL << n);

    /* select slot n, then set which event it should count. */

    select_evt_counter(n);

    /* event id lives in the low byte for the base event field. */

    write_pmxevtyper_el0(event_id & 0xff);

    /* reset the selected event counter. */

    write_pmxevcntr_el0(0);

    /* enable PMU globally and request counter resets. */

    pmcr = read_pmcr_el0();

    pmcr |= (1ULL << 0); /* E bit: global enable. */

    pmcr |= (1ULL << 1); /* P bit: reset event counters. */

    pmcr |= (1ULL << 2); /* C bit: reset cycle counter. */

    write_pmcr_el0(pmcr);

    /* clear overflow flags for this event slot and the cycle counter. */

    write_pmovsclr_el0((1ULL << n) | (1ULL << 31));

    /* enable this event slot and the cycle counter (bit 31). */

    write_pmcntenset_el0((1ULL << n) | (1ULL << 31));
}

static void pmu_stop_counter(u32 n, u64 *evt, u64 *cyc) {
    select_evt_counter(n);

    *evt = read_pmxevcntr_el0();

    *cyc = read_pmccntr_el0();
    
    write_pmcntenclr_el0((1ULL << n) | (1ULL << 31));
}

static int __init cache_miss_init(void) {
    pr_info("Module init\n");

    /* diagnostic snapshot before programming */
    {
        u64 v_pmcr = read_pmcr_el0();
        u64 v_pmcnt = read_pmcntenset_el0();
        select_evt_counter(0);
        u64 v_pmxev = read_pmxevtyper_el0();
        u64 v_evt = read_pmxevcntr_el0();
        u64 v_cyc = read_pmccntr_el0();

        pr_info("before: PMCR=%llu PMCNTENSET=0x%llx PMXEVTYPER=0x%llx evt=%llu cyc=%llu\n",
                (unsigned long long)v_pmcr,
                (unsigned long long)v_pmcnt,
                (unsigned long long)v_pmxev,
                (unsigned long long)v_evt,
                (unsigned long long)v_cyc);
    }

    /* start counting L1 data cache refill (event id 0x03) on slot 0 */
    pmu_start_counter(0, 0x03);

    /* sleep for sampling window (~5 seconds) then read and report counters */
    msleep(5000);

    /* diagnostic snapshot after sampling */
    {
        u64 v_pmcr = read_pmcr_el0();
        u64 v_pmcnt = read_pmcntenset_el0();
        select_evt_counter(0);
        u64 v_pmxev = read_pmxevtyper_el0();
        u64 evt = read_pmxevcntr_el0();
        u64 cyc = read_pmccntr_el0();

        pr_info("after:  PMCR=%llu PMCNTENSET=0x%llx PMXEVTYPER=0x%llx evt=%llu cyc=%llu\n",
                (unsigned long long)v_pmcr,
                (unsigned long long)v_pmcnt,
                (unsigned long long)v_pmxev,
                (unsigned long long)evt,
                (unsigned long long)cyc);
    }

    return 0;
}

static void __exit cache_miss_exit(void) {
    pr_info("Module exit\n");
}

module_init(cache_miss_init);
module_exit(cache_miss_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("NOVEL ALAM, DANIEL HUFNAGLE, WILLIAM MCGARRY");
MODULE_DESCRIPTION("Testing kernel module functionality on Nvidia Jetson");
MODULE_VERSION("0.0");

