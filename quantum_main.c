#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/fs.h>
#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/uaccess.h>
#include "quantum_types.h"

/* 前向声明：各模块初始化/退出函数 */
int  quantum_interface_init(void);
void quantum_interface_exit(void);
int  quantum_sched_init(void);
void quantum_sched_exit(void);
int  quantum_alloc_init(void);
void quantum_alloc_exit(void);
int  quantum_calib_init(void);
void quantum_calib_exit(void);
void qresult_store_clear(void);

/* 字符设备相关 */
static dev_t        quantum_devno;
static struct cdev  quantum_cdev;
static struct class *quantum_class;

/* 字符设备文件操作（由 interface.c 实现，这里声明） */
extern const struct file_operations quantum_fops;

static int __init quantum_os_init(void)
{
    int ret;

    printk(KERN_INFO "QuantumOS: initializing quantum kernel framework\n");

    /* 1. 动态申请设备号 */
    ret = alloc_chrdev_region(&quantum_devno, 0, 1, QUANTUM_DEV_NAME);
    if (ret < 0) {
        printk(KERN_ERR "QuantumOS: failed to alloc chrdev region, ret=%d\n", ret);
        return ret;
    }
    printk(KERN_INFO "QuantumOS: device number allocated: major=%d minor=%d\n",
           MAJOR(quantum_devno), MINOR(quantum_devno));

    /* 2. 初始化并注册字符设备 */
    cdev_init(&quantum_cdev, &quantum_fops);
    quantum_cdev.owner = THIS_MODULE;
    ret = cdev_add(&quantum_cdev, quantum_devno, 1);
    if (ret < 0) {
        printk(KERN_ERR "QuantumOS: failed to add cdev, ret=%d\n", ret);
        goto err_cdev;
    }

    /* 3. 创建设备类（/sys/class/quantum） */
    quantum_class = class_create(THIS_MODULE, QUANTUM_DEV_NAME);
    if (IS_ERR(quantum_class)) {
        ret = PTR_ERR(quantum_class);
        printk(KERN_ERR "QuantumOS: failed to create class, ret=%d\n", ret);
        goto err_class;
    }

    /* 4. 创建设备节点（/dev/quantum） */
    if (IS_ERR(device_create(quantum_class, NULL, quantum_devno,
                             NULL, QUANTUM_DEV_NAME))) {
        ret = -EINVAL;
        printk(KERN_ERR "QuantumOS: failed to create device node\n");
        goto err_device;
    }
    printk(KERN_INFO "QuantumOS: /dev/%s created\n", QUANTUM_DEV_NAME);

    /* 5. 初始化各子模块 */
    ret = quantum_alloc_init();
    if (ret) {
        printk(KERN_ERR "QuantumOS: alloc module init failed\n");
        goto err_alloc;
    }

    ret = quantum_sched_init();
    if (ret) {
        printk(KERN_ERR "QuantumOS: sched module init failed\n");
        goto err_sched;
    }

    ret = quantum_interface_init();
    if (ret) {
        printk(KERN_ERR "QuantumOS: interface module init failed\n");
        goto err_interface;
    }

    ret = quantum_calib_init();
    if (ret) {
        printk(KERN_ERR "QuantumOS: calib module init failed\n");
        goto err_calib;
    }

    printk(KERN_INFO "QuantumOS: framework ready\n");
    return 0;

err_calib:
    quantum_interface_exit();
err_interface:
    quantum_sched_exit();
err_sched:
    quantum_alloc_exit();
err_alloc:
    device_destroy(quantum_class, quantum_devno);
err_device:
    class_destroy(quantum_class);
err_class:
    cdev_del(&quantum_cdev);
err_cdev:
    unregister_chrdev_region(quantum_devno, 1);
    return ret;
}

static void __exit quantum_os_exit(void)
{
    printk(KERN_INFO "QuantumOS: shutting down\n");

    quantum_calib_exit();
    quantum_interface_exit();
    quantum_sched_exit();
    quantum_alloc_exit();
    qresult_store_clear();

    device_destroy(quantum_class, quantum_devno);
    class_destroy(quantum_class);
    cdev_del(&quantum_cdev);
    unregister_chrdev_region(quantum_devno, 1);

    printk(KERN_INFO "QuantumOS: framework unloaded\n");
}

module_init(quantum_os_init);
module_exit(quantum_os_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("QuantumOS");
MODULE_DESCRIPTION("Quantum task management kernel framework");
MODULE_VERSION("0.1");