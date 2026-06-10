#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <string.h>
#include "pmic_driver.h"

int main(int argc, char *argv[]) {
    if (argc < 3) {
        fprintf(stderr, "Usage: %s <avg|conv> <value_0_to_7>\n", argv[0]);
        fprintf(stderr, "  avg:  0=1, 1=4, 2=16, 3=64, 4=128, 5=256, 6=512, 7=1024 samples\n");
        fprintf(stderr, "  conv: 0=140us, 1=204us, 2=332us, 3=588us, 4=1.1ms, 5=2.116ms, 6=4.156ms, 7=8.244ms\n");
        return -1;
    }

    const char *mode = argv[1];
    unsigned long val = strtoul(argv[2], NULL, 10);

    if (val > 7) {
        fprintf(stderr, "Error: Value must be 0 to 7\n");
        return -1;
    }

    int fd = open("/dev/pmic_driver_novel", O_RDWR);
    if (fd < 0) {
        perror("open failed");
        return -1;
    }

    unsigned long cmd;
    if (strcmp(mode, "avg") == 0) {
        cmd = PMIC_IOC_SET_AVG;
        printf("Setting averaging mode to %lu...\n", val);
    } else if (strcmp(mode, "conv") == 0) {
        cmd = PMIC_IOC_SET_CONV;
        printf("Setting conversion times to %lu...\n", val);
    } else {
        fprintf(stderr, "Error: Mode must be 'avg' or 'conv'\n");
        close(fd);
        return -1;
    }

    if (ioctl(fd, cmd, &val) < 0) {
        perror("ioctl failed");
        close(fd);
        return -1;
    }

    printf("Successfully updated PMIC configuration!\n");

    close(fd);
    return 0;
}
