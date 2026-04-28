#ifndef QUANTUM_TYPES_H
#define QUANTUM_TYPES_H

#include <linux/types.h>
#include <linux/list.h>

/* ============================================================
 * 第一部分：系统常量
 * ============================================================ */

#define QUANTUM_DEV_NAME            "quantum"
#define QUANTUM_ABI_VERSION         3   /* restart-phase ABI; bump from v2 */
#define QUANTUM_MAX_QUBITS          64
#define QUANTUM_MAX_TASKS           256
#define QUANTUM_MAX_QERNELS         256 /* qernel_table size */
#define QUANTUM_QIR_SIZE            4096
#define QUANTUM_MAX_OUTCOMES        64  /* bumped 32 -> 64 for v3 */
#define QUANTUM_KEY_LEN             192
#define QUANTUM_MAX_BACKENDS        8
#define QUANTUM_MAX_SUB_CIRCUITS    8
#define QUANTUM_MAX_FRAGMENTS       QUANTUM_MAX_SUB_CIRCUITS /* alias per restart spec */
#define QUANTUM_MAX_VARIANTS        32  /* per fragment, EM x wire-basis expansion (demo: 4 em x 6 basis = 24) */
#define QUANTUM_DEMO_WIRE_BASIS_MAX 8   /* CutQC standard 8-basis cap; demo uses 6 */
#define QUANTUM_SUB_QIR_SIZE        2048
#define QUANTUM_MAX_TOTAL_QUBITS    (QUANTUM_MAX_QUBITS * QUANTUM_MAX_BACKENDS)
#define QUANTUM_MAX_NEIGHBORS       8

#define QUANTUM_SCHED_INTERVAL_MS   500
#define QUANTUM_EXEC_BASE_NS        1000000ULL
#define QUANTUM_CALIB_INTERVAL_S    1800

/* ============================================================
 * 第二部分：策略枚举
 * ============================================================ */

/* 切分策略 */
#define QSPLIT_STRATEGY_NONE        0
#define QSPLIT_STRATEGY_SPACE_NAIVE 1
#define QSPLIT_STRATEGY_TIME        2
#define QSPLIT_STRATEGY_SPACE_PROB  3
#define QSPLIT_STRATEGY_TOPO_AWARE  4

/* 批处理模式 */
#define QBATCH_MODE_SERIAL          0
#define QBATCH_MODE_PARALLEL        1
#define QBATCH_MODE_PIPELINE        2

/* 合并策略（与切分一一对应） */
#define QMERGE_STRATEGY_DIRECT      0
#define QMERGE_STRATEGY_TENSOR      1
#define QMERGE_STRATEGY_WEIGHTED    2
#define QMERGE_STRATEGY_MLFT        3

/* 分配策略 */
#define QALLOC_STRATEGY_FIRST_FIT   0
#define QALLOC_STRATEGY_FIDELITY    1
#define QALLOC_STRATEGY_REGRESSION  2
#define QALLOC_STRATEGY_TOPO        3

/* 调度策略 */
#define QSCHED_STRATEGY_HRRN        0
#define QSCHED_STRATEGY_SJF         1
#define QSCHED_STRATEGY_WEIGHTED    2

/* 误差缓解等级 */
#define QMIT_NONE                   0
#define QMIT_MEM                    1
#define QMIT_CDR                    2
#define QMIT_PEC                    3

/* ABI v3: cut kind (preproc decision) */
#define QCUT_NONE                   0
#define QCUT_WIRE                   1
#define QCUT_GATE                   2
#define QCUT_AUTO                   3   /* preproc decides */

/* ABI v3: error-mitigation kind (Manifest.em_kind) */
#define QEM_NONE                    0
#define QEM_READOUT                 1
#define QEM_ZNE                     2
#define QEM_PEC                     3

/* ABI v3: postproc reconstruct rule (Manifest.reconstruct_rule) */
#define QRECON_DIRECT               0
#define QRECON_TENSOR               1
#define QRECON_QUASI_PROB           2

/* ABI v3: quantum_value.kind */
#define QVAL_COUNTS                 0
#define QVAL_EXPECTATION            1
#define QVAL_DISTRIBUTION           2

/* ============================================================
 * 第三部分：状态码与类型码
 * ============================================================ */

/* 任务状态 (legacy v2 names — kept for in-flight modules) */
#define QTASK_STATE_UNKNOWN         0
#define QTASK_STATE_RECEIVED        1
#define QTASK_STATE_QUEUED          2
#define QTASK_STATE_RUNNING         3
#define QTASK_STATE_SUCCESS         4
#define QTASK_STATE_FAILED          5
#define QTASK_STATE_CANCELLED       6
#define QTASK_STATE_MERGING         7

/* ABI v3: Qernel lifecycle (§01.2) — owned by qernel_table_set_state */
#define QSTATE_UNKNOWN              0
#define QSTATE_RECEIVED             1
#define QSTATE_PREPARED             2
#define QSTATE_ASSIGNED             3
#define QSTATE_BUNDLED              4
#define QSTATE_RUNNING              5
#define QSTATE_EM_COMBINING         6
#define QSTATE_RECONSTRUCTING       7
#define QSTATE_DONE                 8
#define QSTATE_FAILED               9
#define QSTATE_CANCELLED            10

/* ABI v3: per-fragment-variant (task_row) state */
#define QVSTATE_UNKNOWN             0
#define QVSTATE_PREPARED            1
#define QVSTATE_ASSIGNED            2
#define QVSTATE_BUNDLED             3
#define QVSTATE_RUNNING             4
#define QVSTATE_DONE_VAR            5
#define QVSTATE_FAILED_VAR          6
#define QVSTATE_CANCELLED_VAR       7

/*
 * ABI v3: state-to-string helpers — used by every module's pr_info()
 * logging template (§02 modules-spec) and by the upcoming debugfs view.
 * Inline so kernel callers stay header-only and so the strings live with
 * the constants themselves (single source of truth).
 */
static inline const char *quantum_state_to_str(int s)
{
    switch (s) {
    case QSTATE_RECEIVED:       return "RECEIVED";
    case QSTATE_PREPARED:       return "PREPARED";
    case QSTATE_ASSIGNED:       return "ASSIGNED";
    case QSTATE_BUNDLED:        return "BUNDLED";
    case QSTATE_RUNNING:        return "RUNNING";
    case QSTATE_EM_COMBINING:   return "EM_COMBINING";
    case QSTATE_RECONSTRUCTING: return "RECONSTRUCTING";
    case QSTATE_DONE:           return "DONE";
    case QSTATE_FAILED:         return "FAILED";
    case QSTATE_CANCELLED:      return "CANCELLED";
    default:                    return "UNKNOWN";
    }
}

static inline const char *quantum_vstate_to_str(int s)
{
    switch (s) {
    case QVSTATE_PREPARED:      return "PREPARED";
    case QVSTATE_ASSIGNED:      return "ASSIGNED";
    case QVSTATE_BUNDLED:       return "BUNDLED";
    case QVSTATE_RUNNING:       return "RUNNING";
    case QVSTATE_DONE_VAR:      return "DONE";
    case QVSTATE_FAILED_VAR:    return "FAILED";
    case QVSTATE_CANCELLED_VAR: return "CANCELLED";
    default:                    return "UNKNOWN";
    }
}

/* 任务类型 */
#define QTASK_TYPE_NORMAL           0
#define QTASK_TYPE_CALIB            1

/* 后端状态 */
#define QBACKEND_STATE_IDLE         0
#define QBACKEND_STATE_BUSY         1
#define QBACKEND_STATE_CALIBRATING  2
#define QBACKEND_STATE_OFFLINE      3

/* 错误码 */
#define QERR_OK                     0
#define QERR_SYNTAX                 1
#define QERR_QUBIT_EXCEED           2
#define QERR_QUEUE_FULL             3
#define QERR_COMPILE_FAIL           4
#define QERR_NO_RESOURCE            5
#define QERR_BACKEND_FAIL           6
#define QERR_TIMEOUT                7
#define QERR_SPLIT_FAIL             8
#define QERR_MERGE_FAIL             9
#define QERR_CALIB_FAIL             10
#define QERR_UNKNOWN                99

/* ============================================================
 * 第四部分：ioctl命令字（仅 magic；命令字定义在文件尾部以便 _IOWR 使用完整 struct 类型）
 * ============================================================ */

#define QIOC_MAGIC      'Q'

/* ============================================================
 * 第五部分：硬件描述符
 * ============================================================ */

/*
 * 单个物理qubit的校准与拓扑数据
 *
 * 写：仅 quantum_calib.c 通过 quantum_alloc_update_qubit*() 写入
 * 读：其他模块通过 quantum_alloc_get_dev_info() 拍快照后读取
 *
 * 整数化规则（内核不用浮点）：
 *   t1/t2:      μs × 10
 *   fidelity:   × 1000（范围 0~1000）
 */
struct quantum_qubit_info {
    int     qubit_id;       /* 全局编号：backend_id * QUANTUM_MAX_QUBITS + local_id */
    int     backend_id;
    int     local_id;
    int     available;

    /* 校准数据 */
    int     t1_us_x10;
    int     t2_us_x10;
    int     readout_fidelity_x1000;
    int     gate1_fidelity_x1000;
    int     gate2_fidelity_x1000;

    /* 拓扑数据（当前stub，calib实装后填写） */
    int     neighbors[QUANTUM_MAX_NEIGHBORS]; /* 相邻qubit全局编号，-1终止 */
    int     num_neighbors;
    __u64   coupling_map;   /* 连接关系位掩码，暂未使用 */

    __u64   last_update_time;
};

/*
 * 全局硬件描述符，对标经典OS的 cpu_data[]
 * 访问规则：写由calib通过alloc接口，读通过get_dev_info快照
 */
struct quantum_dev_info {
    int                         num_backends;
    int                         total_qubits;
    struct quantum_qubit_info   qubits[QUANTUM_MAX_TOTAL_QUBITS];
    __u64                       last_calibration_time;
    int                         calib_in_progress;
    /* DEMO-PIVOT D-1: per-backend ETA in nanoseconds, written by batch on
     * cluster commit, decremented by sched on cluster done. alloc reads
     * a snapshot to pick the lowest-eta backend that still fits. */
    __u64                       backend_eta_ns[QUANTUM_MAX_BACKENDS];
};

/*
 * 单个QPU后端描述符
 */
struct quantum_backend {
    int     id;
    char    name[32];
    int     total_qubits;
    int     state;          /* QBACKEND_STATE_* */
    int     current_qid;    /* -1=空闲 */
    __u64   last_calibration_time;
    int     fidelity_score; /* 整体保真度 0~1000，calib更新 */
    int     num_qubits_available;
    int     connectivity_type; /* 0=全连接 1=nearest-neighbor 2=heavy-hex */
};

struct quantum_backend_pool {
    struct quantum_backend  backends[QUANTUM_MAX_BACKENDS];
    int                     num_backends;
};

/* ============================================================
 * 第六部分：执行结果
 * ============================================================ */

struct quantum_result {
    int     shots;
    int     num_outcomes;
    char    keys[QUANTUM_MAX_OUTCOMES][QUANTUM_KEY_LEN];
    int     counts[QUANTUM_MAX_OUTCOMES];
    int     error_code;
    char    error_info[128];
};

/* ============================================================
 * 第七部分：子线路描述符
 * ============================================================ */

/*
 * 字段填写职责：
 *   preproc：index/num_qubits/gate_count/circuit_depth/qasm/state/
 *            weight_num/weight_den/dep_sub_id/backend_constraint/qubit_mapping[]
 *   alloc：  phys_qubits[]/fidelity_score
 *   sched：  state（RUNNING）/assigned_backend_id
 *   postproc：mit_data（分配与释放）/result（写入）
 */
struct quantum_sub_circuit {
    /* 基本信息（preproc填写） */
    int     index;
    int     num_qubits;
    int     gate_count;
    int     circuit_depth;
    char    qasm[QUANTUM_SUB_QIR_SIZE];

    /* 调度状态（sched维护） */
    int     state;
    int     assigned_backend_id;

    /* 切分元数据（preproc填写，其他模块只读） */
    int     weight_num;         /* 准概率权重分子，SPACE_NAIVE填1 */
    int     weight_den;         /* 准概率权重分母，SPACE_NAIVE填n */
    int     dep_sub_id;         /* 前序依赖，-1=无 */
    int     backend_constraint; /* 锁定QPU编号，-1=自动 */

    /*
     * 局部qubit → 原始全局qubit 映射表
     * SPACE_NAIVE: qubit_mapping[j] = group*QUANTUM_MAX_QUBITS + j
     * TOPO_AWARE:  由切分算法按连通子图填写
     */
    int     qubit_mapping[QUANTUM_MAX_QUBITS];

    /* 量子页表（alloc填写） */
    int     phys_qubits[QUANTUM_MAX_QUBITS];
    int     fidelity_score;

    /* 误差缓解辅助数据（preproc分配，postproc使用并释放） */
    void   *mit_data;           /* NULL=不需要 */

    /* 执行结果（sched通过commit写入） */
    struct quantum_result result;
};

/* ============================================================
 * 第八部分：任务描述符（核心，对标task_struct）
 * ============================================================ */

/*
 * 字段填写职责（严格，不得违反）：
 *   interface：qid/task_type/priority/qir/shots/error_mitigation/
 *              alloc_strategy/submit_time/state(RECEIVED)
 *   preproc：  num_qubits/circuit_depth/gate_count/need_split/
 *              split_strategy/batch_mode/merge_strategy/
 *              num_sub_circuits/sub_circuits[]
 *   alloc：    assigned_backend_id/phys_qubits[]/fidelity_score
 *   batch：    batch_group_id
 *   sched：    state/start_time/finish_time/estimated_exec_ns/
 *              hrrn_score_int/num_sub_done
 *   postproc： result（合并后最终结果）
 */
struct quantum_task_struct {
    /* 基本标识（interface填写） */
    int     qid;
    int     task_type;          /* QTASK_TYPE_* */
    int     priority;           /* 0~9 */

    /* 线路描述 */
    char    qir[QUANTUM_QIR_SIZE];
    int     num_qubits;
    int     circuit_depth;
    int     gate_count;
    int     shots;
    int     error_mitigation;   /* QMIT_* */
    int     alloc_strategy;     /* QALLOC_STRATEGY_* */

    /* 切分描述（preproc填写） */
    int     need_split;
    int     split_strategy;     /* QSPLIT_STRATEGY_* */
    int     batch_mode;         /* QBATCH_MODE_* */
    int     merge_strategy;     /* QMERGE_STRATEGY_*，与split对应 */
    int     num_sub_circuits;
    int     num_sub_done;       /* sched维护，已完成的子线路数 */
    struct  quantum_sub_circuit sub_circuits[QUANTUM_MAX_SUB_CIRCUITS];

    /* 比特分配（alloc填写，need_split=0时有效） */
    int     assigned_backend_id;
    int     phys_qubits[QUANTUM_MAX_QUBITS];
    int     fidelity_score;

    /* 批处理（batch填写） */
    int     batch_group_id;     /* -1=独立 */

    /* 调度状态（sched维护） */
    int     state;              /* QTASK_STATE_* */
    __u64   submit_time;
    __u64   start_time;
    __u64   finish_time;
    __u64   estimated_exec_ns;
    int     hrrn_score_int;

    struct list_head list;

    /* 最终结果（postproc填写） */
    struct quantum_result result;
    int     error_code;
    char    error_info[128];
};

/* ============================================================
 * 第九部分：ioctl传输结构体
 * ============================================================ */

/* FETCH：内核 → daemon */
struct quantum_fetch_req {
    int     qid;
    int     shots;
    int     num_qubits;
    int     circuit_depth;
    int     error_mitigation;
    char    qasm[QUANTUM_QIR_SIZE]; /* 已剥离配置头的纯QASM */
    int     need_split;
    int     sub_index;
    int     num_sub_circuits;
    int     phys_qubits[QUANTUM_MAX_QUBITS]; /* 量子页表，里程碑6噪声建模使用 */
};

/* COMMIT：daemon → 内核 */
struct quantum_commit_req {
    int     qid;
    int     success;
    int     shots;
    int     num_outcomes;
    char    keys[QUANTUM_MAX_OUTCOMES][QUANTUM_KEY_LEN];
    int     counts[QUANTUM_MAX_OUTCOMES];
    int     error_code;
    char    error_info[128];
    int     need_split;
    int     sub_index;
};

/* STATUS ioctl传输 */
struct quantum_status_req {
    int     qid;
    int     state;
};

/* RESULT ioctl传输 */
struct quantum_result_req {
    int     qid;
    struct  quantum_result result;
};

/* CANCEL ioctl传输 */
struct quantum_cancel_req {
    int     qid;
};

/* ============================================================
 * 第十部分：ABI v3 新增结构（§01 数据模型 + §03 ABI）
 * 与第三/八/九部分的 v2 结构并存，迁移完成后可送接。
 * ============================================================ */

/* ---------- (a) Manifest: preproc 权威切分/EM 计划 ---------- */
struct quantum_manifest {
    __u32   abi_version;
    __u8    cut_kind;          /* QCUT_NONE / QCUT_WIRE / QCUT_GATE */
    __u8    em_kind;           /* QEM_NONE / QEM_READOUT / QEM_ZNE / QEM_PEC */
    __u8    num_fragments;     /* 1..QUANTUM_MAX_FRAGMENTS */
    __u8    reconstruct_rule;  /* QRECON_DIRECT / QRECON_TENSOR / QRECON_QUASI_PROB */

    struct {
        __u8    num_variants;        /* fragments × variants = sub-jobs */
        __s32   weight_num[QUANTUM_MAX_VARIANTS]; /* signed quasi-prob numerator */
        __u32   weight_den[QUANTUM_MAX_VARIANTS]; /* common denominator scaled ×1000 */
        __u8    qubit_count;
        __u8    classical_count;
        __u16   depth_estimate;
        /* DEMO-PIVOT D-2: K wire-basis preparations per fragment (CutQC) */
        __u8    num_wire_basis;
        __u8    _pad_d2[3];
    } fragment[QUANTUM_MAX_FRAGMENTS];
};

/* ---------- (b) QernelResult: postproc 最终产物 ---------- */
struct quantum_value {
    __u8    kind;              /* QVAL_COUNTS / QVAL_EXPECTATION / QVAL_DISTRIBUTION */
    __u32   total_shots;
    __u16   num_outcomes;
    char    keys[QUANTUM_MAX_OUTCOMES][QUANTUM_KEY_LEN];
    __s64   counts_x1000[QUANTUM_MAX_OUTCOMES]; /* allow negative for quasi-prob */
};

struct quantum_stats {
    __u64   submit_ns;
    __u64   first_dispatch_ns;
    __u64   finish_ns;
    __u32   num_fragments;
    __u32   num_variants_total;
    __u32   em_overhead_x1000;       /* ratio of variants/fragment */
    __u32   barrier_wait_ns;
    __u32   reconstruct_ns;
    char    backend_used[QUANTUM_MAX_FRAGMENTS][32]; /* per-fragment */
};

struct quantum_qernel_result {
    int                          qid;
    int                          error_code;
    char                         error_info[128];
    struct quantum_value         value;
    struct quantum_stats         stats;
};

/* ---------- (c) Provenance tag：FETCH 打出，COMMIT 返回 ---------- */
struct quantum_provenance {
    __u32   qid;
    __u8    fragment_index;
    __u8    variant_index;
    /* DEMO-PIVOT D-2: which CutQC wire-basis preparation this row carries
     * (0..num_wire_basis-1). Encoded redundantly with variant_index for
     * postproc convenience: variant_index = wire_basis_index * M_em + em_idx. */
    __u8    wire_basis_index;
    __u8    _pad_d2;
    __u32   variant_seed;
    __s32   variant_weight_num;
    __u32   variant_weight_den;
    __u32   shots;
    char    backend_assigned[32];
};

/*
 * quantum_provenance_init — fill the (qid, frag, variant) primary key
 * portion of a provenance tag and clear the rest. Preproc/alloc/batch
 * stamp the remaining fields (variant_seed, variant_weight_*, shots,
 * backend_assigned) on their own paths. Inline so callers don't pull in
 * quantum_result_store.c just to construct a key.
 */
static inline void quantum_provenance_init(struct quantum_provenance *prov,
                                           __u32 qid, __u8 frag, __u8 var)
{
    if (!prov)
        return;
    prov->qid                = qid;
    prov->fragment_index     = frag;
    prov->variant_index      = var;
    prov->wire_basis_index   = 0;
    prov->variant_seed       = 0;
    prov->variant_weight_num = 1;
    prov->variant_weight_den = 1;
    prov->shots              = 0;
    prov->backend_assigned[0] = '\0';
}

/* ---------- (d) Kernel Data Space rows（§03.4） ---------- */
struct quantum_qernel_row {
    int     qid;
    int     state;                       /* QSTATE_* */
    int     error_code;
    char    error_info[128];
    /* user request */
    int     priority;
    int     shots;
    int     em_kind;
    int     cut_hint;
    /* preproc output */
    struct quantum_manifest manifest;
    /* aggregated bookkeeping */
    __u32   num_variants_total;
    __u32   num_variants_done;
    __u32   num_fragments_done;
    /* timings */
    __u64   submit_ns;
    __u64   first_dispatch_ns;
    __u64   barrier_ready_ns;
    __u64   finish_ns;
    /* final */
    struct quantum_qernel_result result;
    /* control */
    int     cancel_requested;
    /* original QASM (debug + replay) */
    char    qasm[QUANTUM_QIR_SIZE];
};

struct quantum_task_row {
    struct quantum_provenance prov;      /* qid + frag + var */
    int     state;                       /* QVSTATE_* */
    int     assigned_backend_id;         /* -1 until alloc sets */
    int     phys_qubits[QUANTUM_MAX_QUBITS];
    int     bundle_id;                   /* -1 until batch sets */
    char    qasm[QUANTUM_SUB_QIR_SIZE];
    struct quantum_result result;        /* set on COMMIT */
    __u64   assigned_ns;
    __u64   bundled_ns;
    __u64   dispatched_ns;
    __u64   committed_ns;
};

/* ---------- (e) ABI v3 user-facing ioctl payloads ---------- */
struct quantum_submit_req {
    __u32   abi_version;
    int     priority;            /* 0..9 */
    int     shots;
    int     error_mitigation;    /* QMIT_*  */
    int     alloc_strategy;      /* QALLOC_STRATEGY_* */
    int     cut_hint;            /* QCUT_NONE / QCUT_WIRE / QCUT_AUTO */
    char    qasm[QUANTUM_QIR_SIZE];
    /* OUT */
    int     qid;
};

struct quantum_calib_blob {
    __u32   abi_version;
    __u32   num_backends;
    char    payload[8192]; /* daemon-side device_params.json blob */
};

/* ============================================================
 * 第十一部分：ioctl 命令字（ABI v3，§03.1）
 *
 * Design choice: all cmd words use _IO (size = 0) so that:
 *   1. Several ABI v3 payloads exceed _IOC's 14-bit (16 KiB) size limit
 *      (quantum_dev_info, quantum_calib_blob with 8 KiB payload, etc.).
 *   2. Userspace ABI discipline is enforced by qabi.py struct.pack format
 *      strings + an explicit abi_version field embedded in each request.
 * The 8-bit cmd numbers stay stable across struct-layout changes.
 * ============================================================ */

#define QIOC_SUBMIT     _IO(QIOC_MAGIC, 1) /* payload: struct quantum_submit_req */
#define QIOC_STATUS     _IO(QIOC_MAGIC, 2) /* payload: struct quantum_status_req */
#define QIOC_RESULT     _IO(QIOC_MAGIC, 3) /* payload: struct quantum_result_req */
#define QIOC_CANCEL     _IO(QIOC_MAGIC, 4) /* payload: struct quantum_cancel_req */
#define QIOC_RESOURCE   _IO(QIOC_MAGIC, 5) /* payload: struct quantum_dev_info   */
#define QIOC_FETCH      _IO(QIOC_MAGIC, 6) /* payload: struct quantum_fetch_req  */
#define QIOC_COMMIT     _IO(QIOC_MAGIC, 7) /* payload: struct quantum_commit_req */
#define QIOC_CALIB_LOAD _IO(QIOC_MAGIC, 8) /* payload: struct quantum_calib_blob */

#endif /* QUANTUM_TYPES_H */