
#include <linux/module.h>
#include <linux/i2c.h>
#include <linux/kernel.h>
#include <linux/types.h>
#include <linux/errno.h>
#include <linux/kthread.h>
#include <linux/delay.h>

#include <linux/fs.h>
#include <linux/uaccess.h>

#include "pmic_driver.h"

#define DRIVER_NAME "jetson_pmic_probe"

#define MANUFACTUERE_ID (21577)

/* INA3221 reg stuff */
#define INA3221_MFG_ID          0xFE  /* Manufacturer ID (should be 0x5449 = "TI") */
#define INA3221_DIE_ID          0xFF  /* Die ID (should be 0x3220) */
#define INA3221_CONFIG          0x00  /* Configuration register */
/* ch1 */
#define INA3221_CH1_SHUNT       0x01  /* Channel 1 Shunt Voltage (40 µV per LSB) */
#define INA3221_CH1_BUS         0x02  /* Channel 1 Bus Voltage (8 mV per LSB) */
/* ch2 stuff */
#define INA3221_CH2_SHUNT       0x03  /* Channel 2 Shunt Voltage (40 µV per LSB) */
#define INA3221_CH2_BUS         0x04  /* Channel 2 Bus Voltage (8 mV per LSB) */
/* ch3 */
#define INA3221_CH3_SHUNT       0x05  /* Channel 3 Shunt Voltage (40 µV per LSB) */
#define INA3221_CH3_BUS         0x06  /* Channel 3 Bus Voltage (8 mV per LSB) */
/* sum/mask reg */
#define INA3221_SHUNT_SUM       0x0D  /* Shunt-Voltage Sum - Read-Only (40 µV per LSB) */



/* declarations are in pmic_driver.h */







    // struct file_operations {
    //    struct module *owner;
    //    loff_t (*llseek) (struct file *, loff_t, int);
    //    ssize_t (*read) (struct file *, char *, size_t, loff_t *);
    //    ssize_t (*write) (struct file *, const char *, size_t, loff_t *);
    //    int (*readdir) (struct file *, void *, filldir_t);
    //    unsigned int (*poll) (struct file *, struct poll_table_struct *);
    //    int (*ioctl) (struct inode *, struct file *, unsigned int, unsigned long);
    //    int (*mmap) (struct file *, struct vm_area_struct *);
    //    int (*open) (struct inode *, struct file *);
    //    int (*flush) (struct file *);
    //    int (*release) (struct inode *, struct file *);
    //    int (*fsync) (struct file *, struct dentry *, int datasync);
    //    int (*fasync) (int, struct file *, int);
    //    int (*lock) (struct file *, int, struct file_lock *);
    // 	 ssize_t (*readv) (struct file *, const struct iovec *, unsigned long,
    //       loff_t *);
    // 	 ssize_t (*writev) (struct file *, const struct iovec *, unsigned long,
    //       loff_t *);
    // };


/* create a read function to be called from userspace */

ssize_t usr_read(struct file* fptr, char* __user buf, size_t length_buf, loff_t* file_offset) {
    // num of bytes read so far
    int bytes_read = 0;

    const int max_len_measurements = 14;

    if (buf == NULL || length_buf != sizeof(ina3221_measurements_t)) {
        pr_info("Given NULL buffer or incorrect len\n");
        return -1;
    }

    if (*file_offset > 0) {
        return 0; 
    }

    // temp object to hold most recent measurement
    ina3221_measurements_t measurements = {0};


    /* Read the latest measurement */
    if (pmic_read_measurement_out(&measurements) == 0) {
            /* short log for the 1 ms loop */
            pr_info("Read MEAS: C1_S=%d C1_B=%d C2_S=%d C2_B=%d C3_S=%d C3_B=%d SUM=%d (µV/mV)\n",
                    measurements.ch1_shunt_uv, measurements.ch1_bus_mv,
                    measurements.ch2_shunt_uv, measurements.ch2_bus_mv,
                    measurements.ch3_shunt_uv, measurements.ch3_bus_mv,
                    measurements.shunt_sum_uv);
    } else {
            pr_err("Failed to read measurements in thread\n");
            return -EINVAL;
    }

    /* local intermdeiary buffer */
    char k_buf[max_len_measurements];

        // Channel 1 Shunt
    k_buf[0]  = ((measurements.ch1_shunt_uv >> 8) & 0xFF);
    k_buf[1]  = (measurements.ch1_shunt_uv & 0xFF);

    // Channel 1 Bus
    k_buf[2]  = ((measurements.ch1_bus_mv >> 8) & 0xFF);
    k_buf[3]  = (measurements.ch1_bus_mv & 0xFF);

    // Channel 2 Shunt
    k_buf[4]  = ((measurements.ch2_shunt_uv >> 8) & 0xFF);
    k_buf[5]  = (measurements.ch2_shunt_uv & 0xFF);

    // Channel 2 Bus
    k_buf[6]  = ((measurements.ch2_bus_mv >> 8) & 0xFF);
    k_buf[7]  = (measurements.ch2_bus_mv & 0xFF);

    // Channel 3 Shunt
    k_buf[8]  = ((measurements.ch3_shunt_uv >> 8) & 0xFF);
    k_buf[9]  = (measurements.ch3_shunt_uv & 0xFF);

    // Channel 3 Bus
    k_buf[10] = ((measurements.ch3_bus_mv >> 8) & 0xFF);
    k_buf[11] = (measurements.ch3_bus_mv & 0xFF);

    // Shunt Sum
    k_buf[12] = ((measurements.shunt_sum_uv >> 8) & 0xFF);
    k_buf[13] = (measurements.shunt_sum_uv & 0xFF);

    /* copy over the measuremnt to the buffer */

    if ( copy_to_user( buf, k_buf, (size_t)sizeof(k_buf)) != 0 ) {
        pr_err("Failed to copy_to_user  kernel buffer to user buffer\n");
        return -EFAULT;
    }

    *file_offset += sizeof(k_buf);

    return sizeof(k_buf);

}


/* register char dev so virtual fs can hook onto it */
struct file_operations file_ops = {
    .owner = THIS_MODULE,
    .read = usr_read,
};






/* internal context */
typedef struct {
    struct i2c_client *client;
    const struct i2c_device_id *dev_id;
    struct task_struct *print_thread;   /* thread that keeps printing */
    int print_enabled;                  /* flip this to stop it */
    u16 original_config;                /* config we put back later */
} jetson_pmic_data;

static jetson_pmic_data pmic_ctx = {0};

/* read a 16-bit reg from teh INA3221
 * gives back the value, or negative if it blows up
 */
/* function prototypes are in pmic_driver.h */


/* ID reg read */

/*
 * probe when teh kernel hooks this driver to the device
 */
int pmic_probe(struct i2c_client *i2c_client, const struct i2c_device_id *dev_id)
{
    u16 manufacturer_id;

    /* basic checks... */
    if (i2c_client == NULL || dev_id == NULL) {
        pr_err("invalid arguments\n");
        return -1;
    }

    if (i2c_client->adapter == NULL) {
        pr_err("invalid argumetns\n");
        return -1;
    }

    /* stash client for helper funcs here */
    pmic_ctx.client = i2c_client;
    pmic_ctx.dev_id = dev_id;

    if (read_register_16(INA3221_MFG_ID, &manufacturer_id) != 0) {
        pr_err("Failed to read manufacturer_id\n");
        return -1;
    }

    pr_info("manufacturer_id: %04hu\n", manufacturer_id);

    if (manufacturer_id == (u16)MANUFACTUERE_ID) {
        pr_info("Manufacturer ID matched: %04hu\n", manufacturer_id);
    } else {
        pr_err("Manufacturer ID mismatch: expected %u, got %04hu\n", MANUFACTUERE_ID, manufacturer_id);
        return -ENODEV;
    }

    /* save current config for later undo (kinda important) */
    if (read_register_16(INA3221_CONFIG, &pmic_ctx.original_config) != 0) {
        pr_err("Failed to read original config register\n");
        return -1;
    }
    pr_info("Original config register: 0x%04hx\n", pmic_ctx.original_config);

    /* decrease conversion timing a bit */
    if (pmic_configure_conversions() != 0) {
        pr_err("Failed to configure conversion times\n");
        return -1;
    }

    /* start measurement thread */
    pmic_ctx.print_enabled = 1;
    pmic_ctx.print_thread = kthread_run(pmic_print_measurements_thread, NULL, "pmic_print_thread");
    if (IS_ERR(pmic_ctx.print_thread)) {
        pr_err("Failed to create print thread\n");
        return PTR_ERR(pmic_ctx.print_thread);
    }
    pr_info("Measurement print thread started\n");

    return 0;
 }


/* unload / teardown hook */
int pmic_remove(struct i2c_client *i2c_client)
{
    return 0;
}


/* device tree match table */
 static const struct of_device_id device_pmic_match[] = {
    {.compatible = "ti,ina3221"},
    {}
 };

 MODULE_DEVICE_TABLE(of, device_pmic_match);


 static const struct i2c_device_id pmic_id[] = {
    {"ina3221", 0},
    {}
 };

MODULE_DEVICE_TABLE(i2c, pmic_id);



/* i2c driver registration */
struct i2c_driver pmic_register_struct = {
    .probe = pmic_probe,
    .remove = pmic_remove,
    .id_table = pmic_id,
    .driver = {
        .name = DRIVER_NAME,
        .of_match_table = device_pmic_match,
        .owner = THIS_MODULE,
    },
 };


/* module init */
static int __init pmic_init(void)
{
    int ret;

    /* without device tree: need i2c_get_adapter() and i2c_new_device() */

    ret = i2c_add_driver(&pmic_register_struct);
    if (ret != 0) {
        pr_err("Failed to register I2C driver: %d\n", ret);
        return ret;
    }

    pr_info("I2C driver registered successfully\n");
    return 0;
}

int read_register_16(u8 reg_addr, u16 *reg_data)
{
    struct i2c_msg i2c_msg_read_reg[2];
    u8 read_buf[2];

    if (reg_data == NULL) {
        pr_err("reg_data no valid pointer\n");
        return -2; /* invalid argument */
    }

    i2c_msg_read_reg[0].addr = pmic_ctx.client->addr;
    i2c_msg_read_reg[0].flags = (u16)0; /* write */
    i2c_msg_read_reg[0].len = sizeof(reg_addr);
    i2c_msg_read_reg[0].buf = &reg_addr;

    i2c_msg_read_reg[1].addr = pmic_ctx.client->addr;
    i2c_msg_read_reg[1].flags = I2C_M_RD;
    i2c_msg_read_reg[1].len = sizeof(read_buf);
    i2c_msg_read_reg[1].buf = read_buf;

    /* i2c_transfer(struct i2c_adapter *adap, struct i2c_msg *msgs, int num) */
    if (i2c_transfer(pmic_ctx.client->adapter, i2c_msg_read_reg, 2U) != 2) {
        pr_err("i2c transfer error\n");
        return -1;
    }

    /* rebuild 16-bit value */
    *reg_data = (read_buf[0] << 8U) | read_buf[1];
    return 0;

}

/*
 * write a 16-bit register to INA3221
 * Returns: 0 on success, negative on error
 */
int write_register_16(u8 reg_addr, u16 reg_data)
{
    struct i2c_msg i2c_msg_write_reg;
    u8 write_buf[3];

    write_buf[0] = reg_addr;
    write_buf[1] = (u8)(reg_data >> 8U);   /* msb */
    write_buf[2] = (u8)(reg_data & 0xFFU); /* lsb */

    i2c_msg_write_reg.addr = pmic_ctx.client->addr;
    i2c_msg_write_reg.flags = (u16)0; /* write */
    i2c_msg_write_reg.len = sizeof(write_buf);
    i2c_msg_write_reg.buf = write_buf;

    if (i2c_transfer(pmic_ctx.client->adapter, &i2c_msg_write_reg, 1U) != 1) {
        pr_err("i2c write transfer error\n");
        return -1;
    }

    return 0;
}

/*
 * set INA3221 conversion times to 001 (204 us)
 * both VBUSCT2:0 and VSHCT2:0 set to 001
 * returns 0 on success, negative on error
 */
int pmic_configure_conversions(void)
{
    u16 config_value;

    /* read current config */
    if (read_register_16(INA3221_CONFIG, &config_value) != 0) {
        pr_err("Failed to read config register for modification\n");
        return -1;
    }

    pr_info("Current config: 0x%04hx\n", config_value);

    /* clear VBUSCT2:0 and VSHCT2:0 bits */
    config_value &= ~(0x1C0);  /* clear bits [8:6] */
    config_value &= ~(0x038);  /* clear bits [5:3] */

    /* set VBUSCT2:0 to 001 */
    config_value |= (0x1 << 6);   /* 001 in bits [8:6] */

    /* set VSHCT2:0 to 001 */
    config_value |= (0x1 << 3);   /* 001 in bits [5:3] */

    pr_info("New config: 0x%04hx (conversion times set to 001 = 204 µs)\n", config_value);

    /* write updated config */
    if (write_register_16(INA3221_CONFIG, config_value) != 0) {
        pr_err("Failed to write modified config register\n");
        return -1;
    }

    pr_info("INA3221 conversion times configured to 204 µs for faster sampling\n");
    return 0;
}

/*
 * read all INA3221 measurement outputs (read-only registers)
 * fills ina3221_measurements_t with the current voltages
 * returns 0 on success, negative on error
 */
int pmic_read_measurement_out(ina3221_measurements_t *measurements)
{
    u16 raw_value;

    if (measurements == NULL) {
        pr_err("measurements pointer is NULL\n");
        return -EINVAL;
    }

    /* channel 1 shunt */
    if (read_register_16(INA3221_CH1_SHUNT, &raw_value) != 0) {
        pr_err("Failed to read Ch1 Shunt Voltage\n");
        return -1;
    }
    measurements->ch1_shunt_uv = (s16)(raw_value >> 3) * 40;  /* uV */

    /* channel 1 bus */
    if (read_register_16(INA3221_CH1_BUS, &raw_value) != 0) {
        pr_err("Failed to read Ch1 Bus Voltage\n");
        return -1;
    }
    measurements->ch1_bus_mv = (s16)(raw_value >> 3) * 8;  /* mV */

    /* channel 2 shunt */
    if (read_register_16(INA3221_CH2_SHUNT, &raw_value) != 0) {
        pr_err("Failed to read Ch2 Shunt Voltage\n");
        return -1;
    }
    measurements->ch2_shunt_uv = (s16)(raw_value >> 3) * 40;

    /* channel 2 bus */
    if (read_register_16(INA3221_CH2_BUS, &raw_value) != 0) {
        pr_err("Failed to read Ch2 Bus Voltage\n");
        return -1;
    }
    measurements->ch2_bus_mv = (s16)(raw_value >> 3) * 8;

    /* channel 3 shunt */
    if (read_register_16(INA3221_CH3_SHUNT, &raw_value) != 0) {
        pr_err("Failed to read Ch3 Shunt Voltage\n");
        return -1;
    }
    measurements->ch3_shunt_uv = (s16)(raw_value >> 3) * 40;

    /* channel 3 bus */
    if (read_register_16(INA3221_CH3_BUS, &raw_value) != 0) {
        pr_err("Failed to read Ch3 Bus Voltage\n");
        return -1;
    }
    measurements->ch3_bus_mv = (s16)(raw_value >> 3) * 8;

    /* shunt sum */
    if (read_register_16(INA3221_SHUNT_SUM, &raw_value) != 0) {
        pr_err("Failed to read Shunt-Voltage Sum\n");
        return -1;
    }
    measurements->shunt_sum_uv = (s16)(raw_value >> 3) * 40;

    pr_info("Measurement outputs read successfully\n");
    pr_info("Ch1 Shunt: %d µV, Ch1 Bus: %d mV\n", measurements->ch1_shunt_uv, measurements->ch1_bus_mv);
    pr_info("Ch2 Shunt: %d µV, Ch2 Bus: %d mV\n", measurements->ch2_shunt_uv, measurements->ch2_bus_mv);
    pr_info("Ch3 Shunt: %d µV, Ch3 Bus: %d mV\n", measurements->ch3_shunt_uv, measurements->ch3_bus_mv);
    pr_info("Shunt Sum: %d µV\n", measurements->shunt_sum_uv);

    return 0;
}

/*
 * periodic print thread - spits out measurements every 1 ms
 * stops when pmic_ctx.print_enabled goes to 0
 */
int pmic_print_measurements_thread(void *arg)
{
    ina3221_measurements_t measurements;

    pr_info("Measurement print thread started\n");

    while (pmic_ctx.print_enabled) {
        /* read current measurements */
        if (pmic_read_measurement_out(&measurements) == 0) {
            /* short log for the 1 ms loop */
            pr_info("MEAS: C1_S=%d C1_B=%d C2_S=%d C2_B=%d C3_S=%d C3_B=%d SUM=%d (µV/mV)\n",
                    measurements.ch1_shunt_uv, measurements.ch1_bus_mv,
                    measurements.ch2_shunt_uv, measurements.ch2_bus_mv,
                    measurements.ch3_shunt_uv, measurements.ch3_bus_mv,
                    measurements.shunt_sum_uv);
        } else {
            pr_err("Failed to read measurements in thread\n");
        }
        
        /* chill 1 ms before next read */
        msleep(1);
    }

    pr_info("Measurement print thread stopped\n");
    return 0;
}


/*
struct i2c_driver {
  unsigned int class;
  int (* attach_adapter) (struct i2c_adapter *);
  int (* probe) (struct i2c_client *, const struct i2c_device_id *);
  int (* remove) (struct i2c_client *);
  void (* shutdown) (struct i2c_client *);
  void (* alert) (struct i2c_client *, unsigned int data);
  int (* command) (struct i2c_client *client, unsigned int cmd, void *arg);
  struct device_driver driver;
  const struct i2c_device_id * id_table;
  int (* detect) (struct i2c_client *, struct i2c_board_info *);
  const unsigned short * address_list;
  struct list_head clients;
};
*/

/*
 * module cleanup
 */
static void __exit pmic_exit(void)
{
    /* stop print thread */
    if (pmic_ctx.print_thread != NULL) {
        pmic_ctx.print_enabled = 0;
        kthread_stop(pmic_ctx.print_thread); /* stop thread */
        pr_info("Print thread stopped\n");
    }

    /* restore saved config */
    if (write_register_16(INA3221_CONFIG, pmic_ctx.original_config) == 0) {
        pr_info("Original config register restored: 0x%04hx\n", pmic_ctx.original_config);
    } else {
        pr_err("Failed to restore original config register\n");
    }

    i2c_del_driver(&pmic_register_struct);
}

module_init(pmic_init);
module_exit(pmic_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("MLPower Team");
MODULE_DESCRIPTION("Jetson PMIC INA3221 Probe - Out-of-tree I2C driver");
MODULE_VERSION("0.1");
