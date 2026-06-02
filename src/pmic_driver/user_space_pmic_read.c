#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdint.h>

int main() {
    uint8_t rx_buf[14];
    
    int fd = open("/dev/pmic_driver_novel", O_RDONLY);
    if (fd < 0) {
        perror("open failed");
        return -1;
    }

    while(1) {
    ssize_t bytes_read = read(fd, rx_buf, sizeof(rx_buf));
    if (bytes_read < 0) {
        perror("read failed");
        close(fd);
        return -1;
    }

    int16_t ch1_shunt = (int16_t)((rx_buf[0] << 8)  | rx_buf[1]);
    int16_t ch1_bus   = (int16_t)((rx_buf[2] << 8)  | rx_buf[3]);
    int16_t ch2_shunt = (int16_t)((rx_buf[4] << 8)  | rx_buf[5]);
    int16_t ch2_bus   = (int16_t)((rx_buf[6] << 8)  | rx_buf[7]);
    int16_t ch3_shunt = (int16_t)((rx_buf[8] << 8)  | rx_buf[9]);
    int16_t ch3_bus   = (int16_t)((rx_buf[10] << 8) | rx_buf[11]);
    int16_t shunt_sum = (int16_t)((rx_buf[12] << 8) | rx_buf[13]);

    printf("Read %zd bytes\n", bytes_read);
    printf("C1 Shunt: %d uV, Bus: %d mV\n", ch1_shunt, ch1_bus);
    printf("C2 Shunt: %d uV, Bus: %d mV\n", ch2_shunt, ch2_bus);
    printf("C3 Shunt: %d uV, Bus: %d mV\n", ch3_shunt, ch3_bus);
    printf("Shunt Sum: %d uV\n", shunt_sum);
}

    close(fd);
    return 0;
}