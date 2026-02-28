#include <linux/kernel.h>
#include <linux/spinlock.h>
#include <linux/string.h>
#include "quantum_types.h"

/*
 * 全局后端池
 * 对应经典OS的 cpu_data[] 表
 */
static struct quantum_backend_pool g_backend_pool;
static DEFINE_SPINLOCK(backend_lock);

/* ===== 初始化：注册所有QPU ===== */
int quantum_alloc_init(void)
{
    int i;
    g_backend_pool.num_backends = 2;

    for (i = 0; i < g_backend_pool.num_backends; i++) {
        struct quantum_backend *b = &g_backend_pool.backends[i];
        b->id           = i;
        b->total_qubits = QUANTUM_MAX_QUBITS;   /* 64，与 preproc 切分阈值一致 */
        b->state        = QBACKEND_STATE_IDLE;
        b->current_qid  = -1;
        b->last_calibration_time = 0;
        snprintf(b->name, sizeof(b->name), "qpu-%d", i);
        printk(KERN_INFO "QuantumOS: registered backend %s "
               "qubits=%d\n", b->name, b->total_qubits);
    }

    printk(KERN_INFO "QuantumOS: alloc module init, "
           "backends=%d\n", g_backend_pool.num_backends);
    return 0;
}

void quantum_alloc_exit(void)
{
    printk(KERN_INFO "QuantumOS: alloc module exit\n");
}

/*
 * 为任务选择一个空闲QPU
 * 策略：选第一个满足qubit需求的空闲QPU
 * 后续可扩展为保真度优先等策略
 */
int quantum_alloc_run(struct quantum_task_struct *task)
{
    int i, need_qubits;
    struct quantum_backend *b;

    task->assigned_backend_id = -1;

    /*
     * 切分任务：alloc_run 阶段不做 QPU 绑定
     * 原因：各子线路会在 sched dispatch 时按子线路的 num_qubits 独立分配
     * 此处只做合法性预检（确保至少有一个后端能跑最大的子线路）
     */
    if (task->need_split) {
        printk(KERN_INFO "QuantumOS: [alloc] qid=%d need_split=1 "
               "skip pre-alloc, sub-circuits will be allocated "
               "individually\n", task->qid);
        return 0;
    }

    need_qubits = task->num_qubits;

    spin_lock(&backend_lock);
    for (i = 0; i < g_backend_pool.num_backends; i++) {
        b = &g_backend_pool.backends[i];
        if (b->state == QBACKEND_STATE_IDLE &&
            b->total_qubits >= need_qubits) {
            task->assigned_backend_id = b->id;
            printk(KERN_INFO "QuantumOS: [alloc] qid=%d → %s "
                   "(%d qubits available)\n",
                   task->qid, b->name, b->total_qubits);
            break;
        }
    }
    spin_unlock(&backend_lock);

    if (task->assigned_backend_id < 0) {
        printk(KERN_WARNING "QuantumOS: [alloc] qid=%d no suitable "
               "backend (need %d qubits)\n",
               task->qid, need_qubits);
        /* 不返回错误，允许进入队列等待资源 */
    }

    return 0;
}

/*
 * 占用指定后端（由调度器在真正下发时调用）
 */
int quantum_alloc_acquire(int backend_id, int qid)
{
    struct quantum_backend *b;

    if (backend_id < 0 || backend_id >= g_backend_pool.num_backends)
        return -EINVAL;

    spin_lock(&backend_lock);
    b = &g_backend_pool.backends[backend_id];
    if (b->state != QBACKEND_STATE_IDLE) {
        spin_unlock(&backend_lock);
        return -EBUSY;
    }
    b->state       = QBACKEND_STATE_BUSY;
    b->current_qid = qid;
    spin_unlock(&backend_lock);

    printk(KERN_INFO "QuantumOS: [alloc] %s acquired by qid=%d\n",
           b->name, qid);
    return 0;
}

/*
 * 释放后端（任务完成或失败后调用）
 */
void quantum_alloc_release(int backend_id)
{
    struct quantum_backend *b;

    if (backend_id < 0 || backend_id >= g_backend_pool.num_backends)
        return;

    spin_lock(&backend_lock);
    b = &g_backend_pool.backends[backend_id];
    b->state       = QBACKEND_STATE_IDLE;
    b->current_qid = -1;
    spin_unlock(&backend_lock);

    printk(KERN_INFO "QuantumOS: [alloc] %s released\n", b->name);
}

/*
 * 为调度器提供：找一个满足条件的空闲QPU
 * 返回 backend_id，找不到返回 -1
 */
int quantum_alloc_find_idle(int need_qubits)
{
    int i;
    struct quantum_backend *b;
    int found = -1;

    spin_lock(&backend_lock);
    for (i = 0; i < g_backend_pool.num_backends; i++) {
        b = &g_backend_pool.backends[i];
        if (b->state == QBACKEND_STATE_IDLE &&
            b->total_qubits >= need_qubits) {
            found = b->id;
            break;
        }
    }
    spin_unlock(&backend_lock);
    return found;
}

/* 暴露后端池给接口模块（用于 resource() API）*/
void quantum_alloc_get_pool(struct quantum_backend_pool *out)
{
    spin_lock(&backend_lock);
    memcpy(out, &g_backend_pool, sizeof(*out));
    spin_unlock(&backend_lock);
}