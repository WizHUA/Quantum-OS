#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/list.h>
#include "quantum_types.h"

/*
 * 结果存储节点
 * 任务完成后将结果从 quantum_task_struct 中摘出存入此处
 * 等待用户态 ioctl(QIOC_RESULT) 取走
 */
struct qresult_node {
    int                   qid;
    int                   state;      /* QTASK_STATE_SUCCESS / FAILED */
    struct quantum_result result;
    struct list_head      list;
};

static LIST_HEAD(result_store);
static DEFINE_SPINLOCK(result_lock);

/*
 * 任务完成后由调度模块调用
 * 将结果存入结果表
 */
int qresult_store_put(struct quantum_task_struct *task)
{
    struct qresult_node *node;

    node = kzalloc(sizeof(*node), GFP_KERNEL);
    if (!node)
        return -ENOMEM;

    node->qid   = task->qid;
    node->state = task->state;
    memcpy(&node->result, &task->result, sizeof(task->result));

    spin_lock(&result_lock);
    list_add_tail(&node->list, &result_store);
    spin_unlock(&result_lock);

    printk(KERN_INFO "QuantumOS: [result_store] stored qid=%d state=%d\n",
           node->qid, node->state);
    return 0;
}

/*
 * 用户态 ioctl(QIOC_STATUS) 调用
 * 返回任务状态，找不到返回 QTASK_STATE_UNKNOWN
 */
int qresult_store_status(int qid)
{
    struct qresult_entry *entry;
    unsigned long flags;
    int state = QTASK_STATE_UNKNOWN;

    spin_lock_irqsave(&store_lock, flags);
    list_for_each_entry(entry, &result_list, list) {
        if (entry->task.qid == qid) {
            state = entry->task.state;
            break;
        }
    }
    spin_unlock_irqrestore(&store_lock, flags);
    return state;
}

/*
 * 用户态 ioctl(QIOC_RESULT) 调用
 * 取走结果（取后从结果表删除）
 * 返回 0 成功，-ENOENT 未找到
 */
int qresult_store_get(int qid, struct quantum_result *out)
{
    struct qresult_node *node, *tmp;
    int found = 0;

    spin_lock(&result_lock);
    list_for_each_entry_safe(node, tmp, &result_store, list) {
        if (node->qid == qid) {
            memcpy(out, &node->result, sizeof(*out));
            list_del(&node->list);
            kfree(node);
            found = 1;
            break;
        }
    }
    spin_unlock(&result_lock);

    if (!found) {
        printk(KERN_WARNING "QuantumOS: [result_store] qid=%d not found\n",
               qid);
        return -ENOENT;
    }

    printk(KERN_INFO "QuantumOS: [result_store] delivered qid=%d\n", qid);
    return 0;
}

/*
 * 模块退出时清空结果表
 */
void qresult_store_clear(void)
{
    struct qresult_node *node, *tmp;

    spin_lock(&result_lock);
    list_for_each_entry_safe(node, tmp, &result_store, list) {
        list_del(&node->list);
        kfree(node);
    }
    spin_unlock(&result_lock);
}