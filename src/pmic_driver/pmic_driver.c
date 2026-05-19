
#include <linux/module.h>
#include <linux/i2c.h>
#include <linux/kernel.h>
#include <linux/types.h>
#include <linux/errno.h>

#define DRIVER_NAME "jetson_pmic_probe"

#define MANUFACTUERE_ID (21577)

/* INA3221 I2C register addresses */
#define INA3221_MFG_ID          0xFE  /* Manufacturer ID (should be 0x5449 = "TI") */
#define INA3221_DIE_ID          0xFF  /* Die ID (should be 0x3220) */
#define INA3221_CONFIG          0x00  /* Configuration register */
#define INA3221_BUS1            0x02  /* Channel 1 bus voltage */
#define INA3221_SHUNT1          0x01  /* Channel 1 shunt voltage */

int pmic_probe(struct i2c_client* i2c_client, const struct i2c_device_id* dev_id);
int pmic_remove( struct i2c_client* i2c_client);


typedef struct {
    struct i2c_client *client;
    struct i2c_device_id* dev_id;
} jetson_pmic_data;

static jetson_pmic_data pmic_ctx = {0};

/*
 * Read a 16-bit register from INA3221
 * Returns: register value in big-endian, or negative on error
 */
int read_register_16(u8 reg_addr, u16* reg_data);


 /* REad the who am i registesr*/

/*
 * Probe function - called by kernel when device is detected
 * Executes when kernel finds a device with compatible = "ti,ina3221"
 * matching our driver's match table (see bottom of file)
 */
 int pmic_probe(struct i2c_client* i2c_client, const struct i2c_device_id* dev_id) {

    /* check acid inputs*/
    if ( i2c_client == NULL || dev_id == NULL ) {
        pr_err("invalid argumetns\n");
        return -1;
    }

    if ( i2c_client->adapter == NULL ) {
        pr_err("invalid argumetns\n");
        return -1;
    }
    
    /* set to global*/
    pmic_ctx.client = i2c_client;
    pmic_ctx.dev_id = dev_id;

    u16 manufacturer_id;

   if ( read_register_16(INA3221_MFG_ID, &manufacturer_id) != 0 ) {
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



    return 0;
 }


/*
 * Remove function - called when module is unloaded or device is removed
 */
 //  int (* remove) (struct i2c_client *);
 int pmic_remove( struct i2c_client* i2c_client) {
    return 0;
 } 


/*
 * Device matching table
 * Tells kernel: "I handle devices with compatible string 'ti,ina3221'"
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



/*
 * Driver structure
 * Registered with kernel to handle I2C devices
 */
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


/*
 * Module initialization - called when module is loaded
 */

 
static int __init pmic_init(void)
{
    // if it werent for device treee i would have to use i2c_get_adapter to get pointer to i2c bus
    //      and i2c_new_device to create an i2c device on bus

    i2c_add_driver(&pmic_register_struct);

    return 0;
}

int read_register_16(u8 reg_addr, u16* reg_data) {
    if (reg_data == NULL) {
         pr_err("reg_data no valid pointer\n");
        return -2; // invalid argument
    }

    u8 read_buf[2];

    struct i2c_msg i2c_msg_read_reg[] = {
        
        {
            .addr = pmic_ctx.client->addr,
            .flags = (u16)0, //write
            .len = sizeof(reg_addr),
            .buf = &reg_addr

        },

        {
            .addr = pmic_ctx.client->addr,
            .flags = I2C_M_RD,
            .len = sizeof(read_buf),
            .buf = read_buf,
        }

    };

    // int i2c_transfer(struct i2c_adapter *adap, struct i2c_msg *msgs, int num);
    if (  i2c_transfer( pmic_ctx.client->adapter, i2c_msg_read_reg, 2U ) != 2 ) {
        pr_err("i2c transfer error\n");
        return -1;
    }

    /* reconstruct the word*/
    *reg_data = (read_buf[0] << 8U) | read_buf[1] ;

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
 * Module cleanup - called when module is unloaded
 */
static void __exit pmic_exit(void)
{
   i2c_del_driver(&pmic_register_struct);
}

module_init(pmic_init);
module_exit(pmic_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("MLPower Team");
MODULE_DESCRIPTION("Jetson PMIC INA3221 Probe - Out-of-tree I2C driver");
MODULE_VERSION("0.1");
