/* pmic_driver.h - shared declarations for pmic_driver.c */
#ifndef PMIC_DRIVER_H
#define PMIC_DRIVER_H

#include <linux/types.h>
#include <linux/fs.h>
#include <linux/i2c.h>

/* quick snapshot of INA3221 readings */
typedef struct {
    s16 ch1_shunt_uv;      /* ch1 shunt, in uV */
    s16 ch1_bus_mv;        /* ch1 bus, in mV */
    s16 ch2_shunt_uv;      /* ch2 shunt, in uV */
    s16 ch2_bus_mv;        /* ch2 bus, in mV */
    s16 ch3_shunt_uv;      /* ch3 shunt, in uV */
    s16 ch3_bus_mv;        /* ch3 bus, in mV */
    s16 shunt_sum_uv;      /* shunt sum, in uV */
} ina3221_measurements_t;

/* Function prototypes used by the driver */
int pmic_probe(struct i2c_client *i2c_client, const struct i2c_device_id *dev_id);
int pmic_remove(struct i2c_client *i2c_client);

int read_register_16(u8 reg_addr, u16 *reg_data);
int write_register_16(u8 reg_addr, u16 reg_data);
int pmic_read_measurement_out(ina3221_measurements_t *measurements);
int pmic_print_measurements_thread(void *arg);
int pmic_configure_conversions(void);

ssize_t usr_read(struct file *fptr, char __user *user_buf, size_t length_buf, loff_t *file_offset);

extern struct file_operations file_ops;

#endif /* PMIC_DRIVER_H */
