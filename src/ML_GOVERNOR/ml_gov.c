#define pr_fmt(fmt) "ml_gov: " fmt

#include <linux/module.h>
#include <linux/kthread.h>
#include <linux/delay.h>
#include <linux/cpufreq.h>
#include <asm/neon.h>

#include "../pmic_driver/pmic_driver.h"
#include "../data_collection/pmu_profiler.h"
#include "../training/mlp_weights.h"

static struct task_struct *worker_task;

static inline float my_clamp(float val, float min_val, float max_val) {
    if (val < min_val) return min_val;
    if (val > max_val) return max_val;
    return val;
}

static void prep_inputs(const struct pmu_live_snapshot *pmu, 
                        const struct pmic_snap_live *pmic, 
                        float *out_buf) 
{
    int c, ev;

    out_buf[0] = (float)pmu->freq_khz_p0;
    out_buf[1] = (float)pmu->freq_khz_p4;

    for (ev = 0; ev < 6; ev++) {
        for (c = 0; c < 6; c++) {
            out_buf[2 + ev * 6 + c] = (float)pmu->deltas[c][ev];
        }
    }
    for (c = 0; c < 6; c++) {
        out_buf[38 + c] = (float)pmu->deltas[c][6];
    }

    for (c = 0; c < 6; c++) {
        float cyc = pmu->deltas[c][6] > 0 ? (float)pmu->deltas[c][6] : 1.0f;
        float inst = pmu->deltas[c][0] > 0 ? (float)pmu->deltas[c][0] : 1.0f;

        out_buf[44 + c] = my_clamp((float)pmu->deltas[c][1] / cyc, 0.0f, 1.0f);
        out_buf[50 + c] = my_clamp((float)pmu->deltas[c][2] / cyc, 0.0f, 1.0f);
        out_buf[56 + c] = my_clamp((float)pmu->deltas[c][0] / cyc, 0.0f, 5.0f);
        out_buf[62 + c] = my_clamp((float)pmu->deltas[c][3] / inst, 0.0f, 0.5f);
        out_buf[68 + c] = my_clamp((float)pmu->deltas[c][4] / inst, 0.0f, 0.5f);
        out_buf[74 + c] = my_clamp((float)pmu->deltas[c][5] / inst, 0.0f, 0.5f);
    }

    out_buf[80] = (float)pmic->cpu_util_avg_pct;
    out_buf[81] = 4.6f; 
    out_buf[82] = (float)pmic->ram_used_mb;
    out_buf[83] = (float)pmic->cpu_temp_c;
    out_buf[84] = (float)pmic->tj_temp_c;
    out_buf[85] = (float)pmic->cpu_gpu_cv_power_mw;
    out_buf[86] = (float)pmic->vdd_in_power_mw;
}

static int get_best_freq_idx(const float *x) 
{
    float h1[MLP_L1_OUT], h2[MLP_L2_OUT], h3[MLP_L3_OUT];
    float max_score = -1e30f;
    int i, j, best_idx = 21; 

    for (i = 0; i < MLP_L1_OUT; i++) {
        float sum = mlp_b1[i];
        for (j = 0; j < MLP_INPUT_DIM; j++) sum += mlp_w1[i * 174 + j] * x[j];
        h1[i] = sum > 0.0f ? sum : 0.0f;
    }

    for (i = 0; i < MLP_L2_OUT; i++) {
        float sum = mlp_b2[i];
        for (j = 0; j < MLP_L1_OUT; j++) sum += mlp_w2[i * MLP_L1_OUT + j] * h1[j];
        h2[i] = sum > 0.0f ? sum : 0.0f;
    }

    for (i = 0; i < MLP_L3_OUT; i++) {
        float sum = mlp_b3[i];
        for (j = 0; j < MLP_L2_OUT; j++) sum += mlp_w3[i * MLP_L2_OUT + j] * h2[j];
        h3[i] = sum > 0.0f ? sum : 0.0f;
    }

    for (i = 0; i < MLP_N_CLASSES; i++) {
        float sum = mlp_b4[i];
        for (j = 0; j < MLP_L3_OUT; j++) sum += mlp_w4[i * MLP_L3_OUT + j] * h3[j];
        if (sum > max_score) {
            max_score = sum;
            best_idx = i;
        }
    }
    return best_idx;
}

static int gov_worker(void *data) 
{
    float prev_inputs[87] = {0};
    float full_state[174] = {0};
    bool is_first_run = true;

    pr_info("started.\n");

    while (!kthread_should_stop()) {
        struct pmu_live_snapshot pmu_data;
        struct pmic_snap_live pmic_data;
        float current_inputs[87];
        int i, freq_idx;
        unsigned int target_khz;
        struct cpufreq_policy *pol;

        msleep_interruptible(16);
        if (kthread_should_stop()) break;

        get_latest_pmu_snapshot(&pmu_data);
        get_live_pmic_data(&pmic_data);

        kernel_neon_begin();
        prep_inputs(&pmu_data, &pmic_data, current_inputs);

        if (is_first_run) {
            memcpy(prev_inputs, current_inputs, sizeof(current_inputs));
            is_first_run = false;
            kernel_neon_end();
            continue;
        }

        for (i = 0; i < 87; i++) {
            full_state[i] = current_inputs[i];
            full_state[i + 87] = current_inputs[i] - prev_inputs[i];
            prev_inputs[i] = current_inputs[i];
        }

        freq_idx = get_best_freq_idx(full_state);
        kernel_neon_end();

        target_khz = mlp_dvfs_steps[freq_idx];

        pol = cpufreq_cpu_get(0);
        if (pol) {
            cpufreq_driver_target(pol, target_khz, CPUFREQ_RELATION_C);
            cpufreq_cpu_put(pol);
        }
    	pol = cpufreq_cpu_get(4);                                                                                                                                                                               
  	if (pol) {                                                                                                                                                                                            	      cpufreq_driver_target(pol, target_khz, CPUFREQ_RELATION_C);
      		cpufreq_cpu_put(pol);                                                                                                                                                          
  	}
    }

    pr_info("stopped.\n");
    return 0;
}

static int __init ml_gov_init(void) 
{
    worker_task = kthread_run(gov_worker, NULL, "ml_gov_thread");
    if (IS_ERR(worker_task)) {
        pr_err("failed to spawn worker thread.\n");
        return PTR_ERR(worker_task);
    }
    return 0;
}

static void __exit ml_gov_exit(void) 
{
    if (worker_task) {
        kthread_stop(worker_task);
    }
}

module_init(ml_gov_init);
module_exit(ml_gov_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("MLPOWER");
MODULE_DESCRIPTION("ML Governor");

