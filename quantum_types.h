#ifndef _QUANTUM_TYPES_H
#define _QUANTUM_TYPES_H

#include <linux/types.h>
#include <linux/ioctl.h>
#include <linux/list.h>

/* ===== 常量定义 ===== */
#define QUANTUM_DEV_NAME            "quantum"
#define QUANTUM_MAX_QUBITS          64
#define QUANTUM_MAX_TASKS           256
#define QUANTUM_QIR_SIZE            4096
#define QUANTUM_MAX_OUTCOMES        32      /* 缩减：256→32，足够演示 */
#define QUANTUM_MAX_BACKENDS        8
#define QUANTUM_MAX_SUB_CIRCUITS    8
#define QUANTUM_SUB_QIR_SIZE        2048

#define QUANTUM_SCHED_INTERVAL_MS   500
#define QUANTUM_EXEC_BASE_NS        1000000ULL

/* ===== 任务状态码 ===== */
#define QTASK_STATE_UNKNOWN    0
#define QTASK_STATE_RECEIVED   1
#define QTASK_STATE_QUEUED     2
#define QTASK_STATE_RUNNING    3
#define QTASK_STATE_SUCCESS    4
#define QTASK_STATE_FAILED     5
#define QTASK_STATE_CANCELLED  6
#define QTASK_STATE_MERGING    7

/* ===== 任务类型 ===== */
#define QTASK_TYPE_NORMAL      0
#define QTASK_TYPE_CALIB       1

/* ===== 错误码 ===== */
#define QERR_OK                0
#define QERR_SYNTAX            1
#define QERR_QUBIT_EXCEED      2
#define QERR_QUEUE_FULL        3
#define QERR_COMPILE_FAIL      4
#define QERR_NO_RESOURCE       5
#define QERR_BACKEND_FAIL      6
#define QERR_TIMEOUT           7
#define QERR_SPLIT_FAIL        8
#define QERR_MERGE_FAIL        9
#define QERR_UNKNOWN           99

/* ===== 后端状态 ===== */
#define QBACKEND_STATE_IDLE        0
#define QBACKEND_STATE_BUSY        1
#define QBACKEND_STATE_CALIBRATING 2
#define QBACKEND_STATE_OFFLINE     3

/* ===== 切分/批处理/合并策略 ===== */
#define QSPLIT_STRATEGY_NONE        0
#define QSPLIT_STRATEGY_SPACE_NAIVE 1
#define QSPLIT_STRATEGY_TIME        2   /* 里程碑6 */
#define QSPLIT_STRATEGY_SPACE_PROB  3   /* 里程碑6 */

#define QBATCH_MODE_SERIAL      0
#define QBATCH_MODE_PARALLEL    1       /* 里程碑6 */
#define QBATCH_MODE_PIPELINE    2       /* 里程碑6 */
#define QBATCH_MODE_DYNAMIC     3       /* 里程碑6 */

#define QMERGE_STRATEGY_DIRECT    0
#define QMERGE_STRATEGY_TENSOR    1
#define QMERGE_STRATEGY_WEIGHTED  2     /* 里程碑6 */
#define QMERGE_STRATEGY_PIPELINE  3     /* 里程碑6 */

/* ===== 结果结构（OUTCOMES缩减后约1.4KB）===== */
struct quantum_result {
    int    qid;
    int    shots;
    int    num_outcomes;
    char   keys[QUANTUM_MAX_OUTCOMES][96];  /* 32*32 = 1024B */
    int    counts[QUANTUM_MAX_OUTCOMES];    /* 32*4  = 128B  */
    int    error_code;
    char   error_info[128];
};
/* sizeof(quantum_result) ≈ 1.4 KB */

/* ===== 子线路描述符（约3.5KB）===== */
struct quantum_sub_circuit {
    int  index;
    int  num_qubits;
    int  gate_count;
    int  circuit_depth;
    char qasm[QUANTUM_SUB_QIR_SIZE];    /* 2048B */

    /* 调度 */
    int  state;
    int  assigned_backend_id;

    /* 扩展接口（里程碑6填写）*/
    int  weight_num;
    int  weight_den;
    int  dep_sub_id;
    int  backend_constraint;

    struct quantum_result result;       /* 1.4KB */
};
/* sizeof(quantum_sub_circuit) ≈ 3.5 KB */

/* ===== 后端描述符 ===== */
struct quantum_backend {
    int    id;
    char   name[32];
    int    total_qubits;
    int    state;
    int    current_qid;
    __u64  last_calibration_time;
};

struct quantum_backend_pool {
    struct quantum_backend backends[QUANTUM_MAX_BACKENDS];
    int    num_backends;
};

/* ===== 任务描述符（内核内部，kmalloc 分配）===== */
struct quantum_task_struct {
    /* 基本标识 */
    int    qid;
    int    task_type;
    int    priority;

    /* 线路描述 */
    char   qir[QUANTUM_QIR_SIZE];       /* 4096B */
    int    num_qubits;
    int    circuit_depth;
    int    gate_count;
    int    shots;
    int    error_mitigation;

    /* 线路切分 */
    int    need_split;
    int    split_strategy;
    int    batch_mode;
    int    merge_strategy;
    int    num_sub_circuits;
    int    num_sub_done;
    struct quantum_sub_circuit
           sub_circuits[QUANTUM_MAX_SUB_CIRCUITS]; /* 8*3.5KB = 28KB */

    /* 资源 */
    int    assigned_backend_id;

    /* 调度 */
    int    state;
    __u64  submit_time;
    __u64  start_time;
    __u64  finish_time;
    __u64  estimated_exec_ns;
    int    hrrn_score_int;

    struct list_head list;

    /* 父任务结果 */
    struct quantum_result result;       /* 1.4KB */
    int    error_code;
    char   error_info[128];
};
/*
 * sizeof(quantum_task_struct) ≈ 38KB
 * 通过 kmalloc 分配，不在栈上，无问题
 */

/* ===================================================================
 * ioctl 传输结构体
 * 原则：只传必要字段，不内嵌大数组
 * =================================================================== */

/* FETCH：内核→daemon，传递待执行的线路 */
struct quantum_fetch_req {
    int  qid;
    int  shots;
    int  num_qubits;
    int  circuit_depth;
    int  error_mitigation;
    char qasm[QUANTUM_QIR_SIZE];    /* 4096B，传当前要执行的线路 */

    /* 子线路信息 */
    int  need_split;
    int  sub_index;                 /* 当前 fetch 的子线路编号 */
    int  num_sub_circuits;
};
/* sizeof ≈ 4.1KB，合理 */

/* COMMIT：daemon→内核，传递执行结果 */
struct quantum_commit_req {
    int  qid;
    int  success;
    int  shots;
    int  num_outcomes;
    char keys[QUANTUM_MAX_OUTCOMES][96];
    int  counts[QUANTUM_MAX_OUTCOMES];
    int  error_code;
    char error_info[128];

    /* 子线路信息 */
    int  need_split;
    int  sub_index;                 /* commit 的是第几条子线路 */
};
/* sizeof ≈ 1.4KB，合理 */

/* STATUS 查询 */
struct quantum_status_req {
    int qid;
    int state;
    int error_code;
    char error_info[128];
};

/* RESULT 查询（用户态取结果）*/
struct quantum_result_req {
    int qid;
    struct quantum_result result;
};

/* ===== ioctl 命令字 ===== */
#define QIOC_MAGIC      'Q'
#define QIOC_SUBMIT     _IO(QIOC_MAGIC, 1)
#define QIOC_STATUS     _IO(QIOC_MAGIC, 2)
#define QIOC_RESULT     _IO(QIOC_MAGIC, 3)
#define QIOC_CANCEL     _IO(QIOC_MAGIC, 4)
#define QIOC_RESOURCE   _IO(QIOC_MAGIC, 5)
#define QIOC_FETCH      _IO(QIOC_MAGIC, 6)
#define QIOC_COMMIT     _IO(QIOC_MAGIC, 7)

#endif /* _QUANTUM_TYPES_H */