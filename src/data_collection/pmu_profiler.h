#ifndef PMU_PROFILER_H
#define PMU_PROFILER_H 

struct pmu_live_snapshot { 
	s64 timestamp_ns; 
	u32 freq_khz_p0; 
	u32 freq_khz_p4; 
	s64 deltas[6][7];
}; 

void get_latest_pmu_snapshot(struct pmu_live_snapshot *out); 

#endif
