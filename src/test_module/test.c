#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/module.h>

static int __init test_init(void) {
	printk("Hello world!\n");
	return 0;
}

static void __exit test_exit(void) {
	printk("Goodbye world!\n");
}

module_init(test_init);
module_exit(test_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("NOVEL ALAM, DANIEL HUFNAGLE, WILLIAM MCGARRY");
MODULE_DESCRIPTION("Testing kernel module functionality on Nvidia Jetson");
MODULE_VERSION("0.0");
