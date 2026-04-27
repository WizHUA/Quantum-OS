#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/miscdevice.h>
#include <linux/fs.h>

#include "quantum_types.h"

/* 前向声明：各模块init/exit */
int  quantum_alloc_init(void);
void quantum_alloc_exit(void);
int  quantum_sched_init(void);
void quantum_sched_exit(void);
int  quantum_interface_init(void);
void quantum_interface_exit(void);
int  quantum_calib_init(void);
void quantum_calib_exit(void);

static int __init quantum_os_init(void)
{
    int ret;

    pr_info("quantum_os: initializing\n");

    /* 1. alloc首先初始化（其他模块依赖DevInfo和backend pool） */
    ret = quantum_alloc_init();
    if (ret) {
        pr_err("quantum_os: alloc init failed: %d\n", ret);
        return ret;
    }

    /* 2. sched初始化（启动kthread，依赖alloc） */
    ret = quantum_sched_init();
    if (ret) {
        pr_err("quantum_os: sched init failed: %d\n", ret);
        goto err_sched;
    }

    /* 3. calib初始化（当前stub，里程碑6实装） */
    ret = quantum_calib_init();
    if (ret) {
        pr_err("quantum_os: calib init failed: %d\n", ret);
        goto err_calib;
    }

    /* 4. interface最后初始化（注册设备文件，开始接受用户请求） */
    ret = quantum_interface_init();
    if (ret) {
        pr_err("quantum_os: interface init failed: %d\n", ret);
        goto err_interface;
    }

    pr_info("[main] framework ready abi=%d dev=/dev/%s\n",
            QUANTUM_ABI_VERSION, QUANTUM_DEV_NAME);
    return 0;

err_interface:
    quantum_calib_exit();
err_calib:
    quantum_sched_exit();
err_sched:
    quantum_alloc_exit();
    return ret;
}

static void __exit quantum_os_exit(void)
{
    pr_info("quantum_os: shutting down\n");

    /* 逆序退出：先停止接受新请求，再停止执行，再释放资源 */
    quantum_interface_exit();
    quantum_calib_exit();
    quantum_sched_exit();
    quantum_alloc_exit();

    pr_info("quantum_os: shutdown complete\n");
}

module_init(quantum_os_init);
module_exit(quantum_os_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("QuantumOS");
MODULE_DESCRIPTION("Quantum OS Kernel Framework");
MODULE_VERSION("4.0");