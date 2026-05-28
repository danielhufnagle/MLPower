
#include <linux/module.h>
#include <linux/i2c.h>
#include <linux/kernel.h>
#include <linux/types.h>
#include <linux/errno.h>
#include <linux/kthread.h>
#include <linux/delay.h>

#define DRIVER_NAME "jetson_pmic_probe"

#define MANUFACTUERE_ID (21577)

/* INA3221 register map */
#define INA3221_MFG_ID          0xFE  /* Manufacturer ID (should be 0x5449 = "TI") */
#define INA3221_DIE_ID          0xFF  /* Die ID (should be 0x3220) */
#define INA3221_CONFIG          0x00  /* Configuration register */
/* Channel 1 measurements */
#define INA3221_CH1_SHUNT       0x01  /* Channel 1 Shunt Voltage (40 µV per LSB) */
#define INA3221_CH1_BUS         0x02  /* Channel 1 Bus Voltage (8 mV per LSB) */
/* Channel 2 measurements */
#define INA3221_CH2_SHUNT       0x03  /* Channel 2 Shunt Voltage (40 µV per LSB) */
#define INA3221_CH2_BUS         0x04  /* Channel 2 Bus Voltage (8 mV per LSB) */
/* Channel 3 measurements */
#define INA3221_CH3_SHUNT       0x05  /* Channel 3 Shunt Voltage (40 µV per LSB) */
#define INA3221_CH3_BUS         0x06  /* Channel 3 Bus Voltage (8 mV per LSB) */
/* Sum / mask register */
#define INA3221_SHUNT_SUM       0x0D  /* Shunt-Voltage Sum - Read-Only (40 µV per LSB) */

int pmic_probe(struct i2c_client* i2c_client, const struct i2c_device_id* dev_id);
int pmic_remove( struct i2c_client* i2c_client);

/* Quick snapshot of INA3221 readings */
typedef struct {
    s16 ch1_shunt_uv;      /* ch1 shunt voltage, in uV */
    s16 ch1_bus_mv;        /* ch1 bus voltage, in mV */
    s16 ch2_shunt_uv;      /* ch2 shunt voltage, in uV */
    s16 ch2_bus_mv;        /* ch2 bus voltage, in mV */
    s16 ch3_shunt_uv;      /* ch3 shunt voltage, in uV */
    s16 ch3_bus_mv;        /* ch3 bus voltage, in mV */
    s16 shunt_sum_uv;      /* summed shunt voltage, in uV */
} ina3221_measurements_t;

/* Read all INA3221 measurements.
 * Returns 0 on success, negative on error.
 */
int pmic_read_measurement_out(ina3221_measurements_t* measurements);

typedef struct {
    struct i2c_client *client;
    const struct i2c_device_id *dev_id;
    struct task_struct *print_thread;   /* periodic print kthread */
    int print_enabled;                  /* thread stop flag */
    u16 original_config;                /* saved config for restore on unload */
} jetson_pmic_data;

static jetson_pmic_data pmic_ctx = {0};

/* Read a 16-bit register from the INA3221.
 * Returns register value in big-endian, or negative on error.
 */
int read_register_16(u8 reg_addr, u16* reg_data);
int write_register_16(u8 reg_addr, u16 reg_data);
int pmic_read_measurement_out(ina3221_measurements_t* measurements);
int pmic_print_measurements_thread(void* arg);
int pmic_configure_conversions(void);


/* Read the ID register. */

/*
 * Probe runs when the kernel matches this driver to the device.
 */
int pmic_probe(struct i2c_client *i2c_client, const struct i2c_device_id *dev_id)
{
    u16 manufacturer_id;

    /* Basic argument checks. */
    if (i2c_client == NULL || dev_id == NULL) {
        pr_err("invalid arguments\n");
        return -1;
    }

    if (i2c_client->adapter == NULL) {
        pr_err("invalid argumetns\n");
        return -1;
    }

    /* Keep the client around for the helper functions below. */
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

    /* Save the current config so it can be put back later. */
    if (read_register_16(INA3221_CONFIG, &pmic_ctx.original_config) != 0) {
        pr_err("Failed to read original config register\n");
        return -1;
    }
    pr_info("Original config register: 0x%04hx\n", pmic_ctx.original_config);

    /* Speed up the conversion timing a little. */
    if (pmic_configure_conversions() != 0) {
        pr_err("Failed to configure conversion times\n");
        return -1;
    }

    /* Start the little measurement thread. */
    pmic_ctx.print_enabled = 1;
    pmic_ctx.print_thread = kthread_run(pmic_print_measurements_thread, NULL, "pmic_print_thread");
    if (IS_ERR(pmic_ctx.print_thread)) {
        pr_err("Failed to create print thread\n");
        return PTR_ERR(pmic_ctx.print_thread);
    }
    pr_info("Measurement print thread started\n");

    return 0;
 }


/* Remove hook for device teardown or module unload. */
int pmic_remove(struct i2c_client *i2c_client)
{
    return 0;
}


/*
 * Device tree match table for the INA3221.
 */
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



/* I2C driver registration. */
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


/* Module init. */
static int __init pmic_init(void)
{
    int ret;

    /* Without device tree, we'd need i2c_get_adapter() and i2c_new_device(). */

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

    /* int i2c_transfer(struct i2c_adapter *adap, struct i2c_msg *msgs, int num); */
    if (i2c_transfer(pmic_ctx.client->adapter, i2c_msg_read_reg, 2U) != 2) {
        pr_err("i2c transfer error\n");
        return -1;
    }

    /* Rebuild the 16-bit value. */
    *reg_data = (read_buf[0] << 8U) | read_buf[1];
    return 0;

}

/*
 * Write a 16-bit register to INA3221
 * Returns: 0 on success, negative on error
 */
int write_register_16(u8 reg_addr, u16 reg_data)
{
    struct i2c_msg i2c_msg_write_reg;
    u8 write_buf[3];

    write_buf[0] = reg_addr;
    write_buf[1] = (u8)(reg_data >> 8U);   /* MSB first */
    write_buf[2] = (u8)(reg_data & 0xFFU); /* LSB */

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
 * Set INA3221 conversion times to 001 (204 us).
 * That means both VBUSCT2:0 and VSHCT2:0 get set to 001.
 * Returns 0 on success, negative on error.
 */
int pmic_configure_conversions(void)
{
    u16 config_value;

    /* Read the current config first. */
    if (read_register_16(INA3221_CONFIG, &config_value) != 0) {
        pr_err("Failed to read config register for modification\n");
        return -1;
    }

    pr_info("Current config: 0x%04hx\n", config_value);

    /* Clear VBUSCT2:0 (bits 8-6) and VSHCT2:0 (bits 5-3). */
    config_value &= ~(0x1C0);  /* clear bits [8:6] */
    config_value &= ~(0x038);  /* clear bits [5:3] */

    /* Set VBUSCT2:0 to 001. */
    config_value |= (0x1 << 6);   /* 001 in bits [8:6] */

    /* Set VSHCT2:0 to 001. */
    config_value |= (0x1 << 3);   /* 001 in bits [5:3] */

    pr_info("New config: 0x%04hx (conversion times set to 001 = 204 µs)\n", config_value);

    /* Write the updated config. */
    if (write_register_16(INA3221_CONFIG, config_value) != 0) {
        pr_err("Failed to write modified config register\n");
        return -1;
    }

    pr_info("INA3221 conversion times configured to 204 µs for faster sampling\n");
    return 0;
}

/*
 * Read all INA3221 measurement outputs (read-only registers).
 * Fills ina3221_measurements_t with the current voltages.
 * Returns 0 on success, negative on error.
 */
int pmic_read_measurement_out(ina3221_measurements_t *measurements)
{
    u16 raw_value;

    if (measurements == NULL) {
        pr_err("measurements pointer is NULL\n");
        return -EINVAL;
    }

    /* Channel 1 shunt voltage. */
    if (read_register_16(INA3221_CH1_SHUNT, &raw_value) != 0) {
        pr_err("Failed to read Ch1 Shunt Voltage\n");
        return -1;
    }
    measurements->ch1_shunt_uv = (s16)(raw_value >> 3) * 40;  /* convert to uV */

    /* Channel 1 bus voltage. */
    if (read_register_16(INA3221_CH1_BUS, &raw_value) != 0) {
        pr_err("Failed to read Ch1 Bus Voltage\n");
        return -1;
    }
    measurements->ch1_bus_mv = (s16)(raw_value >> 3) * 8;  /* convert to mV */

    /* Channel 2 shunt voltage. */
    if (read_register_16(INA3221_CH2_SHUNT, &raw_value) != 0) {
        pr_err("Failed to read Ch2 Shunt Voltage\n");
        return -1;
    }
    measurements->ch2_shunt_uv = (s16)(raw_value >> 3) * 40;

    /* Channel 2 bus voltage. */
    if (read_register_16(INA3221_CH2_BUS, &raw_value) != 0) {
        pr_err("Failed to read Ch2 Bus Voltage\n");
        return -1;
    }
    measurements->ch2_bus_mv = (s16)(raw_value >> 3) * 8;

    /* Channel 3 shunt voltage. */
    if (read_register_16(INA3221_CH3_SHUNT, &raw_value) != 0) {
        pr_err("Failed to read Ch3 Shunt Voltage\n");
        return -1;
    }
    measurements->ch3_shunt_uv = (s16)(raw_value >> 3) * 40;

    /* Channel 3 bus voltage. */
    if (read_register_16(INA3221_CH3_BUS, &raw_value) != 0) {
        pr_err("Failed to read Ch3 Bus Voltage\n");
        return -1;
    }
    measurements->ch3_bus_mv = (s16)(raw_value >> 3) * 8;

    /* Shunt-voltage sum. */
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
 * Periodic print thread - prints measurements every 1 ms.
 * Runs until pmic_ctx.print_enabled is set to 0
 */
int pmic_print_measurements_thread(void *arg)
{
    ina3221_measurements_t measurements;

    pr_info("Measurement print thread started\n");

    while (pmic_ctx.print_enabled) {
        /* Read the current measurements. */
        if (pmic_read_measurement_out(&measurements) == 0) {
            /* Compact log for the 1 ms loop. */
            pr_info("MEAS: C1_S=%d C1_B=%d C2_S=%d C2_B=%d C3_S=%d C3_B=%d SUM=%d (µV/mV)\n",
                    measurements.ch1_shunt_uv, measurements.ch1_bus_mv,
                    measurements.ch2_shunt_uv, measurements.ch2_bus_mv,
                    measurements.ch3_shunt_uv, measurements.ch3_bus_mv,
                    measurements.shunt_sum_uv);
        } else {
            pr_err("Failed to read measurements in thread\n");
        }
        
        /* Wait a millisecond before the next read. */
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
 * Module cleanup.
 */
static void __exit pmic_exit(void)
{
    /* Stop the print thread. */
    if (pmic_ctx.print_thread != NULL) {
        pmic_ctx.print_enabled = 0;
        kthread_stop(pmic_ctx.print_thread); /* stop thread */
        pr_info("Print thread stopped\n");
    }

    /* Restore the saved config register. */
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
