/* pmic_driver.h - shared declarations for pmic_driver.c */
#ifndef PMIC_DRIVER_H
#define PMIC_DRIVER_H

#ifdef __KERNEL__
#include <linux/types.h>
#include <linux/fs.h>
#include <linux/i2c.h>
#else
#include <stdint.h>
typedef int16_t s16;
typedef uint16_t u16;
typedef uint8_t u8;
#endif

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

struct pmic_snap_live { 
	unsigned int cpu_util_avg_pct; 
	unsigned int emc_util_pct; 
	unsigned int ram_used_mb; 
	int cpu_temp_c; 
	int tj_temp_c; 
	unsigned int cpu_gpu_cv_power_mw; 
	unsigned int vdd_in_power_mw; 
}; 

#ifdef __KERNEL__
/* Function prototypes used by the driver */
int pmic_probe(struct i2c_client *i2c_client, const struct i2c_device_id *dev_id);
int pmic_remove(struct i2c_client *i2c_client);

int read_register_16(u8 reg_addr, u16 *reg_data);
int write_register_16(u8 reg_addr, u16 reg_data);
int pmic_read_measurement_out(ina3221_measurements_t *measurements);
int pmic_print_measurements_thread(void *arg);
int pmic_configure_conversions(void);

ssize_t usr_read(struct file *fptr, char __user *user_buf, size_t length_buf, loff_t *file_offset);
long pmic_ioctl(struct file *file, unsigned int cmd, unsigned long arg);

void get_live_pmic_data(struct pmic_snap_live *out); 

extern struct file_operations file_ops;
#endif

#ifdef __KERNEL__
#include <linux/ioctl.h>
#else
#include <sys/ioctl.h>
#endif

#define PMIC_IOC_MAGIC 'p'
#define PMIC_IOC_SET_AVG  _IOW(PMIC_IOC_MAGIC, 1, unsigned long)
#define PMIC_IOC_SET_CONV _IOW(PMIC_IOC_MAGIC, 2, unsigned long)

#endif /* PMIC_DRIVER_H */
