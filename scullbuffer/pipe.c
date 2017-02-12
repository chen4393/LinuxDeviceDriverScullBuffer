/*
 * pipe.c -- fifo driver for scull
 *
 * Copyright (C) 2001 Alessandro Rubini and Jonathan Corbet
 * Copyright (C) 2001 O'Reilly & Associates
 *
 * The source code in this file can be freely used, adapted,
 * and redistributed in source or binary form, so long as an
 * acknowledgment appears in derived source files.  The citation
 * should list that the code comes from the book "Linux Device
 * Drivers" by Alessandro Rubini and Jonathan Corbet, published
 * by O'Reilly & Associates.   No warranty is attached;
 * we cannot take responsibility for errors or fitness for use.
 *
 */
 
#include <linux/sched.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/kernel.h>	/* printk(), min() */
#include <linux/slab.h>		/* kmalloc() */
#include <linux/fs.h>		/* everything... */
#include <linux/proc_fs.h>
#include <linux/errno.h>	/* error codes */
#include <linux/types.h>	/* size_t */
#include <linux/fcntl.h>
#include <linux/poll.h>
#include <linux/cdev.h>
#include <asm/uaccess.h>
#include "scull.h"		/* local definitions */


#define init_MUTEX(_m) sema_init(_m, 1);


struct scull_pipe {
        wait_queue_head_t inq, outq;       /* read and write queues */
        char *buffer, *end;                /* begin of buf, end of buf */
        int buffersize;                    /* used in pointer arithmetic */
        int rp, wp;                     /* where to read, where to write */
        int nreaders, nwriters, num;            /* number of openings for r/w */
        struct semaphore sem;              /* mutual exclusion semaphore */
struct semaphore empty;
struct semaphore full;
        struct cdev cdev;                  /* Char device structure */
};

/* parameters */
static int scull_p_nr_devs = SCULL_P_NR_DEVS;	/* number of pipe devices */
int scull_p_buffer =  SCULL_P_BUFFER;	/* buffer size */
dev_t scull_p_devno;			/* Our first device number */

module_param(scull_p_nr_devs, int, 0);	/* FIXME check perms */
module_param(scull_p_buffer, int, 0);

static struct scull_pipe *scull_p_devices;

static int spacefree(struct scull_pipe *dev);
/*
 * Open and close
 */
static int scull_p_open(struct inode *inode, struct file *filp)
{
	struct scull_pipe *dev;

	dev = container_of(inode->i_cdev, struct scull_pipe, cdev);
	filp->private_data = dev;

	if (down_interruptible(&dev->sem))
		return -ERESTARTSYS;
	if (!dev->buffer) {
		/* allocate the buffer */
		dev->buffer = kmalloc(scull_p_buffer * 32, GFP_KERNEL);
		if (!dev->buffer) {
			up(&dev->sem);
			return -ENOMEM;
		}
		dev->buffersize = scull_p_buffer * 32;
		dev->end = dev->buffer + dev->buffersize;
		dev->rp = dev->wp = 0; /* rd and wr from the beginning */
		dev->num = 0;
		sema_init(&dev->empty, scull_p_buffer);
		sema_init(&dev->full, 0);
	}

	/* use f_mode,not  f_flags: it's cleaner (fs/open.c tells why) */
	if (filp->f_mode & FMODE_READ)
		dev->nreaders++;
	if (filp->f_mode & FMODE_WRITE)
		dev->nwriters++;
	up(&dev->sem);

	return nonseekable_open(inode, filp);
}

static int scull_p_release(struct inode *inode, struct file *filp)
{
	struct scull_pipe *dev = filp->private_data;
	int i;
	down(&dev->sem);
	if (filp->f_mode & FMODE_READ) {
		dev->nreaders--;
		if(dev->nreaders == 0 && dev->num == scull_p_buffer && dev->nwriters > 0) {
			for(i = 0; i < dev->nwriters; i++)//broadcast
				up(&dev->empty);
		}
	}
	if (filp->f_mode & FMODE_WRITE) {
		dev->nwriters--;
		if(dev->nwriters == 0 && dev->num == 0 && dev->nreaders > 0) {
			for(i = 0; i < dev->nreaders; i++)//broadcast
				up(&dev->full);
		}
	}
	
	if (dev->nreaders + dev->nwriters == 0) {
		kfree(dev->buffer);
		dev->buffer = NULL; /* the other fields are not checked on open */
	}
	up(&dev->sem);
	return 0;
}

/*
 * Data management: read and write
*/
static ssize_t scull_p_read(struct file *filp, char __user *buf, size_t count, loff_t *f_pos)
{
	struct scull_pipe *dev = filp->private_data;
	
	if (down_interruptible(&dev->sem))
		return -1;
	if (dev->nwriters == 0 && dev->num == 0) {
		up (&dev->sem);
		return 0;
	}
	
	if (dev->num > 0 || (dev->nwriters > 0 && dev->num == 0)) {
		up (&dev->sem);
		if (down_interruptible(&dev->full))
			return -1;
		if (down_interruptible(&dev->sem))
			return -1;
		if (dev->num > 0) {
			count = min(count, (size_t)32);
			if (copy_to_user(buf, dev->buffer + dev->rp, count)) {
				up (&dev->sem);
				return -1;
			}
			printk("count = %ld in consumer\n", count);
			dev->rp = (dev->rp + 32) % dev->buffersize;
			PDEBUG("\" (scull_p_read) dev->wp:%d    dev->rp:%d\" \n",dev->wp,dev->rp);
			dev->num--;
			printk("num = %d in consumer\n", dev->num);
			up (&dev->sem);
			up(&dev->empty);
			PDEBUG("\"%s\" did read %li bytes\n",current->comm, (long)count);
			return count;
		}
	}
	up(&dev->sem);
	return 0;
}

/* How much space is free? */
static int spacefree(struct scull_pipe *dev)
{
	if (dev->rp == dev->wp)
		return dev->buffersize - 1;
	return ((dev->rp + dev->buffersize - dev->wp) % dev->buffersize) - 1;
}

static ssize_t scull_p_write(struct file *filp, const char __user *buf, size_t count, loff_t *f_pos)
{
	struct scull_pipe *dev = filp->private_data;
	
	if (down_interruptible(&dev->sem))
		return -1;
	if (dev->nreaders == 0 && dev->num == scull_p_buffer) {
		up(&dev->sem);
		return 0;
	}
	if (dev->num < scull_p_buffer || (dev->nreaders > 0 && dev->num == scull_p_buffer)) {
		up(&dev->sem);
		if (down_interruptible(&dev->empty))
			return -1;
		if (down_interruptible(&dev->sem))
			return -1;
		if (dev->num < scull_p_buffer) {
			/* ok, space is there, accept something */
			count = min(count, (size_t)32);
			PDEBUG("Going to accept %li bytes to %d from %p\n", (long)count, dev->wp, buf);
			if (copy_from_user(dev->buffer + dev->wp, buf, count)) {
				up (&dev->sem);
				return -1;
			}
			printk("count = %ld in producer\n", count);
			dev->wp = (dev->wp + 32) % dev->buffersize;
			dev->num++;
			printk("num = %d in producer\n", dev->num);

			PDEBUG("\" (scull_p_write) dev->wp:%d    dev->rp:%d\" \n",dev->wp,dev->rp);
			up(&dev->sem);
			up(&dev->full);

			PDEBUG("\"%s\" did write %li bytes\n",current->comm, (long)count);
			up(&dev->sem);
			return count;
		}
	}
	up(&dev->sem);
	return 0;
}

static unsigned int scull_p_poll(struct file *filp, poll_table *wait)
{
	struct scull_pipe *dev = filp->private_data;
	unsigned int mask = 0;

	/*
	 * The buffer is circular; it is considered full
	 * if "wp" is right behind "rp" and empty if the
	 * two are equal.
	 */
	down(&dev->sem);
	poll_wait(filp, &dev->inq,  wait);
	poll_wait(filp, &dev->outq, wait);
	if (dev->rp != dev->wp)
		mask |= POLLIN | POLLRDNORM;	/* readable */
	if (spacefree(dev))
		mask |= POLLOUT | POLLWRNORM;	/* writable */
	up(&dev->sem);
	return mask;
}

/*
 * The file operations for the pipe device
 * (some are overlayed with bare scull)
 */
struct file_operations scull_pipe_fops = {
	.owner =	THIS_MODULE,
	.llseek =	no_llseek,
	.read =		scull_p_read,
	.write =	scull_p_write,
	.poll =		scull_p_poll,
	.open =		scull_p_open,
	.release =	scull_p_release,
};

/*
 * Set up a cdev entry.
 */
static void scull_p_setup_cdev(struct scull_pipe *dev, int index)
{
	int err, devno = scull_p_devno + index;
    
	cdev_init(&dev->cdev, &scull_pipe_fops);
	dev->cdev.owner = THIS_MODULE;
	err = cdev_add (&dev->cdev, devno, 1);
	/* Fail gracefully if need be */
	if (err)
		printk(KERN_NOTICE "Error %d adding scullpipe%d", err, index);
}

/*
 * Initialize the pipe devs; return how many we did.
 */
int scull_p_init(dev_t firstdev)
{
	int i, result;

	result = register_chrdev_region(firstdev, scull_p_nr_devs, "scullp");
	if (result < 0) {
		printk(KERN_NOTICE "Unable to get scullp region, error %d\n", result);
		return 0;
	}
	scull_p_devno = firstdev;
	scull_p_devices = kmalloc(scull_p_nr_devs * sizeof(struct scull_pipe), GFP_KERNEL);
	if (scull_p_devices == NULL) {
		unregister_chrdev_region(firstdev, scull_p_nr_devs);
		return 0;
	}
	memset(scull_p_devices, 0, scull_p_nr_devs * sizeof(struct scull_pipe));
	for (i = 0; i < scull_p_nr_devs; i++) {
		init_waitqueue_head(&(scull_p_devices[i].inq));
		init_waitqueue_head(&(scull_p_devices[i].outq));
		init_MUTEX(&scull_p_devices[i].sem);
//sema_init(&scull_p_devices[i].empty, scull_p_buffer);
//sema_init(&scull_p_devices[i].full, 0);

		scull_p_setup_cdev(scull_p_devices + i, i);
	}
	return scull_p_nr_devs;
}

/*
 * This is called by cleanup_module or on failure.
 * It is required to never fail, even if nothing was initialized first
 */
void scull_p_cleanup(void)
{
	int i;

	if (!scull_p_devices)
		return; /* nothing else to release */

	for (i = 0; i < scull_p_nr_devs; i++) {
		cdev_del(&scull_p_devices[i].cdev);
		kfree(scull_p_devices[i].buffer);
	}
	kfree(scull_p_devices);
	unregister_chrdev_region(scull_p_devno, scull_p_nr_devs);
	scull_p_devices = NULL; /* pedantic */
}
