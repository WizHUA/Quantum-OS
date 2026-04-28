#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/string.h>
#include <linux/ktime.h>

#include "quantum_types.h"

/* Forward decl from quantum_batch.c — see SSOT §02.5. The real per-backend
 * pool + binder lands in M5; this stub just records the handoff so M3/M4
 * can wire alloc → batch without depending on the M5 rewrite. */
int quantum_batch_intake(int backend_id,
                         const struct quantum_provenance *prov);

/* Forward decls from quantum_result_store.c — used by M3 per-row walk
 * (REVIEW-PRE O-2). */
int task_table_for_each_in_qernel(int qid,
                                  int (*cb)(struct quantum_task_row *,
                                            void *),
                                  void *arg);
int task_table_set_backend(const struct quantum_provenance *prov,
                           int backend_id);
int task_table_set_state(const struct quantum_provenance *prov,
                         int new_state);

/* Forward decls from quantum_result_store.c (DEMO-PIVOT D-1: read manifest
 * to decide per-fragment qubit count for backend selection). */
struct quantum_qernel_row *qernel_table_get_locked(int qid);
void quantum_result_store_lock(void);
void quantum_result_store_unlock(void);

/* ============================================================
 * 全局状态
 * ============================================================ */

/*
 * g_backend_pool：QPU资源池，记录各后端占用状态
 * g_dev_info：    硬件描述符，记录各qubit校准与拓扑数据
 *
 * 并发规则：
 *   g_backend_pool 由 g_alloc_lock 保护
 *   g_dev_info     由 g_devinfo_lock 保护
 *   读取时加锁拍快照，快照后释放锁再使用数据
 */
static struct quantum_backend_pool g_backend_pool;
static struct quantum_dev_info     g_dev_info;
static DEFINE_SPINLOCK(g_alloc_lock);
static DEFINE_SPINLOCK(g_devinfo_lock);

/* ============================================================
 * 内部：分配算法实现
 * ============================================================ */

/*
 * alloc_first_fit —— 第一个满足qubit数需求的空闲QPU
 *
 * SSOT §02.4 (post pre-M3 review B-1): alloc only picks a backend; it does
 * NOT touch phys_qubits[] — physical placement is owned by batch.binder
 * inside the cluster. We keep the legacy signature for the strategy table
 * but silently zero out_phys so callers that still inspect it see a
 * neutral "unset" pattern.
 */
static int alloc_first_fit(int need_qubits,
                            const struct quantum_dev_info *devinfo,
                            const struct quantum_task_struct *task,
                            int *out_backend,
                            int *out_start,
                            int *out_phys)
{
    unsigned long flags;
    int i;

    (void)devinfo; (void)task;

    if (out_phys)
        memset(out_phys, 0, QUANTUM_MAX_QUBITS * sizeof(int));

    spin_lock_irqsave(&g_alloc_lock, flags);
    for (i = 0; i < g_backend_pool.num_backends; i++) {
        struct quantum_backend *b = &g_backend_pool.backends[i];
        if (b->state == QBACKEND_STATE_IDLE &&
            b->total_qubits >= need_qubits) {
            *out_backend = i;
            *out_start   = 0;
            /* TODO(M5): release reservation on terminal states (REVIEW-PRE O-1) */
            b->num_qubits_available -= need_qubits; /* reserve count only */
            spin_unlock_irqrestore(&g_alloc_lock, flags);
            return 0;
        }
    }
    spin_unlock_irqrestore(&g_alloc_lock, flags);
    return -1;
}

/*
 * alloc_fidelity_opt —— 里程碑6占位
 * 选择保真度预估最高的空闲QPU，需要devinfo中的gate_fidelity数据
 */
static int alloc_fidelity_opt(int need_qubits,
                               const struct quantum_dev_info *devinfo,
                               const struct quantum_task_struct *task,
                               int *out_backend,
                               int *out_start,
                               int *out_phys)
{
    /*
     * 里程碑6实现提示：
     * 1. 遍历所有IDLE且qubit数满足需求的后端
     * 2. 对每个后端，计算 qubit[0..need_qubits-1] 的
     *    gate2_fidelity 均值（x1000整数）
     * 3. 乘以退相干衰减因子：exp(-depth/T2)用整数近似
     * 4. 选择综合得分最高的后端
     */
    pr_warn_once("alloc_fidelity_opt: not implemented, falling back to first_fit\n");
    return alloc_first_fit(need_qubits, devinfo, task,
                           out_backend, out_start, out_phys);
}

/*
 * alloc_topo_match —— 里程碑6占位
 * 基于qubit_mapping[]做子图同构匹配，最小化SWAP门数
 */
static int alloc_topo_match(int need_qubits,
                             const struct quantum_dev_info *devinfo,
                             const struct quantum_task_struct *task,
                             int *out_backend,
                             int *out_start,
                             int *out_phys)
{
    /*
     * 里程碑6实现提示：
     * 1. 从 task->sub_circuits[task->num_sub_done].qubit_mapping[]
     *    提取逻辑qubit间的连接关系（门操作图）
     * 2. 在 devinfo->qubits[].neighbors[] 描述的物理拓扑图中
     *    做子图同构搜索（VF2算法）
     * 3. 找到最优映射，填写 out_phys[]
     */
    pr_warn_once("alloc_topo_match: not implemented, falling back to first_fit\n");
    return alloc_first_fit(need_qubits, devinfo, task,
                           out_backend, out_start, out_phys);
}

/* 分配算法函数指针类型 */
typedef int (*quantum_alloc_fn)(int need_qubits,
                                 const struct quantum_dev_info *devinfo,
                                 const struct quantum_task_struct *task,
                                 int *out_backend,
                                 int *out_start,
                                 int *out_phys);

/* 策略表（新增算法：实现函数后在此注册，不修改其他文件） */
static const quantum_alloc_fn alloc_strategy_table[] = {
    [QALLOC_STRATEGY_FIRST_FIT]  = alloc_first_fit,
    [QALLOC_STRATEGY_FIDELITY]   = alloc_fidelity_opt,
    [QALLOC_STRATEGY_REGRESSION] = alloc_first_fit,   /* 里程碑6替换 */
    [QALLOC_STRATEGY_TOPO]       = alloc_topo_match,
};

#define ALLOC_STRATEGY_MAX \
    (sizeof(alloc_strategy_table) / sizeof(alloc_strategy_table[0]))

/*
 * 执行分配策略（内部调用，已加锁或无需加锁的上下文）
 * strategy超出范围时回退到FIRST_FIT
 */
static int alloc_do_strategy(int strategy,
                              int need_qubits,
                              const struct quantum_dev_info *devinfo,
                              const struct quantum_task_struct *task,
                              int *out_backend,
                              int *out_start,
                              int *out_phys)
{
    quantum_alloc_fn fn;

    if (strategy < 0 || strategy >= (int)ALLOC_STRATEGY_MAX ||
        !alloc_strategy_table[strategy]) {
        strategy = QALLOC_STRATEGY_FIRST_FIT;
    }
    fn = alloc_strategy_table[strategy];
    return fn(need_qubits, devinfo, task, out_backend, out_start, out_phys);
}

/* ============================================================
 * 内部：保真度预估（整数近似）
 * ============================================================ */

/*
 * 基于qubit校准数据估算线路保真度
 * 返回值：0~1000
 *
 * 当前实现：简单均值模型
 *   fidelity = avg(gate1_fidelity) * (1 - gate_count * avg_error_rate)
 * 里程碑6：替换为数值代价模型（乘积公式 + 退相干衰减）
 */
static int estimate_fidelity(int backend_id,
                              int num_qubits,
                              int gate_count,
                              const struct quantum_dev_info *devinfo)
{
    int base, i, offset, sum, count;

    if (backend_id < 0 || backend_id >= devinfo->num_backends)
        return 500; /* 默认中等保真度 */

    base  = backend_id * QUANTUM_MAX_QUBITS;
    sum   = 0;
    count = 0;

    for (i = 0; i < num_qubits && i < QUANTUM_MAX_QUBITS; i++) {
        offset = base + i;
        if (offset < QUANTUM_MAX_TOTAL_QUBITS &&
            devinfo->qubits[offset].available) {
            sum   += devinfo->qubits[offset].gate1_fidelity_x1000;
            count++;
        }
    }

    if (count == 0)
        return 500;

    /* avg_fidelity = sum / count（已是×1000） */
    /* 门数越多，保真度衰减：每个门约损失 (1000 - avg)/1000 × 1 ‰ */
    return sum / count;
}

/* ============================================================
 * 对外接口：初始化/退出
 * ============================================================ */

int quantum_alloc_init(void)
{
    int b, i, j, base;
    /*
     * Default backend table — SSOT §04 / pre-M3 review B-3 require at
     * least two distinct backends so alloc has an observable choice and
     * downstream batch can demonstrate cross-backend cluster placement.
     * Real values are overridden by QIOC_CALIB_LOAD (M8); this is the
     * built-in seed so the demo and `qctl resource` work out of the box.
     */
    static const struct {
        const char *name;
        int total_qubits;
        int fidelity_score;     /* x1000 */
        int connectivity_type;  /* 0 full, 1 NN, 2 heavy-hex */
    } seed[] = {
        { "aer0", QUANTUM_MAX_QUBITS, 990, 0 }, /* high-fidelity ideal sim */
        { "aer1", QUANTUM_MAX_QUBITS, 940, 2 }, /* heavy-hex, slightly noisier */
    };
    const int N = (int)(sizeof(seed) / sizeof(seed[0]));

    memset(&g_backend_pool, 0, sizeof(g_backend_pool));
    memset(&g_dev_info,     0, sizeof(g_dev_info));

    g_backend_pool.num_backends = N;
    g_dev_info.num_backends     = N;
    g_dev_info.total_qubits     = N * QUANTUM_MAX_QUBITS;

    for (b = 0; b < N; b++) {
        struct quantum_backend *bk = &g_backend_pool.backends[b];

        bk->id           = b;
        strncpy(bk->name, seed[b].name, sizeof(bk->name) - 1);
        bk->total_qubits          = seed[b].total_qubits;
        bk->state                 = QBACKEND_STATE_IDLE;
        bk->current_qid           = -1;
        bk->fidelity_score        = seed[b].fidelity_score;
        bk->num_qubits_available  = seed[b].total_qubits;
        bk->connectivity_type     = seed[b].connectivity_type;

        base = b * QUANTUM_MAX_QUBITS;
        for (i = 0; i < QUANTUM_MAX_QUBITS; i++) {
            struct quantum_qubit_info *q = &g_dev_info.qubits[base + i];
            q->qubit_id   = base + i;
            q->backend_id = b;
            q->local_id   = i;
            q->available  = 1;

            q->t1_us_x10              = 1000;                /* 100 us */
            q->t2_us_x10              = 1000;
            q->readout_fidelity_x1000 = seed[b].fidelity_score;
            q->gate1_fidelity_x1000   = seed[b].fidelity_score;
            q->gate2_fidelity_x1000   = seed[b].fidelity_score - 10;

            q->num_neighbors = 0;
            for (j = 0; j < QUANTUM_MAX_NEIGHBORS; j++)
                q->neighbors[j] = -1;

            q->coupling_map     = 0;
            q->last_update_time = 0;
        }
    }

    pr_info("quantum_alloc: initialized, %d backend(s), %d qubits total\n",
            g_backend_pool.num_backends, g_dev_info.total_qubits);
    for (b = 0; b < N; b++)
        pr_info("quantum_alloc:   [%d] %s qubits=%d fidelity=%d conn=%d\n",
                b, seed[b].name, seed[b].total_qubits,
                seed[b].fidelity_score, seed[b].connectivity_type);
    return 0;
}

void quantum_alloc_exit(void)
{
    pr_info("quantum_alloc: exiting\n");
}

/* ============================================================
 * 对外接口：提交链调用
 * ============================================================ */

/*
 * quantum_alloc_run —— 提交链中调用
 *
 * need_split=0：执行分配，填写 task->assigned_backend_id/phys_qubits[]/fidelity_score
 * need_split=1：跳过，子线路在sched dispatch时由quantum_alloc_find_idle单独分配
 */

/* ============================================================
 * M3 (SSOT §02.4 / REVIEW-PRE O-2): per-row walk
 *
 * preproc emits N×M task_table rows in QVSTATE_PREPARED. alloc walks them
 * here, picks one backend per fragment (variants of the same fragment share
 * the same backend, per SSOT §02.4), advances each row PREPARED -> ASSIGNED,
 * and hands every variant to batch.intake with REAL (qid, frag, var)
 * provenance.
 * ============================================================ */

struct alloc_plan {
    struct quantum_provenance prov;
    int backend_id;
    int fidelity;
    __u8 frag_qubits;
};

struct alloc_walk_ctx {
    struct alloc_plan *plans;
    int n_plans;
    int capacity;
    int num_backends;
    /* DEMO-PIVOT D-1: per-backend ETA snapshot at start, mutated as we plan */
    __u64 eta_ns[QUANTUM_MAX_BACKENDS];
    /* per-fragment qubit count from manifest (cached during walk) */
    __u8  frag_qubits[QUANTUM_MAX_FRAGMENTS];
    __u8  has_frag_qubits;
};

/* DEMO-PIVOT D-1: pick the backend with lowest eta_ns among those that
 * can fit need_qubits. Falls back to fragment_index % num_backends when
 * all are oversubscribed. Updates eta_ns in place with a 1us/shot estimate
 * so subsequent rows see the load this row will impose. */
static int alloc_demo_pick_backend(struct alloc_walk_ctx *c,
                                   int need_qubits, __u32 shots)
{
    int i, best = -1;
    __u64 best_eta = (__u64)-1;
    __u64 add_ns;

    for (i = 0; i < c->num_backends; i++) {
        struct quantum_backend *bk = &g_backend_pool.backends[i];
        if (need_qubits > bk->total_qubits)
            continue;
        if (c->eta_ns[i] < best_eta) {
            best_eta = c->eta_ns[i];
            best = i;
        }
    }
    if (best < 0)
        best = 0;
    /* per-row provisional load: shots * 1us (sched will reconcile) */
    add_ns = (__u64)shots * 1000ULL;
    c->eta_ns[best] += add_ns;
    return best;
}

static int alloc_walk_cb(struct quantum_task_row *row, void *arg)
{
    struct alloc_walk_ctx *c = arg;
    int b;
    int need_q;

    if (row->state != QVSTATE_PREPARED)
        return 0;
    if (c->n_plans >= c->capacity)
        return -ENOSPC;
    if (c->num_backends <= 0)
        return -ENODEV;

    /* per-fragment qubit count: from manifest if available, else 1 */
    need_q = c->has_frag_qubits ?
             c->frag_qubits[row->prov.fragment_index] : 1;
    if (need_q < 1) need_q = 1;

    b = alloc_demo_pick_backend(c, need_q, row->prov.shots);
    c->plans[c->n_plans].prov        = row->prov;
    c->plans[c->n_plans].backend_id  = b;
    c->plans[c->n_plans].fidelity    = g_backend_pool.backends[b].fidelity_score;
    c->plans[c->n_plans].frag_qubits = (__u8)need_q;
    c->n_plans++;
    return 0;
}

/*
 * alloc_run_per_row — Returns: rows handed off (>=0) on success;
 *                              0 when no task_table rows exist (legacy
 *                              write() path falls back to single-shot);
 *                              <0 errno on fatal error.
 *
 * Note: callbacks under task_table_for_each_in_qernel run with the
 * result_store mutex held, so the cb only snapshots; the actual mutating
 * calls (set_backend / set_state / batch_intake) happen after the walk
 * returns to avoid recursive locking.
 */
static int alloc_run_per_row(struct quantum_task_struct *task)
{
    struct alloc_walk_ctx ctx;
    int ret, i;

    memset(&ctx, 0, sizeof(ctx));
    ctx.num_backends = g_backend_pool.num_backends;
    if (ctx.num_backends <= 0)
        return 0;
    ctx.capacity = QUANTUM_MAX_FRAGMENTS * QUANTUM_MAX_VARIANTS;
    ctx.plans = kzalloc(sizeof(*ctx.plans) * ctx.capacity, GFP_KERNEL);
    if (!ctx.plans)
        return -ENOMEM;

    /* DEMO-PIVOT D-1 + D-2: snapshot dev_info.backend_eta_ns and the
     * per-fragment qubit_count from the qernel manifest under the result
     * store mutex; everything else (set_backend / set_state / batch_intake)
     * runs after the walk to avoid recursive locking. */
    quantum_result_store_lock();
    {
        struct quantum_qernel_row *qrow = qernel_table_get_locked(task->qid);
        if (qrow) {
            int f;
            for (f = 0; f < qrow->manifest.num_fragments &&
                        f < QUANTUM_MAX_FRAGMENTS; f++) {
                ctx.frag_qubits[f] = qrow->manifest.fragment[f].qubit_count;
            }
            ctx.has_frag_qubits = 1;
        }
    }
    quantum_result_store_unlock();
    {
        unsigned long flags;
        spin_lock_irqsave(&g_alloc_lock, flags);
        for (i = 0; i < ctx.num_backends; i++)
            ctx.eta_ns[i] = g_dev_info.backend_eta_ns[i];
        spin_unlock_irqrestore(&g_alloc_lock, flags);
    }

    /* DEMO-PIVOT §3.3: alloc dev_info snapshot (one line per snapshot) */
    if (ctx.num_backends >= 2) {
        pr_info("[alloc]   dev_info snapshot: aer0 eta=%llu.%03llums aer1 eta=%llu.%03llums\n",
                ctx.eta_ns[0] / 1000000ULL,
                (ctx.eta_ns[0] / 1000ULL) % 1000ULL,
                ctx.eta_ns[1] / 1000000ULL,
                (ctx.eta_ns[1] / 1000ULL) % 1000ULL);
    }

    ret = task_table_for_each_in_qernel(task->qid, alloc_walk_cb, &ctx);
    if (ret == -ENOENT) {
        /* legacy write() path: no qernel_row backing this qid */
        kfree(ctx.plans);
        return 0;
    }
    if (ret < 0 && ret != -ENOSPC) {
        pr_warn("[alloc] qid=%d for_each err=%d\n", task->qid, ret);
        kfree(ctx.plans);
        return ret;
    }
    if (ctx.n_plans == 0) {
        kfree(ctx.plans);
        return 0;
    }

    for (i = 0; i < ctx.n_plans; i++) {
        struct alloc_plan *p = &ctx.plans[i];
        int sret;

        sret = task_table_set_backend(&p->prov, p->backend_id);
        if (sret) {
            pr_warn("[alloc] qid=%u frag=%u var=%u set_backend err=%d\n",
                    p->prov.qid, p->prov.fragment_index,
                    p->prov.variant_index, sret);
            continue;
        }
        sret = task_table_set_state(&p->prov, QVSTATE_ASSIGNED);
        if (sret) {
            pr_warn("[alloc] qid=%u frag=%u var=%u set_state err=%d\n",
                    p->prov.qid, p->prov.fragment_index,
                    p->prov.variant_index, sret);
            continue;
        }

        pr_info("[alloc]   qid=%u frag=%u var=%u (q=%u) -> backend=%d fidelity=%d\n",
                p->prov.qid, p->prov.fragment_index,
                p->prov.variant_index, p->frag_qubits,
                p->backend_id, p->fidelity);

        (void)quantum_batch_intake(p->backend_id, &p->prov);
    }

    ret = ctx.n_plans;
    kfree(ctx.plans);
    return ret;
}

int quantum_alloc_run(struct quantum_task_struct *task)
{
    /* 修复：devinfo_snap 从栈改为堆分配 */
    struct quantum_dev_info *devinfo_snap;
    int phys[QUANTUM_MAX_QUBITS];
    int out_backend = -1, out_start = 0;
    int ret;

    if (task->need_split) {
        pr_debug("quantum_alloc: qid=%d need_split=1, skipping global alloc\n",
                 task->qid);
        task->assigned_backend_id = -1;
        task->fidelity_score      = 0;
        return 0;
    }

    devinfo_snap = kzalloc(sizeof(*devinfo_snap), GFP_KERNEL);
    if (!devinfo_snap)
        return -ENOMEM;

    spin_lock(&g_devinfo_lock);
    memcpy(devinfo_snap, &g_dev_info, sizeof(*devinfo_snap));
    spin_unlock(&g_devinfo_lock);

    memset(phys, 0, sizeof(phys));

    ret = alloc_do_strategy(task->alloc_strategy,
                             task->num_qubits,
                             devinfo_snap,
                             task,
                             &out_backend,
                             &out_start,
                             phys);
    if (ret < 0) {
        pr_warn("quantum_alloc: qid=%d no suitable backend "
                "(need %d qubits, strategy=%d)\n",
                task->qid, task->num_qubits, task->alloc_strategy);
        task->error_code = QERR_NO_RESOURCE;
        strncpy(task->error_info, "no available backend with sufficient qubits",
                sizeof(task->error_info) - 1);
        kfree(devinfo_snap);
        return -ENODEV;
    }

    task->assigned_backend_id = out_backend;
    /*
     * SSOT §02.4 (B-1 fix): alloc no longer fills task->phys_qubits[].
     * batch.binder picks physical qubits per cluster; until M5 lands the
     * full pool, we leave the array zeroed and hand the (qid, frag=0,
     * variant=0) provenance off to the batch intake stub so the wiring
     * is observable in dmesg.
     */
    memset(task->phys_qubits, 0, sizeof(task->phys_qubits));
    task->fidelity_score = estimate_fidelity(out_backend,
                                              task->num_qubits,
                                              task->gate_count,
                                              devinfo_snap);

    {
        int per_row = alloc_run_per_row(task);

        if (per_row > 0) {
            /* M3 path: per-(frag,var) intake completed; suppress legacy
             * single-shot summary line. */
        } else {
            /* legacy write() path (no qernel_row, no task_table rows):
             * preserve the pre-M3 single-shot wiring so smoke tests still
             * see one [batch] intake line per submission. */
            struct quantum_provenance prov;
            quantum_provenance_init(&prov, (__u32)task->qid, 0, 0);
            prov.shots = task->shots;
            (void)quantum_batch_intake(out_backend, &prov);

            pr_info("[alloc] qid=%d frag=0 -> backend=%d fidelity=%d (phys deferred to batch)\n",
                    task->qid, out_backend, task->fidelity_score);
        }
    }

    kfree(devinfo_snap);
    return 0;
}

/* ============================================================
 * 对外接口：sched调用（占用/释放/查找）
 * ============================================================ */

/*
 * quantum_alloc_acquire —— 标记QPU为BUSY，sched在dispatch时调用
 */
int quantum_alloc_acquire(int backend_id, int qid)
{
    unsigned long flags;

    if (backend_id < 0 || backend_id >= QUANTUM_MAX_BACKENDS)
        return -EINVAL;

    spin_lock_irqsave(&g_alloc_lock, flags);
    if (g_backend_pool.backends[backend_id].state != QBACKEND_STATE_IDLE) {
        spin_unlock_irqrestore(&g_alloc_lock, flags);
        return -EBUSY;
    }
    g_backend_pool.backends[backend_id].state       = QBACKEND_STATE_BUSY;
    g_backend_pool.backends[backend_id].current_qid = qid;
    spin_unlock_irqrestore(&g_alloc_lock, flags);

    pr_debug("quantum_alloc: backend=%d acquired by qid=%d\n", backend_id, qid);
    return 0;
}

/*
 * quantum_alloc_release —— 释放QPU，sched在commit时调用
 */
void quantum_alloc_release(int backend_id)
{
    unsigned long flags;

    if (backend_id < 0 || backend_id >= QUANTUM_MAX_BACKENDS)
        return;

    spin_lock_irqsave(&g_alloc_lock, flags);
    g_backend_pool.backends[backend_id].state       = QBACKEND_STATE_IDLE;
    g_backend_pool.backends[backend_id].current_qid = -1;
    spin_unlock_irqrestore(&g_alloc_lock, flags);

    pr_debug("quantum_alloc: backend=%d released\n", backend_id);
}

/*
 * quantum_alloc_find_idle —— 快速查找满足qubit数的空闲QPU
 * 返回backend_id，-1=无可用
 * sched调度算法调用，使用FIRST_FIT策略（快速路径）
 */
int quantum_alloc_find_idle(int need_qubits)
{
    unsigned long flags;
    int i, found = -1;

    spin_lock_irqsave(&g_alloc_lock, flags);
    for (i = 0; i < g_backend_pool.num_backends; i++) {
        struct quantum_backend *b = &g_backend_pool.backends[i];
        if (b->state == QBACKEND_STATE_IDLE &&
            b->total_qubits >= need_qubits) {
            found = i;
            break;
        }
    }
    spin_unlock_irqrestore(&g_alloc_lock, flags);
    return found;
}

/*
 * quantum_alloc_find_best —— 保真度最优的空闲QPU
 * 用于sched_weighted策略（里程碑6）
 * 当前实现等同find_idle，里程碑6替换为保真度评分排序
 */
int quantum_alloc_find_best(const struct quantum_task_struct *task)
{
    /* 里程碑6：遍历所有IDLE后端，按fidelity_score排序 */
    return quantum_alloc_find_idle(task->num_qubits);
}

/* ============================================================
 * 对外接口：interface调用
 * ============================================================ */

void quantum_alloc_get_pool(struct quantum_backend_pool *out)
{
    unsigned long flags;

    spin_lock_irqsave(&g_alloc_lock, flags);
    memcpy(out, &g_backend_pool, sizeof(*out));
    spin_unlock_irqrestore(&g_alloc_lock, flags);
}

/* ============================================================
 * 对外接口：快照读取（preproc/sched调用）
 * ============================================================ */

void quantum_alloc_get_dev_info(struct quantum_dev_info *out)
{
    unsigned long flags;

    spin_lock_irqsave(&g_devinfo_lock, flags);
    memcpy(out, &g_dev_info, sizeof(*out));
    spin_unlock_irqrestore(&g_devinfo_lock, flags);
}

/* ============================================================
 * 对外接口：calib调用（写入DevInfo）
 * ============================================================ */

void quantum_alloc_update_qubit(int qubit_id,
                                 int available,
                                 int t1_us_x10,
                                 int t2_us_x10,
                                 int r_fid_x1000,
                                 int g1_fid_x1000,
                                 int g2_fid_x1000)
{
    unsigned long flags;
    struct quantum_qubit_info *q;

    if (qubit_id < 0 || qubit_id >= QUANTUM_MAX_TOTAL_QUBITS)
        return;

    spin_lock_irqsave(&g_devinfo_lock, flags);
    q = &g_dev_info.qubits[qubit_id];
    q->available               = available;
    q->t1_us_x10               = t1_us_x10;
    q->t2_us_x10               = t2_us_x10;
    q->readout_fidelity_x1000  = r_fid_x1000;
    q->gate1_fidelity_x1000    = g1_fid_x1000;
    q->gate2_fidelity_x1000    = g2_fid_x1000;
    q->last_update_time        = ktime_get_ns();
    g_dev_info.last_calibration_time = q->last_update_time;
    spin_unlock_irqrestore(&g_devinfo_lock, flags);
}

/*
 * quantum_alloc_update_qubit_topo —— 更新拓扑连接信息
 * 里程碑6：calib实装后调用
 */
void quantum_alloc_update_qubit_topo(int qubit_id,
                                      const int *neighbors,
                                      int num_neighbors)
{
    unsigned long flags;
    struct quantum_qubit_info *q;
    int i, n;

    if (qubit_id < 0 || qubit_id >= QUANTUM_MAX_TOTAL_QUBITS)
        return;

    n = (num_neighbors < QUANTUM_MAX_NEIGHBORS) ?
         num_neighbors : QUANTUM_MAX_NEIGHBORS;

    spin_lock_irqsave(&g_devinfo_lock, flags);
    q = &g_dev_info.qubits[qubit_id];
    q->num_neighbors = n;
    for (i = 0; i < n; i++)
        q->neighbors[i] = neighbors[i];
    for (i = n; i < QUANTUM_MAX_NEIGHBORS; i++)
        q->neighbors[i] = -1;
    spin_unlock_irqrestore(&g_devinfo_lock, flags);
}

void quantum_alloc_set_backend_state(int backend_id, int state)
{
    unsigned long flags;

    if (backend_id < 0 || backend_id >= QUANTUM_MAX_BACKENDS)
        return;

    spin_lock_irqsave(&g_alloc_lock, flags);
    g_backend_pool.backends[backend_id].state = state;
    if (state == QBACKEND_STATE_IDLE)
        g_backend_pool.backends[backend_id].current_qid = -1;
    spin_unlock_irqrestore(&g_alloc_lock, flags);

    pr_debug("quantum_alloc: backend=%d state→%d\n", backend_id, state);
}

/* ============================================================
 * DEMO-PIVOT D-1: backend ETA accounting
 *
 * Used by quantum_batch.c (commit cluster -> add cluster_estimated_ns)
 * and quantum_sched.c (cluster done -> sub cluster_actual_ns). Sits on
 * g_devinfo_lock since it touches g_dev_info.backend_eta_ns[].
 * ============================================================ */

void quantum_alloc_eta_add(int backend_id, __u64 ns)
{
    unsigned long flags;

    if (backend_id < 0 || backend_id >= QUANTUM_MAX_BACKENDS)
        return;

    spin_lock_irqsave(&g_devinfo_lock, flags);
    g_dev_info.backend_eta_ns[backend_id] += ns;
    spin_unlock_irqrestore(&g_devinfo_lock, flags);
}

void quantum_alloc_eta_sub(int backend_id, __u64 ns)
{
    unsigned long flags;

    if (backend_id < 0 || backend_id >= QUANTUM_MAX_BACKENDS)
        return;

    spin_lock_irqsave(&g_devinfo_lock, flags);
    if (g_dev_info.backend_eta_ns[backend_id] >= ns)
        g_dev_info.backend_eta_ns[backend_id] -= ns;
    else
        g_dev_info.backend_eta_ns[backend_id] = 0;
    spin_unlock_irqrestore(&g_devinfo_lock, flags);
}

__u64 quantum_alloc_eta_get(int backend_id)
{
    unsigned long flags;
    __u64 v;

    if (backend_id < 0 || backend_id >= QUANTUM_MAX_BACKENDS)
        return 0;

    spin_lock_irqsave(&g_devinfo_lock, flags);
    v = g_dev_info.backend_eta_ns[backend_id];
    spin_unlock_irqrestore(&g_devinfo_lock, flags);
    return v;
}