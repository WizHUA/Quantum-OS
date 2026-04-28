#include <linux/kernel.h>
#include <linux/kthread.h>
#include <linux/slab.h>
#include <linux/list.h>
#include <linux/spinlock.h>
#include <linux/delay.h>
#include <linux/ktime.h>
#include <linux/string.h>
#include <linux/atomic.h>

#include "quantum_types.h"

/* 前向声明 */
int  quantum_alloc_acquire(int backend_id, int qid);
void quantum_alloc_release(int backend_id);
int  quantum_alloc_find_idle(int need_qubits);
int  quantum_alloc_find_best(const struct quantum_task_struct *task);
void quantum_alloc_get_dev_info(struct quantum_dev_info *out);
int  quantum_postproc_run(struct quantum_task_struct *task);
int  qresult_store_put(struct quantum_task_struct *task);

/* DEMO-PIVOT D-1: batch pool flush + alloc ETA helpers */
int  quantum_batch_flush_all(void);
void quantum_alloc_eta_sub(int backend_id, __u64 ns);
__u64 quantum_alloc_eta_get(int backend_id);

/* ============================================================
 * 全局状态
 * ============================================================ */

static LIST_HEAD(g_task_queue);
static DEFINE_SPINLOCK(g_queue_lock);
static struct task_struct *g_sched_thread;
static atomic_t            g_qid_counter    = ATOMIC_INIT(0);
static int                 g_sched_strategy = QSCHED_STRATEGY_HRRN;

/* 正在被daemon执行的任务（RUNNING状态，FETCH后等待COMMIT）*/
static struct quantum_task_struct *g_running_task;
static DEFINE_SPINLOCK(g_running_lock);

/* ============================================================
 * 内部：helper函数
 * ============================================================ */

static int sched_get_need_qubits(struct quantum_task_struct *task)
{
    if (!task->need_split)
        return task->num_qubits;
    if (task->num_sub_done < task->num_sub_circuits)
        return task->sub_circuits[task->num_sub_done].num_qubits;
    return 0;
}

static const char *sched_get_dispatchable_qasm(struct quantum_task_struct *task)
{
    if (!task->need_split)
        return task->qir;
    if (task->num_sub_done < task->num_sub_circuits)
        return task->sub_circuits[task->num_sub_done].qasm;
    return NULL;
}

static const int *sched_get_dispatchable_phys(struct quantum_task_struct *task)
{
    if (!task->need_split)
        return task->phys_qubits;
    if (task->num_sub_done < task->num_sub_circuits)
        return task->sub_circuits[task->num_sub_done].phys_qubits;
    return NULL;
}

/* ============================================================
 * 内部：调度算法
 * ============================================================ */

static struct quantum_task_struct *sched_hrrn(struct list_head *queue,
                                               u64 now_ns,
                                               const struct quantum_dev_info *dev_snap,
                                               int *out_backend)
{
    struct quantum_task_struct *task, *best = NULL;
    int best_score = -1;

    list_for_each_entry(task, queue, list) {
        u64 wait_ns;
        int exec_units, score, need_q, backend;

        if (task->state != QTASK_STATE_QUEUED)
            continue;

        need_q  = sched_get_need_qubits(task);
        backend = quantum_alloc_find_idle(need_q);
        if (backend < 0)
            continue;

        wait_ns    = now_ns - task->submit_time;
        exec_units = (int)(task->estimated_exec_ns / QUANTUM_EXEC_BASE_NS);
        if (exec_units < 1) exec_units = 1;

        score = (int)((wait_ns / QUANTUM_EXEC_BASE_NS + 1) * 1000 / exec_units);
        score += task->priority * 100;

        if (task->task_type == QTASK_TYPE_CALIB)
            score = INT_MAX;

        if (score > best_score) {
            best_score   = score;
            best         = task;
            *out_backend = backend;
        }
    }

    return best;
}

static struct quantum_task_struct *sched_sjf(struct list_head *queue,
                                              u64 now_ns,
                                              const struct quantum_dev_info *dev_snap,
                                              int *out_backend)
{
    pr_warn_once("quantum_sched: SJF not implemented, falling back to HRRN\n");
    return sched_hrrn(queue, now_ns, dev_snap, out_backend);
}

static struct quantum_task_struct *sched_weighted(struct list_head *queue,
                                                   u64 now_ns,
                                                   const struct quantum_dev_info *dev_snap,
                                                   int *out_backend)
{
    pr_warn_once("quantum_sched: weighted scheduling not implemented, "
                 "falling back to HRRN\n");
    return sched_hrrn(queue, now_ns, dev_snap, out_backend);
}

typedef struct quantum_task_struct *(*quantum_sched_fn)(
    struct list_head *queue,
    u64 now_ns,
    const struct quantum_dev_info *dev_snap,
    int *out_backend);

static const quantum_sched_fn sched_strategy_table[] = {
    [QSCHED_STRATEGY_HRRN]     = sched_hrrn,
    [QSCHED_STRATEGY_SJF]      = sched_sjf,
    [QSCHED_STRATEGY_WEIGHTED] = sched_weighted,
};

#define SCHED_STRATEGY_MAX \
    (sizeof(sched_strategy_table) / sizeof(sched_strategy_table[0]))

/* ============================================================
 * 内部：dispatch机制层
 * ============================================================ */

/*
 * sched_do_dispatch —— 机制层dispatch（选task后调用）
 *
 * 调用前提：task 已在 sched_dispatch_loop 的队列锁内
 *           通过 list_del_init 从队列移除。
 */
static void sched_do_dispatch(struct quantum_task_struct *task, int backend_id)
{
    unsigned long flags;

    if (task->need_split) {
        struct quantum_sub_circuit *sub = &task->sub_circuits[task->num_sub_done];
        int j;
        sub->assigned_backend_id = backend_id;
        sub->state               = QTASK_STATE_RUNNING;
        for (j = 0; j < sub->num_qubits && j < QUANTUM_MAX_QUBITS; j++)
            sub->phys_qubits[j] = j;
        sub->fidelity_score = 0;
    }

    quantum_alloc_acquire(backend_id, task->qid);
    task->state      = QTASK_STATE_RUNNING;
    task->start_time = ktime_get_ns();

    if (task->estimated_exec_ns == 0) {
        int depth = task->need_split ?
                    task->sub_circuits[task->num_sub_done].circuit_depth :
                    task->circuit_depth;
        task->estimated_exec_ns = (u64)depth * task->shots * QUANTUM_EXEC_BASE_NS;
        if (task->estimated_exec_ns < QUANTUM_EXEC_BASE_NS)
            task->estimated_exec_ns = QUANTUM_EXEC_BASE_NS;
    }

    /* list_del_init 已在 sched_dispatch_loop 的队列锁内完成，此处不重复 */

    spin_lock_irqsave(&g_running_lock, flags);
    g_running_task = task;
    spin_unlock_irqrestore(&g_running_lock, flags);

    pr_info("quantum_sched: dispatched qid=%d sub=%d/%d backend=%d\n",
            task->qid,
            task->need_split ? task->num_sub_done + 1 : 1,
            task->need_split ? task->num_sub_circuits : 1,
            backend_id);
}

/* ============================================================
 * 内部：调度主循环
 * ============================================================ */

/*
 * sched_dispatch_loop —— 每个调度周期调用一次
 *
 * 修复：查找和 list_del_init 在同一队列锁内完成，
 *       消除 TOCTOU，并防止 commit re-queue 时链表双重插入。
 */
static void sched_dispatch_loop(void)
{
    struct quantum_task_struct *task = NULL;
    struct quantum_dev_info    *dev_snap;
    unsigned long flags;
    u64 now_ns;
    int backend_id = -1;
    int strategy;

    /* 有 running task 时不再 dispatch */
    spin_lock_irqsave(&g_running_lock, flags);
    if (g_running_task != NULL) {
        spin_unlock_irqrestore(&g_running_lock, flags);
        return;
    }
    spin_unlock_irqrestore(&g_running_lock, flags);

    dev_snap = kzalloc(sizeof(*dev_snap), GFP_KERNEL);
    if (!dev_snap)
        return;

    quantum_alloc_get_dev_info(dev_snap);
    now_ns = ktime_get_ns();

    strategy = g_sched_strategy;
    if (strategy < 0 || strategy >= (int)SCHED_STRATEGY_MAX ||
        !sched_strategy_table[strategy])
        strategy = QSCHED_STRATEGY_HRRN;

    /*
     * 修复：在同一队列锁内完成：选 task + list_del_init
     * 这样 commit 里的 list_add_tail 不会与已在队列的节点冲突
     */
    spin_lock_irqsave(&g_queue_lock, flags);
    task = sched_strategy_table[strategy](&g_task_queue, now_ns,
                                           dev_snap, &backend_id);
    if (task)
        list_del_init(&task->list);
    spin_unlock_irqrestore(&g_queue_lock, flags);

    kfree(dev_snap);

    if (!task || backend_id < 0)
        return;

    sched_do_dispatch(task, backend_id);
}

/* ============================================================
 * kthread主循环
 * ============================================================ */

static int quantum_sched_thread_fn(void *data)
{
    pr_info("quantum_sched: kthread started\n");

    while (!kthread_should_stop()) {
        sched_dispatch_loop();
        msleep(QUANTUM_SCHED_INTERVAL_MS);
    }

    pr_info("quantum_sched: kthread stopped\n");
    return 0;
}

/* ============================================================
 * 对外接口：初始化/退出
 * ============================================================ */

int quantum_sched_init(void)
{
    g_running_task = NULL;

    g_sched_thread = kthread_run(quantum_sched_thread_fn, NULL,
                                  "quantum_sched");
    if (IS_ERR(g_sched_thread)) {
        pr_err("quantum_sched: failed to create kthread: %ld\n",
               PTR_ERR(g_sched_thread));
        return PTR_ERR(g_sched_thread);
    }

    pr_info("quantum_sched: initialized (strategy=HRRN)\n");
    return 0;
}

void quantum_sched_exit(void)
{
    struct quantum_task_struct *task, *tmp;
    unsigned long flags;

    if (g_sched_thread)
        kthread_stop(g_sched_thread);

    spin_lock_irqsave(&g_queue_lock, flags);
    list_for_each_entry_safe(task, tmp, &g_task_queue, list) {
        list_del(&task->list);
        kfree(task);
    }
    spin_unlock_irqrestore(&g_queue_lock, flags);

    spin_lock_irqsave(&g_running_lock, flags);
    if (g_running_task) {
        kfree(g_running_task);
        g_running_task = NULL;
    }
    spin_unlock_irqrestore(&g_running_lock, flags);

    pr_info("quantum_sched: exited\n");
}

/* ============================================================
 * 对外接口：提交链调用
 * ============================================================ */

int quantum_sched_enqueue(struct quantum_task_struct *task)
{
    unsigned long flags;

    task->state = QTASK_STATE_QUEUED;

    spin_lock_irqsave(&g_queue_lock, flags);
    list_add_tail(&task->list, &g_task_queue);
    spin_unlock_irqrestore(&g_queue_lock, flags);

    pr_debug("quantum_sched: enqueued qid=%d priority=%d\n",
             task->qid, task->priority);
    return 0;
}

/* ============================================================
 * 对外接口：daemon交互（fetch/commit）
 * ============================================================ */

int quantum_sched_fetch(struct quantum_fetch_req *out)
{
    struct quantum_task_struct *task;
    const char *qasm;
    const int  *phys;
    unsigned long flags;

    spin_lock_irqsave(&g_running_lock, flags);
    task = g_running_task;
    spin_unlock_irqrestore(&g_running_lock, flags);

    if (!task)
        return -EAGAIN;

    if (task->state != QTASK_STATE_RUNNING)
        return -EAGAIN;

    qasm = sched_get_dispatchable_qasm(task);
    phys = sched_get_dispatchable_phys(task);

    if (!qasm)
        return -EINVAL;

    memset(out, 0, sizeof(*out));
    out->qid              = task->qid;
    out->shots            = task->shots;
    out->error_mitigation = task->error_mitigation;
    out->need_split       = task->need_split;

    if (task->need_split) {
        struct quantum_sub_circuit *sub = &task->sub_circuits[task->num_sub_done];
        out->num_qubits       = sub->num_qubits;
        out->circuit_depth    = sub->circuit_depth;
        out->sub_index        = task->num_sub_done;
        out->num_sub_circuits = task->num_sub_circuits;
        strncpy(out->qasm, sub->qasm, sizeof(out->qasm) - 1);
        if (phys)
            memcpy(out->phys_qubits, phys,
                   sizeof(int) * QUANTUM_MAX_QUBITS);
    } else {
        const char *p = task->qir;
        out->num_qubits       = task->num_qubits;
        out->circuit_depth    = task->circuit_depth;
        out->sub_index        = 0;
        out->num_sub_circuits = 1;
        /* 跳过配置头行 */
        if (p && *p) {
            const char *line_end = strchr(p, '\n');
            if (line_end && strchr(p, '=') &&
                (line_end - p) < 200)
                p = line_end + 1;
        }
        strncpy(out->qasm, p ? p : task->qir, sizeof(out->qasm) - 1);
        if (phys)
            memcpy(out->phys_qubits, phys,
                   sizeof(int) * QUANTUM_MAX_QUBITS);
    }

    pr_debug("quantum_sched: fetched qid=%d sub=%d/%d\n",
             out->qid, out->sub_index + 1, out->num_sub_circuits);
    return 0;
}

int quantum_sched_commit(struct quantum_commit_req *in)
{
    struct quantum_task_struct *task;
    unsigned long flags;
    int ret = 0;

    spin_lock_irqsave(&g_running_lock, flags);
    task = g_running_task;
    spin_unlock_irqrestore(&g_running_lock, flags);

    if (!task) {
        pr_warn("quantum_sched: commit but no running task\n");
        return -EINVAL;
    }

    if (task->qid != in->qid) {
        pr_warn("quantum_sched: commit qid=%d but running qid=%d\n",
                in->qid, task->qid);
        return -EINVAL;
    }

    /* 写入执行结果 */
    if (task->need_split) {
        struct quantum_sub_circuit *sub = &task->sub_circuits[in->sub_index];
        sub->result.shots        = in->shots;
        sub->result.num_outcomes = in->num_outcomes;
        sub->result.error_code   = in->error_code;
        memcpy(sub->result.keys,   in->keys,   sizeof(in->keys));
        memcpy(sub->result.counts, in->counts, sizeof(in->counts));
        strncpy(sub->result.error_info, in->error_info,
                sizeof(sub->result.error_info) - 1);
        sub->state = in->success ? QTASK_STATE_SUCCESS : QTASK_STATE_FAILED;
    } else {
        task->result.shots        = in->shots;
        task->result.num_outcomes = in->num_outcomes;
        task->result.error_code   = in->error_code;
        memcpy(task->result.keys,   in->keys,   sizeof(in->keys));
        memcpy(task->result.counts, in->counts, sizeof(in->counts));
        strncpy(task->result.error_info, in->error_info,
                sizeof(task->result.error_info) - 1);
    }

    /* 释放QPU */
    if (task->need_split)
        quantum_alloc_release(task->sub_circuits[in->sub_index].assigned_backend_id);
    else
        quantum_alloc_release(task->assigned_backend_id);

    /* 清除 running */
    spin_lock_irqsave(&g_running_lock, flags);
    g_running_task = NULL;
    spin_unlock_irqrestore(&g_running_lock, flags);

    /* 调度状态机推进 */
    if (!in->success) {
        task->error_code = in->error_code ? in->error_code : QERR_BACKEND_FAIL;
        strncpy(task->error_info, in->error_info,
                sizeof(task->error_info) - 1);
        task->state = QTASK_STATE_FAILED;
        goto finalize;
    }

    if (task->need_split) {
        task->num_sub_done++;
        if (task->num_sub_done < task->num_sub_circuits) {
            /*
             * 还有子线路未执行：重新入队等待下次 dispatch
             * task->list 已在 sched_dispatch_loop 里 list_del_init 过，
             * 节点干净，可以直接 list_add_tail
             */
            task->state = QTASK_STATE_QUEUED;
            spin_lock_irqsave(&g_queue_lock, flags);
            list_add_tail(&task->list, &g_task_queue);
            spin_unlock_irqrestore(&g_queue_lock, flags);
            pr_debug("quantum_sched: qid=%d sub %d/%d done, re-queued\n",
                     task->qid, task->num_sub_done, task->num_sub_circuits);
            return 0;
        }
    }

finalize:
    task->finish_time = ktime_get_ns();
    task->state       = QTASK_STATE_MERGING;

    /*
     * DEMO-PIVOT Step D: cluster done callback.
     * Flush any partial pools so trailing rows commit, then subtract the
     * actual cluster runtime from dev_info.backend_eta_ns. Demo uses the
     * task's wallclock (finish - submit) as the actual_ns proxy.
     */
    {
        int   bid = task->assigned_backend_id;
        __u64 actual_ns = (task->finish_time > task->submit_time) ?
                          (task->finish_time - task->submit_time) : 0;
        int flushed = quantum_batch_flush_all();
        (void)flushed;
        if (bid >= 0 && bid < QUANTUM_MAX_BACKENDS) {
            __u64 before = quantum_alloc_eta_get(bid);
            quantum_alloc_eta_sub(bid, actual_ns);
            pr_info("[sched]   cluster done qid=%d backend=%d actual=%llu.%03llums (eta %llu.%03llu -> %llu.%03llu ms)\n",
                    task->qid, bid,
                    actual_ns / 1000000ULL,
                    (actual_ns / 1000ULL) % 1000ULL,
                    before / 1000000ULL,
                    (before / 1000ULL) % 1000ULL,
                    quantum_alloc_eta_get(bid) / 1000000ULL,
                    (quantum_alloc_eta_get(bid) / 1000ULL) % 1000ULL);
        }
        pr_info("[sched]   qid=%d backend=%d dispatch+commit complete, %d outcomes\n",
                task->qid, bid, task->result.num_outcomes);
    }

    /*
     * task 已在 sched_dispatch_loop 里 list_del_init，
     * re-queue 路径会再次 list_add_tail 后又被 dispatch 移除，
     * 走到 finalize 时 list 必然为空，保险起见仍做检查
     */
    spin_lock_irqsave(&g_queue_lock, flags);
    if (!list_empty(&task->list))
        list_del_init(&task->list);
    spin_unlock_irqrestore(&g_queue_lock, flags);

    ret = quantum_postproc_run(task);
    if (ret < 0) {
        task->state = QTASK_STATE_FAILED;
        if (!task->error_code)
            task->error_code = QERR_MERGE_FAIL;
    }

    pr_info("quantum_sched: qid=%d finalized state=%d outcomes=%d\n",
            task->qid, task->state, task->result.num_outcomes);

    qresult_store_put(task);
    kfree(task);
    return ret;
}

/* ============================================================
 * 对外接口：interface调用（查询/取消）
 * ============================================================ */

int quantum_sched_query_state(int qid)
{
    struct quantum_task_struct *task;
    unsigned long flags;
    int state = QTASK_STATE_UNKNOWN;

    spin_lock_irqsave(&g_running_lock, flags);
    if (g_running_task && g_running_task->qid == qid) {
        state = g_running_task->state;
        spin_unlock_irqrestore(&g_running_lock, flags);
        return state;
    }
    spin_unlock_irqrestore(&g_running_lock, flags);

    spin_lock_irqsave(&g_queue_lock, flags);
    list_for_each_entry(task, &g_task_queue, list) {
        if (task->qid == qid) {
            state = task->state;
            break;
        }
    }
    spin_unlock_irqrestore(&g_queue_lock, flags);

    return state;
}

int quantum_sched_cancel(int qid)
{
    struct quantum_task_struct *task, *tmp;
    unsigned long flags;
    int found = 0;

    spin_lock_irqsave(&g_queue_lock, flags);
    list_for_each_entry_safe(task, tmp, &g_task_queue, list) {
        if (task->qid == qid) {
            if (task->state == QTASK_STATE_RUNNING) {
                spin_unlock_irqrestore(&g_queue_lock, flags);
                return -EBUSY;
            }
            task->state = QTASK_STATE_CANCELLED;
            list_del(&task->list);
            kfree(task);
            found = 1;
            break;
        }
    }
    spin_unlock_irqrestore(&g_queue_lock, flags);

    return found ? 0 : -ENOENT;
}

/* ============================================================
 * 对外接口：qid分配
 * ============================================================ */

int quantum_sched_alloc_qid(void)
{
    return atomic_inc_return(&g_qid_counter);
}