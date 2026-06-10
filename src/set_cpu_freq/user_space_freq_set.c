#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include "set_cpu_frequency.h"

int is_valid_enum_freq(unsigned long freq) {
    switch (freq) {
        case CPU_FREQ_115200:
        case CPU_FREQ_192000:
        case CPU_FREQ_268800:
        case CPU_FREQ_345600:
        case CPU_FREQ_422400:
        case CPU_FREQ_499200:
        case CPU_FREQ_576000:
        case CPU_FREQ_652800:
        case CPU_FREQ_729600:
        case CPU_FREQ_806400:
        case CPU_FREQ_883200:
        case CPU_FREQ_960000:
        case CPU_FREQ_1036800:
        case CPU_FREQ_1113600:
        case CPU_FREQ_1190400:
        case CPU_FREQ_1267200:
        case CPU_FREQ_1344000:
        case CPU_FREQ_1420800:
        case CPU_FREQ_1497600:
        case CPU_FREQ_1574400:
        case CPU_FREQ_1651200:
        case CPU_FREQ_1728000:
            return 1;
        default:
            return 0;
    }
}

int main(int argc, char *argv[]) {
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <frequency_in_kHz>\n", argv[0]);
        return -1;
    }

    unsigned long target_freq = strtoul(argv[1], NULL, 10);
    
    if (!is_valid_enum_freq(target_freq)) {
        fprintf(stderr, "Error: Frequency %lu kHz is not one of the allowed hardware supported enums!\n", target_freq);
        return -1;
    }

    int fd = open("/dev/set_cpu_frequency_novel", O_WRONLY);
    if (fd < 0) {
        perror("open failed");
        return -1;
    }

    printf("Calling ioctl to set CPU frequency to %lu kHz...\n", target_freq);
    if (ioctl(fd, SET_FREQ_CMD, &target_freq) < 0) {
        perror("ioctl failed");
        close(fd);
        return -1;
    }

    printf("Successfully sent ioctl command to set CPU frequency!\n");

    close(fd);
    return 0;
}
