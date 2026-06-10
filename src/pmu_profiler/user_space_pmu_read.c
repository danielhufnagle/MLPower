#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdint.h>

/*
 * Reads latest all-core PMU snapshot from /dev/pmu_dc (data_collection module).
 * Struct layout must match pmu_live_snapshot in data_collection/pmu_profiler.c.
 *
 * deltas[core][0..5] = inst_retired, stall_backend, stall_frontend,
 *                      ll_cache_miss_rd, br_mis_pred, dtlb_walk
 * deltas[core][6]    = cycles
 */

#define N_CORES  6
#define N_EVENTS 7  /* 6 PMU events + cycles */

typedef struct {
    int64_t  timestamp_ns;
    uint32_t freq_khz_p0;
    uint32_t freq_khz_p4;
    int64_t  deltas[N_CORES][N_EVENTS];
} pmu_live_snapshot_t;

static const char *event_names[] = {
    "inst_retired", "stall_backend", "stall_frontend",
    "ll_cache_miss_rd", "br_mis_pred", "dtlb_walk", "cycles"
};

int main(void) {
    pmu_live_snapshot_t snap;

    while (1) {
        int fd = open("/dev/pmu_dc", O_RDONLY);
        if (fd < 0) {
            perror("open /dev/pmu_dc failed");
            return -1;
        }

        ssize_t n = read(fd, &snap, sizeof(snap));
        close(fd);

        if (n != sizeof(snap)) {
            fprintf(stderr, "short read: %zd bytes (expected %zu)\n",
                    n, sizeof(snap));
            return -1;
        }

        printf("ts=%ld  p0=%u kHz  p4=%u kHz\n",
               snap.timestamp_ns, snap.freq_khz_p0, snap.freq_khz_p4);

        int core, e;
        for (core = 0; core < N_CORES; core++) {
            printf("  core%d:", core);
            for (e = 0; e < N_EVENTS; e++)
                printf("  %s=%ld", event_names[e], snap.deltas[core][e]);
            printf("\n");
        }
        printf("---\n");

        sleep(1);
    }

    return 0;
}
