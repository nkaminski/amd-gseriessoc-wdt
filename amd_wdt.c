/*****************************************************************************
*
* Copyright (c) 2014, Advanced Micro Devices, Inc.   
* All rights reserved.   
*
* Redistribution and use in source and binary forms, with or without   
* modification, are permitted provided that the following conditions are met:   
*     * Redistributions of source code must retain the above copyright   
*       notice, this list of conditions and the following disclaimer.   
*     * Redistributions in binary form must reproduce the above copyright   
*       notice, this list of conditions and the following disclaimer in the   
*       documentation and/or other materials provided with the distribution.   
*     * Neither the name of Advanced Micro Devices, Inc. nor the names of    
*       its contributors may be used to endorse or promote products derived    
*       from this software without specific prior written permission.   
*    
* THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND   
* ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED   
* WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE   
* DISCLAIMED. IN NO EVENT SHALL ADVANCED MICRO DEVICES, INC. BE LIABLE FOR ANY   
* DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES   
* (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;   
* LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND   
* ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT   
* (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS   
* SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.   
*    
*   
***************************************************************************/
#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/types.h>
#include <linux/miscdevice.h>
#include <linux/watchdog.h>
#include <linux/init.h>
#include <linux/fs.h>
#include <linux/pci.h>
#include <linux/ioport.h>
#include <linux/platform_device.h>
#include <linux/uaccess.h>
#include <linux/io.h>

#include "amd_wdt.h"

/* Module and version information */
#define WDT_VERSION "0.1"
#define WDT_MODULE_NAME "AMD watchdog timer"
#define WDT_DRIVER_NAME   WDT_MODULE_NAME ", v" WDT_VERSION

#define MIN_TIME	1
#define MAX_TIME	(10 * 60) /* 10 minutes */

/* internal variables */
static u32 wdtbase_phys;
static void __iomem *wdtbase;
static DEFINE_SPINLOCK(wdt_lock);
static unsigned int bootstatus;
static unsigned long status;
static struct pci_dev *amd_wdt_pci;

/* watchdog platform device */
static struct platform_device *amd_wdt_platform_device;

/* module parameters */
#define WATCHDOG_HEARTBEAT 60	/* 60 sec default heartbeat. */
static int heartbeat = WATCHDOG_HEARTBEAT;
module_param(heartbeat, int, 0);
MODULE_PARM_DESC(heartbeat, "Watchdog heartbeat in seconds. (default="
		 __MODULE_STRING(WATCHDOG_HEARTBEAT) ")");

static bool nowayout = WATCHDOG_NOWAYOUT;
module_param(nowayout, bool, 0);
MODULE_PARM_DESC(nowayout, "Watchdog cannot be stopped once started"
		" (default=" __MODULE_STRING(WATCHDOG_NOWAYOUT) ")");

#define MAX_LENGTH	9
static char action[MAX_LENGTH] = "reboot";
module_param_string(action, action, MAX_LENGTH, 0);
MODULE_PARM_DESC(action, "Watchdog action (reboot/shutdown). (default=reboot) ");

/*
 * Watchdog specific functions
 */
static int amd_wdt_timer_set_heartbeat(unsigned int t)
{
	unsigned long flags;

	if (t < 0 || t > 0xffff)
		return -EINVAL;

	/* Write new timeout value to watchdog */
	spin_lock_irqsave(&wdt_lock, flags);
	writel(t, AMD_WDT_COUNT(wdtbase));
	spin_unlock_irqrestore(&wdt_lock, flags);

	heartbeat = t;
	return 0;
}

static void amd_wdt_timer_activate(void)
{
	u32 val;
	unsigned long flags;

	/* Enable the watchdog timer */
	spin_lock_irqsave(&wdt_lock, flags);
	val = readl(AMD_WDT_CONTROL(wdtbase));
	val |= AMD_WDT_START_STOP_BIT;
	writel(val, AMD_WDT_CONTROL(wdtbase));
	set_bit(AMD_WDOG_ACTIVE, &status);
	spin_unlock_irqrestore(&wdt_lock, flags);
}

static void amd_wdt_timer_deactivate(void)
{
	u32 val;
	unsigned long flags;

	/* Disable the watchdog timer */
	spin_lock_irqsave(&wdt_lock, flags);
	val = readl(AMD_WDT_CONTROL(wdtbase));
	val &= ~AMD_WDT_START_STOP_BIT;
	writel(val, AMD_WDT_CONTROL(wdtbase));
	clear_bit(AMD_WDOG_ACTIVE, &status);
	spin_unlock_irqrestore(&wdt_lock, flags);
}

static void amd_wdt_timer_keepalive(void)
{
	u32 val;
	unsigned long flags;

	/* Trigger watchdog */
	spin_lock_irqsave(&wdt_lock, flags);
	val = readl(AMD_WDT_CONTROL(wdtbase));
	val |= AMD_WDT_TRIGGER_BIT;
	writel(val, AMD_WDT_CONTROL(wdtbase));
	spin_unlock_irqrestore(&wdt_lock, flags);
}

static unsigned int amd_wdt_timer_get_timeleft(void)
{
	u32 val;
	unsigned long flags;

	spin_lock_irqsave(&wdt_lock, flags);
	val = readl(AMD_WDT_COUNT(wdtbase));
	spin_unlock_irqrestore(&wdt_lock, flags);

	/* Mask out the upper 16-bits and return */
	return val & 0xffff;
}

/*
 *	/dev/watchdog handling
 */

static int amd_wdt_timer_open(struct inode *inode, struct file *file)
{
	/* /dev/watchdog can only be opened once */
	if (test_and_set_bit(AMD_WDOG_DEV_OPEN, &status))
		return -EBUSY;

	/* Start the watchdog timer and ping it once */
	amd_wdt_timer_activate();
	amd_wdt_timer_keepalive();

	return nonseekable_open(inode, file);
}

static int amd_wdt_timer_release(struct inode *inode, struct file *file)
{
	if (test_and_clear_bit(AMD_WDOG_ALLOW_RELEASE, &status)) {
		amd_wdt_timer_deactivate();
	} else {
		pr_crit("Unexpected close, not stopping watchdog!\n");
		amd_wdt_timer_keepalive();
	}

	clear_bit(AMD_WDOG_DEV_OPEN, &status);

	return 0;
}

static ssize_t amd_wdt_timer_write(struct file *file, const char __user *data,
				size_t len, loff_t *ppos)
{
	/* See if we got the magic character 'V' and reload the timer */
	if (len) {
		if (!nowayout) {
			size_t i;

			clear_bit(AMD_WDOG_ALLOW_RELEASE, &status);

			/*
			 * parse user buffer to see if we received magic
			 * character 'V'.
			 */
			for (i = 0; i != len; i++) {
				char c;

				if (get_user(c, data + i))
					return -EFAULT;
				if (c == 'V')
					set_bit(AMD_WDOG_ALLOW_RELEASE, &status);
			}
		}

		/* Reload the timer */
		amd_wdt_timer_keepalive();
	}
	return len;
}

static long amd_wdt_timer_ioctl(struct file *file, unsigned int cmd,
			     unsigned long arg)
{
	int new_options, retval = -EINVAL;
	int new_heartbeat;
	int timeleft;
	void __user *argp = (void __user *)arg;
	int __user *p = argp;
	static const struct watchdog_info ident = {
		.options =		WDIOF_SETTIMEOUT |
					WDIOF_KEEPALIVEPING |
					WDIOF_MAGICCLOSE,
		.firmware_version =	0,
		.identity =		WDT_MODULE_NAME,
	};

	switch (cmd) {
	case WDIOC_GETSUPPORT:
		return copy_to_user(argp, &ident,
			sizeof(ident)) ? -EFAULT : 0;
	case WDIOC_GETSTATUS:
		return put_user(status, p);
	case WDIOC_GETBOOTSTATUS:
		return put_user(bootstatus, p);
	case WDIOC_SETOPTIONS:
		if (get_user(new_options, p))
			return -EFAULT;
		if (new_options & WDIOS_DISABLECARD) {
			amd_wdt_timer_deactivate();
			retval = 0;
		}
		if (new_options & WDIOS_ENABLECARD) {
			amd_wdt_timer_activate();
			amd_wdt_timer_keepalive();
			retval = 0;
		}
		return retval;
	case WDIOC_KEEPALIVE:
		amd_wdt_timer_keepalive();
		return 0;
	case WDIOC_SETTIMEOUT:
		if (get_user(new_heartbeat, p))
			return -EFAULT;
		if (amd_wdt_timer_set_heartbeat(new_heartbeat))
			return -EINVAL;
		amd_wdt_timer_keepalive();
		/* Fall through */
	case WDIOC_GETTIMEOUT:
		return put_user(heartbeat, p);
	case WDIOC_GETTIMELEFT:
		timeleft = amd_wdt_timer_get_timeleft();
		return put_user(timeleft, p);
	default:
		return -ENOTTY;
	}
}

/*
 * Kernel Interfaces
 */

static const struct file_operations amd_wdt_fops = {
	.owner =		THIS_MODULE,
	.llseek =		no_llseek,
	.write =		amd_wdt_timer_write,
	.unlocked_ioctl = 	amd_wdt_timer_ioctl,
	.open =			amd_wdt_timer_open,
	.release =		amd_wdt_timer_release,
};

static struct miscdevice amd_wdt_miscdev = {
	.minor =	WATCHDOG_MINOR,
	.name =		"watchdog",
	.fops =		&amd_wdt_fops,
};

/*
 * The PCI Device ID table below is used to identify the platform
 * the driver is supposed to work for. Since this is a platform
 * device, we need a way for us to be able to find the correct
 * platform when the driver gets loaded, otherwise we should
 * bail out.
 */
static DEFINE_PCI_DEVICE_TABLE(amd_wdt_pci_tbl) = {
	{ PCI_VENDOR_ID_AMD, PCI_DEVICE_ID_AMD_HUDSON2_SMBUS, PCI_ANY_ID,
	  PCI_ANY_ID, },
	{ 0, },
};
MODULE_DEVICE_TABLE(pci, amd_wdt_pci_tbl);

static unsigned char amd_wdt_setupdevice(void)
{
	struct pci_dev *dev = NULL;
	u32 val;
	u8 *byte;

	/* Match the PCI device */
	for_each_pci_dev(dev) {
		if (pci_match_id(amd_wdt_pci_tbl, dev) != NULL) {
			amd_wdt_pci = dev;
			break;
		}
	}

	if (!amd_wdt_pci)
		return 0;

	/* Locate ACPI MMIO Base Address. */
	byte = (u8 *)&val;

	outb(AMD_PM_WATCHDOG_BASE0, AMD_IO_PM_INDEX_REG);
	byte[0] = inb(AMD_IO_PM_DATA_REG);
	outb(AMD_PM_WATCHDOG_BASE1, AMD_IO_PM_INDEX_REG);
	byte[1] = inb(AMD_IO_PM_DATA_REG);
	outb(AMD_PM_WATCHDOG_BASE2, AMD_IO_PM_INDEX_REG);
	byte[2] = inb(AMD_IO_PM_DATA_REG);
	outb(AMD_PM_WATCHDOG_BASE3, AMD_IO_PM_INDEX_REG);
	byte[3] = inb(AMD_IO_PM_DATA_REG);

	/* Bits 31:13 is the actual ACPI MMIO Base Address */
	val &= ~0x1FFF;

	/* Watchdog Base Address starts from ACPI MMIO Base Address + 0xB00 */
	val += 0xB00;

	if (!request_mem_region_exclusive(val, AMD_WDT_MEM_MAP_SIZE,
								"AMD Watchdog")) {
		pr_err("mmio address 0x%04x already in use\n", val);
		goto exit;
	}
	wdtbase_phys = val;

	wdtbase = ioremap(val, AMD_WDT_MEM_MAP_SIZE);
	if (!wdtbase) {
		pr_err("failed to get wdtbase address\n");
		goto unreg_mem_region;
	}

	/* Enable watchdog timer and decode bit */
	outb(AMD_PM_WATCHDOG_MISC_REG, AMD_IO_PM_INDEX_REG);
	val = inb(AMD_IO_PM_DATA_REG);
	val |= AMD_PM_WATCHDOG_DECODE_EN;
	val &= ~AMD_PM_WATCHDOG_DISABLE;
	outb(val, AMD_IO_PM_DATA_REG);

	/* Set the resolution to 1 sec. */
	outb(AMD_PM_WATCHDOG_CONTROL, AMD_IO_PM_INDEX_REG);
	val = inb(AMD_IO_PM_DATA_REG);
	val |= AMD_PM_WATCHDOG_SECOND_RES;
	outb(val, AMD_IO_PM_DATA_REG);

	/* Set the watchdog action depending on module load parameter. */
	val = readl(AMD_WDT_CONTROL(wdtbase));

	if (strncmp(action, "reboot", 6) == 0)
		val &= ~AMD_WDT_ACTION_RESET_BIT;
	else if (strncmp(action, "shutdown", 8) == 0)
		val |= AMD_WDT_ACTION_RESET_BIT;

	writel(val, AMD_WDT_CONTROL(wdtbase));

	return 1;

unreg_mem_region:
	release_mem_region(wdtbase_phys, AMD_WDT_MEM_MAP_SIZE);
exit:
	return 0;
}

static int amd_wdt_init(struct platform_device *dev)
{
	int ret;
	u32 val;

	/* Identify our device and initialize watchdog hardware */
	if (!amd_wdt_setupdevice())
		return -ENODEV;

	/* Check to see if last reboot was due to watchdog timeout */
	val = readl(AMD_WDT_CONTROL(wdtbase));
	if (val & AMD_WDT_FIRED_BIT)
		bootstatus |= WDIOF_CARDRESET;
	else
		bootstatus &= ~WDIOF_CARDRESET;

	pr_info("Watchdog reboot %sdetected\n",
		(val & AMD_WDT_FIRED_BIT) ? "" : "not ");

	/* Clear out the old status */
	val |= AMD_WDT_FIRED_BIT;
	writel(val, AMD_WDT_CONTROL(wdtbase));

	/* Set Watchdog timeout */
	amd_wdt_timer_set_heartbeat(heartbeat);

	ret = misc_register(&amd_wdt_miscdev);
	if (ret != 0) {
		pr_err("Watchdog timer: cannot register miscdevice on minor="
		       "%d (err=%d)\n", WATCHDOG_MINOR, ret);
		goto exit;
	}

	clear_bit(AMD_WDOG_DEV_OPEN, &status);
	if (nowayout)
		set_bit(AMD_WDOG_NO_WAY_OUT, &status);

	pr_info("initialized (0x%p). (heartbeat=%d sec) (nowayout=%d) "
		"(action=%s)\n", wdtbase, heartbeat, nowayout, action);

	return 0;

exit:
	iounmap(wdtbase);
	release_mem_region(wdtbase_phys, AMD_WDT_MEM_MAP_SIZE);
	return ret;
}

static void amd_wdt_cleanup(void)
{
	/* Stop the timer before we leave */
	if (!nowayout)
		amd_wdt_timer_deactivate();

	misc_deregister(&amd_wdt_miscdev);
	iounmap(wdtbase);
	release_mem_region(wdtbase_phys, AMD_WDT_MEM_MAP_SIZE);
}

static int amd_wdt_remove(struct platform_device *dev)
{
	if (wdtbase)
		amd_wdt_cleanup();
	return 0;
}

static void amd_wdt_shutdown(struct platform_device *dev)
{
	amd_wdt_timer_deactivate();
}

static struct platform_driver amd_wdt_driver = {
	.probe		= amd_wdt_init,
	.remove		= amd_wdt_remove,
	.shutdown	= amd_wdt_shutdown,
	.driver		= {
		.owner	= THIS_MODULE,
		.name	= WDT_MODULE_NAME,
	},
};

static int __init amd_wdt_init_module(void)
{
	int err;

	pr_info("AMD WatchDog Timer Driver v%s\n", WDT_VERSION);

	err = platform_driver_register(&amd_wdt_driver);
	if (err)
		return err;

	amd_wdt_platform_device = platform_device_register_simple(
					WDT_MODULE_NAME, -1, NULL, 0);
	if (IS_ERR(amd_wdt_platform_device)) {
		err = PTR_ERR(amd_wdt_platform_device);
		goto unreg_platform_driver;
	}

	return 0;

unreg_platform_driver:
	platform_driver_unregister(&amd_wdt_driver);
	return err;
}

static void __exit amd_wdt_cleanup_module(void)
{
	platform_device_unregister(amd_wdt_platform_device);
	platform_driver_unregister(&amd_wdt_driver);
	pr_info("AMD Watchdog Module Unloaded\n");
}

module_init(amd_wdt_init_module);
module_exit(amd_wdt_cleanup_module);

MODULE_AUTHOR("Arindam Nath <arindam.nath@amd.com>");
MODULE_DESCRIPTION("Watchdog timer driver for AMD chipsets");
MODULE_LICENSE("Dual BSD/GPL");
