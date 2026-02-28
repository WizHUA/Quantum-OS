#include <linux/kernel.h>
#include <linux/module.h>

#include "quantum_types.h"

/*
 * quantum_calib.c —— 量子比特校准模块
 *
 * 当前状态：stub，提供接口框架，里程碑6实装kthread。
 *
 * 里程碑6实现计划：
 *   1. 启动 kthread "quantum_calib"
 *   2. 每 QUANTUM_CALIB_INTERVAL_S 秒触发一次
 *   3. alloc_set_backend_state(id, CALIBRATING)
 *   4. 构造 QTASK_TYPE_CALIB 任务并提交 sched_enqueue
 *   5. postproc 识别CALIB任务后调用 alloc_update_qubit() 更新DevInfo
 *   6. alloc_set_backend_state(id, IDLE)
 */

int quantum_calib_init(void)
{
    pr_info("quantum_calib: initialized (stub, milestone 6 will activate)\n");
    return 0;
}

void quantum_calib_exit(void)
{
    pr_info("quantum_calib: exiting\n");
}

int quantum_calib_trigger(int backend_id)
{
    pr_info("quantum_calib: trigger backend=%d (stub)\n", backend_id);
    return 0;
}