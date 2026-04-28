#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/fs.h>
#include <linux/miscdevice.h>
#include <linux/uaccess.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/ktime.h>
#include <linux/atomic.h>

#include "quantum_types.h"

/* ============================================================
 * 前向声明（所有跨模块调用，不使用.h）
 * ============================================================ */

/* quantum_preproc.c */
int quantum_preproc_run(struct quantum_task_struct *task);

/* quantum_alloc.c */
int  quantum_alloc_run(struct quantum_task_struct *task);
void quantum_alloc_get_pool(struct quantum_backend_pool *out);

/* quantum_batch.c */
int quantum_batch_run(struct quantum_task_struct *task);

/* quantum_sched.c */
int quantum_sched_enqueue(struct quantum_task_struct *task);
int quantum_sched_fetch(struct quantum_fetch_req *out);
int quantum_sched_commit(struct quantum_commit_req *in);
int quantum_sched_query_state(int qid);
int quantum_sched_cancel(int qid);
int quantum_sched_alloc_qid(void);

/* quantum_result_store.c */
int qresult_store_status(int qid);
int qresult_store_get(int qid, struct quantum_result *out);
void qresult_store_clear(void);
int qresult_store_put(struct quantum_task_struct *task);

/* ABI v3 (§03.4) */
int  qernel_table_alloc_qid(void);
int  qernel_table_snapshot(int qid, struct quantum_qernel_row *out);
int  qernel_table_set_state(int qid, int new_state);
int  qernel_table_set_cancel(int qid);
int  qernel_table_wait_terminal(int qid, long timeout_jiffies);
void qernel_table_release(int qid);
void quantum_result_store_lock(void);
void quantum_result_store_unlock(void);
struct quantum_qernel_row *qernel_table_get_locked(int qid);

/* Map ABI v3 QSTATE_* to legacy QTASK_STATE_* for userspace continuity. */
static int qstate_to_legacy(int s)
{
    switch (s) {
    case QSTATE_RECEIVED:       return QTASK_STATE_RECEIVED;
    case QSTATE_PREPARED:
    case QSTATE_ASSIGNED:
    case QSTATE_BUNDLED:        return QTASK_STATE_QUEUED;
    case QSTATE_RUNNING:        return QTASK_STATE_RUNNING;
    case QSTATE_EM_COMBINING:
    case QSTATE_RECONSTRUCTING: return QTASK_STATE_RUNNING;
    case QSTATE_DONE:           return QTASK_STATE_SUCCESS;
    case QSTATE_FAILED:         return QTASK_STATE_FAILED;
    case QSTATE_CANCELLED:      return QTASK_STATE_CANCELLED;
    default:                    return QTASK_STATE_UNKNOWN;
    }
}


/* ============================================================
 * 内部：配置头解析
 *
 * 格式："shots=N priority=P mitigation=M alloc_strategy=A split_strategy=S\n"
 * 字段以空格分隔，缺省字段使用默认值，顺序不固定
 * ============================================================ */

static void parse_submit_header(const char *line,
                                 int *shots,
                                 int *priority,
                                 int *mitigation,
                                 int *alloc_strategy,
                                 int *split_strategy)
{
    const char *p = line;
    char key[32];
    int  val, ki;

    /* 默认值 */
    *shots          = 1000;
    *priority       = 0;
    *mitigation     = QMIT_NONE;
    *alloc_strategy = QALLOC_STRATEGY_FIRST_FIT;
    *split_strategy = QSPLIT_STRATEGY_NONE;

    while (*p && *p != '\n') {
        /* 跳过空格 */
        while (*p == ' ' || *p == '\t') p++;
        if (!*p || *p == '\n') break;

        /* 读 key */
        ki = 0;
        while (*p && *p != '=' && *p != ' ' && *p != '\n' && ki < 31)
            key[ki++] = *p++;
        key[ki] = '\0';

        if (*p != '=')
            continue;
        p++; /* 跳过 '=' */

        /* 读 val（正整数） */
        val = 0;
        while (*p >= '0' && *p <= '9') {
            val = val * 10 + (*p - '0');
            p++;
        }

        if      (strcmp(key, "shots")          == 0) *shots          = val;
        else if (strcmp(key, "priority")       == 0) *priority       = val;
        else if (strcmp(key, "mitigation")     == 0) *mitigation     = val;
        else if (strcmp(key, "alloc_strategy") == 0) *alloc_strategy = val;
        else if (strcmp(key, "split_strategy") == 0) *split_strategy = val;
        /* 未知字段静默忽略，便于后续协议扩展 */
    }
}

/* ============================================================
 * 内部：QASM基础合法性检查
 *
 * 宽松验证：只检查 OPENQASM 声明是否存在
 * 详细的语法检查由 preproc 负责
 * ============================================================ */

static int is_valid_qasm(const char *qasm)
{
    return (strstr(qasm, "OPENQASM") != NULL) ? 1 : 0;
}

/* ============================================================
 * 内部：参数范围修正
 * ============================================================ */

static void sanitize_config(int *shots, int *priority,
                              int *mitigation, int *alloc_strategy)
{
    if (*shots <= 0 || *shots > 100000)
        *shots = 1000;
    if (*priority < 0 || *priority > 9)
        *priority = 0;
    if (*mitigation < QMIT_NONE || *mitigation > QMIT_PEC)
        *mitigation = QMIT_NONE;
    if (*alloc_strategy < QALLOC_STRATEGY_FIRST_FIT ||
        *alloc_strategy > QALLOC_STRATEGY_TOPO)
        *alloc_strategy = QALLOC_STRATEGY_FIRST_FIT;
}

/* ============================================================
 * 内部：提交链失败处理
 *
 * 提交链中任何一步失败，task已分配且error已填写，
 * 此时将失败结果存入result_store（供用户查询），然后释放task
 * ============================================================ */

static void submit_chain_fail(struct quantum_task_struct *task)
{
    task->state = QTASK_STATE_FAILED;
    /*
     * result_store_put 在 sched_commit 中调用（正常路径）
     * 提交链失败时直接写入 result_store，让用户可以查到失败原因
     * 复用 qresult_store_get 对应的 put，需要直接构造
     */
    qresult_store_put(task);
    kfree(task);
}

/* ============================================================
 * Submit chain (shared between legacy write() and QIOC_SUBMIT).
 *
 * Builds a quantum_task_struct, runs preproc/alloc/batch, then enqueues to
 * sched. Caller-supplied qid lets QIOC_SUBMIT pre-allocate a qernel_table
 * row and have the legacy chain operate under the same identity.
 *
 * Returns >0 (qid) on success; <0 errno on failure.
 * ============================================================ */
static int submit_chain_run(int qid_in,
                            const char *qir,
                            int shots, int priority,
                            int mitigation, int alloc_strategy,
                            int split_strategy)
{
    struct quantum_task_struct *task;
    int qid = qid_in;
    int ret;

    task = kzalloc(sizeof(*task), GFP_KERNEL);
    if (!task)
        return -ENOMEM;

    if (qid <= 0)
        qid = quantum_sched_alloc_qid();

    task->qid              = qid;
    task->task_type        = QTASK_TYPE_NORMAL;
    task->priority         = priority;
    task->shots            = shots;
    task->error_mitigation = mitigation;
    task->alloc_strategy   = alloc_strategy;
    task->split_strategy   = split_strategy;
    task->submit_time      = ktime_get_ns();
    task->state            = QTASK_STATE_RECEIVED;
    task->assigned_backend_id = -1;
    task->batch_group_id   = -1;
    task->error_code       = QERR_OK;
    INIT_LIST_HEAD(&task->list);

    strncpy(task->qir, qir, QUANTUM_QIR_SIZE - 1);
    task->qir[QUANTUM_QIR_SIZE - 1] = '\0';

    pr_info("[interface] qid=%d submit shots=%d priority=%d em=%d alloc=%d cut_hint=%d\n",
            qid, shots, priority, mitigation, alloc_strategy, split_strategy);

    ret = quantum_preproc_run(task);
    if (ret) {
        pr_err("[interface] qid=%d preproc failed err=%d\n", qid, ret);
        submit_chain_fail(task);
        return -EINVAL;
    }
    ret = quantum_alloc_run(task);
    if (ret) {
        pr_err("[interface] qid=%d alloc failed err=%d\n", qid, ret);
        submit_chain_fail(task);
        return -EBUSY;
    }
    ret = quantum_batch_run(task);
    if (ret) {
        pr_err("[interface] qid=%d batch failed err=%d\n", qid, ret);
        submit_chain_fail(task);
        return -EINVAL;
    }
    ret = quantum_sched_enqueue(task);
    if (ret) {
        pr_err("[interface] qid=%d enqueue failed err=%d\n", qid, ret);
        submit_chain_fail(task);
        return -EBUSY;
    }
    return qid;
}

/* ============================================================
 * file_operations：open / release
 * ============================================================ */

static int quantum_open(struct inode *inode, struct file *filp)
{
    /*
     * private_data 用于 write() 和 read() 之间传递 qid
     * 初始化为 -1 表示尚未提交任何任务
     */
    filp->private_data = (void *)(long)(-1);
    return 0;
}

static int quantum_release(struct inode *inode, struct file *filp)
{
    return 0;
}

/* ============================================================
 * write()：任务提交
 *
 * 完整流程（严格按设计文档 §模块1）：
 *   parse_submit_header()      解析配置头
 *   is_valid_qasm()            QASM基础验证
 *   kzalloc(task)              分配 task（~38KB，堆分配，不在栈上）
 *   初始化 interface 负责的字段  qid/task_type/priority/shots/...
 *   → quantum_preproc_run()    线路分析、切分、误差缓解预处理
 *   → quantum_alloc_run()      qubit分配、保真度预估
 *   → quantum_batch_run()      子线路验证、批处理标记
 *   → quantum_sched_enqueue()  入调度队列
 *   filp->private_data = qid   供 read() 返回给用户
 * ============================================================ */

static ssize_t quantum_write(struct file *filp,
                              const char __user *ubuf,
                              size_t count,
                              loff_t *ppos)
{
    char *kbuf;
    char *qasm_start;
    struct quantum_task_struct *task;
    int shots, priority, mitigation, alloc_strategy, split_strategy;
    int qid, ret;

    /* 基本长度检查 */
    if (count == 0 || count >= QUANTUM_QIR_SIZE) {
        pr_warn("quantum_interface: write size %zu invalid "
                "(must be 1~%d)\n", count, QUANTUM_QIR_SIZE - 1);
        return -EINVAL;
    }

    /* 1. 拷贝用户数据到内核缓冲区 */
    kbuf = kzalloc(QUANTUM_QIR_SIZE, GFP_KERNEL);
    if (!kbuf)
        return -ENOMEM;

    if (copy_from_user(kbuf, ubuf, count)) {
        kfree(kbuf);
        return -EFAULT;
    }
    kbuf[count] = '\0';

    /* 2. 解析配置头（第一行） */
    parse_submit_header(kbuf, &shots, &priority, &mitigation,
                        &alloc_strategy, &split_strategy);
    sanitize_config(&shots, &priority, &mitigation, &alloc_strategy);

    /* 3. 定位 QASM 正文（第一个 '\n' 之后）
     *    若没有换行符，整体视为 QASM（用户未写配置头，使用默认配置）
     */
    qasm_start = strchr(kbuf, '\n');
    if (qasm_start)
        qasm_start++; /* 跳过 '\n' */
    else
        qasm_start = kbuf;

    /* 4. QASM 基础验证 */
    if (!is_valid_qasm(qasm_start)) {
        pr_warn("quantum_interface: invalid QASM "
                "(missing OPENQASM declaration)\n");
        kfree(kbuf);
        return -EINVAL;
    }

    /* 5. 分配 task（约 38KB，必须堆分配）*/
    task = kzalloc(sizeof(*task), GFP_KERNEL);
    if (!task) {
        kfree(kbuf);
        return -ENOMEM;
    }

    /* 6. 初始化 interface 负责的字段
     *    严格遵守设计文档字段职责分工
     */
    qid = quantum_sched_alloc_qid();

    task->qid             = qid;
    task->task_type       = QTASK_TYPE_NORMAL;
    task->priority        = priority;
    task->shots           = shots;
    task->error_mitigation = mitigation;
    task->alloc_strategy  = alloc_strategy;
    task->submit_time     = ktime_get_ns();
    task->state           = QTASK_STATE_RECEIVED;

    /*
     * split_strategy 用户提示值存入 task：
     *   0=NONE → preproc 自行决定是否需要切分及使用哪种策略
     *   >0     → preproc 将使用用户指定的策略（如有必要）
     */
    task->split_strategy      = split_strategy;
    task->assigned_backend_id = -1;
    task->batch_group_id      = -1;
    task->error_code          = QERR_OK;

    INIT_LIST_HEAD(&task->list);

    /*
     * 将完整提交内容（含配置头）存入 task->qir
     * preproc 解析 QASM 时会跳过配置头行（找第一个 '\n' 后的内容）
     * sched FETCH 时同样剥离配置头再发送给 daemon
     */
    strncpy(task->qir, kbuf, QUANTUM_QIR_SIZE - 1);
    task->qir[QUANTUM_QIR_SIZE - 1] = '\0';

    kfree(kbuf);

    pr_info("quantum_interface: received qid=%d shots=%d priority=%d "
            "mitigation=%d alloc=%d split_hint=%d\n",
            qid, shots, priority, mitigation,
            alloc_strategy, split_strategy);

    /* 7. 提交链：同步，write() 上下文
     *
     * 任何一步失败：填写 error_code → submit_chain_fail → 返回错误
     * 用户可通过 STATUS ioctl 查询到 FAILED 状态和 error_info
     */

    ret = quantum_preproc_run(task);
    if (ret != 0) {
        pr_err("quantum_interface: preproc failed qid=%d err=%d\n", qid, ret);
        submit_chain_fail(task);
        return -EINVAL;
    }

    ret = quantum_alloc_run(task);
    if (ret != 0) {
        pr_err("quantum_interface: alloc failed qid=%d err=%d\n", qid, ret);
        submit_chain_fail(task);
        return -EBUSY;
    }

    ret = quantum_batch_run(task);
    if (ret != 0) {
        pr_err("quantum_interface: batch failed qid=%d err=%d\n", qid, ret);
        submit_chain_fail(task);
        return -EINVAL;
    }

    ret = quantum_sched_enqueue(task);
    if (ret != 0) {
        pr_err("quantum_interface: enqueue failed qid=%d err=%d\n", qid, ret);
        submit_chain_fail(task);
        return -EBUSY;
    }

    /* 8. 保存 qid，供后续 read() 返回给用户 */
    filp->private_data = (void *)(long)qid;

    pr_info("quantum_interface: qid=%d enqueued successfully\n", qid);
    return (ssize_t)count;
}

/* ============================================================
 * read()：返回 qid
 *
 * 用户在 write() 成功后立即调用 read() 获取分配的 qid
 * 协议：read(fd, &qid, sizeof(int)) → qid > 0
 * ============================================================ */

static ssize_t quantum_read(struct file *filp,
                             char __user *ubuf,
                             size_t count,
                             loff_t *ppos)
{
    int qid = (int)(long)filp->private_data;

    if (qid <= 0) {
        pr_warn("quantum_interface: read() called before write() "
                "or write() failed\n");
        return -EINVAL;
    }

    if (count < sizeof(int))
        return -EINVAL;

    if (copy_to_user(ubuf, &qid, sizeof(int)))
        return -EFAULT;

    /* 重置，防止重复读到同一个 qid */
    filp->private_data = (void *)(long)(-1);

    return sizeof(int);
}

/* ============================================================
 * ioctl()：状态查询 / 结果取回 / 取消 / 资源查询 / daemon交互
 *
 * 设计原则：interface 不含业务逻辑，只做路由
 * ============================================================ */

static long quantum_ioctl(struct file *filp,
                           unsigned int cmd,
                           unsigned long arg)
{
    void __user *uarg = (void __user *)arg;
    int ret = 0;

    switch (cmd) {

    case QIOC_STATUS: {
        struct quantum_status_req req;
        struct quantum_qernel_row *snap;
        int state;

        if (copy_from_user(&req, uarg, sizeof(req)))
            return -EFAULT;

        /* Prefer ABI v3 qernel_table; fall back to legacy paths. */
        snap = kzalloc(sizeof(*snap), GFP_KERNEL);
        if (!snap)
            return -ENOMEM;
        if (qernel_table_snapshot(req.qid, snap) == 0) {
            state = qstate_to_legacy(snap->state);
            kfree(snap);
            pr_info("[interface] qid=%d status state=%d (qernel_table)\n",
                    req.qid, state);
        } else {
            kfree(snap);
            state = quantum_sched_query_state(req.qid);
            if (state == QTASK_STATE_UNKNOWN)
                state = qresult_store_status(req.qid);
            if (state == QTASK_STATE_MERGING)
                state = QTASK_STATE_RUNNING;
            if (state < 0)
                state = QTASK_STATE_UNKNOWN;
        }

        req.state = state;
        if (copy_to_user(uarg, &req, sizeof(req)))
            return -EFAULT;

        break;
    }

    /* ── QIOC_RESULT：取回执行结果（堆分配避免栈帧过大）── */
    case QIOC_RESULT: {
        struct quantum_result_req *req;

        req = kzalloc(sizeof(*req), GFP_KERNEL);
        if (!req)
            return -ENOMEM;

        if (copy_from_user(req, uarg, sizeof(*req))) {
            kfree(req);
            return -EFAULT;
        }

        ret = qresult_store_get(req->qid, &req->result);
        if (ret == -ENOENT) {
            kfree(req);
            return -EAGAIN;
        }
        if (ret != 0) {
            kfree(req);
            return ret;
        }

        if (copy_to_user(uarg, req, sizeof(*req))) {
            kfree(req);
            return -EFAULT;
        }

        kfree(req);
        break;
    }

    case QIOC_CANCEL: {
        struct quantum_cancel_req req;

        if (copy_from_user(&req, uarg, sizeof(req)))
            return -EFAULT;

        /* Mark cancellation in qernel_table (best-effort) before signaling sched. */
        (void)qernel_table_set_cancel(req.qid);
        ret = quantum_sched_cancel(req.qid);
        pr_info("[interface] qid=%d cancel requested ret=%d\n", req.qid, ret);
        break;
    }

    /* ── QIOC_RESOURCE：查询后端资源池（堆分配）── */
    case QIOC_RESOURCE: {
        struct quantum_backend_pool *pool;

        pool = kzalloc(sizeof(*pool), GFP_KERNEL);
        if (!pool)
            return -ENOMEM;

        quantum_alloc_get_pool(pool);

        if (copy_to_user(uarg, pool, sizeof(*pool))) {
            kfree(pool);
            return -EFAULT;
        }

        kfree(pool);
        break;
    }

    /* ── QIOC_FETCH：daemon取待执行任务（堆分配）── */
    case QIOC_FETCH: {
        struct quantum_fetch_req *freq;

        freq = kzalloc(sizeof(*freq), GFP_KERNEL);
        if (!freq)
            return -ENOMEM;

        ret = quantum_sched_fetch(freq);
        if (ret != 0) {
            kfree(freq);
            return ret;  /* -EAGAIN或其他错误直接透传 */
        }

        if (copy_to_user(uarg, freq, sizeof(*freq))) {
            kfree(freq);
            return -EFAULT;
        }

        kfree(freq);
        break;
    }

    /* ── QIOC_COMMIT：daemon提交执行结果（堆分配）── */
    case QIOC_COMMIT: {
        struct quantum_commit_req *creq;

        creq = kzalloc(sizeof(*creq), GFP_KERNEL);
        if (!creq)
            return -ENOMEM;

        if (copy_from_user(creq, uarg, sizeof(*creq))) {
            kfree(creq);
            return -EFAULT;
        }

        ret = quantum_sched_commit(creq);
        kfree(creq);
        break;
    }

    /* ── QIOC_SUBMIT (ABI v3 §02.2)：单 ioctl 提交，结果回填 qid ── */
    case QIOC_SUBMIT: {
        struct quantum_submit_req *sreq;
        const char *qir_body;
        int qid_pre;

        sreq = kzalloc(sizeof(*sreq), GFP_KERNEL);
        if (!sreq)
            return -ENOMEM;
        if (copy_from_user(sreq, uarg, sizeof(*sreq))) {
            kfree(sreq);
            return -EFAULT;
        }
        if (sreq->abi_version != QUANTUM_ABI_VERSION) {
            pr_warn("[interface] submit rejected: abi=%u expected=%d\n",
                    sreq->abi_version, QUANTUM_ABI_VERSION);
            kfree(sreq);
            return -EINVAL;
        }
        if (sreq->shots < 1 || sreq->shots > (1 << 20)) {
            kfree(sreq);
            return -EINVAL;
        }
        if (sreq->priority < 0 || sreq->priority > 9)
            sreq->priority = 0;
        if (sreq->error_mitigation < QMIT_NONE ||
            sreq->error_mitigation > QMIT_PEC)
            sreq->error_mitigation = QMIT_NONE;
        sreq->qasm[QUANTUM_QIR_SIZE - 1] = '\0';
        if (!is_valid_qasm(sreq->qasm)) {
            kfree(sreq);
            return -EINVAL;
        }

        /* Pre-allocate qernel_table row so qid identity is shared with the
         * legacy submit chain.  Failure here is a fatal -ENOSPC. */
        qid_pre = qernel_table_alloc_qid();
        if (qid_pre < 0) {
            kfree(sreq);
            return qid_pre;
        }

        /* Stash user cut_hint into the qernel_row so preproc can recover
         * the original intent (task->split_strategy gets reset for small
         * circuits in quantum_preproc_run's no-split branch). */
        {
            struct quantum_qernel_row *qrow;
            quantum_result_store_lock();
            qrow = qernel_table_get_locked(qid_pre);
            if (qrow) {
                qrow->cut_hint = sreq->cut_hint;
                qrow->em_kind  = sreq->error_mitigation;
            }
            quantum_result_store_unlock();
        }

        /* Strip optional first-line config header (legacy compatibility):
         * QIOC_SUBMIT carries config in struct fields, so skip any header. */
        qir_body = sreq->qasm;
        if (strncmp(qir_body, "shots=", 6) == 0 ||
            strncmp(qir_body, "priority=", 9) == 0 ||
            strncmp(qir_body, "mitigation=", 11) == 0) {
            const char *nl = strchr(qir_body, '\n');
            if (nl)
                qir_body = nl + 1;
        }

        ret = submit_chain_run(qid_pre, qir_body,
                               sreq->shots, sreq->priority,
                               sreq->error_mitigation,
                               QALLOC_STRATEGY_FIRST_FIT,
                               (sreq->cut_hint == QCUT_WIRE)
                                   ? QSPLIT_STRATEGY_SPACE_NAIVE
                                   : QSPLIT_STRATEGY_NONE);
        if (ret < 0) {
            qernel_table_release(qid_pre);
            kfree(sreq);
            return ret;
        }

        sreq->qid = ret;
        if (copy_to_user(uarg, sreq, sizeof(*sreq))) {
            kfree(sreq);
            return -EFAULT;
        }
        kfree(sreq);
        ret = 0;
        break;
    }

    default:
        return -ENOTTY;
    }

    return ret;
}

/* ============================================================
 * file_operations 注册表
 * ============================================================ */

static const struct file_operations quantum_fops = {
    .owner          = THIS_MODULE,
    .open           = quantum_open,
    .release        = quantum_release,
    .write          = quantum_write,
    .read           = quantum_read,
    .unlocked_ioctl = quantum_ioctl,
};

/* ============================================================
 * miscdevice 注册
 * ============================================================ */

static struct miscdevice quantum_miscdev = {
    .minor = MISC_DYNAMIC_MINOR,
    .name  = QUANTUM_DEV_NAME,
    .fops  = &quantum_fops,
};

/* ============================================================
 * 对外接口：由 quantum_main.c 调用
 * ============================================================ */

int quantum_interface_init(void)
{
    int ret;

    ret = misc_register(&quantum_miscdev);
    if (ret) {
        pr_err("quantum_interface: misc_register failed: %d\n", ret);
        return ret;
    }

    pr_info("quantum_interface: /dev/%s registered\n", QUANTUM_DEV_NAME);
    return 0;
}

void quantum_interface_exit(void)
{
    misc_deregister(&quantum_miscdev);
    qresult_store_clear();
    pr_info("quantum_interface: /dev/%s unregistered\n", QUANTUM_DEV_NAME);
}