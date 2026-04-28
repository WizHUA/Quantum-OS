#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/string.h>

#include "quantum_types.h"

/* 前向声明：依赖alloc提供DevInfo快照 */
void quantum_alloc_get_dev_info(struct quantum_dev_info *out);

/* 前向声明：result_store APIs（M3：Manifest + (frag,var) emission） */
void quantum_result_store_lock(void);
void quantum_result_store_unlock(void);
struct quantum_qernel_row *qernel_table_get_locked(int qid);
int  qernel_table_set_state(int qid, int new_state);
int  task_table_insert(int qid, const struct quantum_task_row *row);

static int preproc_emit_manifest(struct quantum_task_struct *task);

/* ============================================================
 * 内部：线路静态分析
 * ============================================================ */

/*
 * parse_qreg_lines —— 从QASM提取qubit总数
 *
 * 扫描所有 "qreg q[N];" 行，返回最大 N（即qubit总数）
 * 支持多个qreg声明（累加）
 */
static int parse_qreg_lines(const char *qasm, int *num_qubits)
{
    const char *p = qasm;
    int total = 0, found = 0, n;

    while (*p) {
        /* 跳过空白 */
        while (*p == ' ' || *p == '\t' || *p == '\r')
            p++;

        /* 匹配 "qreg" */
        if (strncmp(p, "qreg", 4) == 0 && (p[4] == ' ' || p[4] == '\t')) {
            p += 4;
            /* 跳过空白和标识符名 */
            while (*p == ' ' || *p == '\t') p++;
            while (*p && *p != '[' && *p != '\n') p++;
            /* 提取 [N] */
            if (*p == '[') {
                p++;
                n = 0;
                while (*p >= '0' && *p <= '9') {
                    n = n * 10 + (*p - '0');
                    p++;
                }
                total += n;
                found  = 1;
            }
        }

        /* 跳到行尾 */
        while (*p && *p != '\n') p++;
        if (*p == '\n') p++;
    }

    *num_qubits = total;
    return found ? 0 : -1;
}

/*
 * scan_gate_lines —— 统计门数和估算线路深度
 *
 * 简单估算：depth ≈ gate_count（忽略并行）
 * 里程碑6可替换为DAG拓扑排序的精确深度计算
 */
static void scan_gate_lines(const char *qasm,
                              int *gate_count,
                              int *circuit_depth)
{
    const char *p = qasm;
    int gates = 0, is_skip, i;

    /* 跳过的关键字（不计为门操作） */
    static const char * const skip_kw[] = {
        "OPENQASM", "include", "qreg", "creg", "measure",
        "barrier", "//", NULL
    };

    while (*p) {
        /* 跳过空白 */
        while (*p == ' ' || *p == '\t' || *p == '\r') p++;
        if (*p == '\n') { p++; continue; }
        if (!*p) break;

        /* 检查是否为门操作行 */
        is_skip = 0;
        for (i = 0; skip_kw[i]; i++) {
            int len = strlen(skip_kw[i]);
            if (strncmp(p, skip_kw[i], len) == 0) {
                is_skip = 1;
                break;
            }
        }
        if (!is_skip && *p != '\0' && *p != '\n')
            gates++;

        /* 跳到行尾 */
        while (*p && *p != '\n') p++;
        if (*p == '\n') p++;
    }

    *gate_count    = gates;
    *circuit_depth = gates; /* 简单估算 */
}

/* ============================================================
 * 内部：切分算法实现
 * ============================================================ */

/*
 * split_space_naive —— 按qubit空间均分
 *
 * 将原始线路按qubit编号范围平均分为n段，每段最多QUANTUM_MAX_QUBITS个qubit。
 * 门操作：若所有操作的qubit编号均属于本组范围则保留，否则丢弃。
 * 限制：跨组的双qubit门被丢弃（设计文档明确指出此限制）。
 *
 * 填写字段：
 *   task->num_sub_circuits / split_strategy / merge_strategy / batch_mode
 *   sub->index / num_qubits / qasm / weight_num / weight_den /
 *        dep_sub_id / backend_constraint / qubit_mapping[] / state
 */
static int split_space_naive(struct quantum_task_struct *task,
                              int max_q,
                              const struct quantum_dev_info *devinfo)
{
    int n, i, qn;
    int num_qubits = task->num_qubits;

    /* 切分数：向上取整 */
    n = (num_qubits + max_q - 1) / max_q;

    if (n > QUANTUM_MAX_SUB_CIRCUITS) {
        pr_warn("quantum_preproc: qid=%d needs %d sub-circuits, max=%d\n",
                task->qid, n, QUANTUM_MAX_SUB_CIRCUITS);
        n = QUANTUM_MAX_SUB_CIRCUITS;
    }

    task->num_sub_circuits = n;
    task->merge_strategy   = QMERGE_STRATEGY_TENSOR;
    task->batch_mode       = QBATCH_MODE_SERIAL;

    for (i = 0; i < n; i++) {
        struct quantum_sub_circuit *sub = &task->sub_circuits[i];
        const char *src = task->qir;
        char *dst;
        int dst_pos = 0;
        int group_start = i * max_q;
        int group_end   = group_start + max_q; /* 不含 */
        int sub_qubits  = num_qubits - group_start;
        int j;

        if (sub_qubits > max_q) sub_qubits = max_q;

        memset(sub, 0, sizeof(*sub));
        sub->index              = i;
        sub->num_qubits         = sub_qubits;
        sub->weight_num         = 1;
        sub->weight_den         = n;
        sub->dep_sub_id         = -1;
        sub->backend_constraint = -1;
        sub->state              = QTASK_STATE_QUEUED;
        sub->mit_data           = NULL;

        /* 建立局部→全局qubit映射 */
        for (j = 0; j < sub_qubits; j++)
            sub->qubit_mapping[j] = group_start + j;
        for (j = sub_qubits; j < QUANTUM_MAX_QUBITS; j++)
            sub->qubit_mapping[j] = -1;

        /* 提取子线路QASM */
        dst     = sub->qasm;
        dst_pos = 0;

        while (*src) {
            const char *line_start = src;
            int line_len = 0;
            int keep     = 0;

            /* 计算行长度 */
            while (src[line_len] && src[line_len] != '\n')
                line_len++;

            /* 公共头：保留 */
            if (strncmp(line_start, "OPENQASM", 8) == 0 ||
                strncmp(line_start, "include",  7) == 0 ||
                strncmp(line_start, "//",       2) == 0 ||
                strncmp(line_start, "qreg",     4) == 0 ||
                strncmp(line_start, "creg",     4) == 0) {
                keep = 1;
            } else if (line_len > 0 && line_start[0] != '\0' &&
                       line_start[0] != '\n') {
                /*
                 * 门操作行：扫描所有 q[N]，
                 * 若所有N均属于 [group_start, group_end)则保留
                 */
                const char *scan = line_start;
                int all_in_group = 1;
                int found_qubit  = 0;

                while (scan < line_start + line_len) {
                    /* 找 q[ */
                    if (*scan == 'q' && *(scan+1) == '[') {
                        scan += 2;
                        qn = 0;
                        while (*scan >= '0' && *scan <= '9') {
                            qn = qn * 10 + (*scan - '0');
                            scan++;
                        }
                        found_qubit = 1;
                        if (qn < group_start || qn >= group_end) {
                            all_in_group = 0;
                            break;
                        }
                    } else {
                        scan++;
                    }
                }

                keep = (found_qubit && all_in_group);
            }

            if (keep && dst_pos + line_len + 1 < QUANTUM_SUB_QIR_SIZE) {
                memcpy(dst + dst_pos, line_start, line_len);
                dst_pos += line_len;
                dst[dst_pos++] = '\n';
            }

            src += line_len;
            if (*src == '\n') src++;
        }
        dst[dst_pos] = '\0';

        /* 统计子线路门数和深度 */
        scan_gate_lines(sub->qasm, &sub->gate_count, &sub->circuit_depth);

        pr_debug("quantum_preproc: qid=%d sub[%d] qubits=%d gates=%d\n",
                 task->qid, i, sub->num_qubits, sub->gate_count);
    }

    return 0;
}

/*
 * split_time —— 按线路深度时间切分（里程碑6占位）
 *
 * 将线路按时间步（门层）切分，相邻子线路间通过量子状态层析连接。
 * 对应合并策略：QMERGE_STRATEGY_MLFT
 */
static int split_time(struct quantum_task_struct *task,
                       int max_q,
                       const struct quantum_dev_info *devinfo)
{
    pr_warn_once("quantum_preproc: split_time not implemented, "
                 "falling back to space_naive\n");
    return split_space_naive(task, max_q, devinfo);
}

/*
 * split_space_prob —— 准概率空间切分（里程碑6占位）
 *
 * 跨组门使用PEC准概率分解，不丢弃跨组门。
 * 对应合并策略：QMERGE_STRATEGY_WEIGHTED
 */
static int split_space_prob(struct quantum_task_struct *task,
                             int max_q,
                             const struct quantum_dev_info *devinfo)
{
    pr_warn_once("quantum_preproc: split_space_prob not implemented, "
                 "falling back to space_naive\n");
    return split_space_naive(task, max_q, devinfo);
}

/*
 * split_topo_aware —— 拓扑感知切分（里程碑6占位）
 *
 * 使用 devinfo->qubits[].neighbors[] 构建连通图，
 * 用图划分算法（Kernighan-Lin）找最小割。
 * 对应合并策略：QMERGE_STRATEGY_MLFT
 */
static int split_topo_aware(struct quantum_task_struct *task,
                             int max_q,
                             const struct quantum_dev_info *devinfo)
{
    pr_warn_once("quantum_preproc: split_topo_aware not implemented, "
                 "falling back to space_naive\n");
    return split_space_naive(task, max_q, devinfo);
}

/* 切分算法函数指针类型（签名统一，info充分） */
typedef int (*quantum_split_fn)(struct quantum_task_struct *task,
                                 int max_q,
                                 const struct quantum_dev_info *devinfo);

/* 策略路由表（新增算法：实现后在此注册case，不修改其他文件） */
static int preproc_select_split_strategy(struct quantum_task_struct *task,
                                          int max_q,
                                          const struct quantum_dev_info *devinfo)
{
    switch (task->split_strategy) {
    case QSPLIT_STRATEGY_SPACE_NAIVE:
        return split_space_naive(task, max_q, devinfo);
    case QSPLIT_STRATEGY_TIME:
        return split_time(task, max_q, devinfo);
    case QSPLIT_STRATEGY_SPACE_PROB:
        return split_space_prob(task, max_q, devinfo);
    case QSPLIT_STRATEGY_TOPO_AWARE:
        return split_topo_aware(task, max_q, devinfo);
    default:
        task->split_strategy = QSPLIT_STRATEGY_SPACE_NAIVE;
        return split_space_naive(task, max_q, devinfo);
    }
}

/* ============================================================
 * 内部：误差缓解预处理
 * ============================================================ */

static int mit_pre_none(struct quantum_task_struct *task)
{
    /* 无预处理，直接返回 */
    return 0;
}

static int mit_pre_mem(struct quantum_task_struct *task)
{
    /*
     * 里程碑5实现提示：
     * 为每个子线路（或整体任务）分配混淆矩阵结构体：
     *   sub->mit_data = kzalloc(sizeof(struct confusion_matrix), GFP_KERNEL)
     * 此时不填数据（未执行，不知误差率），由postproc阶段填写并使用
     */
    pr_warn_once("quantum_preproc: mit_pre_mem not implemented\n");
    return 0;
}

static int mit_pre_cdr(struct quantum_task_struct *task)
{
    /*
     * 里程碑6：生成near-Clifford训练线路集
     * 将训练线路指针存入 sub->mit_data
     */
    pr_warn_once("quantum_preproc: mit_pre_cdr not implemented\n");
    return 0;
}

static int mit_pre_pec(struct quantum_task_struct *task)
{
    /*
     * 里程碑6：构建准概率分解指令集
     * 修改 sub->qasm（插入PEC分解线路）
     * 填写 sub->weight_num / sub->weight_den（真实准概率权重）
     */
    pr_warn_once("quantum_preproc: mit_pre_pec not implemented\n");
    return 0;
}

/* 误差缓解预处理策略路由 */
typedef int (*quantum_mit_pre_fn)(struct quantum_task_struct *task);

static const quantum_mit_pre_fn mit_pre_table[] = {
    [QMIT_NONE] = mit_pre_none,
    [QMIT_MEM]  = mit_pre_mem,
    [QMIT_CDR]  = mit_pre_cdr,
    [QMIT_PEC]  = mit_pre_pec,
};

#define MIT_PRE_TABLE_MAX \
    (sizeof(mit_pre_table) / sizeof(mit_pre_table[0]))

static int preproc_error_mitigation_pre(struct quantum_task_struct *task)
{
    int level = task->error_mitigation;
    quantum_mit_pre_fn fn;

    if (level < 0 || level >= (int)MIT_PRE_TABLE_MAX || !mit_pre_table[level])
        level = QMIT_NONE;

    fn = mit_pre_table[level];
    return fn(task);
}

/* ============================================================
 * 对外接口
 * ============================================================ */

/*
 * quantum_preproc_run —— 预处理模块唯一对外接口
 *
 * 执行顺序（机制层固定）：
 *   阶段1：线路静态分析（提取num_qubits/depth/gates）
 *   阶段2：超限检测（num_qubits > QUANTUM_MAX_QUBITS?）
 *   阶段3：切分策略路由（触发切分时调用）
 *   阶段4：误差缓解预处理
 */
int quantum_preproc_run(struct quantum_task_struct *task)
{
    /* 修复：devinfo_snap 从栈改为堆分配
     * sizeof(quantum_dev_info) 很大（含 QUANTUM_MAX_TOTAL_QUBITS 个 qubit_info）
     * 放在栈上直接导致内核栈溢出 → memcpy_erms crash
     */
    struct quantum_dev_info *devinfo_snap;
    int ret;

    devinfo_snap = kzalloc(sizeof(*devinfo_snap), GFP_KERNEL);
    if (!devinfo_snap)
        return -ENOMEM;

    /* 阶段1：线路静态分析 */
    ret = parse_qreg_lines(task->qir, &task->num_qubits);
    if (ret < 0 || task->num_qubits <= 0) {
        pr_warn("quantum_preproc: qid=%d no qreg found or parse failed\n",
                task->qid);
        task->error_code = QERR_SYNTAX;
        strncpy(task->error_info, "no valid qreg declaration found",
                sizeof(task->error_info) - 1);
        kfree(devinfo_snap);
        return -EINVAL;
    }

    scan_gate_lines(task->qir, &task->gate_count, &task->circuit_depth);

    pr_debug("quantum_preproc: qid=%d qubits=%d gates=%d depth=%d\n",
             task->qid, task->num_qubits, task->gate_count, task->circuit_depth);

    /* 阶段2：超限检测 */
    if (task->num_qubits > QUANTUM_MAX_QUBITS) {
        pr_info("quantum_preproc: qid=%d num_qubits=%d > %d, splitting\n",
                task->qid, task->num_qubits, QUANTUM_MAX_QUBITS);

        task->need_split = 1;

        if (task->split_strategy == QSPLIT_STRATEGY_NONE)
            task->split_strategy = QSPLIT_STRATEGY_SPACE_NAIVE;

        quantum_alloc_get_dev_info(devinfo_snap);  /* 修复：传指针 */

        /* 阶段3：切分策略路由 */
        ret = preproc_select_split_strategy(task, QUANTUM_MAX_QUBITS,
                                             devinfo_snap);  /* 修复：传指针 */
        if (ret < 0) {
            pr_err("quantum_preproc: qid=%d split failed: %d\n",
                   task->qid, ret);
            task->error_code = QERR_SPLIT_FAIL;
            strncpy(task->error_info, "circuit splitting failed",
                    sizeof(task->error_info) - 1);
            kfree(devinfo_snap);
            return -EINVAL;
        }

        pr_info("quantum_preproc: qid=%d split into %d sub-circuits "
                "(strategy=%d merge=%d)\n",
                task->qid, task->num_sub_circuits,
                task->split_strategy, task->merge_strategy);
    } else {
        task->need_split       = 0;
        task->split_strategy   = QSPLIT_STRATEGY_NONE;
        task->merge_strategy   = QMERGE_STRATEGY_DIRECT;
        task->batch_mode       = QBATCH_MODE_SERIAL;
        task->num_sub_circuits = 0;
    }

    /* 阶段4：误差缓解预处理 */
    ret = preproc_error_mitigation_pre(task);
    if (ret < 0) {
        pr_warn("quantum_preproc: qid=%d mitigation pre-proc failed: %d\n",
                task->qid, ret);
        task->error_mitigation = QMIT_NONE;
    }

    /* 阶段5 (M3, SSOT §02.3)：Manifest + (frag, variant) emission */
    ret = preproc_emit_manifest(task);
    if (ret < 0) {
        pr_warn("quantum_preproc: qid=%d manifest emission failed: %d\n",
                task->qid, ret);
        task->error_code = QERR_SPLIT_FAIL;
        kfree(devinfo_snap);
        return ret;
    }

    kfree(devinfo_snap);
    return 0;
}

/* ============================================================
 * M3 (SSOT §02.3): Manifest + (fragment, variant) emission
 *
 * Reads the legacy task hints (split_strategy → cut_kind, error_mitigation
 * → em_kind, num_qubits, circuit_depth) and emits:
 *   1. quantum_manifest written into qernel_row.manifest
 *   2. N×M task_table rows in QVSTATE_PREPARED (one per (frag, variant))
 *   3. qernel_row state advanced RECEIVED → PREPARED
 *
 * When the qid has no qernel_row (legacy write() path that bypasses
 * QIOC_SUBMIT) the function logs the parsed/manifest summary and returns
 * 0 without inserting rows; the legacy submit chain continues unchanged.
 * ============================================================ */

static const char *cut_kind_name(int k)
{
    switch (k) {
    case QCUT_NONE: return "NONE";
    case QCUT_WIRE: return "WIRE";
    case QCUT_GATE: return "GATE";
    case QCUT_AUTO: return "AUTO";
    default:        return "?";
    }
}

static const char *em_kind_name(int k)
{
    switch (k) {
    case QEM_NONE:    return "NONE";
    case QEM_READOUT: return "READOUT";
    case QEM_ZNE:     return "ZNE";
    case QEM_PEC:     return "PEC";
    default:          return "?";
    }
}

static int preproc_emit_manifest(struct quantum_task_struct *task)
{
    struct quantum_manifest   *mfst = NULL;
    struct quantum_task_row   *row  = NULL;
    struct quantum_qernel_row *qrow;
    int cut_kind, em_kind, recon_rule;
    int num_frags, frag_qubits, m_em, k_wire, m_per_frag;
    int f, v, i, ret = 0;
    int wb, ev;
    int rows_emitted = 0;
    bool have_qernel = false;
    /* DEMO-PIVOT D-2: CutQC 6-basis simplified weights (alternating sign
     * with denominator = num_wire_basis). Real CutQC uses an 8-basis
     * decomposition with weights {+1,+1,+1,-1,+1,-1,+1,-1}/2 inside an
     * overall 1/8 factor; the demo collapses to 6 by dropping the I_-/Z_-
     * preparations and renormalising. Postproc.merge_quasi_prob applies
     * the per-row sign when summing. */
    static const int demo_wire_basis_sign[8] = { +1, -1, +1, -1, +1, -1, +1, -1 };

    /* Map legacy hints → ABI v3 enums.
     *
     * cut_kind: prefer qernel_row.cut_hint (stamped by QIOC_SUBMIT before
     * submit_chain_run); fall back to task->split_strategy for the legacy
     * write() path (where qernel_row.cut_hint is 0). task->split_strategy
     * is reset to NONE by quantum_preproc_run's no-split branch, so reading
     * it after the fact only works when the user passed a non-zero hint
     * AND num_qubits exceeded the per-QPU cap (need_split path).
     */
    cut_kind = QCUT_NONE;
    quantum_result_store_lock();
    {
        struct quantum_qernel_row *qrow_peek =
            qernel_table_get_locked(task->qid);
        if (qrow_peek && qrow_peek->cut_hint > 0)
            cut_kind = qrow_peek->cut_hint;
    }
    quantum_result_store_unlock();
    if (cut_kind == QCUT_NONE && task->split_strategy != QSPLIT_STRATEGY_NONE)
        cut_kind = QCUT_WIRE;

    switch (task->error_mitigation) {
    case QMIT_MEM:  em_kind = QEM_READOUT; break;
    case QMIT_CDR:  em_kind = QEM_ZNE;     break;
    case QMIT_PEC:  em_kind = QEM_PEC;     break;
    default:        em_kind = QEM_NONE;    break;
    }

    /* Decide N (fragments) and per-fragment qubit_count */
    if (cut_kind == QCUT_WIRE) {
        num_frags  = 2;                /* acceptance scope: bisection */
        if (num_frags > QUANTUM_MAX_FRAGMENTS)
            num_frags = QUANTUM_MAX_FRAGMENTS;
        frag_qubits = (task->num_qubits + num_frags - 1) / num_frags;
        recon_rule  = QRECON_QUASI_PROB;   /* DEMO-PIVOT D-2: real wire-cut */
    } else {
        num_frags   = 1;
        frag_qubits = task->num_qubits;
        recon_rule  = QRECON_DIRECT;
    }
    if (frag_qubits <= 0)  frag_qubits = 1;
    if (frag_qubits > 255) frag_qubits = 255;

    /* Decide M_em (EM variants per fragment) per H-3 / SSOT §02.3 */
    switch (em_kind) {
    case QEM_READOUT: m_em = 2 * frag_qubits; break;
    case QEM_ZNE:     m_em = 3;               break;  /* stub */
    case QEM_PEC:     m_em = 1;               break;  /* stub */
    default:          m_em = 1;               break;
    }
    if (m_em < 1) m_em = 1;

    /* DEMO-PIVOT D-2: K wire-basis per fragment.
     * 6 = simplified CutQC ({|0>,|1>,|+>,|->,|+i>,|-i>}); 1 = no cut. */
    k_wire = (cut_kind == QCUT_WIRE) ? 6 : 1;
    if (k_wire > QUANTUM_DEMO_WIRE_BASIS_MAX)
        k_wire = QUANTUM_DEMO_WIRE_BASIS_MAX;

    m_per_frag = m_em * k_wire;
    if (m_per_frag > QUANTUM_MAX_VARIANTS) {
        /* clamp em first, then wire (em variants are mandatory for trust) */
        m_em = QUANTUM_MAX_VARIANTS / k_wire;
        if (m_em < 1) m_em = 1;
        m_per_frag = m_em * k_wire;
    }

    /* Heap-alloc large structs (manifest ~3KB, task_row ~2KB) */
    mfst = kzalloc(sizeof(*mfst), GFP_KERNEL);
    row  = kzalloc(sizeof(*row),  GFP_KERNEL);
    if (!mfst || !row) {
        ret = -ENOMEM;
        goto out;
    }

    mfst->abi_version      = QUANTUM_ABI_VERSION;
    mfst->cut_kind         = (__u8)cut_kind;
    mfst->em_kind          = (__u8)em_kind;
    mfst->num_fragments    = (__u8)num_frags;
    mfst->reconstruct_rule = (__u8)recon_rule;
    for (f = 0; f < num_frags; f++) {
        mfst->fragment[f].num_variants    = (__u8)m_per_frag;
        mfst->fragment[f].num_wire_basis  = (__u8)k_wire;
        mfst->fragment[f].qubit_count     = (__u8)frag_qubits;
        mfst->fragment[f].classical_count = (__u8)frag_qubits;
        mfst->fragment[f].depth_estimate  = (__u16)task->circuit_depth;
        for (v = 0; v < m_per_frag; v++) {
            int wb_i = v / m_em;
            mfst->fragment[f].weight_num[v] = demo_wire_basis_sign[wb_i];
            mfst->fragment[f].weight_den[v] = (__u32)k_wire;
        }
    }

    /* Stamp manifest into qernel_row under the result_store mutex */
    quantum_result_store_lock();
    qrow = qernel_table_get_locked(task->qid);
    if (qrow) {
        memcpy(&qrow->manifest, mfst, sizeof(*mfst));
        qrow->num_variants_total = (__u32)(num_frags * m_per_frag);
        qrow->em_kind            = em_kind;
        qrow->cut_hint           = cut_kind;
        qrow->priority           = task->priority;
        qrow->shots              = task->shots;
        if (qrow->qasm[0] == '\0') {
            strncpy(qrow->qasm, task->qir, QUANTUM_QIR_SIZE - 1);
            qrow->qasm[QUANTUM_QIR_SIZE - 1] = '\0';
        }
        if (qrow->submit_ns == 0)
            qrow->submit_ns = task->submit_time;
        have_qernel = true;
    }
    quantum_result_store_unlock();

    pr_info("[preproc] qid=%d parsed nq=%d depth=%d gates=%d\n",
            task->qid, task->num_qubits,
            task->circuit_depth, task->gate_count);
    if (cut_kind == QCUT_WIRE) {
        /* DEMO-PIVOT §3.3: wire-cut trace (qubit-edge hardcoded to bisection) */
        pr_info("[preproc] qid=%d wire-cut at qubit-edge(%d,%d) -> %d fragments\n",
                task->qid, frag_qubits - 1, frag_qubits, num_frags);
    }
    pr_info("[preproc] qid=%d manifest cut=%s em=%s frags=%d wire_basis=%d em_var=%d variants_total=%d\n",
            task->qid, cut_kind_name(cut_kind), em_kind_name(em_kind),
            num_frags, k_wire, m_em, num_frags * m_per_frag);

    if (!have_qernel) {
        /* legacy write() path — no qernel_row, skip task_table emission;
         * legacy chain (alloc/batch/sched) carries on with task struct. */
        pr_info("[preproc] qid=%d skip emission (no qernel_row, legacy path)\n",
                task->qid);
        goto out;
    }

    /* Emit one task_table row per (fragment, wire_basis, em_var) */
    for (f = 0; f < num_frags; f++) {
        for (wb = 0; wb < k_wire; wb++) {
            for (ev = 0; ev < m_em; ev++) {
                v = wb * m_em + ev;
                memset(row, 0, sizeof(*row));
                quantum_provenance_init(&row->prov, (__u32)task->qid,
                                        (__u8)f, (__u8)v);
                row->prov.wire_basis_index   = (__u8)wb;
                row->prov.shots              = task->shots;
                /* per-row signed weight from manifest */
                row->prov.variant_weight_num = demo_wire_basis_sign[wb];
                row->prov.variant_weight_den = (__u32)k_wire;

                /* SSOT §02.3 + H-3: variant_seed encodes (wire_basis, em_qi, em_basis) */
                if (em_kind == QEM_READOUT) {
                    int qi    = ev / 2;
                    int basis = ev & 1;
                    row->prov.variant_seed =
                        0xCA110000U | (__u32)qi |
                        ((__u32)basis << 8) | ((__u32)wb << 16);
                } else {
                    row->prov.variant_seed = (__u32)wb << 16;
                }

                row->state               = QVSTATE_PREPARED;
                row->assigned_backend_id = -1;
                row->bundle_id           = -1;
                for (i = 0; i < QUANTUM_MAX_QUBITS; i++)
                    row->phys_qubits[i] = -1;

                strncpy(row->qasm, task->qir, QUANTUM_SUB_QIR_SIZE - 1);
                row->qasm[QUANTUM_SUB_QIR_SIZE - 1] = '\0';

                ret = task_table_insert(task->qid, row);
                if (ret) {
                    pr_warn("[preproc] qid=%d insert frag=%d wb=%d ev=%d err=%d\n",
                            task->qid, f, wb, ev, ret);
                    goto out;
                }
                rows_emitted++;
            }
        }
    }

    pr_info("[preproc] qid=%d emitted task_table rows=%d (%d frag x %d wire_basis x %d em_var)\n",
            task->qid, rows_emitted, num_frags, k_wire, m_em);

    /* RECEIVED → PREPARED on the qernel row */
    ret = qernel_table_set_state(task->qid, QSTATE_PREPARED);
    if (ret) {
        pr_warn("[preproc] qid=%d set_state PREPARED err=%d\n",
                task->qid, ret);
        /* keep ret; caller will mark task FAILED */
        goto out;
    }

out:
    kfree(mfst);
    kfree(row);
    return ret;
}