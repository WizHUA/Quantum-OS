#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/string.h>

#include "quantum_types.h"

/* 前向声明：calib数据写入接口 */
void quantum_alloc_update_qubit(int qubit_id, int available,
                                 int t1_us_x10, int t2_us_x10,
                                 int r_fid_x1000, int g1_fid_x1000,
                                 int g2_fid_x1000);
void quantum_alloc_set_backend_state(int backend_id, int state);

/* ============================================================
 * 内部：误差缓解算法
 * ============================================================ */

static int mit_none(struct quantum_task_struct *task)
{
    return 0;
}

/*
 * mit_mem —— 测量误差缓解（里程碑5）
 *
 * 使用混淆矩阵对测量结果做矩阵求逆修正
 */
static int mit_mem(struct quantum_task_struct *task)
{
    /*
     * 里程碑5实现提示：
     * for each sub with sub->mit_data != NULL:
     *   confusion_matrix = (struct confusion_matrix *)sub->mit_data
     *   使用 readout_fidelity 构建2^n × 2^n 混淆矩阵
     *   对 sub->result.counts[] 做伪逆修正
     *   kfree(sub->mit_data); sub->mit_data = NULL
     * 注意：内核不支持浮点，需使用整数近似（×1000缩放）
     */
    pr_warn_once("quantum_postproc: mit_mem not implemented\n");
    return 0;
}

static int mit_cdr(struct quantum_task_struct *task)
{
    pr_warn_once("quantum_postproc: mit_cdr not implemented\n");
    return 0;
}

static int mit_pec(struct quantum_task_struct *task)
{
    pr_warn_once("quantum_postproc: mit_pec not implemented\n");
    return 0;
}

typedef int (*quantum_mit_fn)(struct quantum_task_struct *task);

static const quantum_mit_fn mit_strategy_table[] = {
    [QMIT_NONE] = mit_none,
    [QMIT_MEM]  = mit_mem,
    [QMIT_CDR]  = mit_cdr,
    [QMIT_PEC]  = mit_pec,
};

#define MIT_TABLE_MAX \
    (sizeof(mit_strategy_table) / sizeof(mit_strategy_table[0]))

/* ============================================================
 * 内部：结果合并算法
 * ============================================================ */

/*
 * merge_direct —— 直接pass（单线路任务，不需要合并）
 * 将 sub_circuits[0].result 或 task->result复制到最终结果
 */
static int merge_direct(struct quantum_task_struct *task)
{
    /* need_split=0：result已由sched_commit直接写入task->result */
    return 0;
}

/*
 * merge_tensor —— 张量积合并（当前实现，对应SPACE_NAIVE切分）
 *
 * 对应关系：QSPLIT_STRATEGY_SPACE_NAIVE → QMERGE_STRATEGY_TENSOR
 *
 * 算法：将各子线路的测量结果做笛卡尔积
 * 例：sub[0]={|0>:512,|1>:488}, sub[1]={|0>:500,|1>:500}
 *     → merged = {|00>:256,|01>:256,|10>:244,|11>:244}
 *
 * 当前简化实现：两两张量积，处理截断至QUANTUM_MAX_OUTCOMES
 */
static int merge_tensor(struct quantum_task_struct *task)
{
    /* 所有变量声明移到函数顶部（C90要求），且使用堆分配避免栈帧过大 */
    struct quantum_result *cur;
    struct quantum_result *merged;
    int n, i, j, k;
    int ret = 0;

    n = task->num_sub_circuits;
    if (n <= 0) return 0;

    if (n == 1) {
        memcpy(&task->result, &task->sub_circuits[0].result,
               sizeof(task->result));
        return 0;
    }

    /* 堆分配两个工作缓冲区，避免栈帧超限（每个约6KB）*/
    cur = kzalloc(sizeof(*cur), GFP_KERNEL);
    if (!cur)
        return -ENOMEM;

    merged = kzalloc(sizeof(*merged), GFP_KERNEL);
    if (!merged) {
        kfree(cur);
        return -ENOMEM;
    }

    /* 初始化：从 sub[0] 开始 */
    memcpy(cur, &task->sub_circuits[0].result, sizeof(*cur));

    for (i = 1; i < n; i++) {
        struct quantum_result *next = &task->sub_circuits[i].result;
        int merged_count = 0;

        memset(merged, 0, sizeof(*merged));
        merged->shots = cur->shots;

        for (j = 0; j < cur->num_outcomes &&
             merged_count < QUANTUM_MAX_OUTCOMES; j++) {
            for (k = 0; k < next->num_outcomes &&
                 merged_count < QUANTUM_MAX_OUTCOMES; k++) {
                int len_j = strnlen(cur->keys[j], QUANTUM_KEY_LEN);
                int len_k = strnlen(next->keys[k], QUANTUM_KEY_LEN);
                int total_len = len_j + len_k;

                if (total_len >= QUANTUM_KEY_LEN)
                    total_len = QUANTUM_KEY_LEN - 1;

                memcpy(merged->keys[merged_count],
                       cur->keys[j], len_j);
                memcpy(merged->keys[merged_count] + len_j,
                       next->keys[k], total_len - len_j);
                merged->keys[merged_count][total_len] = '\0';

                /*
                 * 概率相乘（整数近似）：
                 * count_merged = count_j * count_k / shots
                 */
                merged->counts[merged_count] =
                    (cur->counts[j] * next->counts[k]) / cur->shots;
                if (merged->counts[merged_count] < 1 &&
                    cur->counts[j] > 0 && next->counts[k] > 0)
                    merged->counts[merged_count] = 1;

                merged_count++;
            }
        }

        merged->num_outcomes = merged_count;
        memcpy(cur, merged, sizeof(*cur));
    }

    memcpy(&task->result, cur, sizeof(task->result));

    kfree(merged);
    kfree(cur);
    return ret;
}

/*
 * merge_weighted —— 准概率加权合并（里程碑6，对应SPACE_PROB切分）
 *
 * 使用 sub->weight_num / sub->weight_den 做加权线性组合
 */
static int merge_weighted(struct quantum_task_struct *task)
{
    pr_warn_once("quantum_postproc: merge_weighted not implemented, "
                 "falling back to tensor\n");
    return merge_tensor(task);
}

/*
 * merge_mlft —— 最大似然片段层析（里程碑6，对应TIME/TOPO_AWARE切分）
 *
 * 1. 各子线路结果 → 密度算子块
 * 2. 子线路间量子连接 → 张量网络边
 * 3. 收缩张量网络 → 联合概率分布
 * 4. 最大似然优化修正
 */
static int merge_mlft(struct quantum_task_struct *task)
{
    pr_warn_once("quantum_postproc: merge_mlft not implemented, "
                 "falling back to tensor\n");
    return merge_tensor(task);
}

typedef int (*quantum_merge_fn)(struct quantum_task_struct *task);

static const quantum_merge_fn merge_strategy_table[] = {
    [QMERGE_STRATEGY_DIRECT]   = merge_direct,
    [QMERGE_STRATEGY_TENSOR]   = merge_tensor,
    [QMERGE_STRATEGY_WEIGHTED] = merge_weighted,
    [QMERGE_STRATEGY_MLFT]     = merge_mlft,
};

#define MERGE_TABLE_MAX \
    (sizeof(merge_strategy_table) / sizeof(merge_strategy_table[0]))

/* ============================================================
 * 内部：校准任务处理
 * ============================================================ */

/*
 * postproc_handle_calib —— 解析校准结果，更新DevInfo
 *
 * 里程碑6实装：
 *   解析 task->result 中的校准测量数据
 *   提取 T1/T2/readout_fidelity/gate_fidelity
 *   调用 quantum_alloc_update_qubit() 更新DevInfo
 *   恢复后端状态为IDLE
 */
static void postproc_handle_calib(struct quantum_task_struct *task)
{
    /*
     * 里程碑6实现提示：
     * int backend_id = task->assigned_backend_id
     * for each qubit in backend:
     *   parse T1/T2 from task->result（标定线路的测量结果）
     *   quantum_alloc_update_qubit(qubit_id, available=1,
     *                              t1, t2, r_fid, g1_fid, g2_fid)
     * quantum_alloc_set_backend_state(backend_id, QBACKEND_STATE_IDLE)
     */
    pr_info("quantum_postproc: calib task qid=%d done, "
            "DevInfo update stub (milestone 6)\n", task->qid);

    if (task->assigned_backend_id >= 0)
        quantum_alloc_set_backend_state(task->assigned_backend_id,
                                         QBACKEND_STATE_IDLE);

    task->state = QTASK_STATE_SUCCESS;
}

/* ============================================================
 * 对外接口
 * ============================================================ */

/*
 * quantum_postproc_run —— 后处理模块唯一对外接口
 *
 * 执行顺序（机制层固定）：
 *   1. 任务类型判断：CALIB任务 → postproc_handle_calib，结束
 *   2. 误差缓解：对所有子线路（或整体任务）执行mit_xxx
 *   3. 结果合并：按merge_strategy路由，将sub_circuits[].result合并为task->result
 */
int quantum_postproc_run(struct quantum_task_struct *task)
{
    int mit_level, merge_strat;
    quantum_mit_fn   mit_fn;
    quantum_merge_fn merge_fn;
    int ret;

    /* 校准任务特殊处理 */
    if (task->task_type == QTASK_TYPE_CALIB) {
        postproc_handle_calib(task);
        return 0;
    }

    /* 误差缓解 */
    mit_level = task->error_mitigation;
    if (mit_level < 0 || mit_level >= (int)MIT_TABLE_MAX ||
        !mit_strategy_table[mit_level])
        mit_level = QMIT_NONE;

    mit_fn = mit_strategy_table[mit_level];
    ret = mit_fn(task);
    if (ret < 0) {
        pr_warn("quantum_postproc: qid=%d mitigation failed: %d, continuing\n",
                task->qid, ret);
        /* 非致命：继续合并 */
    }

    /* 结果合并 */
    merge_strat = task->need_split ? task->merge_strategy : QMERGE_STRATEGY_DIRECT;

    if (merge_strat < 0 || merge_strat >= (int)MERGE_TABLE_MAX ||
        !merge_strategy_table[merge_strat])
        merge_strat = QMERGE_STRATEGY_DIRECT;

    merge_fn = merge_strategy_table[merge_strat];
    ret = merge_fn(task);
    if (ret < 0) {
        pr_err("quantum_postproc: qid=%d merge failed: %d\n",
               task->qid, ret);
        task->error_code = QERR_MERGE_FAIL;
        strncpy(task->error_info, "result merging failed",
                sizeof(task->error_info) - 1);
        task->state = QTASK_STATE_FAILED;
        return ret;
    }

    /* 修正result.shots（以task->shots为准） */
    task->result.shots = task->shots;

    task->state = (task->result.error_code == QERR_OK) ?
                  QTASK_STATE_SUCCESS : QTASK_STATE_FAILED;

    pr_debug("quantum_postproc: qid=%d done state=%d outcomes=%d\n",
             task->qid, task->state, task->result.num_outcomes);
    return 0;
}