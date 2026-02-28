#include <linux/kernel.h>
#include "quantum_types.h"

int quantum_calib_init(void)
{
    printk(KERN_INFO "QuantumOS: calib module init (stub)\n");
    return 0;
}

void quantum_calib_exit(void)
{
    printk(KERN_INFO "QuantumOS: calib module exit\n");
}