#ifndef QUANTUM_TYPES_H
#define QUANTUM_TYPES_H

#include <linux/types.h>
#include <linux/list.h>

/* ============================================================
 * 第一部分：系统常量
 * ============================================================ */

#define QUANTUM_DEV_NAME            "quantum"
#define QUANTUM_MAX_QUBITS          64
#define QUANTUM_MAX_TASKS           256
#define QUANTUM_QIR_SIZE            4096
#define QUANTUM_MAX_OUTCOMES        32
#define QUANTUM_KEY_LEN             192
#define QUANTUM_MAX_BACKENDS        8
#define QUANTUM_MAX_SUB_CIRCUITS    8
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

/* ============================================================
 * 第三部分：状态码与类型码
 * ============================================================ */

/* 任务状态 */
#define QTASK_STATE_UNKNOWN         0
#define QTASK_STATE_RECEIVED        1
#define QTASK_STATE_QUEUED          2
#define QTASK_STATE_RUNNING         3
#define QTASK_STATE_SUCCESS         4
#define QTASK_STATE_FAILED          5
#define QTASK_STATE_CANCELLED       6
#define QTASK_STATE_MERGING         7

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
 * 第四部分：ioctl命令字
 * ============================================================ */

#define QIOC_MAGIC      'Q'
#define QIOC_SUBMIT     _IO(QIOC_MAGIC, 1)
#define QIOC_STATUS     _IO(QIOC_MAGIC, 2)
#define QIOC_RESULT     _IO(QIOC_MAGIC, 3)
#define QIOC_CANCEL     _IO(QIOC_MAGIC, 4)
#define QIOC_RESOURCE   _IO(QIOC_MAGIC, 5)
#define QIOC_FETCH      _IO(QIOC_MAGIC, 6)
#define QIOC_COMMIT     _IO(QIOC_MAGIC, 7)

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

#endif /* QUANTUM_TYPES_H */