#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/cpufreq.h>
#include <linux/cpu.h>


static int set_cpu_frequency(unsigned long target_freq) {
    int cpu;
    int ret;
    for_each_online_cpu(cpu) {
        struct cpufreq_policy *policy;
        policy = cpufreq_cpu_get(cpu);
        if (!policy) {
            printk("Failed to get policy for CPU%d\n", cpu);
            continue;
        }
        ret = cpufreq_driver_target(policy, target_freq, CPUFREQ_RELATION_H);
        if (ret) {
            printk("CPU%d: failed to set frequency\n", cpu);
        }
        cpufreq_cpu_put(policy);
    }
    return 0;
}

static int __init cpu_freq_init(void) {
    printk("This is a test to ensure that CPU frequencies are being set properly\n");
    printk("Calling function to set frequency to 960000 kHz\n");
    if(set_cpu_frequency(960000) == 0) {
        printk("CPU frequency set successfully\n");
    }
    else {
        printk("Error when setting frequency\n");
    }
    return 0;
}

static void __exit cpu_freq_exit(void) {
    printk("Exiting set cpu freq module\n");
}

module_init(cpu_freq_init);
module_exit(cpu_freq_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("NOVEL ALAM, WILLIAM MCGARRY, DANIEL HUFNAGLE");
MODULE_DESCRIPTION("Setting CPU frequency");
MODULE_VERSION("0.0");