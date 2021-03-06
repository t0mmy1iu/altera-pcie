//
// Copyright (C) 2014, 2017 Chris McClelland
// Copyright (C) 2008 Leon Woestenberg    <leon.woestenberg@axon.tv>
// Copyright (C) 2008 Nickolas Heppermann <heppermannwdt@gmail.com>
//
// This program is free software: you can redistribute it and/or modify it under the terms of the
// GNU General Public License as published by the Free Software Foundation, either version 3 of
// the License, or (at your option) any later version.
//
// This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY;
// without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See
// the GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License along with this program. If
// not, see <http://www.gnu.org/licenses/>.
//
#include <linux/sched.h>
#include <linux/kernel.h>
#include <linux/fs.h>
#include <linux/cdev.h>
#include <linux/delay.h>
#include <linux/dma-mapping.h>
#include <linux/init.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/jiffies.h>
#include <linux/module.h>
#include <linux/pci.h>
#include "ioctl_defs.h"

// The number of DMA buffers to use in the circular queue
#define NUM_BUFS 32

// Allow numeric macros to be stringified by the preprocessor
#define STR(a) _STR(a)
#define _STR(a) #a

// These attributes were removed as of kernel 3.8: define them empty for now
#define __devinit
#define __devexit

// FPGA hardware registers
#define DMABASE(x) ((x)+0*2+1)
#define DMACTRL(x) ((x)+1*2+1)

// Driver name
#define DRV_NAME "fpgalink"

// Number of BARs on the device
#define APE_BAR_NUM (1)

// Specifies those BARs to be mapped and the length of each mapping.
//
// Zero (0) means do not map, otherwise specifies the BAR lengths to be mapped.
// If the actual BAR length is less, this is considered an error; then
// reconfigure your PCIe core.
//
// @see ug_pci_express 8.0, table 7-2 at page 7-13.
//
static const unsigned long barMinLen[APE_BAR_NUM] = {
	256
};

// The form of this struct is known also to ip/pcie/tlp_core.vhdl, so if you
// edit it here, you'll probably have to edit something there too. One important
// thing to remember is that each buffer must be aligned to a 128-byte (TLP)
// boundary, otherwise you get weird kernel hangs.
//
struct Buffer {
	u8 data[BUF_SIZE];
};

// Altera PCI Express ('ape') board specific book keeping data
//
// Keeps state of the PCIe core and the Chaining DMA controller
// application.
//
struct AlteraDevice {
	// The kernel pci device data structure provided by probe()
	struct pci_dev *pciDevice;

	// Kernel virtual address of the mapped BAR memory and IO regions of
	// the End Point. Used by mapBars()/unmapBars().
	//
	void __iomem *bar[APE_BAR_NUM];

	// Board revision
	u8 revision;

	// Array of NUM_BUFS blocks implementing a circular buffer
	struct Buffer *bufferArrayVirt;
	dma_addr_t bufferArrayBus;

	// Circular buffer metadata and spinlock
	u32 numAvailable, numSubmitted, outIndex;
	spinlock_t lock;
	wait_queue_head_t wq;

	// Character device and major number
	int major;
	struct cdev charDevice;
};

// Using the subsystem vendor id and subsystem id, it is possible to
// distinguish between different cards bases around the same
// (third-party) logic core.
//
// Default Altera vendor and device ID's, and some (non-reserved)
// ID's are now used here that are used amongst the testers/developers.
//
static const struct pci_device_id ids[] = {
	{ PCI_DEVICE(0x1172, 0xE001), },
	{ PCI_DEVICE(0x2071, 0x2071), },
	{ 0, }
};
MODULE_DEVICE_TABLE(pci, ids);

// Used to register the driver with the PCI kernel subsystem (see LDD3 page 311)
//
static int  __devinit pcieProbe(struct pci_dev *dev, const struct pci_device_id *id);
static void __devexit pcieRemove(struct pci_dev *dev);
static struct pci_driver pciDriver = {
	.name = DRV_NAME,
	.id_table = ids,
	.probe = pcieProbe,
	.remove = pcieRemove
};

// Callbacks for file operations on /dev/fpga0
//
static int cdevOpen(struct inode *inode, struct file *filp);
static int cdevRelease(struct inode *inode, struct file *filp);
static ssize_t cdevRead(struct file *filp, char __user *buf, size_t count, loff_t *filePos);
static long cdevIOCtl(struct file *filp, unsigned int cmd, unsigned long arg);
static const struct file_operations cdevFileOps = {
	.owner          = THIS_MODULE,
	.open           = cdevOpen,
	.release        = cdevRelease,
	.read           = cdevRead,
	.unlocked_ioctl = cdevIOCtl
};

// Submit a DMA request for the given number of TLPs at the specified address
//
static inline void submitDmaReq(struct AlteraDevice *ape, dma_addr_t addr, u32 numTLPs) {
	u32 __iomem *const regSpace = (u32 __iomem *)ape->bar[0];
	iowrite32((u32)addr, DMABASE(regSpace));
	iowrite32(numTLPs, DMACTRL(regSpace));
}

// Interrupt service routine
//
static irqreturn_t serviceInterrupt(int irq, void *devID) {
	struct AlteraDevice *const ape = (struct AlteraDevice *)devID;
	u32 submitCount, submitIndex;
	unsigned long flags;
	if ( !ape ) {
		return IRQ_NONE;
	}
	spin_lock_irqsave(&ape->lock, flags);
	ape->numAvailable++;
	ape->numSubmitted--;
	submitCount = NUM_BUFS - ape->numAvailable - ape->numSubmitted;
	submitIndex = ape->outIndex + ape->numAvailable;
	submitIndex &= NUM_BUFS - 1;
	while ( submitCount-- ) {
		submitDmaReq(ape, ape->bufferArrayBus + submitIndex * sizeof(struct Buffer), BUF_SIZE/128);
		submitIndex++;
		submitIndex &= NUM_BUFS - 1;
		ape->numSubmitted++;
	}
	spin_unlock_irqrestore(&ape->lock, flags);
	wake_up_interruptible(&ape->wq);
	return IRQ_HANDLED;
}

// Unmap the BAR regions that had been mapped earlier using mapBars()
//
static void unmapBars(struct AlteraDevice *ape, struct pci_dev *dev) {
	int i;
	for ( i = 0; i < APE_BAR_NUM; i++ ) {
		if ( ape->bar[i] ) {
			pci_iounmap(dev, ape->bar[i]);
			ape->bar[i] = NULL;
		}
	}
}

// Map the device memory regions into kernel virtual address space after
// verifying their sizes respect the minimum sizes needed, given by the
// barMinLen[] array.
//
// TODO: Sort out return code mess!
//
static int __devinit mapBars(struct AlteraDevice *ape, struct pci_dev *dev) {
	int rc;
	int i;

	// Iterate through all the BARs
	for ( i = 0; i < APE_BAR_NUM; i++ ) {
		unsigned long barStart = pci_resource_start(dev, i);
		unsigned long barEnd = pci_resource_end(dev, i);
		unsigned long barLength = barEnd - barStart + 1;
		ape->bar[i] = NULL;
		// do not map, and skip, BARs with length 0
		if ( !barMinLen[i] ) {
			continue;
		}

		// Do not map BARs with address 0
		if ( !barStart || !barEnd ) {
			printk(KERN_DEBUG "BAR #%d is not present?!\n", i);
			rc = -1; goto fail;
		}
		barLength = barEnd - barStart + 1;

		// BAR length is less than driver requires?
		if ( barLength < barMinLen[i] ) {
			printk(
				KERN_DEBUG "BAR #%d length = %lu bytes but driver requires at least %lu bytes\n",
				i, barLength, barMinLen[i]
			);
			rc = -1; goto fail;
		}

		// Map the device memory or IO region into kernel virtual address space
		ape->bar[i] = pci_iomap(dev, i, barMinLen[i]);
		if ( !ape->bar[i] ) {
			printk(KERN_DEBUG "Could not map BAR #%d.\n", i);
			rc = -1; goto fail;
		}
		printk(KERN_DEBUG "BAR[%d] mapped at 0x%p with length %lu(/%lu).\n", i,
		ape->bar[i], barMinLen[i], barLength);
	}
	// Successfully mapped all required BAR regions
	return 0;
fail:
	// Unmap any BARs that we did map
	unmapBars(ape, dev);
	return rc;
}

// Dump some info about each BAR to syslog
//
static int __devinit scanBars(struct AlteraDevice *ape, struct pci_dev *dev) {
	int i;
	for ( i = 0; i < APE_BAR_NUM; i++ ) {
		const unsigned long barStart = pci_resource_start(dev, i);
		if ( barStart ) {
			const unsigned long barEnd = pci_resource_end(dev, i);
			const unsigned long barFlags = pci_resource_flags(dev, i);
			printk(
				KERN_DEBUG "BAR%d 0x%08lx-0x%08lx flags 0x%08lx\n",
				i, barStart, barEnd, barFlags
			);
		}
	}
	return 0;
}

// Called when the PCI subsystem thinks we can control the given device. Inspect
// if we can support the device and if so take control of it.
//
// Return 0 when we have taken control of the given device.
//
// - allocate board specific bookkeeping
// - enable the board
// - verify board revision
// - request regions
// - query DMA mask
// - obtain and request irq
// - map regions into kernel address space
// - allocate DMA buffer
// - allocate char driver major/minor
//
static int __devinit pcieProbe(struct pci_dev *dev, const struct pci_device_id *id) {
	int rc, devno, alreadyInUse = 0;
	struct AlteraDevice *ape = NULL;
	dev_t charDevice;
	printk(KERN_DEBUG "pcieProbe(dev = 0x%p, pciid = 0x%p)\n", dev, id);

	// Check alignment of Buffer struct. Ideally this check should be done at compile-time.
	if ( sizeof(struct Buffer) % 128 ) {
		printk(KERN_DEBUG "Buffer length is %zu, which will not align to a 128-byte boundary!\n", sizeof(struct Buffer));
		rc = -ENODEV; goto err_align;
	}

	// Allocate memory for per-board bookkeeping
	ape = kzalloc(sizeof(struct AlteraDevice), GFP_KERNEL);
	if ( !ape ) {
		printk(KERN_DEBUG "kzalloc() of struct AlteraDevice failed!\n");
		rc = -ENOMEM; goto err_ape;
	}
	ape->pciDevice = dev;
	dev_set_drvdata(&dev->dev, ape);
	printk(KERN_DEBUG "pcieProbe() ape = 0x%p\n", ape);

	// Enable device
	rc = pci_enable_device(dev);
	if ( rc ) {
		printk(KERN_DEBUG "pci_enable_device() failed (rc=%d)!\n", rc);
		goto err_enable;
	}

	// Enable bus master capability on device
	pci_set_master(dev);

	// Enable message signaled interrupts
	rc = pci_enable_msi(dev);
	if ( rc ) {
		printk(KERN_DEBUG "pci_enable_msi() failed (rc=%d)!\n", rc);
		goto err_msi;
	}

	// Get the revision ID (specified in QSys when PCIe IP is generated)
	pci_read_config_byte(dev, PCI_REVISION_ID, &ape->revision);

	// Reserve I/O regions for all BARs
	rc = pci_request_regions(dev, DRV_NAME);
	if ( rc ) {
		alreadyInUse = 1;
		goto err_regions;
	}

	// Set appropriate DMA mask
	if ( !pci_set_dma_mask(dev, DMA_BIT_MASK(64)) ) {
		pci_set_consistent_dma_mask(dev, DMA_BIT_MASK(64));
		printk(KERN_DEBUG "Using a 64-bit DMA mask.\n");
	} else if ( !pci_set_dma_mask(dev, DMA_BIT_MASK(32)) ) {
		pci_set_consistent_dma_mask(dev, DMA_BIT_MASK(32));
		printk(KERN_DEBUG "Using a 32-bit DMA mask.\n");
	} else {
		printk(KERN_DEBUG "pci_set_dma_mask() fails for both 32-bit and 64-bit DMA!\n");
		rc = -ENODEV; goto err_mask;
	}

	// Request an IRQ (see LDD3 page 259)
	rc = request_irq(dev->irq, serviceInterrupt, IRQF_SHARED, DRV_NAME, (void*)ape);
	if ( rc ) {
		printk(KERN_DEBUG "request_irq(%d, ...) failed (rc=%d)!\n", dev->irq, rc);
		goto err_irq;
	}

	// Show BARs in syslog
	scanBars(ape, dev);

	// Map BARs
	rc = mapBars(ape, dev);
	if ( rc ) {
		goto err_map;
	}

	// Allocate and map coherently-cached memory for a DMA-able buffer (see
	// Documentation/PCI/PCI-DMA-mapping.txt, near line 318)
	//
	ape->bufferArrayBus = 0;
	ape->bufferArrayVirt = (struct Buffer *)pci_alloc_consistent(
		dev, NUM_BUFS*sizeof(struct Buffer), &ape->bufferArrayBus
	);
	if ( !ape->bufferArrayVirt ) {
		printk(KERN_DEBUG "Could not allocate coherent DMA buffer!\n");
		rc = -ENOMEM; goto err_buf_alloc;
	}
	printk(
		KERN_DEBUG "Allocated cache-coherent DMA buffer (virt: %p; bus: 0x%016llX).\n",
		ape->bufferArrayVirt, (u64)ape->bufferArrayBus
	);

	// Allocate char driver major/minor
	rc = alloc_chrdev_region(&charDevice, 0, 1, "fpga0");
	if ( rc ) {
		printk(KERN_ERR "alloc_chrdev_region() failed (rc=%d)\n", rc);
		goto err_cdev_alloc;
	}
	ape->major = MAJOR(charDevice);
	devno = MKDEV(ape->major, 0);

	// Initialise char device
	cdev_init(&ape->charDevice, &cdevFileOps);
	ape->charDevice.owner = THIS_MODULE;
	ape->charDevice.ops = &cdevFileOps;

	// Add a single device node
	rc = cdev_add(&ape->charDevice, devno, 1);
	if ( rc ) {
		printk(KERN_ERR "cdev_add() failed (rc=%d)\n", rc);
		goto err_cdev_add;
	}

	// Wait queue
	init_waitqueue_head(&ape->wq);

	// Successfully took the device
	printk(KERN_DEBUG "pcieProbe() successful.\n");
	return 0;
err_cdev_add:
	unregister_chrdev_region(devno, 1);
err_cdev_alloc:
	pci_free_consistent(dev, NUM_BUFS*sizeof(struct Buffer), (u8*)ape->bufferArrayVirt, ape->bufferArrayBus);
err_buf_alloc:
	unmapBars(ape, dev);
err_map:
	free_irq(dev->irq, (void*)ape);
err_irq:
err_mask:
	pci_release_regions(dev);
err_regions:
	pci_disable_msi(dev);
err_msi:
	if ( alreadyInUse ) {
		pci_disable_device(dev); // only disable the device if we're sure it's really ours
	}
err_enable:
	kfree(ape);
err_align:
err_ape:
	return rc;
}

// Called when the module is removed with rmmod
//
static void __devexit pcieRemove(struct pci_dev *dev) {
	struct AlteraDevice *const ape = dev_get_drvdata(&dev->dev);
	const dev_t devno = MKDEV(ape->major, 0);

	printk(KERN_DEBUG "pcieRemove(dev = 0x%p) where ape = 0x%p\n", dev, ape);

	// Remove the char device node
	cdev_del(&ape->charDevice);

	// Unregister char device
	unregister_chrdev_region(devno, 1);	

	// Free DMA buffer
	pci_free_consistent(dev, NUM_BUFS*sizeof(struct Buffer), (u8 *)ape->bufferArrayVirt, ape->bufferArrayBus);

	// Unmap the BARs
	unmapBars(ape, dev);

	// Free IRQ
	free_irq(dev->irq, (void*)ape);

	// Release BAR mappings
	pci_release_regions(dev);

	// Disable MSI
	pci_disable_msi(dev);

	// Disable the PCIe device
	pci_disable_device(dev);

	// Free the ape structure
	kfree(ape);
}

// Userspace is opening the device
//
static int cdevOpen(struct inode *inode, struct file *filp) {
	struct AlteraDevice *const ape = container_of(inode->i_cdev, struct AlteraDevice, charDevice);
	filp->private_data = ape;	
	printk(KERN_DEBUG "cdevOpen()\n");
	ape->numAvailable = ape->outIndex = ape->numSubmitted = 0;
	return 0;
}

// Userspace is closing the device
//
static int cdevRelease(struct inode *inode, struct file *filp) {
	printk(KERN_DEBUG "cdevRelease()\n");
	return 0;
}

// Userspace is asking for data
//
static ssize_t cdevRead(struct file *filp, char __user *buf, size_t count, loff_t *filePos) {
	struct AlteraDevice *ape = filp->private_data;
	unsigned long rc, flags;
	if ( count < BUF_SIZE ) {
		printk(KERN_DEBUG "cdevRead(): can't read into a buffer smaller than " STR(BUF_SIZE) " bytes!\n");
		return -EINVAL;
	}
	wait_event_interruptible(ape->wq, ape->numAvailable > 0);
	rc = copy_to_user(buf, ape->bufferArrayVirt[ape->outIndex].data, BUF_SIZE);
	spin_lock_irqsave(&ape->lock, flags);
	submitDmaReq(ape, ape->bufferArrayBus + ape->outIndex * sizeof(struct Buffer), BUF_SIZE/128);
	ape->numSubmitted++;
	ape->outIndex++;
	ape->outIndex &= NUM_BUFS - 1;
	ape->numAvailable--;
	spin_unlock_irqrestore(&ape->lock, flags);
	return BUF_SIZE;
	(void)filePos;
	(void)rc;
}

// The ioctl() implementation
//
static long cdevIOCtl(struct file *filp, unsigned int cmd, unsigned long arg) {
	struct AlteraDevice *const ape = filp->private_data;
	u32 __iomem *const regSpace = (u32 __iomem *)ape->bar[0];
	struct CmdList kl;
	struct Cmd kc;
	struct Cmd __user *ucp;
	u32 numCmds, reg;
	int err = 0;

	// Extract the type and number bitfields, and don't decode
	// wrong cmds: return ENOTTY (inappropriate ioctl) before access_ok()
	//
	if ( _IOC_TYPE(cmd) != FPGALINK_IOC_MAGIC ) {
		return -ENOTTY;
	}
	if ( _IOC_NR(cmd) > FPGALINK_IOC_MAXNR ) {
		return -ENOTTY;
	}

	//
	// the direction is a bitmask, and VERIFY_WRITE catches R/W
	// transfers. `Type' is user-oriented, while
	// access_ok is kernel-oriented, so the concept of "read" and
	// "write" is reversed
	//
	if ( _IOC_DIR(cmd) & _IOC_READ ) {
		err = !access_ok(VERIFY_WRITE, (void __user *)arg, _IOC_SIZE(cmd));
	} else if ( _IOC_DIR(cmd) & _IOC_WRITE ) {
		err =  !access_ok(VERIFY_READ, (void __user *)arg, _IOC_SIZE(cmd));
	}
	if ( err ) {
		return -EFAULT;
	}

	switch ( cmd ) {
	case FPGALINK_CMDLIST:
		err = copy_from_user(&kl, (struct CmdList __user *)arg, sizeof(struct CmdList));
		if ( err ) {
			return -EFAULT;
		}
		numCmds = kl.numCmds;
		ucp = (struct Cmd __user *)kl.cmds;
		while ( numCmds ) {
			err = copy_from_user(&kc, ucp, sizeof(struct Cmd));
			if ( err ) {
				return -EFAULT;
			}
			reg = 1 + kc.reg * 2;
			if ( kc.op == OP_RD ) {
				// Read the specified register and copy the result over to userspace
				err = put_user(ioread32(regSpace + reg), &ucp->val);
				if ( err ) {
					return -EFAULT;
				}
			} else if ( kc.op == OP_WR ) {
				// Write to the specified register
				iowrite32(kc.val, regSpace + reg);
			} else if ( kc.op == OP_SD ) {
				// Start DMA
				ape->numAvailable = ape->outIndex = 0;
				ape->numSubmitted = 1;
				submitDmaReq(ape, ape->bufferArrayBus, BUF_SIZE/128);
			} else {
				// Unrecognised operation
				return -EFAULT;
			}
			ucp++;
			numCmds--;
		}
		break;
	}
	return 0;
}

// Module initialization, registers devices.
//
static int __init flInit(void) {
	int rc;
	printk(KERN_DEBUG DRV_NAME " flInit(), built at " __DATE__ " " __TIME__ "\n");

	// register this driver with the PCI bus driver
	rc = pci_register_driver(&pciDriver);
	if ( rc < 0 ) {
		return rc;
	}
	return 0;
}

// Module cleanup, unregisters devices.
//
static void __exit flExit(void) {
	printk(KERN_INFO DRV_NAME " flExit(), built at " __DATE__ " " __TIME__ "\n");

	// Unregister PCIe driver
	pci_unregister_driver(&pciDriver);
}

MODULE_LICENSE("GPL");

module_init(flInit);
module_exit(flExit);
