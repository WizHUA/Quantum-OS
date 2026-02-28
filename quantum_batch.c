#include <linux/kernel.h>
#include <linux/string.h>

#include "quantum_types.h"

/* ============================================================
 * 内部：验证
 * ============================================================ */

/*
 * validate_sub_circuits —— 检查切分算法输出的子线路元数据合法性
 *
 * 检查内容：
 *   - dep_sub_id 范围合法且无自引用
 *   - dep_sub_id 依赖图无环（当前简单检查：只允许dep_sub_id < index）
 *   - num_qubits > 0 且 <= QUANTUM_MAX_QUBITS
 *   - weight_den != 0
 */
static int validate_sub_circuits(struct quantum_task_struct *task)
{
    int i;

    for (i = 0; i < task->num_sub_circuits; i++) {
        struct quantum_sub_circuit *sub = &task->sub_circuits[i];

        if (sub->num_qubits <= 0 || sub->num_qubits > QUANTUM_MAX_QUBITS) {
            pr_warn("quantum_batch: qid=%d sub[%d] invalid num_qubits=%d\n",
                    task->qid, i, sub->num_qubits);
            return -EINVAL;
        }

        if (sub->weight_den <= 0) {
            pr_warn("quantum_batch: qid=%d sub[%d] weight_den=%d (must > 0)\n",
                    task->qid, i, sub->weight_den);
            return -EINVAL;
        }

        if (sub->dep_sub_id != -1) {
            if (sub->dep_sub_id < 0 ||
                sub->dep_sub_id >= task->num_sub_circuits ||
                sub->dep_sub_id >= i) {
                /*
                 * 简单无环检查：dep_sub_id必须严格小于当前index
                 * 里程碑6：替换为完整的Kahn拓扑排序
                 */
                pr_warn("quantum_batch: qid=%d sub[%d] dep_sub_id=%d invalid "
                        "(must be -1 or < %d)\n",
                        task->qid, i, sub->dep_sub_id, i);
                return -EINVAL;
            }
        }
    }

    return 0;
}

/* ============================================================
 * 内部：批处理模式实现
 * ============================================================ */

/*
 * batch_serial —— 串行模式（当前实现）
 *
 * 子线路将由sched按index顺序逐个dispatch，
 * 本函数仅做标记和日志，无实质操作。
 */
static int batch_serial(struct quantum_task_struct *task)
{
    pr_debug("quantum_batch: qid=%d serial mode, %d sub-circuits in order\n",
             task->qid, task->num_sub_circuits);
    return 0;
}

/*
 * batch_parallel —— 并行模式（里程碑6占位）
 *
 * 为每个sub创建独立的child_task并发入队，
 * 各子线路可在不同QPU上同时执行。
 */
static int batch_parallel(struct quantum_task_struct *task)
{
    /*
     * 里程碑6实现提示：
     * for each sub in task->sub_circuits:
     *   child = kzalloc(sizeof(quantum_task_struct))
     *   copy sub->qasm → child->qir
     *   child->parent_qid = task->qid（需在task_struct中添加字段）
     *   quantum_sched_enqueue(child)
     */
    pr_warn_once("quantum_batch: parallel mode not implemented, "
                 "falling back to serial\n");
    return batch_serial(task);
}

/*
 * batch_pipeline —— 流水线模式（里程碑6占位）
 *
 * 按dep_sub_id构建依赖DAG，拓扑排序后按序调度，
 * 前序子线路完成后解锁后续子线路进入QUEUED状态。
 */
static int batch_pipeline(struct quantum_task_struct *task)
{
    pr_warn_once("quantum_batch: pipeline mode not implemented, "
                 "falling back to serial\n");
    return batch_serial(task);
}

/* 批处理模式函数指针类型 */
typedef int (*quantum_batch_fn)(struct quantum_task_struct *task);

/* 模式路由表 */
static const quantum_batch_fn batch_mode_table[] = {
    [QBATCH_MODE_SERIAL]   = batch_serial,
    [QBATCH_MODE_PARALLEL] = batch_parallel,
    [QBATCH_MODE_PIPELINE] = batch_pipeline,
};

#define BATCH_MODE_MAX \
    (sizeof(batch_mode_table) / sizeof(batch_mode_table[0]))

/* ============================================================
 * 对外接口
 * ============================================================ */

/*
 * quantum_batch_run —— 批处理模块唯一对外接口
 *
 * need_split=0：无子线路，直接pass through
 * need_split=1：验证子线路元数据，按batch_mode处理
 */
int quantum_batch_run(struct quantum_task_struct *task)
{
    int mode, ret;
    quantum_batch_fn fn;

    if (!task->need_split) {
        pr_debug("quantum_batch: qid=%d no split, pass through\n", task->qid);
        return 0;
    }

    if (task->num_sub_circuits <= 0 ||
        task->num_sub_circuits > QUANTUM_MAX_SUB_CIRCUITS) {
        pr_warn("quantum_batch: qid=%d invalid num_sub_circuits=%d\n",
                task->qid, task->num_sub_circuits);
        return -EINVAL;
    }

    /* 机制层：验证子线路合法性 */
    ret = validate_sub_circuits(task);
    if (ret < 0) {
        task->error_code = QERR_SPLIT_FAIL;
        strncpy(task->error_info, "sub-circuit metadata validation failed",
                sizeof(task->error_info) - 1);
        return ret;
    }

    /* 策略层：按batch_mode路由 */
    mode = task->batch_mode;
    if (mode < 0 || mode >= (int)BATCH_MODE_MAX || !batch_mode_table[mode])
        mode = QBATCH_MODE_SERIAL;

    fn = batch_mode_table[mode];
    ret = fn(task);
    if (ret < 0) {
        task->error_code = QERR_SPLIT_FAIL;
        strncpy(task->error_info, "batch processing failed",
                sizeof(task->error_info) - 1);
        return ret;
    }

    pr_debug("quantum_batch: qid=%d batch_mode=%d num_sub=%d validated\n",
             task->qid, mode, task->num_sub_circuits);
    return 0;
}