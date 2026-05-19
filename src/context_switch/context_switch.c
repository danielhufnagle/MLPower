#include <linux/tracepoint.h>
#include <trace/events/sched.h>
#include <linux/module.h>
#include <linux/kernel.h>


static void context_switch_cb( void* data, bool preempt, struct task_struct *prev, struct task_struct *next );

static int __init context_switch_init(void) {
    int ret = register_trace_sched_switch(context_switch_cb, NULL);

    if (ret) {
        pr_err("Failed to register context_switch_cb\n");
    }

    return ret;
}

static void __exit context_switch_exit(void) {
    int ret = unregister_trace_sched_switch(context_switch_cb, NULL);

    if (ret) {
        pr_err("Failed to unregister context_switch_cb\n");
    }
}


static void context_switch_cb( void* data, bool preempt, struct task_struct *prev, struct task_struct *next ) {
    printk("Context Switch from pid:%u to pid:%d", (int)prev->pid, (int)next->pid);
}




module_init(context_switch_init);
module_exit(context_switch_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Novel Alam, Daniel Hufnagle, William McGarry");
MODULE_DESCRIPTION("ARM Context Switch handler");
MODULE_VERSION("2.0");
