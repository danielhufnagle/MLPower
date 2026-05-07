#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/kthread.h>
#include <linux/delay.h>
#include <linux/fs.h>

#define POLL_INTERVAL_MS 1000

static struct task_struct *freq_thread;

static unsigned int read_freq_from_sysfs(void) {
    struct file *filp;
    char buf[32];
    loff_t pos = 0;
    unsigned int freq = 0;
    
    filp = filp_open("/sys/devices/system/cpu/cpu0/cpufreq/scaling_cur_freq", O_RDONLY, 0);
    
    if (IS_ERR(filp)) {
        return 0;
    }
    
    kernel_read(filp, buf, sizeof(buf) - 1, &pos);
    buf[31] = '\0';
    
    if (kstrtouint(buf, 10, &freq) == 0) {
        filp_close(filp, NULL);
        return freq;
    }
    
    filp_close(filp, NULL);
    return 0;
}

static int freq_thread_fn(void *data)
{
    while (!kthread_should_stop()) {
        unsigned int frequency = read_freq_from_sysfs();
        printk(KERN_INFO "Current frequency: %u kHz\n", frequency);
        msleep_interruptible(POLL_INTERVAL_MS);
    }
    return 0;
}

static int __init cpu_freq_init(void)
{
    printk(KERN_INFO "cpu_freq: starting\n");
    freq_thread = kthread_run(freq_thread_fn, NULL, "cpu_freq_poller");
    if (IS_ERR(freq_thread)) {
        printk(KERN_ERR "cpu_freq: failed to create thread\n");
        return PTR_ERR(freq_thread);
    }
    return 0;
}

static void __exit cpu_freq_exit(void)
{
    kthread_stop(freq_thread);
    printk(KERN_INFO "cpu_freq: stopped\n");
}

module_init(cpu_freq_init);
module_exit(cpu_freq_exit);
MODULE_LICENSE("GPL");
MODULE_AUTHOR("DANIEL HUFNAGLE");
MODULE_DESCRIPTION("Checking CPU frequency");
MODULE_VERSION("0.1");