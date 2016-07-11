/*This is a Prototype for the PLC Network Driver.
*by Jens Fey*/

/*---INCLUDES---*/

#include <linux/module.h>
#include <linux/init.h>
#include <linux/moduleparam.h>

#include <linux/sched.h>
#include <linux/kernel.h> /* printk() */
#include <linux/slab.h> /* kmalloc() */
#include <linux/errno.h>  /* error codes */
#include <linux/types.h>  /* size_t */
#include <linux/interrupt.h> /* mark_bh */

#include <linux/in.h>
#include <linux/netdevice.h>   /* struct device, and other headers */
#include <linux/etherdevice.h> /* eth_type_trans */
#include <linux/ip.h>          /* struct iphdr */
#include <linux/tcp.h>         /* struct tcphdr */
#include <linux/skbuff.h>

#include <linux/delay.h>

#include "plc_driver.h"	/*the header file for this plc_driver.c file*/

#include <linux/in6.h>
#include <asm/checksum.h>

MODULE_AUTHOR("Jens Fey");
MODULE_LICENSE("Dual BSD/GPL");

/*---GLOBAL VARIABLES---*/

/*this is a array of type net_device to hold all net_devices in this driver.
This is needed because the driver must fake calls from fake remote machines who are actually interfaces on the same machine.*/
struct net_device *plc_devs[3];

/*The size of the packetpool in packts*/
int pool_size = 8;

/*Number of devices this driver handles*/
int number_devs = 3;

static int lockup = 0;
module_param(lockup, int, 0);

static int timeout = PLCDRV_TIMEOUT;
module_param(timeout, int, 0);

/*
 * Do we run in NAPI mode 0=no?
 */
static int use_napi = 0;
module_param(use_napi, int, 0);


/*---STRUCTURES---*/

/*
 * A structure representing an in-flight packet.
 */
struct plc_packet {
	struct plc_packet *next;
	struct net_device *dev;
	int	datalen;
	u8 data[ETH_DATA_LEN];
};

/*Structure for holding the private data of the device*/
struct plcdev_priv {
	struct net_device *dev;	/*the device of which this priv struct belongs*/
	struct napi_struct napi;
	struct net_device_stats stats;
	int status;
	struct plc_packet *ppool;
	struct plc_packet *rx_queue;  /* List of incoming packets */
	int rx_int_enabled;
	int tx_packetlen;
	u8 *tx_packetdata;
	struct sk_buff *skb;
	spinlock_t lock;
};

/*---FUNCTION DECLARATIONS*/

/*placeholder for the interrupt function. This is set later to regular or napi interrupt*/
static void (*plcdrv_interrupt)(int, void *, struct pt_regs *);

/*placeholder for timeout function to set this in the fop*/
static void plcdrv_tx_timeout(struct net_device *dev);


/*---FUNCTIONS---*/

/*
 * Enable and disable receive interrupts.
 */
static void plcdrv_rx_ints(struct net_device *dev, int enable)
{
	struct plcdev_priv *priv = netdev_priv(dev);
	priv->rx_int_enabled = enable;
}

/*
 * Set up a device's packet pool.
 */
void plcdev_setup_pool(struct net_device *dev)
{
	struct plcdev_priv *priv = netdev_priv(dev);
	int i;
	struct plc_packet *pkt;

	priv->ppool = NULL;
	/*Allocate memory for all packets untill the pool is full or no memory is available*/
	for (i = 0; i < pool_size; i++) {
		pkt = kmalloc (sizeof (struct plc_packet), GFP_KERNEL);
		if (pkt == NULL) {
			printk (KERN_NOTICE "Ran out of memory while allocating packet pool\n");
			return;
		}
		pkt->dev = dev;
		pkt->next = priv->ppool;
		priv->ppool = pkt;
	}
}

/*This frees the memory that was allocated for the packet pool of a device*/
void plcdev_teardown_pool(struct net_device *dev)
{
	struct plcdev_priv *priv = netdev_priv(dev);
	struct plc_packet *pkt;
    
	while ((pkt = priv->ppool)) {
		priv->ppool = pkt->next;
		kfree (pkt);
		/* FIXME - in-flight packets ? */
	}
}    


/*
 * Buffer/pool management.
 */
struct plc_packet *plcdrv_get_tx_buffer(struct net_device *dev)
{
	struct plcdev_priv *priv = netdev_priv(dev);
	unsigned long flags;
	struct plc_packet *pkt;
    
	spin_lock_irqsave(&priv->lock, flags);
	pkt = priv->ppool;
	priv->ppool = pkt->next;
	if (priv->ppool == NULL) {
		printk (KERN_INFO "Pool empty\n");
		netif_stop_queue(dev);
	}
	spin_unlock_irqrestore(&priv->lock, flags);
	return pkt;
}

/*enqueues the packet "pkt" at the rx_queue of the device "dev"*/
void plcdrv_enqueue_buf(struct net_device *dev, struct plc_packet *pkt)
{
	unsigned long flags;
	struct plcdev_priv *priv = netdev_priv(dev);

	spin_lock_irqsave(&priv->lock, flags);
	pkt->next = priv->rx_queue;  /* FIXME - misorders packets */
	priv->rx_queue = pkt;
	spin_unlock_irqrestore(&priv->lock, flags);
}

/*dequeus the next packet in the rx_queue*/
struct plc_packet *plcdrv_dequeue_buf(struct net_device *dev)
{
	struct plcdev_priv *priv = netdev_priv(dev);
	struct plc_packet *pkt;
	unsigned long flags;

	spin_lock_irqsave(&priv->lock, flags);
	pkt = priv->rx_queue;
	/*throw out the first packet in the rx_queue if any packets are in*/
	if (pkt != NULL)
		priv->rx_queue = pkt->next;
	spin_unlock_irqrestore(&priv->lock, flags);
	return pkt;
}

/*release the packet and put it back in the pool*/
void plcdrv_release_buffer(struct plc_packet *pkt)
{
	unsigned long flags;
	struct plcdev_priv *priv = netdev_priv(pkt->dev);
	
	spin_lock_irqsave(&priv->lock, flags);
	pkt->next = priv->ppool;
	priv->ppool = pkt;
	spin_unlock_irqrestore(&priv->lock, flags);
	if (netif_queue_stopped(pkt->dev) && pkt->next == NULL)
		netif_wake_queue(pkt->dev);
}

/*
 * Receive a packet: retrieve, encapsulate and pass over to upper levels
 */
void plcdrv_rx(struct net_device *dev, struct plc_packet *pkt)
{
	struct sk_buff *skb;
	struct plcdev_priv *priv = netdev_priv(dev);

	/*
	 * The packet has been retrieved from the transmission
	 * medium. Build an skb around it, so upper layers can handle it
	 */
	skb = dev_alloc_skb(pkt->datalen + 2);
	if (!skb) {
		if (printk_ratelimit())
			printk(KERN_NOTICE "plc driver rx: low on mem - packet dropped\n");
		priv->stats.rx_dropped++;
		goto out;
	}
	skb_reserve(skb, 2); /* align IP on 16B boundary */  
	memcpy(skb_put(skb, pkt->datalen), pkt->data, pkt->datalen);

	/* Write metadata, and then pass to the receive level */
	skb->dev = dev;
	skb->protocol = eth_type_trans(skb, dev);
	skb->ip_summed = CHECKSUM_UNNECESSARY; /* don't check it */
	priv->stats.rx_packets++;
	priv->stats.rx_bytes += pkt->datalen;
	netif_rx(skb);
  out:
	return;
}

/*
 * The poll implementation.
 */
static int plcdrv_poll(struct napi_struct *napi, int budget)
{
	int npackets = 0;
	struct sk_buff *skb;
	struct plcdev_priv *priv;
	struct plc_packet *pkt;
	struct net_device *dev;
    
	priv = container_of(napi, struct plcdev_priv, napi);
	dev = priv->dev;
	/*Reserve memory for skb, put the data in the skb and give the skb to the Kernel
	* repeat this for every packet in rx_queue or at least as often as budget allows it*/
	while (npackets < budget && priv->rx_queue) { //???
		pkt = plcdrv_dequeue_buf(dev);
		skb = dev_alloc_skb(pkt->datalen + 2);
		/*Check if skb alloc went wrong*/
		if (! skb) {
			if (printk_ratelimit())
				printk(KERN_NOTICE "plc driver: packet dropped\n");
			priv->stats.rx_dropped++;
			plcdrv_release_buffer(pkt);
			continue;
		}
		skb_reserve(skb, 2); /* align IP on 16B boundary */  
		memcpy(skb_put(skb, pkt->datalen), pkt->data, pkt->datalen);
		skb->dev = dev;
		skb->protocol = eth_type_trans(skb, dev);
		skb->ip_summed = CHECKSUM_UNNECESSARY; /* don't check it */
		/*fed packet to ther kernel*/
		netif_receive_skb(skb);
		
        	/* Maintain stats */
		npackets++;
		priv->stats.rx_packets++;
		priv->stats.rx_bytes += pkt->datalen;
		plcdrv_release_buffer(pkt);
	}
	/* If we processed all packets, we're done; tell the kernel and reenable ints */
	if (! priv->rx_queue) {
		napi_complete(napi);
		plcdrv_rx_ints(dev, 1);
	}
	/* We couldn't process everything. */
	return npackets;
}

/*
 * The typical interrupt entry point
 * Handles RX and TX interrupts.
 */
static void plcdrv_regular_interrupt(int irq, void *dev_id, struct pt_regs *regs)
{
	int statusword;
	struct plcdev_priv *priv;
	struct plc_packet *pkt = NULL;
	/*
	 * As usual, check the "device" pointer to be sure it is
	 * really interrupting.
	 * Then assign "struct device *dev"
	 */
	struct net_device *dev = (struct net_device *)dev_id;
	/* ... and check with hw if it's really ours */

	printk(KERN_DEBUG "plc driver: In function regular_interrupt!!!\n");

	/* paranoid */
	if (!dev)
		return;

	/* Lock the device */
	priv = netdev_priv(dev);
	spin_lock(&priv->lock);

	/* retrieve statusword: real netdevices use I/O instructions */
	statusword = priv->status;
	/*Set the statusword in the device to 0*/
	priv->status = 0;

	/*Check if the status is RX_INTERRUPT*/
	if (statusword & PLCDRV_RX_INTR) {
		/* send it to rx for handling */
		pkt = priv->rx_queue;
		if (pkt) {
			printk(KERN_DEBUG "plcdrv: Its a rx!!!\n");
			priv->rx_queue = pkt->next;
			plcdrv_rx(dev, pkt);
		}
	}
	/*Ckeck if the status is TX_INTERRUPT*/
	if (statusword & PLCDRV_TX_INTR) {
		printk(KERN_DEBUG "plcdrv: Its a tx!!!\n");
		/* a transmission is over: free the skb */
		priv->stats.tx_packets++;
		priv->stats.tx_bytes += priv->tx_packetlen;
		dev_kfree_skb(priv->skb);
	}

	/* Unlock the device and we are done */
	spin_unlock(&priv->lock);
	if (pkt) plcdrv_release_buffer(pkt); /* Do this outside the lock! */
	return;
}

/*
 * A NAPI interrupt handler.
 */
static void plcdrv_napi_interrupt(int irq, void *dev_id, struct pt_regs *regs)
{
	int statusword;
	struct plcdev_priv *priv;

	/*
	 * As usual, check the "device" pointer for shared handlers.
	 * Then assign "struct device *dev"
	 */
	struct net_device *dev = (struct net_device *)dev_id;
	/* ... and check with hw if it's really ours */

	printk(KERN_DEBUG "plc driver: In function napi_interrupt!!!\n");

	/* paranoid */
	if (!dev)
		return;

	/* Lock the device */
	priv = netdev_priv(dev);
	spin_lock(&priv->lock);

	/* retrieve statusword: real netdevices use I/O instructions */
	statusword = priv->status;
	priv->status = 0;
	if (statusword & PLCDRV_RX_INTR) {
		printk(KERN_DEBUG "plc driver: Its a rx!!!\n");
		plcdrv_rx_ints(dev, 0);  /* Disable further interrupts */
		napi_schedule(&priv->napi);
	}
	if (statusword & PLCDRV_TX_INTR) {
		printk(KERN_DEBUG "plc driver: Its a tx!!!\n");
        	/* a transmission is over: free the skb */
		priv->stats.tx_packets++;
		priv->stats.tx_bytes += priv->tx_packetlen;
		dev_kfree_skb(priv->skb);
	}

	/* Unlock the device and we are done */
	spin_unlock(&priv->lock);
	return;
}


/*
 * This is where the Magig happens.
Sourc and Dest adresses are changed based on the previous dest adress.
 */
static void plcdrv_hw_tx(char *buf, int len, struct net_device *dev)
{
	/*
	 * This function deals with hw details. This interface loops
	 * back the packet to the other snull interface (if any).
	 * In other words, this function implements the snull behaviour,
	 * while all other procedures are rather device-independent
	 */
	struct iphdr *ih;
	struct net_device *dest;
	struct plcdev_priv *priv;
	u32 *saddr, *daddr;
	struct plc_packet *tx_buffer;
    
	/* I am paranoid. Ain't I? */
	if (len < sizeof(struct ethhdr) + sizeof(struct iphdr)) {
		printk("plcdrv: Hmm... packet too short (%i octets)\n",
				len);
		return;
	}
	
	/*
	 * Ethhdr is 14 bytes, but the kernel arranges for iphdr
	 * to be aligned (i.e., ethhdr is unaligned)
	 */

	/*find the ip header in the packet data*/
	ih = (struct iphdr *)(buf+sizeof(struct ethhdr));	
	
	/*Save source and destination adress from the IP header*/
	saddr = &ih->saddr;
	daddr = &ih->daddr;

	printk(KERN_DEBUG "plc_driver: Sending packet from: %u.%u.%u.%u \n", ((u8 *)saddr)[0], ((u8 *)saddr)[1], ((u8 *)saddr)[2], ((u8 *)saddr)[3]);

	/*Copy the last octet in the therd octet of src and dest adress*/
	((u8 *)saddr)[2] = ((u8 *)daddr)[3];
	((u8 *)daddr)[2] = ((u8 *)daddr)[3];

	printk(KERN_DEBUG "plc_driver: Sending packet to: %u.%u.%u.%u \n", ((u8 *)daddr)[0], ((u8 *)daddr)[1], ((u8 *)daddr)[2], ((u8 *)daddr)[3]);

	ih->check = 0;         /* and rebuild the checksum (ip needs it) */
	ih->check = ip_fast_csum((unsigned char *)ih,ih->ihl);

	/*
	 * Ok, now the packet is ready for transmission: 
	 */

	/*the destination dev depends from the last destination adress octet*/
	dest = plc_devs[0];
	switch(((u8 *)daddr)[3]){
		case 1:{
			dest = plc_devs[0];
			break;
		}
		case 2:{
			dest = plc_devs[1];
			break;
		}
		case 3:{
			dest = plc_devs[2];
			break;
		}
	}	
	
	/*Put the packet in the tx_buffer of the sending device*/
	tx_buffer = plcdrv_get_tx_buffer(dev);
	tx_buffer->datalen = len;
	memcpy(tx_buffer->data, buf, len);

	/*put the packet at the rxqueue of the destination device
	* and make a interrupt if poll is not active*/
	priv = netdev_priv(dest);
	plcdrv_enqueue_buf(dest, tx_buffer);
	if (priv->rx_int_enabled) {
		priv->status |= PLCDRV_RX_INTR;
		plcdrv_interrupt(0, dest, NULL);
	}

	/*declare the packet as send in the sending device*/
	priv = netdev_priv(dev);
	priv->tx_packetlen = len;
	priv->tx_packetdata = buf;
	priv->status |= PLCDRV_TX_INTR;
	if (lockup && ((priv->stats.tx_packets + 1) % lockup) == 0) {
        	/* Simulate a dropped transmit interrupt */
		netif_stop_queue(dev);
		//PDEBUG("Simulate lockup at %ld, txp %ld\n", jiffies,(unsigned long) priv->stats.tx_packets);
	}
	else
		plcdrv_interrupt(0, dev, NULL);
}

/*
 * Transmit a packet (called by the kernel)
 */
int plcdrv_tx(struct sk_buff *skb, struct net_device *dev)
{
	int len;
	char *data, shortpkt[ETH_ZLEN];
	struct plcdev_priv *priv = netdev_priv(dev);
	
	printk(KERN_DEBUG "plc driver: Start tx in function plcdrv_tx!!!\n");
	
	data = skb->data;
	len = skb->len;
	/*Check if the length of the packet is shorter than minimum length of a eth-packet.
	* If so will up the rest with 0s. This is a security thing.*/
	if (len < ETH_ZLEN) {
		memset(shortpkt, 0, ETH_ZLEN);
		memcpy(shortpkt, skb->data, skb->len);
		len = ETH_ZLEN;
		data = shortpkt;
	}
	dev->trans_start = jiffies; /* save the timestamp */

	/* Remember the skb, so we can free it at interrupt time */
	priv->skb = skb;

	/* actual deliver of data is device-specific, and not shown here */
	plcdrv_hw_tx(data, len, dev);

	/*TODO: Error check*/	

	return 0; /* Our simple device can not fail */
}

/*
 * Deal with a transmit timeout.
 */
void plcdrv_tx_timeout (struct net_device *dev)
{
	struct plcdev_priv *priv = netdev_priv(dev);

	//PDEBUG("Transmit timeout at %ld, latency %ld\n", jiffies, jiffies - dev->trans_start);
        /* Simulate a transmission interrupt to get things moving */
	priv->status = PLCDRV_TX_INTR;
	plcdrv_interrupt(0, dev, NULL);
	priv->stats.tx_errors++;
	netif_wake_queue(dev);
	return;
}

/*
 * The "change_mtu" method is usually not needed.
 * If you need it, it must be like this.
 */
int plcdrv_change_mtu(struct net_device *dev, int new_mtu)
{
	unsigned long flags;
	struct plcdev_priv *priv = netdev_priv(dev);
	spinlock_t *lock = &priv->lock;
    
	/* check ranges */
	if ((new_mtu < 68) || (new_mtu > 1500))
		return -EINVAL;
	/*
	 * Do anything you need, and then accept the value
	 */
	spin_lock_irqsave(lock, flags);
	dev->mtu = new_mtu;
	spin_unlock_irqrestore(lock, flags);
	return 0; /* success */
}

/*
 * Ioctl commands 
 */
int plcdrv_ioctl(struct net_device *dev, struct ifreq *rq, int cmd)
{
	//PDEBUG("ioctl\n");
	return 0;
}

/*Building up the header for the special conditions of the 3 device structure of this driver*/
int plcdrv_header(struct sk_buff *skb, struct net_device *dev,
                unsigned short type, const void *daddr, const void *saddr,
                unsigned len)
{

	u8 i;
	char address[ETH_ALEN];
	u32 *sipaddr, *dipaddr;
	/*push the eth header in the sb_buff and save a pointer to it*/
	struct ethhdr *eth = (struct ethhdr *)skb_push(skb,ETH_HLEN);
	/*get the ip header (it is right behind the eth header)*/
	struct iphdr *ih = (struct iphdr *)(skb->data+sizeof(struct ethhdr));

	printk(KERN_DEBUG "plc driver: Entering the plcdrv_header function!!!\n");

	/*save the pointer to the ip addresse fields in the header*/
	sipaddr = &ih->saddr;
	dipaddr = &ih->daddr;

	/*This is just for debug*/
	printk(KERN_DEBUG "plc_driver: header source ip: %u.%u.%u.%u \n", ((u8 *)sipaddr)[0], ((u8 *)sipaddr)[1], ((u8 *)sipaddr)[2], ((u8 *)sipaddr)[3]);
	printk(KERN_DEBUG "plc_driver: header destination ip: %u.%u.%u.%u \n", ((u8 *)dipaddr)[0], ((u8 *)dipaddr)[1], ((u8 *)dipaddr)[2], ((u8 *)dipaddr)[3]);

	if(saddr){
		memcpy(address, saddr+1, ETH_ALEN-1);
		address[ETH_ALEN-1]='\0';
		printk(KERN_DEBUG "plc_driver: saddr was given: %s!!!\n", address);
	}
	if(daddr){
		memcpy(address, daddr+1, ETH_ALEN-1);
		address[ETH_ALEN-1]='\0';
		printk(KERN_DEBUG "plc_driver: daddr was given: %s!!!\n", address);
	}	

	/*set the type of the protocol*/
	eth->h_proto = htons(type);
	/*copy the source mac in eth header*/
	memcpy(eth->h_source, saddr ? saddr : dev->dev_addr, dev->addr_len);

	/*take out the last octet of the destination ip*/
	i = ((u8 *)dipaddr)[3];

	/*take the ground MAC and override the last byte with the last octet of the destination ip.
	* this leeds to the rigth "fake" MAC address of the destination device*/
	memcpy(eth->h_dest,   "\0PLCI0", ETH_ALEN);
	eth->h_dest[ETH_ALEN-1]+=i;

	/*Debug prints and finish*/
	memcpy(address, eth->h_dest+1, ETH_ALEN-1);
	address[ETH_ALEN-1]='\0';
	printk(KERN_DEBUG "plc_driver: daddr is now: %s!!!\n", address);

	return (dev->hard_header_len);
}

/*
 * Return statistics to the caller
 */
struct net_device_stats *plcdrv_stats(struct net_device *dev)
{
	struct plcdev_priv *priv = netdev_priv(dev);
	printk(KERN_DEBUG "plc driver: stats was called!!!\n");
	return &priv->stats;
}

/*
 * Open
 */
int plcdrv_open(struct net_device *dev)
{
	
	int i;
	char address[ETH_ALEN];
	/* request_region(), request_irq(), ....  (like fops->open) */
	
	printk(KERN_DEBUG "plc driver: Entering plcdrv_open.!!!\n");

	/* 
	 * Assign the fake hardware address of the board:The first byte is '\0' to avoid being a multicast
	 * address (the first byte of multicast addrs is odd).
	 */
	
	/*Copy the base hw address in the dev structure*/
	memcpy(dev->dev_addr, "\0PLCI0", ETH_ALEN);

	
	/*find out which device dev is*/
	for(i = 0; i<number_devs; i++){
		if(dev == plc_devs[i]){
			//increase the last byte by i to get diffrent addresses
			dev->dev_addr[ETH_ALEN-1]+=i+1;
			
		}
	}
	/*for debug convert the address and print it*/
	memcpy(address, dev->dev_addr+1, ETH_ALEN-1);
	address[ETH_ALEN-1]='\0';
	printk(KERN_DEBUG "plc driver: MAC address = %s\n", address);

	netif_start_queue(dev);
	printk(KERN_DEBUG "plc driver: plcdrv_open start_queue complete!!!\n");
	return 0;
}

/*Close*/
int plcdrv_release(struct net_device *dev)
{
    /* release ports, irq and such -- like fops->close */

	netif_stop_queue(dev); /* can't transmit any more */
	return 0;
}

/*
 * Configuration changes (passed on by ifconfig)
 */
int plcdrv_config(struct net_device *dev, struct ifmap *map)
{
		

	printk(KERN_DEBUG "plc driver: Going in plcdev_config\n");

	if (dev->flags & IFF_UP) /* can't act on a running interface */
		return -EBUSY;

	/* Don't allow changing the I/O address */
	if (map->base_addr != dev->base_addr) {
		printk(KERN_WARNING "plc driver: Can't change I/O address\n");
		return -EOPNOTSUPP;
	}

	/* Allow changing the IRQ */
	if (map->irq != dev->irq) {
		dev->irq = map->irq;
        	/* request_irq() is delayed to open-time */
	}

	/* ignore other fields */
	return 0;
}

static const struct net_device_ops plcdrv_netdev_ops = {
	.ndo_open		= plcdrv_open,
	.ndo_stop		= plcdrv_release,
	.ndo_set_config		= plcdrv_config,
	.ndo_start_xmit		= plcdrv_tx,
	.ndo_do_ioctl		= plcdrv_ioctl,
	.ndo_get_stats		= plcdrv_stats,
	.ndo_change_mtu		= plcdrv_change_mtu,
	.ndo_tx_timeout         = plcdrv_tx_timeout,
};

static const struct header_ops plcdrv_header_ops = {
	.create 	= plcdrv_header,
	.cache 		= NULL,
};

/*
 * This funktion setup a net_device.
 * It is invoked by register_netdev()
 */
void plcdev_init(struct net_device *dev)
{

	struct plcdev_priv *priv;
	printk(KERN_DEBUG "plc driver: going in plcdev_init for a dev!!!\n");

	/*
	 * Then, initialize the priv field. This encloses the statistics
	 * and a few private fields.
	 */
	priv = netdev_priv(dev);
	memset(priv, 0, sizeof(struct plcdev_priv));
	spin_lock_init(&priv->lock);
	priv->dev = dev;

    	/*
	 * Make the usual checks: check_region(), probe irq, ...  -ENODEV
	 * should be returned if no device found.  No resource should be
	 * grabbed: this is done on open(). 
	 */

    	/* 
	 * Then, assign other fields in dev, using ether_setup() and some
	 * hand assignments
	 */
	ether_setup(dev); /* assign some of the fields */

	dev->watchdog_timeo = timeout;
	if (use_napi) {
		netif_napi_add(dev, &priv->napi, plcdrv_poll, 2);
	}

	/* keep the default flags, just add NOARP */
	dev->flags           |= IFF_NOARP;
	dev->features        |= NETIF_F_HW_CSUM;
	dev->netdev_ops = &plcdrv_netdev_ops;
	dev->header_ops = &plcdrv_header_ops;

	plcdrv_rx_ints(dev, 1);		/* enable receive interrupts */
	plcdev_setup_pool(dev);
}

/*Just a cleanup function for deload of the module from the Kernel*/
void plcdrv_cleanup(void)
{
	int i;
    
	for (i = 0; i < number_devs;  i++) {
		if (plc_devs[i]) {
			unregister_netdev(plc_devs[i]);
			plcdev_teardown_pool(plc_devs[i]);
			free_netdev(plc_devs[i]);
		}
	}
	return;
}

/*the function that is called on module load time.
*after this the module/driver can be opend by the OS*/
int plcdrv_init_module(void)
{
	int result, i, ret = -ENOMEM;

	printk(KERN_DEBUG "plc driver: Entering plcdrv_init_module!!!\n");

	/*set the interrupt function depending on which mode sould be used*/
	if(use_napi==0){
		plcdrv_interrupt = plcdrv_regular_interrupt;
	}
	else{
		plcdrv_interrupt = plcdrv_napi_interrupt;
	}

	/* Allocate the devices */
	plc_devs[0] = alloc_netdev(sizeof(struct plcdev_priv), "plc1", NET_NAME_UNKNOWN,
			plcdev_init);
	plc_devs[1] = alloc_netdev(sizeof(struct plcdev_priv), "plc2", NET_NAME_UNKNOWN,
			plcdev_init);
	plc_devs[2] = alloc_netdev(sizeof(struct plcdev_priv), "plc3", NET_NAME_UNKNOWN,
			plcdev_init);

	/*Check if the allocation succeeded*/
	if (plc_devs[0] == NULL || plc_devs[1] == NULL || plc_devs[2] == NULL)
		goto out;

	ret = -ENODEV;
	for (i = 0; i < number_devs;  i++){
		printk(KERN_DEBUG "plc driver: register device %i!!!\n", i);
		if ((result = register_netdev(plc_devs[i]))){
			printk("plc_driver: error %i registering device \"%s\"\n", result, plc_devs[i]->name);
		}
		else{
			printk(KERN_DEBUG "plc driver: register device %i scuceeded!!!\n", i);
			ret = 0;
		}
			
	}
   out:
	if (ret) 
		plcdrv_cleanup();
	return ret;
}


/*---MACROS---*/

module_init(plcdrv_init_module);
module_exit(plcdrv_cleanup);
