#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/cpufreq.h>
#include <linux/kthread.h>
#include <linux/delay.h>

#define POLL_INTERVAL_MS 4

static struct task_struct *freq_thread;

static int freq_thread_fn(void *data) {
    while (!kthread_should_stop()) {
        unsigned int frequency = cpufreq_get(0);
        printk(KERN_INFO "Current frequency: %u kHz\n", frequency);
        msleep_interruptible(POLL_INTERVAL_MS);
    }
    return 0;
}

static int __init cpu_freq_init(void) {
    printk(KERN_INFO "cpu_freq: starting\n");
    freq_thread = kthread_run(freq_thread_fn, NULL, "cpu_freq_poller");
    if (IS_ERR(freq_thread)) {
        printk(KERN_ERR "cpu_freq: failed to create thread\n");
        return PTR_ERR(freq_thread);
    }
    return 0;
}

static void __exit cpu_freq_exit(void) {
    kthread_stop(freq_thread);
    printk(KERN_INFO "cpu_freq: stopped\n");
}

module_init(cpu_freq_init);
module_exit(cpu_freq_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("DANIEL HUFNAGLE");
MODULE_DESCRIPTION("Checking CPU frequency");
MODULE_VERSION("0.1");
