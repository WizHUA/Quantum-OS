#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/list.h>
#include <linux/spinlock.h>
#include <linux/string.h>
#include <linux/mutex.h>
#include <linux/wait.h>
#include <linux/sched.h>
#include <linux/mm.h>
#include <linux/vmalloc.h>
#include <linux/jiffies.h>

#include "quantum_types.h"

/* ============================================================
 * 结果存储节点（对标zombie进程表条目）
 * ============================================================ */

struct result_entry {
    int                  qid;
    int                  state;         /* QTASK_STATE_SUCCESS/FAILED */
    struct quantum_result result;
    struct list_head     list;
};

static LIST_HEAD(g_result_list);
static DEFINE_SPINLOCK(g_result_lock);

/* ============================================================
 * 对外接口
 * ============================================================ */

/*
 * qresult_store_put —— 任务完成，存入结果
 * 由 sched_commit 在 kfree(task) 前调用
 * 注意：此函数在task释放前调用，复制必要字段
 */
int qresult_store_put(struct quantum_task_struct *task)
{
    struct result_entry *entry;
    unsigned long flags;

    entry = kzalloc(sizeof(*entry), GFP_ATOMIC);
    if (!entry) {
        pr_err("quantum_result_store: failed to alloc entry for qid=%d\n",
               task->qid);
        return -ENOMEM;
    }

    entry->qid   = task->qid;
    entry->state = task->state;
    memcpy(&entry->result, &task->result, sizeof(entry->result));

    /* 若内核侧有error_code，同步到result */
    if (task->error_code != QERR_OK) {
        entry->result.error_code = task->error_code;
        strncpy(entry->result.error_info, task->error_info,
                sizeof(entry->result.error_info) - 1);
    }

    spin_lock_irqsave(&g_result_lock, flags);
    list_add_tail(&entry->list, &g_result_list);
    spin_unlock_irqrestore(&g_result_lock, flags);

    pr_debug("quantum_result_store: stored qid=%d state=%d outcomes=%d\n",
             entry->qid, entry->state, entry->result.num_outcomes);
    return 0;
}

/*
 * qresult_store_status —— 查询状态（interface STATUS ioctl调用）
 * 返回 QTASK_STATE_SUCCESS/FAILED，或 -1=未找到
 */
int qresult_store_status(int qid)
{
    struct result_entry *entry;
    unsigned long flags;
    int state = -1;

    spin_lock_irqsave(&g_result_lock, flags);
    list_for_each_entry(entry, &g_result_list, list) {
        if (entry->qid == qid) {
            state = entry->state;
            break;
        }
    }
    spin_unlock_irqrestore(&g_result_lock, flags);
    return state;
}

/*
 * qresult_store_get —— 取回结果并从存储中删除
 * 成功返回0，未找到返回-ENOENT
 */
int qresult_store_get(int qid, struct quantum_result *out)
{
    struct result_entry *entry, *tmp;
    unsigned long flags;
    int found = 0;

    spin_lock_irqsave(&g_result_lock, flags);
    list_for_each_entry_safe(entry, tmp, &g_result_list, list) {
        if (entry->qid == qid) {
            if (out)
                memcpy(out, &entry->result, sizeof(*out));
            list_del(&entry->list);
            kfree(entry);
            found = 1;
            break;
        }
    }
    spin_unlock_irqrestore(&g_result_lock, flags);

    return found ? 0 : -ENOENT;
}

/*
 * qresult_store_clear —— 清空所有结果（模块卸载时调用）
 */
void qresult_store_clear(void)
{
    struct result_entry *entry, *tmp;
    unsigned long flags;

    spin_lock_irqsave(&g_result_lock, flags);
    list_for_each_entry_safe(entry, tmp, &g_result_list, list) {
        list_del(&entry->list);
        kfree(entry);
    }
    spin_unlock_irqrestore(&g_result_lock, flags);
}

/* ============================================================
 * ABI v3 — qernel_table + task_table  (§03.4)
 * ============================================================
 * One coarse mutex protects both tables (`g_store_mtx`).
 * Per-Qernel wait queues let userspace block on terminal state.
 *
 * Storage:
 *   - g_qernel_table: array of QUANTUM_MAX_QERNELS+1 slots, qid is 1-indexed
 *     (qid=0 reserved as "invalid"). Each slot embeds quantum_qernel_row
 *     (~ a few KB) so the whole table is allocated with kvcalloc.
 *   - g_task_lists[qid]: per-Qernel list of task_node{quantum_task_row,link},
 *     each row kmalloc'ed individually. Rows are dropped only when postproc
 *     calls task_table_drop_qernel(qid).
 *
 * Concurrency rules:
 *   - All accessors take g_store_mtx internally.
 *   - "*_locked" variants assume the caller already holds the mutex
 *     (used by debugfs iterators that need to scan the whole table).
 *   - State transitions are validated by qernel_state_rank() / variant_state_rank():
 *     monotone forward, with FAILED/CANCELLED reachable from any non-terminal.
 * ============================================================ */

struct task_node {
    struct quantum_task_row row;
    struct list_head        link;
};

struct qernel_slot {
    bool                    used;
    struct quantum_qernel_row row;
    struct list_head        tasks;       /* head of task_node list */
    wait_queue_head_t       result_waitq;
};

static struct qernel_slot   *g_qernel_table;     /* size = QUANTUM_MAX_QERNELS+1 */
static DEFINE_MUTEX(g_store_mtx);
static atomic_t              g_next_qid = ATOMIC_INIT(0);
static bool                  g_store_inited;

/* -------- state-machine helpers -------- */

static int qernel_state_rank(int s)
{
    switch (s) {
    case QSTATE_UNKNOWN:        return 0;
    case QSTATE_RECEIVED:       return 1;
    case QSTATE_PREPARED:       return 2;
    case QSTATE_ASSIGNED:       return 3;
    case QSTATE_BUNDLED:        return 4;
    case QSTATE_RUNNING:        return 5;
    case QSTATE_EM_COMBINING:   return 6;
    case QSTATE_RECONSTRUCTING: return 7;
    case QSTATE_DONE:           return 100;
    case QSTATE_FAILED:         return 100;
    case QSTATE_CANCELLED:      return 100;
    default:                    return -1;
    }
}

static bool qernel_state_is_terminal(int s)
{
    return s == QSTATE_DONE || s == QSTATE_FAILED || s == QSTATE_CANCELLED;
}

static int variant_state_rank(int s)
{
    switch (s) {
    case QVSTATE_UNKNOWN:        return 0;
    case QVSTATE_PREPARED:       return 1;
    case QVSTATE_ASSIGNED:       return 2;
    case QVSTATE_BUNDLED:        return 3;
    case QVSTATE_RUNNING:        return 4;
    case QVSTATE_DONE_VAR:       return 100;
    case QVSTATE_FAILED_VAR:     return 100;
    case QVSTATE_CANCELLED_VAR:  return 100;
    default:                     return -1;
    }
}

/* -------- store init / exit -------- */

int quantum_result_store_init(void)
{
    int i;
    size_t bytes;

    if (g_store_inited)
        return 0;

    bytes = sizeof(struct qernel_slot) * (QUANTUM_MAX_QERNELS + 1);
    g_qernel_table = kvcalloc(QUANTUM_MAX_QERNELS + 1,
                              sizeof(struct qernel_slot), GFP_KERNEL);
    if (!g_qernel_table) {
        pr_err("[result_store] failed to allocate qernel_table (%zu bytes)\n",
               bytes);
        return -ENOMEM;
    }
    for (i = 0; i <= QUANTUM_MAX_QERNELS; i++) {
        INIT_LIST_HEAD(&g_qernel_table[i].tasks);
        init_waitqueue_head(&g_qernel_table[i].result_waitq);
        g_qernel_table[i].used = false;
    }
    atomic_set(&g_next_qid, 0);
    g_store_inited = true;
    pr_info("[result_store] initialized abi=%d max_qernels=%d slot_bytes=%zu\n",
            QUANTUM_ABI_VERSION, QUANTUM_MAX_QERNELS,
            sizeof(struct qernel_slot));
    return 0;
}

void quantum_result_store_exit(void)
{
    int i;
    struct task_node *tn, *tmp;

    if (!g_store_inited)
        return;

    mutex_lock(&g_store_mtx);
    for (i = 0; i <= QUANTUM_MAX_QERNELS; i++) {
        list_for_each_entry_safe(tn, tmp, &g_qernel_table[i].tasks, link) {
            list_del(&tn->link);
            kfree(tn);
        }
        g_qernel_table[i].used = false;
    }
    mutex_unlock(&g_store_mtx);

    /* legacy result_entry list cleanup */
    qresult_store_clear();

    kvfree(g_qernel_table);
    g_qernel_table = NULL;
    g_store_inited = false;
    pr_info("[result_store] exited\n");
}

/* -------- qernel_table accessors -------- */

int qernel_table_alloc_qid(void)
{
    int i;
    int qid = -ENOSPC;

    mutex_lock(&g_store_mtx);
    /* round-robin starting from last + 1 */
    for (i = 0; i < QUANTUM_MAX_QERNELS; i++) {
        int candidate = (atomic_inc_return(&g_next_qid) - 1)
                        % QUANTUM_MAX_QERNELS + 1;
        if (!g_qernel_table[candidate].used) {
            struct qernel_slot *slot = &g_qernel_table[candidate];
            memset(&slot->row, 0, sizeof(slot->row));
            slot->row.qid                  = candidate;
            slot->row.state                = QSTATE_RECEIVED;
            slot->row.error_code           = QERR_OK;
            slot->row.cancel_requested     = 0;
            slot->row.num_variants_total   = 0;
            slot->row.num_variants_done    = 0;
            slot->row.num_fragments_done   = 0;
            slot->used                     = true;
            qid = candidate;
            break;
        }
    }
    mutex_unlock(&g_store_mtx);
    return qid;
}

void quantum_result_store_lock(void)   { mutex_lock(&g_store_mtx); }
void quantum_result_store_unlock(void) { mutex_unlock(&g_store_mtx); }

/* Caller MUST hold g_store_mtx. */
struct quantum_qernel_row *qernel_table_get_locked(int qid)
{
    if (qid <= 0 || qid > QUANTUM_MAX_QERNELS)
        return NULL;
    if (!g_qernel_table[qid].used)
        return NULL;
    return &g_qernel_table[qid].row;
}

int qernel_table_snapshot(int qid, struct quantum_qernel_row *out)
{
    struct quantum_qernel_row *row;
    int ret = 0;

    if (!out)
        return -EINVAL;
    mutex_lock(&g_store_mtx);
    row = qernel_table_get_locked(qid);
    if (!row) {
        ret = -ENOENT;
        goto out;
    }
    memcpy(out, row, sizeof(*out));
out:
    mutex_unlock(&g_store_mtx);
    return ret;
}

int qernel_table_set_state(int qid, int new_state)
{
    struct quantum_qernel_row *row;
    int old_rank, new_rank;
    int ret = 0;
    bool wake = false;

    new_rank = qernel_state_rank(new_state);
    if (new_rank < 0)
        return -EINVAL;

    mutex_lock(&g_store_mtx);
    row = qernel_table_get_locked(qid);
    if (!row) {
        ret = -ENOENT;
        goto out;
    }
    old_rank = qernel_state_rank(row->state);
    /* monotone: terminal (rank 100) is always reachable; otherwise must advance */
    if (new_rank != 100 && new_rank <= old_rank) {
        pr_warn("[result_store] qid=%d illegal state %d -> %d\n",
                qid, row->state, new_state);
        ret = -EINVAL;
        goto out;
    }
    /* invariant: variants_done <= variants_total */
    if (row->num_variants_done > row->num_variants_total) {
        pr_warn("[result_store] qid=%d invariant: done=%u > total=%u\n",
                qid, row->num_variants_done, row->num_variants_total);
    }
    row->state = new_state;
    if (qernel_state_is_terminal(new_state)) {
        row->finish_ns = ktime_get_ns();
        wake = true;
    }
out:
    mutex_unlock(&g_store_mtx);
    if (wake)
        wake_up_interruptible_all(&g_qernel_table[qid].result_waitq);
    return ret;
}

int qernel_table_set_cancel(int qid)
{
    struct quantum_qernel_row *row;
    int ret = 0;

    mutex_lock(&g_store_mtx);
    row = qernel_table_get_locked(qid);
    if (!row) {
        ret = -ENOENT;
        goto out;
    }
    row->cancel_requested = 1;
out:
    mutex_unlock(&g_store_mtx);
    return ret;
}

int qernel_table_wait_terminal(int qid, long timeout_jiffies)
{
    struct quantum_qernel_row *row;
    int state;
    long left;

    if (qid <= 0 || qid > QUANTUM_MAX_QERNELS)
        return -EINVAL;

    mutex_lock(&g_store_mtx);
    row = qernel_table_get_locked(qid);
    if (!row) {
        mutex_unlock(&g_store_mtx);
        return -ENOENT;
    }
    state = row->state;
    mutex_unlock(&g_store_mtx);

    if (qernel_state_is_terminal(state))
        return state;

    if (timeout_jiffies <= 0) {
        left = wait_event_interruptible(
            g_qernel_table[qid].result_waitq,
            ({
                int s;
                mutex_lock(&g_store_mtx);
                s = g_qernel_table[qid].used
                    ? g_qernel_table[qid].row.state : QSTATE_UNKNOWN;
                mutex_unlock(&g_store_mtx);
                qernel_state_is_terminal(s) || !g_qernel_table[qid].used;
            }));
        if (left < 0)
            return -ERESTARTSYS;
    } else {
        left = wait_event_interruptible_timeout(
            g_qernel_table[qid].result_waitq,
            ({
                int s;
                mutex_lock(&g_store_mtx);
                s = g_qernel_table[qid].used
                    ? g_qernel_table[qid].row.state : QSTATE_UNKNOWN;
                mutex_unlock(&g_store_mtx);
                qernel_state_is_terminal(s) || !g_qernel_table[qid].used;
            }),
            timeout_jiffies);
        if (left < 0)
            return -ERESTARTSYS;
        if (left == 0)
            return -ETIMEDOUT;
    }

    mutex_lock(&g_store_mtx);
    state = g_qernel_table[qid].used
            ? g_qernel_table[qid].row.state : QSTATE_UNKNOWN;
    mutex_unlock(&g_store_mtx);
    return state;
}

void qernel_table_release(int qid)
{
    struct task_node *tn, *tmp;

    if (qid <= 0 || qid > QUANTUM_MAX_QERNELS)
        return;
    mutex_lock(&g_store_mtx);
    if (g_qernel_table[qid].used) {
        list_for_each_entry_safe(tn, tmp,
                                 &g_qernel_table[qid].tasks, link) {
            list_del(&tn->link);
            kfree(tn);
        }
        g_qernel_table[qid].used = false;
    }
    mutex_unlock(&g_store_mtx);
}

int qernel_table_for_each(int (*cb)(struct quantum_qernel_row *, void *),
                          void *arg)
{
    int i, ret = 0;

    if (!cb)
        return -EINVAL;
    mutex_lock(&g_store_mtx);
    for (i = 1; i <= QUANTUM_MAX_QERNELS; i++) {
        if (!g_qernel_table[i].used)
            continue;
        ret = cb(&g_qernel_table[i].row, arg);
        if (ret)
            break;
    }
    mutex_unlock(&g_store_mtx);
    return ret;
}

/* -------- task_table accessors -------- */

static struct task_node *
task_lookup_locked(int qid, __u8 frag, __u8 var)
{
    struct task_node *tn;

    if (qid <= 0 || qid > QUANTUM_MAX_QERNELS)
        return NULL;
    if (!g_qernel_table[qid].used)
        return NULL;
    list_for_each_entry(tn, &g_qernel_table[qid].tasks, link) {
        if (tn->row.prov.fragment_index == frag &&
            tn->row.prov.variant_index  == var)
            return tn;
    }
    return NULL;
}

int task_table_insert(int qid, const struct quantum_task_row *row)
{
    struct task_node *tn;
    int ret = 0;

    if (!row)
        return -EINVAL;
    if (qid <= 0 || qid > QUANTUM_MAX_QERNELS)
        return -EINVAL;

    tn = kzalloc(sizeof(*tn), GFP_KERNEL);
    if (!tn)
        return -ENOMEM;
    memcpy(&tn->row, row, sizeof(tn->row));
    tn->row.prov.qid = qid;
    if (tn->row.assigned_backend_id == 0)
        tn->row.assigned_backend_id = -1;
    if (tn->row.bundle_id == 0)
        tn->row.bundle_id = -1;

    mutex_lock(&g_store_mtx);
    if (!g_qernel_table[qid].used) {
        ret = -ENOENT;
        goto out_free;
    }
    if (task_lookup_locked(qid, row->prov.fragment_index,
                           row->prov.variant_index)) {
        pr_warn("[result_store] qid=%d duplicate task frag=%u var=%u\n",
                qid, row->prov.fragment_index, row->prov.variant_index);
        ret = -EEXIST;
        goto out_free;
    }
    list_add_tail(&tn->link, &g_qernel_table[qid].tasks);
    mutex_unlock(&g_store_mtx);
    return 0;

out_free:
    mutex_unlock(&g_store_mtx);
    kfree(tn);
    return ret;
}

int task_table_get(const struct quantum_provenance *prov,
                   struct quantum_task_row *out)
{
    struct task_node *tn;
    int ret = 0;

    if (!prov || !out)
        return -EINVAL;
    mutex_lock(&g_store_mtx);
    tn = task_lookup_locked(prov->qid, prov->fragment_index,
                            prov->variant_index);
    if (!tn) {
        ret = -ENOENT;
        goto out;
    }
    memcpy(out, &tn->row, sizeof(*out));
out:
    mutex_unlock(&g_store_mtx);
    return ret;
}

int task_table_set_state(const struct quantum_provenance *prov, int new_state)
{
    struct task_node *tn;
    int old_rank, new_rank;
    int ret = 0;

    if (!prov)
        return -EINVAL;
    new_rank = variant_state_rank(new_state);
    if (new_rank < 0)
        return -EINVAL;

    mutex_lock(&g_store_mtx);
    tn = task_lookup_locked(prov->qid, prov->fragment_index,
                            prov->variant_index);
    if (!tn) {
        ret = -ENOENT;
        goto out;
    }
    old_rank = variant_state_rank(tn->row.state);
    if (new_rank != 100 && new_rank <= old_rank) {
        pr_warn("[result_store] qid=%d frag=%u var=%u illegal var-state %d -> %d\n",
                prov->qid, prov->fragment_index, prov->variant_index,
                tn->row.state, new_state);
        ret = -EINVAL;
        goto out;
    }
    tn->row.state = new_state;
out:
    mutex_unlock(&g_store_mtx);
    return ret;
}

int task_table_set_backend(const struct quantum_provenance *prov,
                           int backend_id)
{
    struct task_node *tn;
    int ret = 0;

    if (!prov || backend_id < 0)
        return -EINVAL;
    mutex_lock(&g_store_mtx);
    tn = task_lookup_locked(prov->qid, prov->fragment_index,
                            prov->variant_index);
    if (!tn) {
        ret = -ENOENT;
        goto out;
    }
    tn->row.assigned_backend_id = backend_id;
    tn->row.assigned_ns = ktime_get_ns();
out:
    mutex_unlock(&g_store_mtx);
    return ret;
}

int task_table_set_bundle(const struct quantum_provenance *prov,
                          int bundle_id, const int *phys_qubits, int nq)
{
    struct task_node *tn;
    int ret = 0;

    if (!prov || bundle_id < 0)
        return -EINVAL;
    if (nq < 0 || nq > QUANTUM_MAX_QUBITS)
        return -EINVAL;

    mutex_lock(&g_store_mtx);
    tn = task_lookup_locked(prov->qid, prov->fragment_index,
                            prov->variant_index);
    if (!tn) {
        ret = -ENOENT;
        goto out;
    }
    tn->row.bundle_id = bundle_id;
    if (phys_qubits && nq > 0)
        memcpy(tn->row.phys_qubits, phys_qubits,
               nq * sizeof(int));
    tn->row.bundled_ns = ktime_get_ns();
out:
    mutex_unlock(&g_store_mtx);
    return ret;
}

int task_table_set_result(const struct quantum_provenance *prov,
                          const struct quantum_result *result)
{
    struct task_node *tn;
    int ret = 0;

    if (!prov || !result)
        return -EINVAL;
    mutex_lock(&g_store_mtx);
    tn = task_lookup_locked(prov->qid, prov->fragment_index,
                            prov->variant_index);
    if (!tn) {
        ret = -ENOENT;
        goto out;
    }
    memcpy(&tn->row.result, result, sizeof(tn->row.result));
    tn->row.committed_ns = ktime_get_ns();
out:
    mutex_unlock(&g_store_mtx);
    return ret;
}

void task_table_drop_qernel(int qid)
{
    struct task_node *tn, *tmp;

    if (qid <= 0 || qid > QUANTUM_MAX_QERNELS)
        return;
    mutex_lock(&g_store_mtx);
    if (g_qernel_table[qid].used) {
        list_for_each_entry_safe(tn, tmp,
                                 &g_qernel_table[qid].tasks, link) {
            list_del(&tn->link);
            kfree(tn);
        }
    }
    mutex_unlock(&g_store_mtx);
}

int task_table_for_each_in_qernel(int qid,
                                  int (*cb)(struct quantum_task_row *,
                                            void *),
                                  void *arg)
{
    struct task_node *tn;
    int ret = 0;

    if (!cb)
        return -EINVAL;
    if (qid <= 0 || qid > QUANTUM_MAX_QERNELS)
        return -EINVAL;
    mutex_lock(&g_store_mtx);
    if (!g_qernel_table[qid].used) {
        mutex_unlock(&g_store_mtx);
        return -ENOENT;
    }
    list_for_each_entry(tn, &g_qernel_table[qid].tasks, link) {
        ret = cb(&tn->row, arg);
        if (ret)
            break;
    }
    mutex_unlock(&g_store_mtx);
    return ret;
}
