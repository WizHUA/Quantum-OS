#include <linux/module.h>
#define INCLUDE_VERMAGIC
#include <linux/build-salt.h>
#include <linux/elfnote-lto.h>
#include <linux/vermagic.h>
#include <linux/compiler.h>

BUILD_SALT;
BUILD_LTO_INFO;

MODULE_INFO(vermagic, VERMAGIC_STRING);
MODULE_INFO(name, KBUILD_MODNAME);

__visible struct module __this_module
__section(".gnu.linkonce.this_module") = {
	.name = KBUILD_MODNAME,
	.init = init_module,
#ifdef CONFIG_MODULE_UNLOAD
	.exit = cleanup_module,
#endif
	.arch = MODULE_ARCH_INIT,
};

#ifdef CONFIG_RETPOLINE
MODULE_INFO(retpoline, "Y");
#endif

static const struct modversion_info ____versions[]
__used __section("__versions") = {
	{ 0x16226510, "module_layout" },
	{ 0x2f250c, "cdev_del" },
	{ 0x43f79047, "kmalloc_caches" },
	{ 0xe19cdba4, "cdev_init" },
	{ 0xf9a482f9, "msleep" },
	{ 0x349cba85, "strchr" },
	{ 0x754d539c, "strlen" },
	{ 0xb43f9365, "ktime_get" },
	{ 0xc606da82, "device_destroy" },
	{ 0x6729d3df, "__get_user_4" },
	{ 0x6091b333, "unregister_chrdev_region" },
	{ 0xdd64e639, "strscpy" },
	{ 0x9a31300c, "pv_ops" },
	{ 0x18bfbaaf, "kthread_create_on_node" },
	{ 0x6b10bee1, "_copy_to_user" },
	{ 0x5b8239ca, "__x86_return_thunk" },
	{ 0xd35cce70, "_raw_spin_unlock_irqrestore" },
	{ 0x6aaedb85, "current_task" },
	{ 0xbcab6ee6, "sscanf" },
	{ 0x3b1195d7, "kthread_stop" },
	{ 0x9166fada, "strncpy" },
	{ 0x5a921311, "strncmp" },
	{ 0xfb6c788a, "device_create" },
	{ 0x59ae7730, "cdev_add" },
	{ 0x87a21cb3, "__ubsan_handle_out_of_bounds" },
	{ 0xa916b694, "strnlen" },
	{ 0xd0da656b, "__stack_chk_fail" },
	{ 0xb8b9f817, "kmalloc_order_trace" },
	{ 0x92997ed8, "_printk" },
	{ 0x2bf8ab5b, "wake_up_process" },
	{ 0xbdfb6dbb, "__fentry__" },
	{ 0xcbd4898c, "fortify_panic" },
	{ 0x42ad08e2, "kmem_cache_alloc_trace" },
	{ 0xba8fbd64, "_raw_spin_lock" },
	{ 0x34db050b, "_raw_spin_lock_irqsave" },
	{ 0xb3f7646e, "kthread_should_stop" },
	{ 0x37a0cba, "kfree" },
	{ 0x9ef993e6, "class_destroy" },
	{ 0x656e4a6e, "snprintf" },
	{ 0x13c49cc2, "_copy_from_user" },
	{ 0xa6034d3c, "__class_create" },
	{ 0x88db9f48, "__check_object_size" },
	{ 0xe3ec2f2b, "alloc_chrdev_region" },
};

MODULE_INFO(depends, "");


MODULE_INFO(srcversion, "9605D512D2F8AEF494C2FD5");
