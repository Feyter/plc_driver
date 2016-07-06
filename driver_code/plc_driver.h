/*This is the Header file for the plc_driver.c*/

/* These are the flags in the statusword */
#define PLCDRV_RX_INTR 0x0001
#define PLCDRV_TX_INTR 0x0002

/* Default timeout period */
#define PLCDRV_TIMEOUT 5   /* In jiffies */

/*an array for all devices the driver needs to hold.
* this is just needed because the driver fakes a hardware transmit*/
extern struct net_device *plc_devs[];




