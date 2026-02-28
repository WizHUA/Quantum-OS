#include <linux/fs.h>
#include <linux/uaccess.h>
#include <linux/slab.h>
#include <linux/atomic.h>
#include <linux/ktime.h>
#include "quantum_types.h"

/* 全局 qid 计数器 */
static atomic_t qid_counter = ATOMIC_INIT(0);

/* 前向声明 */
int quantum_sched_enqueue(struct quantum_task_struct *task);
int quantum_preproc_run(struct quantum_task_struct *task);
int quantum_alloc_run(struct quantum_task_struct *task);
int quantum_batch_run(struct quantum_task_struct *task);
int qresult_store_status(int qid);
int qresult_store_get(int qid, struct quantum_result *out);
void quantum_alloc_get_pool(struct quantum_backend_pool *out);
int quantum_sched_fetch(struct quantum_fetch_req *out);
int quantum_sched_commit(struct quantum_commit_req *in);
int quantum_sched_query_state(int qid);
int quantum_sched_cancel(int qid);


/* ===== 工具函数 ===== */

static const char *parse_submit_header(const char *buf,
                                        int *shots,
                                        int *priority,
                                        int *mitigation)
{
    const char *next;

    /* 默认值 */
    *shots      = 1000;
    *priority   = 0;
    *mitigation = 0;

    /* 没有配置头，直接是QASM */
    if (strncmp(buf, "OPENQASM", 8) == 0)
        return buf;

    /* 有配置头，解析参数 */
    if (strncmp(buf, "shots=", 6) == 0) {
        sscanf(buf, "shots=%d priority=%d mitigation=%d",
               shots, priority, mitigation);
        /* 跳过配置头这一行 */
        next = strchr(buf, '\n');
        if (next)
            return next + 1;
    }

    return buf;
}

static int is_valid_qasm(const char *buf, size_t len)
{
    if (len < 8)
        return 0;
    return (strncmp(buf, "OPENQASM", 8) == 0) ? 1 : 0;
}

/* ===== 字符设备操作 ===== */
static int quantum_open(struct inode *inode, struct file *filp)
{
    printk(KERN_INFO "QuantumOS: device opened by pid=%d\n",
           current->pid);
    return 0;
}

static int quantum_release(struct inode *inode, struct file *filp)
{
    printk(KERN_INFO "QuantumOS: device closed by pid=%d\n",
           current->pid);
    return 0;
}

static ssize_t quantum_write(struct file *filp, const char __user *buf,
                              size_t count, loff_t *pos)
{
    struct quantum_task_struct *task;
    const char *qasm_start;
    int shots, priority, mitigation;
    int ret;

    if (count == 0 || count >= QUANTUM_QIR_SIZE) {
        printk(KERN_WARNING "QuantumOS: invalid write size=%zu\n", count);
        return -EINVAL;
    }

    task = kzalloc(sizeof(*task), GFP_KERNEL);
    if (!task)
        return -ENOMEM;

    if (copy_from_user(task->qir, buf, count)) {
        kfree(task);
        return -EFAULT;
    }
    task->qir[count] = '\0';

    /* 解析可选配置头，获取QASM起始位置 */
    qasm_start = parse_submit_header(task->qir,
                                     &shots, &priority, &mitigation);

    /* 验证QASM语法 */
    if (!is_valid_qasm(qasm_start, strlen(qasm_start))) {
        printk(KERN_WARNING "QuantumOS: invalid QASM syntax\n");
        kfree(task);
        return -EINVAL;
    }

    /* 初始化任务元数据 */
    task->qid              = atomic_inc_return(&qid_counter);
    task->task_type        = QTASK_TYPE_NORMAL;
    task->priority         = priority;
    task->shots            = shots;
    task->error_mitigation = mitigation;
    task->state            = QTASK_STATE_RECEIVED;
    task->submit_time      = ktime_get_ns();
    task->error_code       = QERR_OK;

    printk(KERN_INFO "QuantumOS: task received qid=%d pid=%d shots=%d\n",
           task->qid, current->pid, task->shots);

    /* 依次送入各模块处理 */
    ret = quantum_preproc_run(task);
    if (ret) {
        /* preproc 返回错误：QASM 本身有问题（非超限）
         * 超限时 preproc 设置 need_split=1 并返回0，不走这里 */
        task->state      = QTASK_STATE_FAILED;
        task->error_code = QERR_COMPILE_FAIL;
        printk(KERN_ERR "QuantumOS: preproc failed qid=%d ret=%d\n",
               task->qid, ret);
        kfree(task);
        return ret;
    }

    /* 切分失败：need_split=1 但 num_sub_circuits=0 */
    if (task->need_split && task->num_sub_circuits == 0) {
        printk(KERN_ERR "QuantumOS: split failed qid=%d\n", task->qid);
        kfree(task);
        return -EINVAL;
    }

    ret = quantum_alloc_run(task);
    if (ret) {
        task->state      = QTASK_STATE_FAILED;
        task->error_code = QERR_NO_RESOURCE;
        printk(KERN_ERR "QuantumOS: alloc failed qid=%d\n", task->qid);
        kfree(task);
        return ret;
    }

    ret = quantum_batch_run(task);
    if (ret) {
        task->state      = QTASK_STATE_FAILED;
        task->error_code = QERR_COMPILE_FAIL;
        printk(KERN_ERR "QuantumOS: batch failed qid=%d\n", task->qid);
        kfree(task);
        return ret;
    }

    /* 入调度队列 */
    ret = quantum_sched_enqueue(task);
    if (ret) {
        task->state      = QTASK_STATE_FAILED;
        task->error_code = QERR_QUEUE_FULL;
        printk(KERN_ERR "QuantumOS: enqueue failed qid=%d\n", task->qid);
        kfree(task);
        return ret;
    }

    /* 把 qid 存入 file 私有数据，供 read() 取回 */
    filp->private_data = (void *)(long)task->qid; 

    printk(KERN_INFO "QuantumOS: task queued qid=%d\n", task->qid);
    return (ssize_t)count;
}

static ssize_t quantum_read(struct file *filp, char __user *buf,
                             size_t count, loff_t *pos)
{
    int qid = (int)(long)filp->private_data;

    if (count < sizeof(int))
        return -EINVAL;
    if (qid <= 0)
        return -ENODATA;
    if (copy_to_user(buf, &qid, sizeof(int)))
        return -EFAULT;

    return sizeof(int);
}

static long quantum_ioctl(struct file *filp, unsigned int cmd,
                           unsigned long arg)
{
    int qid;
    int state;
    int ret = 0;
    struct quantum_result       *result = NULL;
    struct quantum_backend_pool *pool   = NULL;

    switch (cmd) {

    case QIOC_STATUS:
        if (get_user(qid, (int __user *)arg))
            return -EFAULT;

        /* 先查调度队列（任务还在执行中）*/
        state = quantum_sched_query_state(qid);

        /* 队列里没有，再查结果表（任务已完成）*/
        if (state == QTASK_STATE_UNKNOWN)
            state = qresult_store_status(qid);

        /*
         * MERGING 是内核内部状态，对用户态表现为 RUNNING
         * 避免 libquantum 因看到未知状态而提前 timeout
         */
        if (state == QTASK_STATE_MERGING)
            state = QTASK_STATE_RUNNING;

        printk(KERN_INFO "QuantumOS: ioctl STATUS qid=%d state=%d\n",
               qid, state);
        return state;

    case QIOC_RESULT:
        result = kzalloc(sizeof(*result), GFP_KERNEL);
        if (!result)
            return -ENOMEM;
        if (copy_from_user(result, (void __user *)arg, sizeof(*result))) {
            ret = -EFAULT;
            goto out_result;
        }
        qid = result->qid;
        if (qresult_store_get(qid, result) < 0) {
            ret = -ENOENT;
            goto out_result;
        }
        if (copy_to_user((void __user *)arg, result, sizeof(*result))) {
            ret = -EFAULT;
            goto out_result;
        }
        printk(KERN_INFO "QuantumOS: ioctl RESULT qid=%d delivered\n", qid);
out_result:
        kfree(result);
        return ret;

    case QIOC_CANCEL:
        if (get_user(qid, (int __user *)arg))
            return -EFAULT;
        ret = quantum_sched_cancel(qid);
        printk(KERN_INFO "QuantumOS: ioctl CANCEL qid=%d ret=%d\n", qid, ret);
        return ret;

    case QIOC_RESOURCE:
        pool = kzalloc(sizeof(*pool), GFP_KERNEL);
        if (!pool)
            return -ENOMEM;
        quantum_alloc_get_pool(pool);
        if (copy_to_user((void __user *)arg, pool, sizeof(*pool)))
            ret = -EFAULT;
        else
            printk(KERN_INFO "QuantumOS: ioctl RESOURCE delivered\n");
        kfree(pool);
        return ret;

    case QIOC_FETCH: {
        struct quantum_fetch_req *freq;

        freq = kzalloc(sizeof(*freq), GFP_KERNEL);
        if (!freq)
            return -ENOMEM;

        ret = quantum_sched_fetch(freq);
        if (ret < 0) {
            kfree(freq);
            return ret;
        }

        if (copy_to_user((void __user *)arg, freq, sizeof(*freq))) {
            kfree(freq);
            return -EFAULT;
        }
        kfree(freq);
        return 0;
    }

    case QIOC_COMMIT: {
        struct quantum_commit_req *creq;

        creq = kzalloc(sizeof(*creq), GFP_KERNEL);
        if (!creq)
            return -ENOMEM;

        if (copy_from_user(creq, (void __user *)arg, sizeof(*creq))) {
            kfree(creq);
            return -EFAULT;
        }

        ret = quantum_sched_commit(creq);
        kfree(creq);
        return ret;
    }

    default:
        return -ENOTTY;
    }
}

/* ===== fops 导出给 quantum_main.c ===== */
const struct file_operations quantum_fops = {
    .owner          = THIS_MODULE,
    .open           = quantum_open,
    .release        = quantum_release,
    .write          = quantum_write,
    .read           = quantum_read,
    .unlocked_ioctl = quantum_ioctl,
};

int quantum_interface_init(void)
{
    printk(KERN_INFO "QuantumOS: interface module init\n");
    return 0;
}

void quantum_interface_exit(void)
{
    printk(KERN_INFO "QuantumOS: interface module exit\n");
}