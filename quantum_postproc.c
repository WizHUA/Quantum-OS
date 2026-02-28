#include <linux/kernel.h>
#include <linux/string.h>
#include "quantum_types.h"

static int merge_tensor_two(const struct quantum_result *r0,
                             const struct quantum_result *r1,
                             struct quantum_result *out,
                             int parent_qid, int parent_shots)
{
    int i, j, idx = 0;

    out->qid          = parent_qid;
    out->shots        = parent_shots;
    out->num_outcomes = 0;
    out->error_code   = 0;

    for (i = 0; i < r0->num_outcomes && idx < QUANTUM_MAX_OUTCOMES; i++) {
        for (j = 0; j < r1->num_outcomes && idx < QUANTUM_MAX_OUTCOMES; j++) {
            /* 拼接 key：r0->keys[i] + r1->keys[j] */
            int len0 = strnlen(r0->keys[i], sizeof(r0->keys[i]));
            int len1 = strnlen(r1->keys[j], sizeof(r1->keys[j]));
            int total_len = len0 + len1;

            if (total_len >= (int)sizeof(out->keys[idx]))
                total_len = (int)sizeof(out->keys[idx]) - 1;

            memcpy(out->keys[idx], r0->keys[i], len0);
            memcpy(out->keys[idx] + len0, r1->keys[j],
                   total_len - len0);
            out->keys[idx][total_len] = '\0';

            /*
             * tensor 积概率：p(i,j) = p(i) * p(j)
             * count(i,j) = shots * p(i) * p(j)
             *            = shots * count(i)/shots0 * count(j)/shots1
             *
             * 用 64 位避免溢出，四舍五入减少精度损失
             */
            out->counts[idx] = (int)(
                ((long long)r0->counts[i] * r1->counts[j]
                 * parent_shots
                 + (long long)(r0->shots > 0 ? r0->shots : 1)
                   * (r1->shots > 0 ? r1->shots : 1) / 2)
                / ((long long)(r0->shots > 0 ? r0->shots : 1)
                               * (r1->shots > 0 ? r1->shots : 1))
            );
            idx++;
        }
    }
    out->num_outcomes = idx;
    return 0;
}

/*
 * 使用 static 局部变量避免栈帧过大
 * quantum_postproc_run 由 workqueue 串行调用，无并发问题
 */
static struct quantum_result s_merge_tmp;
static struct quantum_result s_merge_next;

static int merge_sub_results(struct quantum_task_struct *task)
{
    int i, ret = 0;

    if (task->num_sub_circuits == 0)
        return 0;

    if (task->num_sub_circuits == 1) {
        memcpy(&task->result, &task->sub_circuits[0].result,
               sizeof(struct quantum_result));
        task->result.qid = task->qid;
        return 0;
    }

    switch (task->merge_strategy) {
    case QMERGE_STRATEGY_TENSOR:
        memcpy(&s_merge_tmp, &task->sub_circuits[0].result,
               sizeof(s_merge_tmp));

        for (i = 1; i < task->num_sub_circuits; i++) {
            ret = merge_tensor_two(&s_merge_tmp,
                                   &task->sub_circuits[i].result,
                                   &s_merge_next,
                                   task->qid, task->shots);
            if (ret)
                return ret;
            memcpy(&s_merge_tmp, &s_merge_next, sizeof(s_merge_tmp));
        }
        memcpy(&task->result, &s_merge_tmp, sizeof(task->result));
        break;

    /*
     * 里程碑6：
     * case QMERGE_STRATEGY_WEIGHTED:
     *     break;
     * case QMERGE_STRATEGY_PIPELINE:
     *     break;
     */

    default:
        memcpy(&task->result, &task->sub_circuits[0].result,
               sizeof(struct quantum_result));
        task->result.qid = task->qid;
        break;
    }

    task->result.qid = task->qid;
    printk(KERN_INFO "QuantumOS: [postproc] qid=%d merged %d "
           "sub-circuits strategy=%d → %d outcomes\n",
           task->qid, task->num_sub_circuits,
           task->merge_strategy, task->result.num_outcomes);
    return ret;
}

int quantum_postproc_run(struct quantum_task_struct *task)
{
    struct quantum_result *r = &task->result;
    int i, total, ret;
    int pct_int, pct_frac;

    total = 0;
    ret   = 0;

    if (task->need_split && task->num_sub_circuits > 0) {
        printk(KERN_INFO "QuantumOS: [postproc] qid=%d "
               "merging %d sub-circuits strategy=%d\n",
               task->qid, task->num_sub_circuits,
               task->merge_strategy);
        ret = merge_sub_results(task);
        if (ret) {
            printk(KERN_ERR "QuantumOS: [postproc] qid=%d "
                   "merge failed ret=%d\n", task->qid, ret);
            return ret;
        }
    }

    printk(KERN_INFO "QuantumOS: [postproc] qid=%d "
           "shots=%d outcomes=%d\n",
           task->qid, task->shots, r->num_outcomes);

    for (i = 0; i < r->num_outcomes; i++)
        total += r->counts[i];

    if (total > 0 && total != r->shots) {
        printk(KERN_WARNING "QuantumOS: [postproc] qid=%d "
               "counts sum=%d != shots=%d, correcting\n",
               task->qid, total, r->shots);
        r->shots = total;
    }

    for (i = 0; i < r->num_outcomes; i++) {
        pct_int  = total > 0 ? r->counts[i] * 100  / total : 0;
        pct_frac = total > 0 ? r->counts[i] * 1000 / total % 10 : 0;
        printk(KERN_INFO "QuantumOS: [postproc] qid=%d "
               "|%s>=%d (%d.%d%%)\n",
               task->qid, r->keys[i], r->counts[i],
               pct_int, pct_frac);
    }

    return 0;
}