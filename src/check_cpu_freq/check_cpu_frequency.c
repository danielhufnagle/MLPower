#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/cpufreq.h>

static int __init cpu_freq_init(void) {
  unsigned int frequency = cpufreq_get(0);
  printk("Current frequency: %u kHz\n", frequency);
  return 0;
}

static void __exit cpu_freq_exit(void) {
  printk("CPU frequency check completed\n");
}

module_init(cpu_freq_init);
module_exit(cpu_freq_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("DANIEL HUFNAGLE");
MODULE_DESCRIPTION("Checking CPU frequency");
MODULE_VERSION("0.0");
