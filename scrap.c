/*
  scrap.c
 
  Copyright Scott Ellis, 2010
 
  This program is free software; you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation; either version 2 of the License, or
  (at your option) any later version.
 
  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.
 
  You should have received a copy of the GNU General Public License
  along with this program; if not, write to the Free Software
  Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
  
*/
#include <linux/init.h>
#include <linux/module.h>
#include <linux/ioctl.h>
#include <linux/fs.h>
#include <linux/device.h>
#include <linux/err.h>
#include <linux/errno.h>
#include <linux/mutex.h>
#include <linux/slab.h>
#include <linux/smp_lock.h>
#include <linux/cdev.h>
#include <linux/spi/spi.h>
#include <linux/string.h>
#include <asm/uaccess.h>
#include <linux/hrtimer.h>

#define SPI_BUFF_SIZE	32
#define USER_BUFF_SIZE	128

/* SPI bus speed = 1 MHz */
#define SPI_BUS_SPEED 1000000

/* 5 ms = 200 Hz */
#define TIMER_PERIOD_NS 5000000

#define SPI_BUS 1
#define SPI_BUS_CS1 0

const char this_driver_name[] = "scrap";

struct scrap_message {
	u32 busy;
	struct list_head list;
	struct spi_message spi_msg;
	struct spi_transfer transfer;
	u8 *tx_buff; 
};

static struct scrap_message scrap_msg;

struct scrap_dev {
	struct semaphore spi_sem;
	struct semaphore fop_sem;
	dev_t devt;
	struct cdev cdev;
	struct class *class;
	struct spi_device *spi_device;	
	struct hrtimer timer;
	u32 running;
	u32 spi_callbacks;
	u32 timer_callbacks;
	u32 timer_misses;
	char *user_buff;
};

static struct scrap_dev scrap_dev;


static void scrap_spi_callback(void *arg)
{
	scrap_msg.busy = 0;
	scrap_dev.spi_callbacks++;
}

static int scrap_queue_spi_transaction(void)
{
	int status;
	struct spi_message *message;

	if (down_interruptible(&scrap_dev.spi_sem))
		return -EFAULT;

	if (scrap_dev.spi_device == NULL) {
		printk(KERN_ALERT "scrap_async(): spi_device is NULL\n");
		status = -ESHUTDOWN;
		goto scrap_async_done;
	}

	message = &scrap_msg.spi_msg;
	spi_message_init(message);
	message->complete = scrap_spi_callback;
	/* Not using this, but it will be the argument to scrap_spi_callback. */
	message->context = &scrap_msg;

	/* put some data in there to watch on the scope */	
	scrap_msg.tx_buff[0] = 0;
	scrap_msg.tx_buff[1] = 1;
	scrap_msg.tx_buff[2] = 2;
	scrap_msg.tx_buff[3] = 3;

	scrap_msg.transfer.tx_buf = scrap_msg.tx_buff;
	scrap_msg.transfer.rx_buf = NULL;
	scrap_msg.transfer.len = 4;

	spi_message_add_tail(&scrap_msg.transfer, message);
			
	status = spi_async(scrap_dev.spi_device, message);

	if (status)
		printk(KERN_ALERT "spi_async() failed - error %d\n", status);
	else
		scrap_msg.busy = 1;

scrap_async_done:

	up(&scrap_dev.spi_sem);

	return status;
}

static enum hrtimer_restart timer_callback(struct hrtimer *timer)
{
	scrap_dev.timer_callbacks++;

	if (!scrap_dev.running) {
		return HRTIMER_NORESTART;
	}

	if (scrap_msg.busy) {
		/*
		 * Don't clobber a pending spi transaction, but do restart the
		 * timer. Haven't hit this case yet.
		 */
		printk(KERN_ALERT "scrap_msg still busy in timer callback\n");
	}
	else if (scrap_queue_spi_transaction()) {
		return HRTIMER_NORESTART;
	}

	scrap_dev.timer_misses += hrtimer_forward_now(&scrap_dev.timer,
		ktime_set(0, TIMER_PERIOD_NS)) - 1;
	
	return HRTIMER_RESTART;
}

static ssize_t scrap_write(struct file *filp, const char __user *buff,
		size_t count, loff_t *f_pos)
{
	ssize_t status;
	size_t len;

	if(down_interruptible(&scrap_dev.fop_sem))
		return -ERESTARTSYS;

	memset(scrap_dev.user_buff, 0, 32);
	len = count > 8 ? 8 : count;

	if (copy_from_user(scrap_dev.user_buff, buff, len)) {
		status = -EFAULT;
		goto scrap_write_done;
	}

	status = count;

	/* Accept two commands, "start" or "stop" and ignore anything else. */
	if (!strnicmp(scrap_dev.user_buff, "start", 5)) {
		if (scrap_dev.running) {
			printk(KERN_ALERT "already running\n");
			goto scrap_write_done;
		}

		if (scrap_msg.busy) {
			printk(KERN_ALERT "scrap_msg is waiting for spi\n");
			goto scrap_write_done;		
		}

		if (scrap_queue_spi_transaction())
			goto scrap_write_done;
		
		scrap_dev.spi_callbacks = 0;		
		scrap_dev.timer_callbacks = 0;
		scrap_dev.timer_misses = 0;

		hrtimer_start(&scrap_dev.timer, 
				ktime_set(0, TIMER_PERIOD_NS),
        	               	HRTIMER_MODE_REL);

		scrap_dev.running = 1; 
	} 
	else if (!strnicmp(scrap_dev.user_buff, "stop", 4)) {
		hrtimer_cancel(&scrap_dev.timer);
		scrap_dev.running = 0;
	}

scrap_write_done:
	up(&scrap_dev.fop_sem);

	return count;
}

static ssize_t scrap_read(struct file *filp, char __user *buff, size_t count,
			loff_t *offp)
{
	size_t len;
	ssize_t status = 0;

	if (!buff) 
		return -EFAULT;

	/* 
	 * Tell the user no more data. A hack so 'cat /dev/scrap' works since
	 * we aren't keeping track of data we've already copied. 
	 */
	if (*offp > 0) 
		return 0;

	if (down_interruptible(&scrap_dev.fop_sem)) 
		return -ERESTARTSYS;

	sprintf(scrap_dev.user_buff, 
		"%s : spi %u  timer %u  timer_misses %u\n",
		scrap_dev.running ? "running" : "not running",
		scrap_dev.spi_callbacks, 
		scrap_dev.timer_callbacks,
		scrap_dev.timer_misses);

	len = strlen(scrap_dev.user_buff);
 
	if (len < count) 
		count = len;

	if (copy_to_user(buff, scrap_dev.user_buff, count))  {
		printk(KERN_ALERT "scrap_read(): copy_to_user() failed\n");
		status = -EFAULT;
	} else {
		*offp += count;
		status = count;
	}

	up(&scrap_dev.fop_sem);

	return status;	
}

static int scrap_open(struct inode *inode, struct file *filp)
{	
	int status = 0;

	if (down_interruptible(&scrap_dev.fop_sem)) 
		return -ERESTARTSYS;

	if (!scrap_dev.user_buff) {
		scrap_dev.user_buff = kmalloc(USER_BUFF_SIZE, GFP_KERNEL);
		if (!scrap_dev.user_buff) 
			status = -ENOMEM;
	}	

	if (!scrap_msg.tx_buff) {
		scrap_msg.tx_buff = kmalloc(SPI_BUFF_SIZE, GFP_KERNEL | GFP_DMA);
		if (!scrap_msg.tx_buff)
			status = -ENOMEM;
	}

	up(&scrap_dev.fop_sem);

	return status;
}

static int scrap_probe(struct spi_device *spi_device)
{
	int status = 0;

	if (down_interruptible(&scrap_dev.spi_sem))
		return -EBUSY;

	if (spi_device->chip_select == SPI_BUS_CS1)
		scrap_dev.spi_device = spi_device;
	else
		status = -ENODEV;

	if (!status) {
		if (spi_device->max_speed_hz != SPI_BUS_SPEED)
			printk(KERN_ALERT 
				"SPI%d.%d max_speed_hz %d Hz bus_speed %d Hz\n", 
				SPI_BUS,
				spi_device->chip_select, 
				spi_device->max_speed_hz, 
				SPI_BUS_SPEED);
		else
			printk(KERN_ALERT
				"SPI%d.%d bus_speed %d Hz\n",
				SPI_BUS, 
				spi_device->chip_select,
				SPI_BUS_SPEED);
	}	
	
	up(&scrap_dev.spi_sem);

	return status;
}

static int scrap_remove(struct spi_device *spi_device)
{
	if (scrap_dev.running) {
		hrtimer_cancel(&scrap_dev.timer);
		scrap_dev.running = 0;
	}

	if (down_interruptible(&scrap_dev.spi_sem))
		return -EBUSY;
	
	if (spi_device->chip_select == 0)
		scrap_dev.spi_device = NULL;

	up(&scrap_dev.spi_sem);

	return 0;
}

static int __init add_scrap_device_to_bus(void)
{
	struct spi_master *spi_master;
	struct spi_device *spi_device;
	struct device *pdev;
	int status;
	char buff[64];

	/* Get a handle to the SPI-1 bus. */
	spi_master = spi_busnum_to_master(SPI_BUS);

	if (!spi_master) {
		printk(KERN_ALERT "spi_busnum_to_master(1) returned NULL\n");
		printk(KERN_ALERT "Missing modprobe omap2_mcspi?\n");
		return -1;
	}

	/*
	 * Allocate a device for this bus that we can use in bus queries and
	 * if needed for adding to the bus.
	 */
	spi_device = spi_alloc_device(spi_master);

	if (!spi_device) {
		status = -1;
		printk(KERN_ALERT "spi_alloc_device() failed\n");
		return -1;
	}

	/* CS1 = 0 */
	spi_device->chip_select = SPI_BUS_CS1;

	/* First check if the bus already knows about us. */
	snprintf(buff, sizeof(buff), "%s.%u", 
			dev_name(&spi_device->master->dev),
			spi_device->chip_select);

	pdev = bus_find_device_by_name(spi_device->dev.bus, NULL, buff);
 
	if (pdev) {
		/* 
		 * Don't need this new device. 
		 * The device free operation, spi_dev_put(), will crash without 
		 * a patched omap2_mcspi_cleanup() on kernels < 2.6.34. 
		 */
		spi_dev_put(spi_device);
		
		/* 
		 * There is already a device at this cs registered on the bus.
		 * If it is us, there is nothing to do. If it is some other 
		 * driver registered at this cs, then complain and fail.
		 */
		if (pdev->driver && pdev->driver->name && 
				strcmp(this_driver_name, pdev->driver->name)) {
			printk(KERN_ALERT 
				"Driver [%s] already registered for %s\n",
				pdev->driver->name, buff);
			status = -1;
		} 
		else {
			status = 0;
		}
	} else {
		spi_device->max_speed_hz = SPI_BUS_SPEED;
		spi_device->mode = SPI_MODE_0;
		spi_device->bits_per_word = 8;
		spi_device->irq = -1;
		spi_device->controller_state = NULL;
		spi_device->controller_data = NULL;
		strlcpy(spi_device->modalias, this_driver_name, SPI_NAME_SIZE);
		status = spi_add_device(spi_device);
		
		if (status < 0) {	
			/* 
			 * Crashes without patched omap2_mcspi_cleanup() on
			 * kernels < 2.6.34 
			 */	
			spi_dev_put(spi_device);
			printk(KERN_ALERT "spi_add_device() failed: %d\n", 
				status);		
		}				
	}

	put_device(&spi_master->dev);

	return status;
}

static struct spi_driver scrap_spi = {
	.driver = {
		.name =	"scrap",
		.owner = THIS_MODULE,
	},
	.probe = scrap_probe,
	.remove = __devexit_p(scrap_remove),	
};

static int __init scrap_init_spi(void)
{
	int error;

	error = spi_register_driver(&scrap_spi);
	if (error < 0) {
		printk(KERN_ALERT "spi_register_driver() failed %d\n", error);
		return -1;
	}

	error = add_scrap_device_to_bus();
	if (error < 0) {
		printk(KERN_ALERT "add_scrap_to_bus() failed\n");
		spi_unregister_driver(&scrap_spi);		
	}

	return error;
}

static const struct file_operations scrap_fops = {
	.owner =	THIS_MODULE,
	.read = 	scrap_read,
	.write =	scrap_write,
	.open =		scrap_open,	
};

static int __init scrap_init_cdev(void)
{
	int error;

	scrap_dev.devt = MKDEV(0, 0);

	error = alloc_chrdev_region(&scrap_dev.devt, 0, 1, this_driver_name);
	if (error < 0) {
		printk(KERN_ALERT "alloc_chrdev_region() failed: %d \n", 
			error);
		return -1;
	}

	cdev_init(&scrap_dev.cdev, &scrap_fops);
	scrap_dev.cdev.owner = THIS_MODULE;
	
	error = cdev_add(&scrap_dev.cdev, scrap_dev.devt, 1);
	if (error) {
		printk(KERN_ALERT "cdev_add() failed: %d\n", error);
		unregister_chrdev_region(scrap_dev.devt, 1);
		return -1;
	}	

	return 0;
}

static int __init scrap_init_class(void)
{
	scrap_dev.class = class_create(THIS_MODULE, this_driver_name);

	if (!scrap_dev.class) {
		printk(KERN_ALERT "class_create() failed\n");
		return -1;
	}

	if (!device_create(scrap_dev.class, NULL, scrap_dev.devt, NULL, 	
			this_driver_name)) {
		printk(KERN_ALERT "device_create(..., %s) failed\n",
			this_driver_name);
		class_destroy(scrap_dev.class);
		return -1;
	}

	return 0;
}

static int __init scrap_init(void)
{
	memset(&scrap_dev, 0, sizeof(struct scrap_dev));
	memset(&scrap_msg, 0, sizeof(struct scrap_message));

	sema_init(&scrap_dev.spi_sem, 1);
	sema_init(&scrap_dev.fop_sem, 1);
	
	if (scrap_init_cdev() < 0) 
		goto fail_1;
	
	if (scrap_init_class() < 0)  
		goto fail_2;

	if (scrap_init_spi() < 0) 
		goto fail_3;

	hrtimer_init(&scrap_dev.timer, CLOCK_MONOTONIC, HRTIMER_MODE_REL);
	scrap_dev.timer.function = timer_callback; 

	return 0;

fail_3:
	device_destroy(scrap_dev.class, scrap_dev.devt);
	class_destroy(scrap_dev.class);

fail_2:
	cdev_del(&scrap_dev.cdev);
	unregister_chrdev_region(scrap_dev.devt, 1);

fail_1:
	return -1;
}

static void __exit scrap_exit(void)
{
	spi_unregister_driver(&scrap_spi);

	device_destroy(scrap_dev.class, scrap_dev.devt);
	class_destroy(scrap_dev.class);

	cdev_del(&scrap_dev.cdev);
	unregister_chrdev_region(scrap_dev.devt, 1);

	if (scrap_msg.tx_buff)
		kfree(scrap_msg.tx_buff);

	if (scrap_dev.user_buff)
		kfree(scrap_dev.user_buff);
}

module_init(scrap_init);
module_exit(scrap_exit);

MODULE_AUTHOR("Scott Ellis");
MODULE_DESCRIPTION("SPI test scrap driver");
MODULE_LICENSE("GPL");

