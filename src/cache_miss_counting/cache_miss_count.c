#include <linux/module.h>
#include <linux/io.h>
#include <asm/sysreg.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/preempt.h>
#include <linux/irqflags.h>


/*
    https://developer.arm.com/documentation/ddi0487/mb/-Part-D-The-AArch64-System-Level-Architecture/-Chapter-D13-The-Performance-Monitors-Extension
*/


// void init_pmu_counters();



// Pick a slot index n (like 0).
// Write event ID into PMEVTYPER0_EL0 (example: 0x03 for L1D refill).
// Zero PMEVCNTR0_EL0 (optional but common before measuring).
// Enable counting:
// PMCR_EL0.E = 1 (global PMU enable)
// PMCNTENSET_EL0.P0 = 1 (enable slot 0 specifically)
// Run workload.
// Read PMEVCNTR0_EL0 -> that value is the count for the selected event.



static inline void isb_barrier(void) { asm volatile("isb" ::: "memory"); }


static inline u64 read_pmcr_el0(void) {
    u64 v;

    /* read the 64 bit value of PMCR_EL0 registers*/
    /* PMCR_EL0 is a global enableer for the pmu counters*/
    asm volatile("mrs %0, pmcr_el0" : "=r"(v));
    isb_barrier();

    return v;
}

/* write_pmcr_el0 register enables global pmu counting */
static inline void write_pmcr_el0(u64 v) {
    /* write 64 bit value into pmcr_el0 registers */
    asm volatile("msr pmcr_el0, %0" :: "r"(v));
    isb_barrier();
}

static void print_binary(u64 v)
{
    char buf[65];
    int i;

    for (i = 0; i < 64; i++)
        buf[i] = (v & (1ULL << (63 - i))) ? '1' : '0';
    buf[64] = '\0';

    pr_info("pmcr_el0 (bin): %s\n", buf);
}

/* pmselr_el0 selects wjhich of the 32 slot to start counting a type (put later)*/
static inline void select_evt_counter(u32 n) {
    asm volatile("msr pmselr_el0, %0" :: "r"((u64)n));
    isb_barrier();
}

/* pmxevtyper_el0 sets what type the counter in "select_evt_counter" will count */
static inline void write_pmxevtyper_el0(u64 v)
{
    asm volatile("msr pmxevtyper_el0, %0" :: "r"(v));
    isb_barrier();
}

/* pmxevcntr_el0 sets the counter value of pmselr_el0 */
static inline void write_pmxevcntr_el0(u64 v)
{
    asm volatile("msr pmxevcntr_el0, %0" :: "r"(v));
    isb_barrier();
}

/* reads the current count of selected event counter */
static inline u64 read_pmxevcntr_el0(void)
{
    u64 v;
    asm volatile("mrs %0, pmxevcntr_el0" : "=r"(v));
    return v;
}

/* enable the counters bit mask */
static inline void write_pmcntenset_el0(u64 v)
{
    asm volatile("msr pmcntenset_el0, %0" :: "r"(v));
    isb_barrier();
}

/* disable counter bitmaske */
static inline void write_pmcntenclr_el0(u64 v)
{
    asm volatile("msr pmcntenclr_el0, %0" :: "r"(v));
    isb_barrier();
}

// void init_pmu_counters() {

//     return v;
// }





static int __init cache_miss_init(void) {
    pr_info("Module init\n");

    {
        u64 v = read_pmcr_el0();
        pr_info("read_pmcr_el0: %llu\n", (unsigned long long)v);
        print_binary(v);
    }

    pr_info("write_pmcr_el0(0b1)\n");
    write_pmcr_el0(0b1);
    {
        u64 v2 = read_pmcr_el0();
        pr_info("read_pmcr_el0: %llu\n", (unsigned long long)v2);
        print_binary(v2);
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

