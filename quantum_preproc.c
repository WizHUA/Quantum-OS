#include <linux/kernel.h>
#include <linux/string.h>
#include "quantum_types.h"

static const char *skip_spaces_custom(const char *p)
{
    while (*p == ' ' || *p == '\t')
        p++;
    return p;
}

static int parse_qreg_line(const char *line)
{
    const char *p = line + 4;
    const char *bracket;
    int n = 0;

    p = skip_spaces_custom(p);
    bracket = strchr(p, '[');
    if (!bracket)
        return 0;
    bracket++;
    while (*bracket >= '0' && *bracket <= '9')
        n = n * 10 + (*bracket++ - '0');
    return n;
}

static int is_gate_line(const char *line)
{
    const char *p = skip_spaces_custom(line);

    if (*p == '\0' || *p == '/' || *p == '#')   return 0;
    if (strncmp(p, "OPENQASM", 8) == 0)          return 0;
    if (strncmp(p, "include",  7) == 0)          return 0;
    if (strncmp(p, "qreg",     4) == 0)          return 0;
    if (strncmp(p, "creg",     4) == 0)          return 0;
    if (strncmp(p, "barrier",  7) == 0)          return 0;
    return strchr(p, ';') ? 1 : 0;
}

/*
 * 从门操作行提取所有 q[N] 中的 N
 * 返回提取到的 qubit 索引数量
 */
static int extract_qubit_indices(const char *line, int *indices, int max)
{
    const char *p = line;
    int count = 0, n;

    while ((p = strchr(p, '[')) != NULL && count < max) {
        p++;
        n = 0;
        while (*p >= '0' && *p <= '9')
            n = n * 10 + (*p++ - '0');
        indices[count++] = n;
    }
    return count;
}

/*
 * 空间切分：构建第 group 组的子线路 QASM
 *
 * 策略（SPACE_NAIVE）：
 *   - 公共头（OPENQASM/include）：每组都保留
 *   - qreg/creg 声明：全部保留（里程碑6按组精确过滤）
 *   - 门操作：只保留所有操作 qubit 都属于本组的行
 *             跨组的门（如跨组CNOT）被丢弃（SPACE_NAIVE的局限）
 *
 * 里程碑6扩展点：
 *   - SPACE_PROB：把跨组门替换为准概率分解的等效线路集
 *   - 硬件感知：根据 backend_constraint 调整分组方式
 */
static int build_sub_qasm_space(struct quantum_task_struct *task,
                                 int group, int max_qubits,
                                 struct quantum_sub_circuit *sub)
{
    char        line[256];
    const char *p = task->qir;
    const char *end;
    const char *trimmed;
    int         line_len, offset, gate_count;
    int         indices[8], n, i, all_in_group, keep;

    offset     = 0;
    gate_count = 0;

    if (strncmp(p, "OPENQASM", 8) != 0) {
        p = strchr(p, '\n');
        p = p ? p + 1 : task->qir;
    }

    while (*p != '\0') {
        end      = strchr(p, '\n');
        line_len = end ? (int)(end - p) : (int)strlen(p);
        if (!end) end = p + line_len;

        if (line_len >= (int)sizeof(line))
            line_len = (int)sizeof(line) - 1;
        memcpy(line, p, line_len);
        line[line_len] = '\0';

        trimmed = skip_spaces_custom(line);
        keep    = 0;

        if (strncmp(trimmed, "OPENQASM", 8) == 0 ||
            strncmp(trimmed, "include",  7) == 0) {
            keep = 1;
        } else if (strncmp(trimmed, "qreg", 4) == 0 ||
                   strncmp(trimmed, "creg", 4) == 0) {
            keep = 1;
        } else if (is_gate_line(line)) {
            n = extract_qubit_indices(line, indices, 8);
            all_in_group = 1;
            for (i = 0; i < n; i++) {
                if (indices[i] < group * max_qubits ||
                    indices[i] >= (group + 1) * max_qubits) {
                    all_in_group = 0;
                    break;
                }
            }
            if (all_in_group) {
                keep = 1;
                gate_count++;
            }
        }

        if (keep && offset < QUANTUM_SUB_QIR_SIZE - line_len - 2) {
            memcpy(sub->qasm + offset, line, line_len);
            offset += line_len;
            sub->qasm[offset++] = '\n';
        }

        p = (*end == '\n') ? end + 1 : end;
    }

    sub->qasm[offset] = '\0';
    sub->gate_count   = gate_count;
    return 0;
}

/*
 * preproc_split
 * 根据 split_strategy 执行切分
 * 当前实现：SPACE_NAIVE
 * 扩展：在 switch 里加新 case 即可
 */
static int preproc_split(struct quantum_task_struct *task, int max_qubits)
{
    int n, i;
    struct quantum_sub_circuit *sub;

    n = (task->num_qubits + max_qubits - 1) / max_qubits;
    if (n > QUANTUM_MAX_SUB_CIRCUITS) {
        printk(KERN_WARNING "QuantumOS: [preproc] qid=%d "
               "need %d splits exceeds max=%d\n",
               task->qid, n, QUANTUM_MAX_SUB_CIRCUITS);
        return -EINVAL;
    }

    task->split_strategy  = QSPLIT_STRATEGY_SPACE_NAIVE;
    task->batch_mode      = QBATCH_MODE_SERIAL;   /* 里程碑6改为PARALLEL */
    task->merge_strategy  = QMERGE_STRATEGY_TENSOR;
    task->num_sub_circuits = n;
    task->num_sub_done     = 0;

    for (i = 0; i < n; i++) {
        sub = &task->sub_circuits[i];
        memset(sub, 0, sizeof(*sub));

        sub->index               = i;
        sub->num_qubits          = (i < n - 1) ? max_qubits
                                 : task->num_qubits - i * max_qubits;
        sub->circuit_depth       = task->circuit_depth / n;
        sub->state               = QTASK_STATE_QUEUED;
        sub->assigned_backend_id = -1;
        sub->weight_num          = 1;   /* 里程碑6：准概率分解时填真实权重 */
        sub->weight_den          = n;
        sub->dep_sub_id          = -1;  /* 里程碑6：流水线时填依赖编号 */
        sub->backend_constraint  = -1;  /* 里程碑6：硬件感知时填指定QPU */

        switch (task->split_strategy) {
        case QSPLIT_STRATEGY_SPACE_NAIVE:
        default:
            build_sub_qasm_space(task, i, max_qubits, sub);
            break;
        /* 里程碑6在此添加：
         * case QSPLIT_STRATEGY_TIME:
         *     build_sub_qasm_time(task, i, sub); break;
         * case QSPLIT_STRATEGY_SPACE_PROB:
         *     build_sub_qasm_prob(task, i, max_qubits, sub); break;
         */
        }

        printk(KERN_INFO "QuantumOS: [preproc] qid=%d "
               "sub[%d] qubits=%d gates=%d depth=%d\n",
               task->qid, i,
               sub->num_qubits, sub->gate_count, sub->circuit_depth);
    }

    printk(KERN_INFO "QuantumOS: [preproc] qid=%d split into %d "
           "sub-circuits strategy=%d batch=%d merge=%d\n",
           task->qid, n, task->split_strategy,
           task->batch_mode, task->merge_strategy);
    return 0;
}

int quantum_preproc_run(struct quantum_task_struct *task)
{
    char        line[256];
    const char *p = task->qir;
    const char *end;
    int         num_qubits = 0, gate_count = 0, line_len;

    /* 初始化切分字段 */
    task->need_split       = 0;
    task->split_strategy   = QSPLIT_STRATEGY_NONE;
    task->batch_mode       = QBATCH_MODE_SERIAL;
    task->merge_strategy   = QMERGE_STRATEGY_DIRECT;
    task->num_sub_circuits = 0;
    task->num_sub_done     = 0;

    if (strncmp(p, "OPENQASM", 8) != 0) {
        p = strchr(p, '\n');
        p = p ? p + 1 : task->qir;
    }

    while (*p != '\0') {
        end      = strchr(p, '\n');
        line_len = end ? (int)(end - p) : (int)strlen(p);
        if (!end) end = p + line_len;
        if (line_len >= (int)sizeof(line))
            line_len = (int)sizeof(line) - 1;
        memcpy(line, p, line_len);
        line[line_len] = '\0';

        if (strncmp(skip_spaces_custom(line), "qreg", 4) == 0)
            num_qubits += parse_qreg_line(skip_spaces_custom(line));
        if (is_gate_line(line))
            gate_count++;

        p = (*end == '\n') ? end + 1 : end;
    }

    if (num_qubits <= 0) {
        printk(KERN_WARNING "QuantumOS: [preproc] qid=%d "
               "no qreg found, default to 1\n", task->qid);
        num_qubits = 1;
    }

    task->num_qubits    = num_qubits;
    task->gate_count    = gate_count;
    task->circuit_depth = gate_count;

    printk(KERN_INFO "QuantumOS: [preproc] qid=%d "
           "qubits=%d gates=%d depth=%d\n",
           task->qid, task->num_qubits,
           task->gate_count, task->circuit_depth);

    /* 超限检测：触发切分，不返回错误 */
    if (task->num_qubits > QUANTUM_MAX_QUBITS) {
        printk(KERN_WARNING "QuantumOS: [preproc] qid=%d "
               "qubits=%d > max=%d, triggering split\n",
               task->qid, task->num_qubits, QUANTUM_MAX_QUBITS);
        task->need_split = 1;
        return preproc_split(task, QUANTUM_MAX_QUBITS);
    }

    return 0;
}