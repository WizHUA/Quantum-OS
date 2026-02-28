#include <linux/kernel.h>
#include "quantum_types.h"

int quantum_batch_run(struct quantum_task_struct *task)
{
    int i;
    struct quantum_sub_circuit *sub;

    if (!task->need_split) {
        printk(KERN_INFO "QuantumOS: [batch] qid=%d "
               "single circuit, pass through\n", task->qid);
        return 0;
    }

    printk(KERN_INFO "QuantumOS: [batch] qid=%d "
           "%d sub-circuits mode=%d\n",
           task->qid, task->num_sub_circuits, task->batch_mode);

    for (i = 0; i < task->num_sub_circuits; i++) {
        sub = &task->sub_circuits[i];
        printk(KERN_INFO "QuantumOS: [batch] qid=%d sub[%d] "
               "qubits=%d gates=%d dep=%d backend_hint=%d\n",
               task->qid, i,
               sub->num_qubits, sub->gate_count,
               sub->dep_sub_id, sub->backend_constraint);
    }

    switch (task->batch_mode) {
    case QBATCH_MODE_SERIAL:
    default:
        printk(KERN_INFO "QuantumOS: [batch] qid=%d "
               "serial: sched will dispatch sub-circuits one by one\n",
               task->qid);
        break;
    /*
     * 里程碑6：
     * case QBATCH_MODE_PARALLEL:
     *     for each sub: create child_task, sched_enqueue(child_task)
     *     break;
     * case QBATCH_MODE_PIPELINE:
     *     enqueue subs with dep_sub_id==-1 first
     *     break;
     */
    }

    return 0;
}