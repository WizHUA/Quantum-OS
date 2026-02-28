#include <linux/kernel.h>
#include <linux/kthread.h>
#include <linux/list.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/delay.h>
#include <linux/ktime.h>
#include "quantum_types.h"

/* 前向声明 */
int  quantum_postproc_run(struct quantum_task_struct *task);
int  quantum_alloc_acquire(int backend_id, int qid);
void quantum_alloc_release(int backend_id);
int  quantum_alloc_find_idle(int need_qubits);
int  qresult_store_put(struct quantum_task_struct *task);

/* ===== 调度队列 ===== */
static LIST_HEAD(quantum_task_queue);
static DEFINE_SPINLOCK(queue_lock);
static struct task_struct *sched_thread;

/* ===== HRRN 计算 ===== */
static u64 estimate_exec_time(struct quantum_task_struct *task)
{
    int depth = task->circuit_depth > 0 ? task->circuit_depth : 1;
    int shots = task->shots        > 0 ? task->shots         : 1;
    return (u64)shots * (u64)depth * QUANTUM_EXEC_BASE_NS;
}

static int calc_hrrn_score(struct quantum_task_struct *task, u64 now_ns)
{
    u64 wait_ns, est_ns, score;

    if (task->task_type == QTASK_TYPE_CALIB)
        return INT_MAX;

    wait_ns = now_ns > task->submit_time
              ? now_ns - task->submit_time : 0;
    est_ns  = task->estimated_exec_ns;
    if (est_ns == 0)
        est_ns = QUANTUM_EXEC_BASE_NS;

    score  = (wait_ns / est_ns + 1) * 1000
             + (wait_ns % est_ns) * 1000 / est_ns;
    score += (u64)task->priority * 200;

    return (int)min_t(u64, score, (u64)INT_MAX);
}

/*
 * pick_best_task
 *
 * 切分任务的 num_qubits 是原始总 qubit 数（可能超限），
 * 调度时应按当前待执行子线路的 num_qubits 来匹配 QPU。
 * need_split=0：直接用 task->num_qubits
 * need_split=1：用 sub_circuits[num_sub_done].num_qubits
 */
static struct quantum_task_struct *pick_best_task(u64 now_ns,
                                                   int *out_backend_id)
{
    struct quantum_task_struct *task, *best = NULL;
    int best_score = -1;
    int backend_id;
    int need_qubits;

    list_for_each_entry(task, &quantum_task_queue, list) {
        if (task->state != QTASK_STATE_QUEUED)
            continue;

        task->hrrn_score_int = calc_hrrn_score(task, now_ns);
        if (task->hrrn_score_int <= best_score)
            continue;

        if (task->need_split && task->num_sub_circuits > 0) {
            int idx = task->num_sub_done;
            if (idx >= task->num_sub_circuits)
                continue;
            /* 按当前子线路的 qubit 数找 QPU */
            need_qubits = task->sub_circuits[idx].num_qubits;
        } else {
            need_qubits = task->num_qubits;
        }

        /* 动态找空闲 QPU（切分任务和普通任务统一走这里）*/
        backend_id = quantum_alloc_find_idle(need_qubits);
        if (backend_id < 0) {
            printk(KERN_DEBUG "QuantumOS: [sched] qid=%d "
                   "no idle backend for %d qubits, skip\n",
                   task->qid, need_qubits);
            continue;
        }

        best_score      = task->hrrn_score_int;
        best            = task;
        *out_backend_id = backend_id;
    }
    return best;
}

/* ===== 调度器主循环 ===== */
static int quantum_sched_thread(void *data)
{
    struct quantum_task_struct *task;
    int backend_id;
    u64 now_ns;
    int ret;

    printk(KERN_INFO "QuantumOS: sched kthread started "
           "(HRRN + multi-QPU)\n");

    while (!kthread_should_stop()) {
        msleep(QUANTUM_SCHED_INTERVAL_MS);

        while (1) {
            backend_id = -1;

            spin_lock(&queue_lock);
            now_ns = ktime_get_ns();
            task   = pick_best_task(now_ns, &backend_id);

            if (!task) {
                spin_unlock(&queue_lock);
                break;
            }

            task->state               = QTASK_STATE_RUNNING;
            task->start_time          = now_ns;
            task->assigned_backend_id = backend_id;
            spin_unlock(&queue_lock);

            ret = quantum_alloc_acquire(backend_id, task->qid);
            if (ret) {
                task->state = QTASK_STATE_QUEUED;
                break;
            }

            if (task->need_split) {
                printk(KERN_INFO "QuantumOS: [sched] dispatched "
                       "qid=%d sub[%d/%d] → qpu-%d score=%d\n",
                       task->qid, task->num_sub_done,
                       task->num_sub_circuits,
                       backend_id, task->hrrn_score_int);
            } else {
                printk(KERN_INFO "QuantumOS: [sched] dispatched "
                       "qid=%d → qpu-%d score=%d\n",
                       task->qid, backend_id, task->hrrn_score_int);
            }
        }
    }

    printk(KERN_INFO "QuantumOS: sched kthread stopped\n");
    return 0;
}

/* ===== 对外接口 ===== */
int quantum_sched_enqueue(struct quantum_task_struct *task)
{
    if (!task)
        return -EINVAL;

    task->estimated_exec_ns = estimate_exec_time(task);
    task->state             = QTASK_STATE_QUEUED;

    spin_lock(&queue_lock);
    list_add_tail(&task->list, &quantum_task_queue);
    spin_unlock(&queue_lock);

    printk(KERN_INFO "QuantumOS: [sched] enqueued qid=%d "
           "priority=%d est=%llums\n",
           task->qid, task->priority,
           task->estimated_exec_ns / 1000000);
    return 0;
}

/*
 * quantum_sched_fetch
 *
 * need_split=0：返回 task->qir，行为与之前一致
 * need_split=1：返回 sub_circuits[num_sub_done].qasm
 *               并在 fetch_req 中填写 sub_index/num_sub_circuits
 *               供 daemon 记录和透传给 commit
 */
int quantum_sched_fetch(struct quantum_fetch_req *out)
{
    struct quantum_task_struct *task;
    unsigned long flags;
    int found = 0;
    int sub_idx;
    const char *qasm_src;

    spin_lock_irqsave(&queue_lock, flags);
    list_for_each_entry(task, &quantum_task_queue, list) {
        if (task->state != QTASK_STATE_RUNNING)
            continue;

        out->qid              = task->qid;
        out->shots            = task->shots;
        out->error_mitigation = task->error_mitigation;

        if (task->need_split && task->num_sub_circuits > 0) {
            /*
             * 切分任务：取当前待执行的子线路
             * sub_circuits[num_sub_done] 是下一条待执行的子线路
             */
            sub_idx = task->num_sub_done;
            if (sub_idx >= task->num_sub_circuits) {
                /* 所有子线路已提交，等待 commit 触发 postproc */
                continue;
            }

            qasm_src            = task->sub_circuits[sub_idx].qasm;
            out->num_qubits     = task->sub_circuits[sub_idx].num_qubits;
            out->circuit_depth  = task->sub_circuits[sub_idx].circuit_depth;
            out->need_split     = 1;
            out->sub_index      = sub_idx;
            out->num_sub_circuits = task->num_sub_circuits;

            /* 标记该子线路为 RUNNING */
            task->sub_circuits[sub_idx].state = QTASK_STATE_RUNNING;

            printk(KERN_INFO "QuantumOS: [fetch] qid=%d sub[%d/%d] "
                   "qubits=%d\n",
                   task->qid, sub_idx, task->num_sub_circuits,
                   out->num_qubits);
        } else {
            /* 普通任务：返回原始 QASM */
            qasm_src           = task->qir;
            out->num_qubits    = task->num_qubits;
            out->circuit_depth = task->circuit_depth;
            out->need_split    = 0;
            out->sub_index     = 0;
            out->num_sub_circuits = 1;
        }

        strncpy(out->qasm, qasm_src, QUANTUM_QIR_SIZE - 1);
        out->qasm[QUANTUM_QIR_SIZE - 1] = '\0';
        found = 1;
        break;
    }
    spin_unlock_irqrestore(&queue_lock, flags);

    if (!found) {
        out->qid = -1;
        return -EAGAIN;
    }

    return 0;
}

/*
 * quantum_sched_commit
 *
 * need_split=0：直接写 task->result，触发 postproc，从队列删除
 * need_split=1：
 *   ① 把结果写入 sub_circuits[sub_index].result
 *   ② num_sub_done++，释放 QPU
 *   ③ 若还有子线路未执行：把任务重新置为 QUEUED，等待下一轮调度
 *   ④ 若全部完成：触发 postproc，从队列删除，存入结果表
 */
int quantum_sched_commit(struct quantum_commit_req *in)
{
    struct quantum_task_struct *task = NULL, *pos;
    unsigned long flags;
    int sub_idx, i, all_done;
    struct quantum_result *sr;

    /* 找到对应任务 */
    spin_lock_irqsave(&queue_lock, flags);
    list_for_each_entry(pos, &quantum_task_queue, list) {
        if (pos->qid == in->qid) {
            task = pos;
            break;
        }
    }
    spin_unlock_irqrestore(&queue_lock, flags);

    if (!task) {
        printk(KERN_WARNING "QuantumOS: [commit] qid=%d not found\n",
               in->qid);
        return -ENOENT;
    }

    if (task->need_split && task->num_sub_circuits > 0) {
        /*
         * 切分任务：把结果写入对应子线路
         */
        sub_idx = in->sub_index;
        if (sub_idx < 0 || sub_idx >= task->num_sub_circuits) {
            printk(KERN_WARNING "QuantumOS: [commit] qid=%d "
                   "invalid sub_index=%d\n", in->qid, sub_idx);
            return -EINVAL;
        }

        /* 写入子线路结果 */
        sr = &task->sub_circuits[sub_idx].result;
        sr->qid          = in->qid;
        sr->shots        = in->shots;
        sr->num_outcomes = in->num_outcomes;
        sr->error_code   = in->error_code;
        strncpy(sr->error_info, in->error_info,
                sizeof(sr->error_info) - 1);
        for (i = 0; i < in->num_outcomes && i < QUANTUM_MAX_OUTCOMES; i++) {
            strncpy(sr->keys[i], in->keys[i],
                    sizeof(sr->keys[i]) - 1);
            sr->counts[i] = in->counts[i];
        }

        if (in->success)
            task->sub_circuits[sub_idx].state = QTASK_STATE_SUCCESS;
        else
            task->sub_circuits[sub_idx].state = QTASK_STATE_FAILED;

        task->num_sub_done++;
        quantum_alloc_release(task->assigned_backend_id);

        printk(KERN_INFO "QuantumOS: [commit] qid=%d sub[%d/%d] %s\n",
               in->qid, sub_idx, task->num_sub_circuits,
               in->success ? "success" : "failed");

        all_done = (task->num_sub_done >= task->num_sub_circuits);

        if (!all_done) {
            /*
             * 还有子线路未执行：重新入队等待调度
             * 把 state 改回 QUEUED，sched 下一轮会继续取
             */
            spin_lock_irqsave(&queue_lock, flags);
            task->state = QTASK_STATE_QUEUED;
            spin_unlock_irqrestore(&queue_lock, flags);

            printk(KERN_INFO "QuantumOS: [commit] qid=%d "
                   "re-queued for sub[%d/%d]\n",
                   in->qid, task->num_sub_done,
                   task->num_sub_circuits);
            return 0;
        }

        /* 全部子线路完成 */
        task->state       = QTASK_STATE_MERGING;
        task->finish_time = ktime_get_ns();
        printk(KERN_INFO "QuantumOS: [commit] qid=%d all %d "
            "sub-circuits done, merging\n",
            in->qid, task->num_sub_circuits);

        /* 触发后处理（合并子线路结果）*/
        quantum_postproc_run(task);

        /* postproc 完成后更新最终状态 */
        if (task->result.error_code == 0)
            task->state = QTASK_STATE_SUCCESS;
        else
            task->state = QTASK_STATE_FAILED;

    } else {
        /* 普通任务：直接写结果 */
        task->result.qid          = in->qid;
        task->result.shots        = in->shots;
        task->result.num_outcomes = in->num_outcomes;
        task->result.error_code   = in->error_code;
        strncpy(task->result.error_info, in->error_info,
                sizeof(task->result.error_info) - 1);
        for (i = 0; i < in->num_outcomes && i < QUANTUM_MAX_OUTCOMES; i++) {
            strncpy(task->result.keys[i], in->keys[i],
                    sizeof(task->result.keys[i]) - 1);
            task->result.counts[i] = in->counts[i];
        }

        if (in->success)
            task->state = QTASK_STATE_SUCCESS;
        else
            task->state = QTASK_STATE_FAILED;

        task->finish_time = ktime_get_ns();
        quantum_alloc_release(task->assigned_backend_id);

        /* 普通任务也在这里触发 postproc */
        quantum_postproc_run(task);
    }

    /* 从队列删除，存入结果表 */
    spin_lock_irqsave(&queue_lock, flags);
    list_del(&task->list);
    spin_unlock_irqrestore(&queue_lock, flags);

    qresult_store_put(task);
    kfree(task);
    return 0;
}

int quantum_sched_query_state(int qid)
{
    struct quantum_task_struct *task;
    unsigned long flags;
    int state = QTASK_STATE_UNKNOWN;

    spin_lock_irqsave(&queue_lock, flags);
    list_for_each_entry(task, &quantum_task_queue, list) {
        if (task->qid == qid) {
            state = task->state;
            break;
        }
    }
    spin_unlock_irqrestore(&queue_lock, flags);
    return state;
}

int quantum_sched_cancel(int qid)
{
    struct quantum_task_struct *task = NULL, *pos;
    unsigned long flags;

    spin_lock_irqsave(&queue_lock, flags);
    list_for_each_entry(pos, &quantum_task_queue, list) {
        if (pos->qid == qid) {
            task = pos;
            list_del(&task->list);
            break;
        }
    }
    spin_unlock_irqrestore(&queue_lock, flags);

    if (!task)
        return -ENOENT;

    if (task->state == QTASK_STATE_RUNNING &&
        task->assigned_backend_id >= 0)
        quantum_alloc_release(task->assigned_backend_id);

    task->state                   = QTASK_STATE_CANCELLED;
    task->result.qid              = task->qid;
    task->result.shots            = task->shots;
    task->result.num_outcomes     = 0;
    task->result.error_code       = -ECANCELED;
    strscpy(task->result.error_info, "cancelled by user",
            sizeof(task->result.error_info));

    qresult_store_put(task);
    kfree(task);
    printk(KERN_INFO "QuantumOS: [sched] cancelled qid=%d\n", qid);
    return 0;
}

int quantum_sched_init(void)
{
    sched_thread = kthread_run(quantum_sched_thread, NULL, "quantum_sched");
    if (IS_ERR(sched_thread)) {
        printk(KERN_ERR "QuantumOS: failed to create sched kthread\n");
        return PTR_ERR(sched_thread);
    }
    printk(KERN_INFO "QuantumOS: sched module init (HRRN + multi-QPU)\n");
    return 0;
}

void quantum_sched_exit(void)
{
    struct quantum_task_struct *task, *tmp;

    if (sched_thread)
        kthread_stop(sched_thread);

    spin_lock(&queue_lock);
    list_for_each_entry_safe(task, tmp, &quantum_task_queue, list) {
        list_del(&task->list);
        kfree(task);
    }
    spin_unlock(&queue_lock);

    printk(KERN_INFO "QuantumOS: sched module exit\n");
}