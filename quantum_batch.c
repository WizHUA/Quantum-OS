#include <linux/kernel.h>
#include <linux/string.h>
#include <linux/spinlock.h>

#include "quantum_types.h"

/* DEMO-PIVOT D-1: alloc-side ETA helpers (defined in quantum_alloc.c) */
extern void quantum_alloc_eta_add(int backend_id, __u64 ns);

/* ============================================================
 * DEMO-PIVOT Step C: per-backend bundling pool (K=5 FIFO).
 *
 * SSOT §02.5 prescribes a bundling pool that groups intake rows into
 * clusters before they leave for sched. M5 will own the binder with
 * adaptive K_BUNDLE / timeout; the demo collapses it to:
 *   - one pool per backend, capacity K=5, FIFO, no sort
 *   - on hitting K=5: emit a cluster commit log + write
 *     dev_info.backend_eta_ns += cluster_estimated_ns (= total_shots * 1us)
 *
 * The legacy per-Qernel chain (interface->preproc->alloc->batch_run->
 * sched_enqueue) still drives execution; the pool here is bookkeeping
 * + ETA accounting that downstream alloc/sched rely on.
 * ============================================================ */

#define QBATCH_POOL_CAP 5

struct qbatch_pool {
    int     count;
    int     total_shots;
    __u32   first_qid;
    __u8    first_frag;
    __u8    first_var;
};

static struct qbatch_pool g_pool[QUANTUM_MAX_BACKENDS];
static DEFINE_SPINLOCK(g_pool_lock);
static int g_cluster_id;

static void pool_reset(struct qbatch_pool *p)
{
    p->count = 0;
    p->total_shots = 0;
    p->first_qid = 0;
    p->first_frag = 0;
    p->first_var = 0;
}

int quantum_batch_intake(int backend_id,
                         const struct quantum_provenance *prov)
{
    unsigned long flags;
    bool committed = false;
    int  cluster_id = 0, cluster_shots = 0;
    __u32 first_qid = 0;
    __u8  first_frag = 0;
    int   cur_count = 0;
    __u64 add_ns = 0;

    if (!prov)
        return -EINVAL;
    if (backend_id < 0 || backend_id >= QUANTUM_MAX_BACKENDS)
        return -EINVAL;

    pr_info("[batch]   intake backend=%d qid=%u frag=%u var=%u shots=%u\n",
            backend_id, prov->qid, prov->fragment_index,
            prov->variant_index, prov->shots);

    spin_lock_irqsave(&g_pool_lock, flags);
    {
        struct qbatch_pool *p = &g_pool[backend_id];
        if (p->count == 0) {
            p->first_qid  = prov->qid;
            p->first_frag = prov->fragment_index;
            p->first_var  = prov->variant_index;
        }
        p->count       += 1;
        p->total_shots += (int)prov->shots;
        cur_count = p->count;
        if (p->count >= QBATCH_POOL_CAP) {
            g_cluster_id++;
            cluster_id    = g_cluster_id;
            cluster_shots = p->total_shots;
            first_qid     = p->first_qid;
            first_frag    = p->first_frag;
            committed     = true;
            pool_reset(p);
        }
    }
    spin_unlock_irqrestore(&g_pool_lock, flags);

    if (committed) {
        add_ns = (__u64)cluster_shots * 1000ULL;
        quantum_alloc_eta_add(backend_id, add_ns);
        pr_info("[batch]   backend=%d pool size=%d/%d -> commit cluster_id=%d (head qid=%u/%u, total_shots=%d)\n",
                backend_id, QBATCH_POOL_CAP, QBATCH_POOL_CAP,
                cluster_id, first_qid, first_frag, cluster_shots);
        pr_info("[batch]   dev_info update: aer%d eta+=%llu.%03llums (cluster_%d enqueued)\n",
                backend_id, add_ns / 1000000ULL,
                (add_ns / 1000ULL) % 1000ULL, cluster_id);
    } else {
        pr_info("[batch]   backend=%d pool size=%d/%d -> hold (waiting for K=%d)\n",
                backend_id, cur_count, QBATCH_POOL_CAP, QBATCH_POOL_CAP);
    }
    return 0;
}
EXPORT_SYMBOL_GPL(quantum_batch_intake);

/*
 * quantum_batch_flush_all — flush partial pools as residual clusters.
 * Called by sched after the legacy task finishes so trailing rows
 * don't sit in the pool forever in the demo (N×K×M typically not a
 * multiple of K).
 */
int quantum_batch_flush_all(void)
{
    unsigned long flags;
    int b;
    int flushed = 0;

    for (b = 0; b < QUANTUM_MAX_BACKENDS; b++) {
        bool committed = false;
        int  cluster_id = 0, cluster_shots = 0;
        __u32 first_qid = 0;
        __u64 add_ns;

        spin_lock_irqsave(&g_pool_lock, flags);
        if (g_pool[b].count > 0) {
            g_cluster_id++;
            cluster_id    = g_cluster_id;
            cluster_shots = g_pool[b].total_shots;
            first_qid     = g_pool[b].first_qid;
            committed     = true;
            pool_reset(&g_pool[b]);
        }
        spin_unlock_irqrestore(&g_pool_lock, flags);

        if (committed) {
            add_ns = (__u64)cluster_shots * 1000ULL;
            quantum_alloc_eta_add(b, add_ns);
            pr_info("[batch]   backend=%d flush -> commit cluster_id=%d (residual head qid=%u, total_shots=%d)\n",
                    b, cluster_id, first_qid, cluster_shots);
            pr_info("[batch]   dev_info update: aer%d eta+=%llu.%03llums (cluster_%d residual)\n",
                    b, add_ns / 1000000ULL,
                    (add_ns / 1000ULL) % 1000ULL, cluster_id);
            flushed++;
        }
    }
    return flushed;
}
EXPORT_SYMBOL_GPL(quantum_batch_flush_all);

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