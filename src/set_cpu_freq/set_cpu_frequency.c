#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/cpufreq.h>
#include <linux/cpu.h>
#include <linux/fs.h>
#include <linux/uaccess.h>
#include <linux/device.h>

#include "set_cpu_frequency.h"

static const char* name = "set_cpu_frequency_novel";
static struct class *freq_class = NULL;
static struct device* created_dev = NULL; 
unsigned int major_num;

static int is_valid_frequency(unsigned long freq) {
    switch (freq) {
        case CPU_FREQ_115200:
        case CPU_FREQ_192000:
        case CPU_FREQ_268800:
        case CPU_FREQ_345600:
        case CPU_FREQ_422400:
        case CPU_FREQ_499200:
        case CPU_FREQ_576000:
        case CPU_FREQ_652800:
        case CPU_FREQ_729600:
        case CPU_FREQ_806400:
        case CPU_FREQ_883200:
        case CPU_FREQ_960000:
        case CPU_FREQ_1036800:
        case CPU_FREQ_1113600:
        case CPU_FREQ_1190400:
        case CPU_FREQ_1267200:
        case CPU_FREQ_1344000:
        case CPU_FREQ_1420800:
        case CPU_FREQ_1497600:
        case CPU_FREQ_1574400:
        case CPU_FREQ_1651200:
        case CPU_FREQ_1728000:
            return 1;
        default:
            return 0;
    }
}

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

static long freq_ioctl(struct file *file, unsigned int cmd, unsigned long arg) {
    if (cmd == SET_FREQ_CMD) {
        unsigned long target_freq;
        if (copy_from_user(&target_freq, (unsigned long __user *)arg, sizeof(target_freq))) {
            pr_err("Failed to copy_from_user target frequency\n");
            return -EFAULT;
        }
        
        if (!is_valid_frequency(target_freq)) {
            pr_err("Requested frequency %lu kHz is not supported by Orin Nano\n", target_freq);
            return -EINVAL;
        }

        pr_info("ioctl setting frequency to %lu kHz\n", target_freq);
        if (set_cpu_frequency(target_freq) == 0) {
            return 0;
        } else {
            return -EINVAL;
        }
    }
    return -EINVAL;
}

struct file_operations file_ops = {
    .owner = THIS_MODULE,
    .unlocked_ioctl = freq_ioctl,
};

static int __init cpu_freq_init(void) {
    printk("This is a test to ensure that CPU frequencies are being set properly\n");
    
    /* registering this driver as a character device */
    major_num = register_chrdev(0, name, &file_ops);

    if (major_num <= 0) {
        pr_err("Failed to register character device\n");
        return -EFAULT;
    }

    /* create a new class that will be in the dev (similiar how there is ttyl or serial )*/
    freq_class = class_create(THIS_MODULE, name);
    if (IS_ERR(freq_class)) {
        pr_err("Failed to create device class\n");
        unregister_chrdev(major_num, name);
        return PTR_ERR(freq_class); 
    }

    created_dev = device_create(freq_class, NULL, MKDEV(major_num, 0), NULL, name);

    if (IS_ERR(created_dev)) {
        pr_err("Failed to create device class\n");
        class_destroy(freq_class);
        unregister_chrdev(major_num, name);
        return PTR_ERR(created_dev); 
    }

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

    if (created_dev) {
        device_destroy(freq_class, MKDEV(major_num, 0));
    }

    if (freq_class) {
        class_destroy(freq_class);
    }

    if (major_num > 0) {
        unregister_chrdev(major_num, name);
    }
}

module_init(cpu_freq_init);
module_exit(cpu_freq_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("NOVEL ALAM, WILLIAM MCGARRY, DANIEL HUFNAGLE");
MODULE_DESCRIPTION("Setting CPU frequency");
MODULE_VERSION("0.0");