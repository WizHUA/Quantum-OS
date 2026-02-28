#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/list.h>
#include <linux/spinlock.h>
#include <linux/string.h>

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