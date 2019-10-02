/*
 * drivers\media\cedar_ve
 * (C) Copyright 2010-2016
 * Reuuimlla Technology Co., Ltd. <www.allwinnertech.com>
 * fangning<fangning@allwinnertech.com>
 *
 * some simple description for this code
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of
 * the License, or (at your option) any later version.
 *
 */

#include <linux/init.h>
#include <linux/module.h>
#include <linux/ioctl.h>
#include <linux/fs.h>
#include <linux/device.h>
#include <linux/err.h>
#include <linux/list.h>
#include <linux/errno.h>
#include <linux/mutex.h>
#include <linux/slab.h>
#include <linux/preempt.h>
#include <linux/cdev.h>
#include <linux/platform_device.h>
#include <linux/interrupt.h>
#include <linux/clk.h>
#include <linux/rmap.h>
#include <linux/wait.h>
#include <linux/semaphore.h>
#include <linux/poll.h>
#include <linux/spinlock.h>
#include <linux/sched.h>
#include <linux/kthread.h>
#include <linux/delay.h>
#include <linux/version.h>
#include <asm/uaccess.h>
#include <asm/io.h>
#include <asm/dma.h>
#include <linux/dma-mapping.h>
#include <linux/mm.h>
#include <asm/siginfo.h>
#include <asm/signal.h>
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4,11,0)
#include <linux/reset.h>
#include <linux/sched/signal.h>
#include <linux/mfd/syscon.h>
#include <linux/regmap.h>
#else /*LINUX_VERSION_CODE >= KERNEL_VERSION(4,11,0)*/
#include <linux/clk/sunxi.h>
#endif /*LINUX_VERSION_CODE < KERNEL_VERSION(4,11,0)*/

#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/of_irq.h>

#include "cedar_ve.h"
#include <linux/regulator/consumer.h>
#include <linux/of_reserved_mem.h>

struct regulator *regu;

//#define USE_ION
//#define CEDAR_DEBUG
#define HAVE_TIMER_SETUP

#define DRV_VERSION "0.01beta"

#undef USE_CEDAR_ENGINE

#ifndef CEDARDEV_MAJOR
#define CEDARDEV_MAJOR (150)
#endif
#ifndef CEDARDEV_MINOR
#define CEDARDEV_MINOR (0)
#endif

#define MACC_REGS_BASE      (0x01C0E000)           // Media ACCelerate

#ifndef CONFIG_OF
//H3: arch/arm/mach-sunxi/include/mach/sun8i/irqs-sun8iw7p1.h
//#define SUNXI_IRQ_VE		(90)
#endif

#define cedar_ve_printk(level, msg...) printk(level "cedar_ve: " msg)

#define VE_CLK_HIGH_WATER  (700)//400MHz
#define VE_CLK_LOW_WATER   (100) //160MHz

static int g_dev_major = CEDARDEV_MAJOR;
static int g_dev_minor = CEDARDEV_MINOR;
module_param(g_dev_major, int, S_IRUGO);//S_IRUGO represent that g_dev_major can be read,but canot be write
module_param(g_dev_minor, int, S_IRUGO);

#if LINUX_VERSION_CODE >= KERNEL_VERSION(4,11,0)
#define SYSCON_SRAM_CTRL_REG0	0x0
#define SYSCON_SRAM_C1_MAP_VE	0x7fffffff
#else /*LINUX_VERSION_CODE >= KERNEL_VERSION(4,11,0)*/
struct clk *ve_moduleclk = NULL;
struct clk *ve_parent_pll_clk = NULL;
struct clk *ve_power_gating = NULL;
static u32 ve_parent_clk_rate = 300000000;
#endif /*LINUX_VERSION_CODE >= KERNEL_VERSION(4,11,0)*/

struct iomap_para{
	volatile char* regs_macc;
	volatile char* regs_avs;
};

static DECLARE_WAIT_QUEUE_HEAD(wait_ve);
struct cedar_dev {
	struct cdev cdev;	             /* char device struct                 */
        struct platform_device  *pdev;
        struct device *dev;
        struct device *odev;              /* ptr to class device struct         */
	struct class  *class;            /* class for auto create device node  */

	struct semaphore sem;            /* mutual exclusion semaphore         */

	wait_queue_head_t wq;            /* wait queue for poll ops            */

	struct iomap_para iomap_addrs;   /* io remap addrs                     */

	struct timer_list cedar_engine_timer;
	struct timer_list cedar_engine_timer_rel;

	u32 irq;                         /* cedar video engine irq number      */
	u32 de_irq_flag;                    /* flag of video decoder engine irq generated */
	u32 de_irq_value;                   /* value of video decoder engine irq          */
	u32 en_irq_flag;                    /* flag of video encoder engine irq generated */
	u32 en_irq_value;                   /* value of video encoder engine irq          */
	u32 irq_has_enable;
	u32 ref_count;

	u32 jpeg_irq_flag;                    /* flag of video jpeg dec irq generated */
	u32 jpeg_irq_value;                   /* value of video jpeg dec  irq */

	volatile u32* sram_bass_vir ;
	volatile u32* clk_bass_vir;

#if LINUX_VERSION_CODE >= KERNEL_VERSION(4,11,0)
  struct clk *mod_clk;
  struct clk *ahb_clk;
  struct clk *ram_clk;
  struct reset_control *rstc;
  struct regmap *syscon;
#endif /*LINUX_VERSION_CODE < KERNEL_VERSION(4,11,0)*/
  	unsigned long ve_start;
	unsigned long ve_size;
	void *ve_start_virt;
	resource_size_t ve_start_pa;
};

struct ve_info {
	u32 set_vol_flag;
};

struct cedar_dev *cedar_devp;
struct file *ve_file;

u32 int_sta=0,int_value;

/*
 * Video engine interrupt service routine
 * To wake up ve wait queue
 */

#if defined(CONFIG_OF)
static struct of_device_id sunxi_cedar_ve_match[] = {
	{ .compatible = "allwinner,sunxi-cedar-ve", },
	//{ .compatible = "allwinner,sun8i-h3-video-engine", },
	{}
};
MODULE_DEVICE_TABLE(of, sunxi_cedar_ve_match);
#endif

#if LINUX_VERSION_CODE >= KERNEL_VERSION(4,11,0) || ((defined CONFIG_ARCH_SUN8IW7P1) || (defined CONFIG_ARCH_SUN8IW8P1) || (defined CONFIG_ARCH_SUN8IW9P1))
#if  defined(CONFIG_SUNXI_TRUSTZONE)
static inline u32 sunxi_smc_readl(void __iomem *addr)
{
if (sunxi_soc_is_secure()) {
return call_firmware_op(read_reg, addr);
} else {
return readl(addr);
}

}
static inline void sunxi_smc_writel(u32 val, void __iomem *addr)
{
  if (sunxi_soc_is_secure()) {
    call_firmware_op(write_reg, val, addr);
  } else {
    writel(val, addr);
  }
}
#else
static inline u32 sunxi_smc_readl(void __iomem *addr)
{
return readl(addr);
}
static inline void sunxi_smc_writel(u32 val, void __iomem *addr)
{
  writel(val, addr);
}
#endif
#endif /*((defined CONFIG_ARCH_SUN8IW7P1) || (defined CONFIG_ARCH_SUN8IW8P1) || (defined CONFIG_ARCH_SUN8IW9P1))*/

static irqreturn_t VideoEngineInterupt(int irq, void *dev)
{
	ulong ve_int_status_reg;
	ulong ve_int_ctrl_reg;
	u32 status = 0;
	volatile int val;
	int modual_sel = 0;
	u32 interrupt_enable = 0;
	struct iomap_para addrs = cedar_devp->iomap_addrs;

	modual_sel = readl(addrs.regs_macc + 0);

	/* case for VE 1633 and newer */
	if (modual_sel&(3<<6)) {
		if (modual_sel&(1<<7)) {
			/*avc enc*/
			ve_int_status_reg = (ulong)(addrs.regs_macc + 0xb00 + 0x1c);
			ve_int_ctrl_reg = (ulong)(addrs.regs_macc + 0xb00 + 0x14);
			interrupt_enable = readl((void*)ve_int_ctrl_reg) &(0x7);
			status = readl((void*)ve_int_status_reg);
			status &= 0xf;
		} else {
			/*isp*/
			ve_int_status_reg = (ulong)(addrs.regs_macc + 0xa00 + 0x10);
			ve_int_ctrl_reg = (ulong)(addrs.regs_macc + 0xa00 + 0x08);
			interrupt_enable = readl((void*)ve_int_ctrl_reg) &(0x1);
			status = readl((void*)ve_int_status_reg);
			status &= 0x1;
		}

		/*modify by fangning 2013-05-22*/
		if (status && interrupt_enable) {
			/*disable interrupt*/
			/*avc enc*/
			if (modual_sel&(1<<7)) {
				ve_int_ctrl_reg = (ulong)(addrs.regs_macc + 0xb00 + 0x14);
				val = readl((void*)ve_int_ctrl_reg);
				writel(val & (~0x7), (void*)ve_int_ctrl_reg);
			} else {
				/*isp*/
				ve_int_ctrl_reg = (ulong)(addrs.regs_macc + 0xa00 + 0x08);
				val = readl((void*)ve_int_ctrl_reg);
				writel(val & (~0x1), (void*)ve_int_ctrl_reg);
			}
			/*hx modify 2011-8-1 16:08:47*/
			cedar_devp->en_irq_value = 1;
			cedar_devp->en_irq_flag = 1;
			/*any interrupt will wake up wait queue*/
			//printk("Video interrupt occurs EN!!!\n");
			wake_up_interruptible(&wait_ve);
		}
	}

#if ((defined CONFIG_ARCH_SUN8IW8P1) || (defined CONFIG_ARCH_SUN50I))
	if (modual_sel&(0x20)) {
		ve_int_status_reg = (ulong)(addrs.regs_macc + 0xe00 + 0x1c);
		ve_int_ctrl_reg = (ulong)(addrs.regs_macc + 0xe00 + 0x14);
		interrupt_enable = readl((void*)ve_int_ctrl_reg) & (0x38);

		status = readl((void*)ve_int_status_reg);

		if ((status&0x7) && interrupt_enable) {
			/*disable interrupt*/
			val = readl((void*)ve_int_ctrl_reg);
			writel(val & (~0x38), (void*)ve_int_ctrl_reg);

			cedar_devp->jpeg_irq_value = 1;
			cedar_devp->jpeg_irq_flag = 1;

			/*any interrupt will wake up wait queue*/
			wake_up_interruptible(&wait_ve);
		}
	}
#endif

	/* case for all VE versions */
	modual_sel &= 0xf;
	if((modual_sel<=4) || (modual_sel == 0xB)) {
		/*estimate Which video format*/
		switch (modual_sel)
		{
			case 0: /*mpeg124*/
				ve_int_status_reg = (ulong)
					(addrs.regs_macc + 0x100 + 0x1c);
				ve_int_ctrl_reg = (ulong)(addrs.regs_macc + 0x100 + 0x14);
				interrupt_enable = readl((void*)ve_int_ctrl_reg) & (0x7c);
				break;
			case 1: /*h264*/
				ve_int_status_reg = (ulong)
					(addrs.regs_macc + 0x200 + 0x28);
				ve_int_ctrl_reg = (ulong)(addrs.regs_macc + 0x200 + 0x20);
				interrupt_enable = readl((void*)ve_int_ctrl_reg) & (0xf);
				break;
			case 2: /*vc1*/
				ve_int_status_reg = (ulong)(addrs.regs_macc +
					0x300 + 0x2c);
				ve_int_ctrl_reg = (ulong)(addrs.regs_macc + 0x300 + 0x24);
				interrupt_enable = readl((void*)ve_int_ctrl_reg) & (0xf);
				break;
			case 3: /*rmvb*/
				ve_int_status_reg = (ulong)
					(addrs.regs_macc + 0x400 + 0x1c);
				ve_int_ctrl_reg = (ulong)(addrs.regs_macc + 0x400 + 0x14);
				interrupt_enable = readl((void*)ve_int_ctrl_reg) & (0xf);
				break;

			case 4: /*hevc*/
				ve_int_status_reg = (ulong)
					(addrs.regs_macc + 0x500 + 0x38);
				ve_int_ctrl_reg = (ulong)(addrs.regs_macc + 0x500 + 0x30);
				interrupt_enable = readl((void*)ve_int_ctrl_reg) & (0xf);
				break;

			case 0xB: /*AVC (h264 encoder)*/
				ve_int_status_reg = (unsigned int)(addrs.regs_macc + 0xb00 + 0x1c);
    			ve_int_ctrl_reg = (unsigned int)(addrs.regs_macc + 0xb00 + 0x14);
				interrupt_enable = readl((void*)ve_int_ctrl_reg) &(0x7);
				break;	

			default:   
				ve_int_status_reg = (ulong)(addrs.regs_macc + 0x100 + 0x1c);
				ve_int_ctrl_reg = (ulong)(addrs.regs_macc + 0x100 + 0x14);
				interrupt_enable = readl((void*)ve_int_ctrl_reg) & (0xf);
				cedar_ve_printk(KERN_WARNING, "ve mode :%x "
					"not defined!\n", modual_sel);
				break;
		}

		status = readl((void*)ve_int_status_reg);

		/*modify by fangning 2013-05-22*/
		if ((status&0xf) && interrupt_enable) {
			/*disable interrupt*/
			if (modual_sel == 0) {
				val = readl((void*)ve_int_ctrl_reg);
				writel(val & (~0x7c), (void*)ve_int_ctrl_reg);
			} else {
				val = readl((void*)ve_int_ctrl_reg);
				writel(val & (~0xf), (void*)ve_int_ctrl_reg);
			}

			cedar_devp->de_irq_value = 1;
			cedar_devp->de_irq_flag = 1;
			/*any interrupt will wake up wait queue*/
			wake_up_interruptible(&wait_ve);
		}
	}

	return IRQ_HANDLED;
}

static int clk_status = 0;
static LIST_HEAD(run_task_list);
static LIST_HEAD(del_task_list);
static spinlock_t cedar_spin_lock;
#define CEDAR_RUN_LIST_NONULL	-1
#define CEDAR_NONBLOCK_TASK  0
#define CEDAR_BLOCK_TASK 1
#define CLK_REL_TIME 10000
#define TIMER_CIRCLE 50
#define TASK_INIT      0x00
#define TASK_TIMEOUT   0x55
#define TASK_RELEASE   0xaa
#define SIG_CEDAR		35

static int enable_cedar_hw_clk(void)
{
	ulong flags;
	int res = -EFAULT;

	spin_lock_irqsave(&cedar_spin_lock, flags);		

	if (clk_status == 1)
		goto out;

	clk_status = 1;

#if LINUX_VERSION_CODE >= KERNEL_VERSION(4,11,0)
	reset_control_deassert(cedar_devp->rstc);
	if (clk_enable(cedar_devp->mod_clk)) { cedar_ve_printk(KERN_WARNING, "try to enable ve_moduleclk failed! (%d=\n", __LINE__); goto out; }
	res = 0;
#else /*LINUX_VERSION_CODE >= KERNEL_VERSION(4,11,0)*/
	sunxi_periph_reset_deassert(ve_moduleclk);
	if (clk_enable(ve_moduleclk)) {
		cedar_ve_printk(KERN_WARNING, "enable ve_moduleclk failed;\n");
		goto out;
	}else {
		res = 0;
	}
#endif /*LINUX_VERSION_CODE >= KERNEL_VERSION(4,11,0)*/

#ifdef CEDAR_DEBUG
	printk("%s,%d\n",__func__,__LINE__);
#endif

out:
	spin_unlock_irqrestore(&cedar_spin_lock, flags);
	return res;
}

int disable_cedar_hw_clk(void)
{
	ulong flags;
	int res = -EFAULT;

	spin_lock_irqsave(&cedar_spin_lock, flags);		

	if (clk_status == 0) {
		res = 0;
		goto out;
	}
	clk_status = 0;

#if LINUX_VERSION_CODE >= KERNEL_VERSION(4,11,0)
	if ((NULL == cedar_devp->mod_clk)||(IS_ERR(cedar_devp->mod_clk))) {
		cedar_ve_printk(KERN_WARNING, "ve_moduleclk is invalid\n");
	} else {
		clk_disable(cedar_devp->mod_clk);
		reset_control_assert(cedar_devp->rstc);
		res = 0;
	}
#else /*LINUX_VERSION_CODE >= KERNEL_VERSION(4,11,0)*/
	if ((NULL == ve_moduleclk)||(IS_ERR(ve_moduleclk))) {
		cedar_ve_printk(KERN_WARNING, "ve_moduleclk is invalid\n");
	} else {
		clk_disable(ve_moduleclk);
		sunxi_periph_reset_assert(ve_moduleclk);
		res = 0;
	}
#endif /*LINUX_VERSION_CODE >= KERNEL_VERSION(4,11,0)*/

#ifdef CEDAR_DEBUG
	printk("%s,%d\n",__func__,__LINE__);
#endif

out:
	spin_unlock_irqrestore(&cedar_spin_lock, flags);
	return res;
}

void cedardev_insert_task(struct cedarv_engine_task* new_task)
{	
	struct cedarv_engine_task *task_entry;
	ulong flags;

	spin_lock_irqsave(&cedar_spin_lock, flags);		

	if (list_empty(&run_task_list))
		new_task->is_first_task = 1;


	list_for_each_entry(task_entry, &run_task_list, list) {
		if ((task_entry->is_first_task == 0) && (task_entry->running == 0) && (task_entry->t.task_prio < new_task->t.task_prio)) {
			break;
		}
	}

	list_add(&new_task->list, task_entry->list.prev);	

#ifdef CEDAR_DEBUG
	printk("%s,%d, TASK_ID:",__func__,__LINE__);
	list_for_each_entry(task_entry, &run_task_list, list) {
		printk("%d!", task_entry->t.ID);
	}
	printk("\n");
#endif

	mod_timer(&cedar_devp->cedar_engine_timer, jiffies + 0);

	spin_unlock_irqrestore(&cedar_spin_lock, flags);
}

int cedardev_del_task(int task_id)
{
	struct cedarv_engine_task *task_entry;
	ulong flags;

	spin_lock_irqsave(&cedar_spin_lock, flags);		

	list_for_each_entry(task_entry, &run_task_list, list) {
		if (task_entry->t.ID == task_id && task_entry->status != TASK_RELEASE) {
			task_entry->status = TASK_RELEASE;

			spin_unlock_irqrestore(&cedar_spin_lock, flags);
			mod_timer(&cedar_devp->cedar_engine_timer, jiffies + 0);
			return 0;
		}
	}
	spin_unlock_irqrestore(&cedar_spin_lock, flags);

	return -1;
}

int cedardev_check_delay(int check_prio)
{
	struct cedarv_engine_task *task_entry;
	int timeout_total = 0;
	ulong flags;

	spin_lock_irqsave(&cedar_spin_lock, flags);
	list_for_each_entry(task_entry, &run_task_list, list) {
		if ((task_entry->t.task_prio >= check_prio) || (task_entry->running == 1) || (task_entry->is_first_task == 1))							
			timeout_total = timeout_total + task_entry->t.frametime;
	}

	spin_unlock_irqrestore(&cedar_spin_lock, flags);
#ifdef CEDAR_DEBUG
	printk("%s,%d,%d\n", __func__, __LINE__, timeout_total);
#endif
	return timeout_total;
}

#ifdef HAVE_TIMER_SETUP
static void cedar_engine_for_timer_rel(struct timer_list *t)
#else
static void cedar_engine_for_timer_rel(unsigned long arg)
#endif
{
	ulong flags;
	int ret = 0;
	spin_lock_irqsave(&cedar_spin_lock, flags);		

	if (list_empty(&run_task_list)) {
		ret = disable_cedar_hw_clk(); 
		if (ret < 0) {
			cedar_ve_printk(KERN_WARNING, "clk disable error!\n");
		}
	} else {
		cedar_ve_printk(KERN_WARNING, "clk disable time out "
			"but task left\n");
		mod_timer( &cedar_devp->cedar_engine_timer, jiffies + msecs_to_jiffies(TIMER_CIRCLE));
	}

	spin_unlock_irqrestore(&cedar_spin_lock, flags);
}

#ifdef HAVE_TIMER_SETUP
static void cedar_engine_for_events(struct timer_list *t)
#else
static void cedar_engine_for_events(unsigned long arg)
#endif
{
	struct cedarv_engine_task *task_entry, *task_entry_tmp;
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4,20,0)	
	struct kernel_siginfo info;
#else 
	struct siginfo info;
#endif	
	ulong flags;

	spin_lock_irqsave(&cedar_spin_lock, flags);		

	list_for_each_entry_safe(task_entry, task_entry_tmp, &run_task_list, list) {
		mod_timer(&cedar_devp->cedar_engine_timer_rel, jiffies + msecs_to_jiffies(CLK_REL_TIME));
		if (task_entry->status == TASK_RELEASE || 
				time_after(jiffies, task_entry->t.timeout)) {
			if (task_entry->status == TASK_INIT)
				task_entry->status = TASK_TIMEOUT;
			list_move(&task_entry->list, &del_task_list);	
		}
	}

	list_for_each_entry_safe(task_entry, task_entry_tmp, &del_task_list, list) {		
		info.si_signo = SIG_CEDAR;
		info.si_code = task_entry->t.ID;
		if (task_entry->status == TASK_TIMEOUT) {
			info.si_errno = TASK_TIMEOUT;			
			send_sig_info(SIG_CEDAR, &info, task_entry->task_handle);
		} else if (task_entry->status == TASK_RELEASE) {
			info.si_errno = TASK_RELEASE;			
			send_sig_info(SIG_CEDAR, &info, task_entry->task_handle);
		}
		list_del(&task_entry->list);
		kfree(task_entry);
	}

	if (!list_empty(&run_task_list)) {
		task_entry = list_entry(run_task_list.next, struct cedarv_engine_task, list);
		if (task_entry->running == 0) {
			task_entry->running = 1;
			info.si_signo = SIG_CEDAR;
			info.si_code = task_entry->t.ID;
			info.si_errno = TASK_INIT;
			send_sig_info(SIG_CEDAR, &info, task_entry->task_handle);
		}

		mod_timer( &cedar_devp->cedar_engine_timer, jiffies + msecs_to_jiffies(TIMER_CIRCLE));
	}

	spin_unlock_irqrestore(&cedar_spin_lock, flags);
}

#ifdef CONFIG_COMPAT
static long compat_cedardev_ioctl(struct file *filp, u32 cmd, unsigned long arg)
{
	int ret = 0;
	int ve_timeout = 0;
	/*struct cedar_dev *devp;*/
#ifdef USE_CEDAR_ENGINE
	int rel_taskid = 0;
	struct __cedarv_task task_ret;
	struct cedarv_engine_task *task_ptr = NULL;
#endif
	ulong flags;
	struct ve_info *info;

	info = filp->private_data;

	switch (cmd)
	{
		case IOCTL_ENGINE_REQ:
#ifdef USE_CEDAR_ENGINE
			if (copy_from_user(&task_ret, (void __user *)arg,
				sizeof(struct __cedarv_task))) {
				cedar_ve_printk(KERN_WARNING, "USE_CEDAR_ENGINE "
					"copy_from_user fail\n");
				return -EFAULT;
			}
			spin_lock_irqsave(&cedar_spin_lock, flags);

			if (!list_empty(&run_task_list) &&
				(task_ret.block_mode == CEDAR_NONBLOCK_TASK)) {
				spin_unlock_irqrestore(&cedar_spin_lock, flags);
				return CEDAR_RUN_LIST_NONULL;
			}
			spin_unlock_irqrestore(&cedar_spin_lock, flags);

			task_ptr = kmalloc(sizeof(struct cedarv_engine_task), GFP_KERNEL);
			if (!task_ptr) {
				cedar_ve_printk(KERN_WARNING, "get "
					"task_ptr error\n");
				return PTR_ERR(task_ptr);
			}
			task_ptr->task_handle = current;
			task_ptr->t.ID = task_ret.ID;
			/*ms to jiffies*/
			task_ptr->t.timeout = jiffies +
				msecs_to_jiffies(1000*task_ret.timeout);
			task_ptr->t.frametime = task_ret.frametime;
			task_ptr->t.task_prio = task_ret.task_prio;
			task_ptr->running = 0;
			task_ptr->is_first_task = 0;
			task_ptr->status = TASK_INIT;

			cedardev_insert_task(task_ptr);

			ret = enable_cedar_hw_clk();
			if (ret < 0) {
				cedar_ve_printk(KERN_WARNING, "IOCTL_ENGINE_REQ "
					"clk enable error!\n");
				return -EFAULT;
			}
			return task_ptr->is_first_task;
#else
			cedar_devp->ref_count++;
			if (1 == cedar_devp->ref_count)
				enable_cedar_hw_clk();
			break;
#endif
		case IOCTL_ENGINE_REL:
#ifdef USE_CEDAR_ENGINE
			rel_taskid = (int)arg;

			ret = cedardev_del_task(rel_taskid);
#else
			cedar_devp->ref_count--;
			if (0 == cedar_devp->ref_count) {
				ret = disable_cedar_hw_clk();
				if (ret < 0) {
					cedar_ve_printk(KERN_WARNING, "IOCTL_ENGINE_REL "
						"clk disable error!\n");
					return -EFAULT;
				}
			}
#endif
			return ret;
		case IOCTL_ENGINE_CHECK_DELAY:
			{
				struct cedarv_engine_task_info task_info;

				if (copy_from_user(&task_info, (void __user *)arg,
					sizeof(struct cedarv_engine_task_info))) {
					cedar_ve_printk(KERN_WARNING, "%d "
						"copy_from_user fail\n",
						IOCTL_ENGINE_CHECK_DELAY);
					return -EFAULT;
				}
				task_info.total_time = cedardev_check_delay(task_info.task_prio);
#ifdef CEDAR_DEBUG
				printk("%s,%d,%d\n", __func__, __LINE__, task_info.total_time);
#endif
				task_info.frametime = 0;
				spin_lock_irqsave(&cedar_spin_lock, flags);
				if (!list_empty(&run_task_list)) {

					struct cedarv_engine_task *task_entry;
#ifdef CEDAR_DEBUG
					printk("%s,%d\n",__func__,__LINE__);
#endif
					task_entry = list_entry(run_task_list.next, struct cedarv_engine_task, list);
					if (task_entry->running == 1)
						task_info.frametime = task_entry->t.frametime;
#ifdef CEDAR_DEBUG
					printk("%s,%d,%d\n",__func__,__LINE__,task_info.frametime);
#endif
				}
				spin_unlock_irqrestore(&cedar_spin_lock, flags);

				if (copy_to_user((void *)arg, &task_info, sizeof(struct cedarv_engine_task_info))){
					cedar_ve_printk(KERN_WARNING, "%d "
						"copy_to_user fail\n",
						IOCTL_ENGINE_CHECK_DELAY);
					return -EFAULT;
				}
			}
			break;
		case IOCTL_WAIT_VE_DE:
			ve_timeout = (int)arg;
			cedar_devp->de_irq_value = 0;

			spin_lock_irqsave(&cedar_spin_lock, flags);
			if (cedar_devp->de_irq_flag)
				cedar_devp->de_irq_value = 1;
			spin_unlock_irqrestore(&cedar_spin_lock, flags);

			wait_event_interruptible_timeout(wait_ve, cedar_devp->de_irq_flag, ve_timeout*HZ);
			cedar_devp->de_irq_flag = 0;

			return cedar_devp->de_irq_value;

		case IOCTL_WAIT_VE_EN:

			ve_timeout = (int)arg;
			cedar_devp->en_irq_value = 0;

			spin_lock_irqsave(&cedar_spin_lock, flags);
			if (cedar_devp->en_irq_flag)
				cedar_devp->en_irq_value = 1;
			spin_unlock_irqrestore(&cedar_spin_lock, flags);

			wait_event_interruptible_timeout(wait_ve, cedar_devp->en_irq_flag, ve_timeout*HZ);
			cedar_devp->en_irq_flag = 0;

			return cedar_devp->en_irq_value;

#if ((defined CONFIG_ARCH_SUN8IW8P1) || (defined CONFIG_ARCH_SUN50I))

		case IOCTL_WAIT_JPEG_DEC:
			ve_timeout = (int)arg;
			cedar_devp->jpeg_irq_value = 0;

			spin_lock_irqsave(&cedar_spin_lock, flags);
			if (cedar_devp->jpeg_irq_flag)
				cedar_devp->jpeg_irq_value = 1;
			spin_unlock_irqrestore(&cedar_spin_lock, flags);

			wait_event_interruptible_timeout(wait_ve, cedar_devp->jpeg_irq_flag, ve_timeout*HZ);
			cedar_devp->jpeg_irq_flag = 0;
			return cedar_devp->jpeg_irq_value;
#endif
		case IOCTL_ENABLE_VE:
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4,11,0)
		        if (clk_prepare_enable(cedar_devp->mod_clk)) {
			        cedar_ve_printk(KERN_WARNING, "IOCTL_ENABLE_VE enable ve_moduleclk failed! (%d)\n", __LINE__);
			}
#else /*LINUX_VERSION_CODE >= KERNEL_VERSION(4,11,0)*/
		        if (clk_prepare_enable(ve_moduleclk)) {
				cedar_ve_printk(KERN_WARNING, "IOCTL_ENABLE_VE "
					"enable ve_moduleclk failed!\n");
			}
#endif /*LINUX_VERSION_CODE >= KERNEL_VERSION(4,11,0)*/
			break;

		case IOCTL_DISABLE_VE:
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4,11,0)
			if ((NULL == cedar_devp->mod_clk)||IS_ERR(cedar_devp->mod_clk)) {
			        cedar_ve_printk(KERN_WARNING, "IOCTL_DISABLE_VE ve_moduleclk is invalid (%d)\n", __LINE__);
				return -EFAULT;
			} else {
				clk_disable_unprepare(cedar_devp->mod_clk);
			}
#else /*LINUX_VERSION_CODE >= KERNEL_VERSION(4,11,0)*/
			if ((NULL == ve_moduleclk)||IS_ERR(ve_moduleclk)) {
				cedar_ve_printk(KERN_WARNING, "IOCTL_DISABLE_VE "
					"ve_moduleclk is invalid\n");
				return -EFAULT;
			} else {
				clk_disable_unprepare(ve_moduleclk);
			}
#endif /*LINUX_VERSION_CODE >= KERNEL_VERSION(4,11,0)*/
			break;

		case IOCTL_RESET_VE:
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4,11,0)
		        reset_control_assert(cedar_devp->rstc);
		        reset_control_deassert(cedar_devp->rstc);
#else /*LINUX_VERSION_CODE >= KERNEL_VERSION(4,11,0)*/
			sunxi_periph_reset_assert(ve_moduleclk);
			sunxi_periph_reset_deassert(ve_moduleclk);
#endif /*LINUX_VERSION_CODE >= KERNEL_VERSION(4,11,0)*/
			break;

		case IOCTL_SET_VE_FREQ:
			{
				int arg_rate = (int)arg;
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4,11,0)
				struct clk *const ve_moduleclk=cedar_devp->mod_clk;
				struct clk *const ve_parent_pll_clk=cedar_devp->ahb_clk;
				u32 ve_parent_clk_rate;
#endif /*LINUX_VERSION_CODE >= KERNEL_VERSION(4,11,0)*/
				if (arg_rate >= VE_CLK_LOW_WATER &&
						arg_rate <= VE_CLK_HIGH_WATER &&
						clk_get_rate(ve_moduleclk)/1000000 != arg_rate) {
					if (!clk_set_rate(ve_parent_pll_clk, arg_rate*1000000)) {
						ve_parent_clk_rate = clk_get_rate(ve_parent_pll_clk);
						if (clk_set_rate(ve_moduleclk, ve_parent_clk_rate)) {
							cedar_ve_printk(KERN_WARNING, "set ve clock failed\n");
						}

					} else {
						cedar_ve_printk(KERN_WARNING, "set pll4 clock failed\n");
					}
				}
				ret = clk_get_rate(ve_moduleclk);
				break;
			}
		case IOCTL_GETVALUE_AVS2:
		case IOCTL_ADJUST_AVS2:
		case IOCTL_ADJUST_AVS2_ABS:
		case IOCTL_CONFIG_AVS2:
		case IOCTL_RESET_AVS2:
		case IOCTL_PAUSE_AVS2:
		case IOCTL_START_AVS2:
			cedar_ve_printk(KERN_WARNING, "do not supprot this ioctrl now\n");
			break;

		case IOCTL_GET_ENV_INFO:
			{
				struct cedarv_env_infomation_compat env_info;
#if defined(USE_ION)
				env_info.phymem_start = 0; // do not use this interface ,ve get phy mem form ion now
				env_info.phymem_total_size = 0;//ve_size = 0x04000000 
				env_info.address_macc = 0;
#else				
				env_info.phymem_start = (unsigned int)phys_to_virt(cedar_devp->ve_start); // do not use this interface ,ve get phy mem form ion now
				env_info.phymem_total_size = cedar_devp->ve_size;
				env_info.address_macc = (unsigned int) cedar_devp->iomap_addrs.regs_macc;
#endif	
				if (copy_to_user((char *)arg, &env_info,
					sizeof(struct cedarv_env_infomation_compat)))
					return -EFAULT;
			}
			break;
		case IOCTL_GET_IC_VER:
			{
				return 0;
			}
		case IOCTL_SET_REFCOUNT:
			cedar_devp->ref_count = (int)arg;
			break;
		case IOCTL_SET_VOL:
			{

#if defined CONFIG_ARCH_SUN9IW1P1
				int ret;
				int vol = (int)arg;

				if (down_interruptible(&cedar_devp->sem)) {
					return -ERESTARTSYS;
				}
				info->set_vol_flag = 1;

				//set output voltage to arg mV
				ret = regulator_set_voltage(regu,vol*1000,3300000);
				if (IS_ERR(regu)) {
					cedar_ve_printk(KERN_WARNING, \
						"fail to set axp15_dcdc4 regulator voltage!\n");
				}
				up(&cedar_devp->sem);
#endif
				break;
			}
		default:
			return -1;
	}
	return ret;
}
#endif /* CONFIG_COMPAT */

/*
 * ioctl function
 * including : wait video engine done,
 *             AVS Counter control,
 *             Physical memory control,
 *             module clock/freq control.
 *				cedar engine
 */ 
static long cedardev_ioctl(struct file *filp, u32 cmd, unsigned long arg)
{
	int ret = 0;	
	int ve_timeout = 0;
	//struct cedar_dev *devp;
#ifdef USE_CEDAR_ENGINE
	int rel_taskid = 0;
	struct __cedarv_task task_ret;
	struct cedarv_engine_task *task_ptr = NULL;
#endif
	ulong flags;
	struct ve_info *info;

	info = filp->private_data;

	switch (cmd)
	{
		case IOCTL_ENGINE_REQ:
#ifdef USE_CEDAR_ENGINE
			if (copy_from_user(&task_ret, (void __user *)arg, sizeof(struct __cedarv_task))) {
				cedar_ve_printk(KERN_WARNING, \
					"IOCTL_ENGINE_REQ copy_from_user fail\n");
				return -EFAULT;
			}
			spin_lock_irqsave(&cedar_spin_lock, flags);

			if (!list_empty(&run_task_list) && (task_ret.block_mode == CEDAR_NONBLOCK_TASK)) {
				spin_unlock_irqrestore(&cedar_spin_lock, flags);
				return CEDAR_RUN_LIST_NONULL;
			}
			spin_unlock_irqrestore(&cedar_spin_lock, flags);

			task_ptr = kmalloc(sizeof(struct cedarv_engine_task), GFP_KERNEL);
			if (!task_ptr) {
				cedar_ve_printk(KERN_WARNING, "get mem for IOCTL_ENGINE_REQ\n");
				return PTR_ERR(task_ptr);
			}
			task_ptr->task_handle = current;
			task_ptr->t.ID = task_ret.ID;
			task_ptr->t.timeout = jiffies + msecs_to_jiffies(1000*task_ret.timeout);//ms to jiffies
			task_ptr->t.frametime = task_ret.frametime;
			task_ptr->t.task_prio = task_ret.task_prio;
			task_ptr->running = 0;
			task_ptr->is_first_task = 0;
			task_ptr->status = TASK_INIT;

			cedardev_insert_task(task_ptr);

			ret = enable_cedar_hw_clk();
			if (ret < 0) {
				cedar_ve_printk(KERN_WARNING, \
					"cedar clk enable somewhere error!\n");
				return -EFAULT;
			}
			return task_ptr->is_first_task;		
#else
			cedar_devp->ref_count++;
			if (1 == cedar_devp->ref_count)
				enable_cedar_hw_clk();
			break;
#endif	
		case IOCTL_ENGINE_REL:
#ifdef USE_CEDAR_ENGINE 
			rel_taskid = (int)arg;		

			ret = cedardev_del_task(rel_taskid);					
#else
			cedar_devp->ref_count--;
			if (0 == cedar_devp->ref_count) {
				ret = disable_cedar_hw_clk();
				if (ret < 0) {
					cedar_ve_printk(KERN_WARNING, "IOCTL_ENGINE_REL "
						"clk disable error!\n");
					return -EFAULT;
				}
			}
#endif
			return ret;
		case IOCTL_ENGINE_CHECK_DELAY:		
			{
				struct cedarv_engine_task_info task_info;

				if (copy_from_user(&task_info,
					(void __user *)arg,
					sizeof(struct cedarv_engine_task_info))) {
					cedar_ve_printk(KERN_WARNING, \
						"IOCTL_ENGINE_CHECK_DELAY copy_from_user fail\n");
					return -EFAULT;
				}
				task_info.total_time = cedardev_check_delay(task_info.task_prio);
#ifdef CEDAR_DEBUG
				printk("%s,%d,%d\n", __func__, __LINE__, task_info.total_time);
#endif
				task_info.frametime = 0;
				spin_lock_irqsave(&cedar_spin_lock, flags);
				if (!list_empty(&run_task_list)) {

					struct cedarv_engine_task *task_entry;
#ifdef CEDAR_DEBUG
					printk("%s,%d\n",__func__,__LINE__);
#endif
					task_entry = list_entry(run_task_list.next, struct cedarv_engine_task, list);
					if (task_entry->running == 1)
						task_info.frametime = task_entry->t.frametime;
#ifdef CEDAR_DEBUG
					printk("%s,%d,%d\n",__func__,__LINE__,task_info.frametime);
#endif
				}
				spin_unlock_irqrestore(&cedar_spin_lock, flags);

				if (copy_to_user((void *)arg, &task_info, sizeof(struct cedarv_engine_task_info))){
					cedar_ve_printk(KERN_WARNING, \
						"IOCTL_ENGINE_CHECK_DELAY copy_to_user fail\n");
					return -EFAULT;
				}
			}
			break;
		case IOCTL_WAIT_VE_DE:            
			ve_timeout = (int)arg;
			cedar_devp->de_irq_value = 0;

			spin_lock_irqsave(&cedar_spin_lock, flags);
			if (cedar_devp->de_irq_flag)
				cedar_devp->de_irq_value = 1;
			spin_unlock_irqrestore(&cedar_spin_lock, flags);

			wait_event_interruptible_timeout(wait_ve, cedar_devp->de_irq_flag, ve_timeout*HZ);            
			cedar_devp->de_irq_flag = 0;	

			return cedar_devp->de_irq_value;

		case IOCTL_WAIT_VE_EN:
			ve_timeout = (int)arg;
			cedar_devp->en_irq_value = 0;

			spin_lock_irqsave(&cedar_spin_lock, flags);
			if (cedar_devp->en_irq_flag)
				cedar_devp->en_irq_value = 1;
			spin_unlock_irqrestore(&cedar_spin_lock, flags);

			wait_event_interruptible_timeout(wait_ve, cedar_devp->en_irq_flag, ve_timeout*HZ);            
			cedar_devp->en_irq_flag = 0;	

			return cedar_devp->en_irq_value;

#if ((defined CONFIG_ARCH_SUN8IW8P1) || (defined CONFIG_ARCH_SUN50I))

		case IOCTL_WAIT_JPEG_DEC:            
			ve_timeout = (int)arg;
			cedar_devp->jpeg_irq_value = 0;

			spin_lock_irqsave(&cedar_spin_lock, flags);
			if (cedar_devp->jpeg_irq_flag)
				cedar_devp->jpeg_irq_value = 1;
			spin_unlock_irqrestore(&cedar_spin_lock, flags);

			wait_event_interruptible_timeout(wait_ve, cedar_devp->jpeg_irq_flag, ve_timeout*HZ);            
			cedar_devp->jpeg_irq_flag = 0;	
			return cedar_devp->jpeg_irq_value;	
#endif
		case IOCTL_ENABLE_VE:
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4,11,0)
			if (clk_prepare_enable(cedar_devp->mod_clk)) {
				cedar_ve_printk(KERN_WARNING, \
					"try to enable ve_moduleclk failed!\n");
			}
#else /*LINUX_VERSION_CODE >= KERNEL_VERSION(4,11,0)*/
			if (clk_prepare_enable(ve_moduleclk)) {
				cedar_ve_printk(KERN_WARNING, \
					"try to enable ve_moduleclk failed!\n");
			}
#endif /*LINUX_VERSION_CODE >= KERNEL_VERSION(4,11,0)*/
			break;

		case IOCTL_DISABLE_VE:
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4,11,0)
		  if ((NULL == cedar_devp->mod_clk)||IS_ERR(cedar_devp->mod_clk)) {
				cedar_ve_printk(KERN_WARNING, \
					"ve_moduleclk is invalid,just return!\n");
				return -EFAULT;
			} else {
				clk_disable_unprepare(cedar_devp->mod_clk);
			}
#else /*LINUX_VERSION_CODE >= KERNEL_VERSION(4,11,0)*/
		  if ((NULL == ve_moduleclk)||IS_ERR(ve_moduleclk)) {
				cedar_ve_printk(KERN_WARNING, \
					"ve_moduleclk is invalid,just return!\n");
				return -EFAULT;
			} else {
				clk_disable_unprepare(ve_moduleclk);
			}
#endif /*LINUX_VERSION_CODE >= KERNEL_VERSION(4,11,0)*/
		  break;

		case IOCTL_RESET_VE:
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4,11,0)
		        reset_control_assert(cedar_devp->rstc);
		        reset_control_deassert(cedar_devp->rstc);
#else /*LINUX_VERSION_CODE >= KERNEL_VERSION(4,11,0)*/
			sunxi_periph_reset_assert(ve_moduleclk);
			sunxi_periph_reset_deassert(ve_moduleclk);
#endif /*LINUX_VERSION_CODE >= KERNEL_VERSION(4,11,0)*/
			break;

		case IOCTL_SET_VE_FREQ:	
			{
				int arg_rate = (int)arg;
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4,11,0)
				struct clk *const ve_moduleclk=cedar_devp->mod_clk;
				struct clk *const ve_parent_pll_clk=cedar_devp->ahb_clk;
				u32 ve_parent_clk_rate;
#endif /*LINUX_VERSION_CODE >= KERNEL_VERSION(4,11,0)*/

				if (arg_rate >= VE_CLK_LOW_WATER &&
						arg_rate <= VE_CLK_HIGH_WATER &&
						clk_get_rate(ve_moduleclk)/1000000 != arg_rate) {
					if (!clk_set_rate(ve_parent_pll_clk, arg_rate*1000000)) {
						ve_parent_clk_rate = clk_get_rate(ve_parent_pll_clk);
						if (clk_set_rate(ve_moduleclk, ve_parent_clk_rate)) {
							cedar_ve_printk(KERN_WARNING, "set ve clock failed\n");
						}

					} else {
						cedar_ve_printk(KERN_WARNING, "set pll4 clock failed\n");
					}
				}
				ret = clk_get_rate(ve_moduleclk);
				break;
			}
		case IOCTL_GETVALUE_AVS2:
		case IOCTL_ADJUST_AVS2:	    
		case IOCTL_ADJUST_AVS2_ABS:
		case IOCTL_CONFIG_AVS2:
		case IOCTL_RESET_AVS2:
		case IOCTL_PAUSE_AVS2:
		case IOCTL_START_AVS2:
			cedar_ve_printk(KERN_WARNING, "do not supprot this ioctrl now\n");
			break;

		case IOCTL_GET_ENV_INFO:
			{
				struct cedarv_env_infomation env_info;
#if defined(USE_ION)
				env_info.phymem_start = 0; // do not use this interface ,ve get phy mem form ion now
				env_info.phymem_total_size = 0;//ve_size = 0x04000000 
				env_info.address_macc = 0;
#else				
				env_info.phymem_start = (unsigned int)phys_to_virt(cedar_devp->ve_start); // do not use this interface ,ve get phy mem form ion now
				env_info.phymem_total_size = cedar_devp->ve_size;
				env_info.address_macc = (unsigned int) cedar_devp->iomap_addrs.regs_macc;
#endif				
				if (copy_to_user((char *)arg, &env_info, sizeof(struct cedarv_env_infomation)))
					return -EFAULT;
			}
			break;
		case IOCTL_GET_IC_VER:
			{        	
				return 0;
			}
		case IOCTL_SET_REFCOUNT:
			cedar_devp->ref_count = (int)arg;
			break;
		case IOCTL_SET_VOL:
			{

#if defined CONFIG_ARCH_SUN9IW1P1
				int ret;
				int vol = (int)arg;

				if (down_interruptible(&cedar_devp->sem)) {
					return -ERESTARTSYS;
				}
				info->set_vol_flag = 1;

				//set output voltage to arg mV
				ret = regulator_set_voltage(regu,vol*1000,3300000);
				if (IS_ERR(regu))
					cedar_ve_printk(KERN_WARNING, "fail to set axp15_dcdc4 regulator voltage!\n");
				up(&cedar_devp->sem);
#endif
				break;
			}
		default:
			return -1;
	}
	return ret;
}

static int cedardev_open(struct inode *inode, struct file *filp)
{
	//struct cedar_dev *devp;
	struct ve_info *info;

	info = kmalloc(sizeof(struct ve_info), GFP_KERNEL);
	if (!info)
		return -ENOMEM;

	info->set_vol_flag = 0;
	ve_file = filp;

	//devp = container_of(inode->i_cdev, struct cedar_dev, cdev);
	filp->private_data = info;
	if (down_interruptible(&cedar_devp->sem)) {
		return -ERESTARTSYS;
	}

	/* init other resource here */
	if (0 == cedar_devp->ref_count) {
		cedar_devp->de_irq_flag = 0;
		cedar_devp->en_irq_flag = 0;
		cedar_devp->jpeg_irq_flag = 0;
	}

	up(&cedar_devp->sem);
	nonseekable_open(inode, filp);	
	return 0;
}

static int cedardev_release(struct inode *inode, struct file *filp)
{   
	//struct cedar_dev *devp;
	struct ve_info *info;
	//int ret = 0;

	info = filp->private_data;

	if (down_interruptible(&cedar_devp->sem)) {
		return -ERESTARTSYS;
	}

#if defined CONFIG_ARCH_SUN9IW1P1

	if (info->set_vol_flag == 1) {
		regulator_set_voltage(regu,900000,3300000);
		if (IS_ERR(regu)) {
			cedar_ve_printk(KERN_WARNING, \
				"some error happen, fail to set axp15_dcdc4 regulator voltage!\n");
			return -EINVAL;
		}
	}
#endif

	/* release other resource here */
	if (0 == cedar_devp->ref_count) {
		cedar_devp->de_irq_flag = 1;
		cedar_devp->en_irq_flag = 1;
		cedar_devp->jpeg_irq_flag = 1;
	}
	up(&cedar_devp->sem);

	kfree(info);
	ve_file = NULL;
	return 0;
}

static void cedardev_vma_open(struct vm_area_struct *vma)
{	
} 

static void cedardev_vma_close(struct vm_area_struct *vma)
{	
}

static struct vm_operations_struct cedardev_remap_vm_ops = {
	.open  = cedardev_vma_open,
	.close = cedardev_vma_close,
};

#if defined(USE_ION)
static int cedardev_mmap(struct file *filp, struct vm_area_struct *vma)
{
	u32 temp_pfn;

	if (vma->vm_end - vma->vm_start == 0)
	{
		cedar_ve_printk(KERN_WARNING, "vma->vm_end is equal vma->vm_start : %lx\n",\
			vma->vm_start);
		return 0;
	}
	if (vma->vm_pgoff > (~0UL >> PAGE_SHIFT))
	{
		cedar_ve_printk(KERN_WARNING, \
			"the vma->vm_pgoff is %lx,it is large than the largest page number\n", vma->vm_pgoff);
		return -EINVAL;
	}


	temp_pfn = MACC_REGS_BASE >> 12;


	/* Set reserved and I/O flag for the area. */
	vma->vm_flags |= /*VM_RESERVED | */VM_IO;
	/* Select uncached access. */
	vma->vm_page_prot = pgprot_noncached(vma->vm_page_prot);

	if (io_remap_pfn_range(vma, vma->vm_start, temp_pfn,
				vma->vm_end - vma->vm_start, vma->vm_page_prot)) {
		return -EAGAIN;
	}


	vma->vm_ops = &cedardev_remap_vm_ops;
	cedardev_vma_open(vma);

	return 0; 
}
#else
static int cedardev_mmap(struct file *filp, struct vm_area_struct *vma)
{
    unsigned long temp_pfn;
    unsigned int  VAddr;
	struct iomap_para addrs;

	unsigned int io_ram = 0;
    VAddr = vma->vm_pgoff << 12;
	addrs = cedar_devp->iomap_addrs;

    if (VAddr == (unsigned int)addrs.regs_macc) {
        temp_pfn = MACC_REGS_BASE >> 12;
        io_ram = 1;
    } else {
        temp_pfn = (__pa(vma->vm_pgoff << 12))>>12;
        io_ram = 0;
    }

    if (io_ram == 0) {   
        /* Set reserved and I/O flag for the area. */
        vma->vm_flags |= /* VM_RESERVED | */VM_IO;

        /* Select uncached access. */
        //vma->vm_page_prot = pgprot_noncached(vma->vm_page_prot);

        if (remap_pfn_range(vma, vma->vm_start, temp_pfn,
                            vma->vm_end - vma->vm_start, vma->vm_page_prot)) {
            return -EAGAIN;
        }
    } else {
        /* Set reserved and I/O flag for the area. */
        vma->vm_flags |= /*VM_RESERVED |*/ VM_IO;
        /* Select uncached access. */
        vma->vm_page_prot = pgprot_noncached(vma->vm_page_prot);

        if (io_remap_pfn_range(vma, vma->vm_start, temp_pfn,
                               vma->vm_end - vma->vm_start, vma->vm_page_prot)) {
            return -EAGAIN;
        }
    }
    
    vma->vm_ops = &cedardev_remap_vm_ops;
    cedardev_vma_open(vma);
    
    return 0; 
}
#endif

#ifdef CONFIG_PM
static int snd_sw_cedar_suspend(struct platform_device *pdev,pm_message_t state)
{	
	int ret = 0;

	printk("[cedar] standby suspend\n");
	ret = disable_cedar_hw_clk();

#if defined CONFIG_ARCH_SUN9IW1P1
	clk_disable_unprepare(ve_power_gating);
#endif

	if (ret < 0) {
		cedar_ve_printk(KERN_WARNING, "cedar clk disable somewhere error!\n");
		return -EFAULT;
	}

	return 0;
}

static int snd_sw_cedar_resume(struct platform_device *pdev)
{
	int ret = 0;

	printk("[cedar] standby resume\n");

#if defined CONFIG_ARCH_SUN9IW1P1
	clk_prepare_enable(ve_power_gating);
#endif

	if (cedar_devp->ref_count == 0) {
		return 0;
	}

	ret = enable_cedar_hw_clk();
	if (ret < 0) {
		cedar_ve_printk(KERN_WARNING, "cedar clk enable somewhere error!\n");
		return -EFAULT;
	}
	return 0;
}
#endif

static const struct file_operations cedardev_fops = {
	.owner   = THIS_MODULE,
	.mmap    = cedardev_mmap,
	.open    = cedardev_open,
	.release = cedardev_release,
	.llseek  = no_llseek,
	.unlocked_ioctl   = cedardev_ioctl,
#ifdef CONFIG_COMPAT
	.compat_ioctl = compat_cedardev_ioctl,
#endif
};

static int cedardev_init(struct platform_device *pdev)
{
	int ret = 0;
	int devno;
#ifdef CONFIG_OF
	struct device_node *node;
#endif /*CONFIG_OF*/
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4,11,0)
	struct device_node *np;
#endif /*LINUX_VERSION_CODE >= KERNEL_VERSION(4,11,0)*/
	dev_t dev;

	dev = 0;

	printk("[cedar]: install start!!!\n");


#if defined(CONFIG_OF)
	node = pdev->dev.of_node;
#endif

#if LINUX_VERSION_CODE >= KERNEL_VERSION(4,11,0)
	np = of_find_matching_node(NULL, sunxi_cedar_ve_match);
	if (!np) {
	  printk(KERN_ERR "Couldn't find the VE node\n");
	  return -ENODEV;
	}
#endif /*LINUX_VERSION_CODE >= KERNEL_VERSION(4,11,0)*/

	/*register or alloc the device number.*/
	if (g_dev_major) {
		dev = MKDEV(g_dev_major, g_dev_minor);	
		ret = register_chrdev_region(dev, 1, "cedar_dev");
	} else {
		ret = alloc_chrdev_region(&dev, g_dev_minor, 1, "cedar_dev");
		g_dev_major = MAJOR(dev);
		g_dev_minor = MINOR(dev);
	}

	if (ret < 0) {
		cedar_ve_printk(KERN_WARNING, "cedar_dev: can't get major %d\n", \
			g_dev_major);
		return ret;
	}
	spin_lock_init(&cedar_spin_lock);
	cedar_devp = kmalloc(sizeof(struct cedar_dev), GFP_KERNEL);
	if (cedar_devp == NULL) {
		cedar_ve_printk(KERN_WARNING, "malloc mem for cedar device err\n");
		return -ENOMEM;
	}		
	memset(cedar_devp, 0, sizeof(struct cedar_dev));

	cedar_devp->dev = &pdev->dev;
	cedar_devp->pdev = pdev;

#if defined(CONFIG_OF)
	cedar_devp->irq = irq_of_parse_and_map(node, 0);
	cedar_ve_printk(KERN_INFO, "cedar-ve the get irq is %d\n", \
		cedar_devp->irq);
	if (cedar_devp->irq <= 0)
		cedar_ve_printk(KERN_WARNING, "Can't parse IRQ");
#else
	cedar_devp->irq = SUNXI_IRQ_VE;
#endif

#if defined(CONFIG_OF) && !defined(USE_ION)
	// assign reserved memory for the VE
	ret = of_reserved_mem_device_init(cedar_devp->dev);
    if (ret) {
        dev_err(cedar_devp->dev, "could not assign reserved memory\n");
        return -ENODEV;
    }
#endif

	sema_init(&cedar_devp->sem, 1);
	init_waitqueue_head(&cedar_devp->wq);	

	memset(&cedar_devp->iomap_addrs, 0, sizeof(struct iomap_para));

	ret = request_irq(cedar_devp->irq, VideoEngineInterupt, 0, "cedar_dev", NULL);
	if (ret < 0) {
		cedar_ve_printk(KERN_WARNING, "request irq err\n");
		return -EINVAL;
	}

	/* map for macc io space */
#if defined(CONFIG_OF)
	cedar_devp->iomap_addrs.regs_macc = of_iomap(node, 0);
	if (!cedar_devp->iomap_addrs.regs_macc)
		cedar_ve_printk(KERN_WARNING, "ve Can't map registers");

	cedar_devp->sram_bass_vir = (u32*)of_iomap(node, 1);
	if (!cedar_devp->sram_bass_vir)
		cedar_ve_printk(KERN_WARNING, "ve Can't map sram_bass_vir registers");

	cedar_devp->clk_bass_vir = (u32*)of_iomap(node, 2);
	if (!cedar_devp->clk_bass_vir)
		cedar_ve_printk(KERN_WARNING, "ve Can't map clk_bass_vir registers");
#endif

#if LINUX_VERSION_CODE < KERNEL_VERSION(4,11,0) && ((defined CONFIG_ARCH_SUN8IW7P1) || (defined CONFIG_ARCH_SUN8IW8P1) || (defined CONFIG_ARCH_SUN8IW9P1))
#ifdef CONFIG_OF
	if (of_device_is_compatible(np, "allwinner,sun8i-h3-video-engine")) {
#endif /* CONFIG_OF*/
	/*VE_SRAM mapping to AC320*/
	  {
	u32 val;
	//printk(KERN_DEBUG "patching H3... %08x\n"\n, sunxi_smc_readl((void __iomem *)0xf1c00000));
	val = sunxi_smc_readl((void __iomem *)0xf1c00000);
	val &= 0x80000000;
	sunxi_smc_writel(val, (void __iomem *)0xf1c00000); 
	//printk(KERN_DEBUG "patching H3... %08x\n"\n, sunxi_smc_readl((void __iomem *)0xf1c00000));

	/*remapping SRAM to MACC for codec test*/
	val = sunxi_smc_readl((void __iomem *)0xf1c00000);
	val |= 0x7fffffff;
	sunxi_smc_writel(val, (void __iomem *)0xf1c00000);

	//clear bootmode bit for give sram to ve
	val = sunxi_smc_readl((void __iomem *)0xf1c00004);
	val &= 0xfeffffff;
	sunxi_smc_writel(val, (void __iomem *)0xf1c00004);
	  }
#if LINUX_VERSION_CODE < KERNEL_VERSION(4,11,0)
	  ve_parent_pll_clk = clk_get(NULL, "pll_ve");
	if ((!ve_parent_pll_clk)||IS_ERR(ve_parent_pll_clk)) {
		printk("try to get ve_parent_pll_clk fail\n");
		return -EINVAL;
	}
#endif /*LINUX_VERSION_CODE < KERNEL_VERSION(4,11,0)*/
#ifdef CONFIG_OF
	} else {
#endif /* CONFIG_OF*/
#if (defined CONFIG_ARCH_SUNIVW1P1) /*for 1663*/
	  {
	    u32 val;
	val = readl(cedar_devp->clk_bass_vir+6);
	val &= 0x7fff80f0;
	val = val | (1<<31) | (8<<8);
	writel(val, cedar_devp->clk_bass_vir+6);

	/*set VE clock dividor*/
	val = readl(cedar_devp->clk_bass_vir+79);
	val |= (1<<31);
	writel(val, cedar_devp->clk_bass_vir+79);

	/*Active AHB bus to MACC*/
	val = readl(cedar_devp->clk_bass_vir+25);
	val |= (1<<0);
	writel(val, cedar_devp->clk_bass_vir+25);

	/*Power on and release reset ve*/
	val = readl(cedar_devp->clk_bass_vir+177);
	val &= ~(1<<0); /*reset ve*/
	writel(val, cedar_devp->clk_bass_vir+177);

	val = readl(cedar_devp->clk_bass_vir+177);
	val |= (1<<0);
	writel(val, cedar_devp->clk_bass_vir+177);

	/*gate on the bus to SDRAM*/
	val = readl(cedar_devp->clk_bass_vir+64);
	val |= (1<<0);
	writel(val, cedar_devp->clk_bass_vir+64);

	/*VE_SRAM mapping to AC320*/
	val = readl(cedar_devp->sram_bass_vir);
	val &= 0x80000000;
	writel(val, cedar_devp->sram_bass_vir);

	/*remapping SRAM to MACC for codec test*/
	val = readl(cedar_devp->sram_bass_vir);
	val |= 0x7fffffff;
	writel(val, cedar_devp->sram_bass_vir);

	/*clear bootmode bit for give sram to ve*/
	val = readl((cedar_devp->sram_bass_vir + 1));
	val &= 0xefffffff;
	writel(val, (cedar_devp->sram_bass_vir + 1));
	  }
#else
	{
	  u32 val;
	/*VE_SRAM mapping to AC320*/
	val = readl(cedar_devp->sram_bass_vir);
	val &= 0x80000000;
	writel(val, cedar_devp->sram_bass_vir);

	/*remapping SRAM to MACC for codec test*/
	val = readl(cedar_devp->sram_bass_vir);
	val |= 0x7fffffff;
	writel(val, cedar_devp->sram_bass_vir);

	/*clear bootmode bit for give sram to ve*/
	val = readl((cedar_devp->sram_bass_vir + 1));
	val &= 0xfeffffff;
	writel(val, (cedar_devp->sram_bass_vir + 1));
	}
#endif
#ifdef CONFIG_OF
	}
#endif /* CONFIG_OF*/
#endif /* ((defined CONFIG_ARCH_SUN8IW7P1) || (defined CONFIG_ARCH_SUN8IW8P1) || (defined CONFIG_ARCH_SUN8IW9P1)) */

#if LINUX_VERSION_CODE >= KERNEL_VERSION(4,11,0)
	cedar_devp->syscon = syscon_regmap_lookup_by_phandle(cedar_devp->dev->of_node, "syscon");
	if (IS_ERR(cedar_devp->syscon)) {
	  dev_err(cedar_devp->dev, "syscon failed...\n");
	  cedar_devp->syscon = NULL;
	} else {
		// remap SRAM C1 to the VE
	  	regmap_write_bits(cedar_devp->syscon, SYSCON_SRAM_CTRL_REG0,
					      SYSCON_SRAM_C1_MAP_VE,
					      SYSCON_SRAM_C1_MAP_VE);
	}

	cedar_devp->ahb_clk = devm_clk_get(cedar_devp->dev, "ahb");
       if (IS_ERR(cedar_devp->ahb_clk)) {
               dev_err(cedar_devp->dev, "failed to get ahb clock\n");
               return PTR_ERR(cedar_devp->ahb_clk);
       }
       cedar_devp->mod_clk = devm_clk_get(cedar_devp->dev, "mod");
       if (IS_ERR(cedar_devp->mod_clk)) {
               dev_err(cedar_devp->dev, "failed to get mod clock\n");
               return PTR_ERR(cedar_devp->mod_clk);
       }
       cedar_devp->ram_clk = devm_clk_get(cedar_devp->dev, "ram");
       if (IS_ERR(cedar_devp->ram_clk)) {
               dev_err(cedar_devp->dev, "failed to get ram clock\n");
               return PTR_ERR(cedar_devp->ram_clk);
       }
#else /*LINUX_VERSION_CODE >= KERNEL_VERSION(4,11,0)*/
# if defined(CONFIG_OF)
	ve_parent_pll_clk = of_clk_get(node, 0);
	if ((!ve_parent_pll_clk) || IS_ERR(ve_parent_pll_clk)) {
		cedar_ve_printk(KERN_WARNING, "try to get ve_parent_pll_clk fail\n");
		return -EINVAL;
	}

	ve_moduleclk = of_clk_get(node, 1);
	if (!ve_moduleclk || IS_ERR(ve_moduleclk)) {
		cedar_ve_printk(KERN_WARNING, "get ve_moduleclk failed; \n");
	}
# endif
#endif /*LINUX_VERSION_CODE >= KERNEL_VERSION(4,11,0)*/

	// no reset ve module
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4,11,0)
	ret = clk_prepare_enable(cedar_devp->ahb_clk);
	if (ret) {
	  dev_err(cedar_devp->dev, "could not enable ahb clock\n");
	  return -EFAULT;
	}
	ret = clk_prepare_enable(cedar_devp->mod_clk);
	if (ret) {
	  clk_disable_unprepare(cedar_devp->ahb_clk);
	  dev_err(cedar_devp->dev, "could not enable mod clock\n");
	  return -EFAULT;
	}
	ret = clk_prepare_enable(cedar_devp->ram_clk);
	if (ret) {
	  clk_disable_unprepare(cedar_devp->mod_clk);
	  clk_disable_unprepare(cedar_devp->ahb_clk);
	  dev_err(cedar_devp->dev, "could not enable ram clock\n");
	  return -EFAULT;
	}
	cedar_devp->rstc = devm_reset_control_get(cedar_devp->dev, NULL);
	if (IS_ERR(cedar_devp->rstc)) {
		dev_err(cedar_devp->dev, "Failed to get reset control\n");
		ret = PTR_ERR(cedar_devp->rstc);
		return -EFAULT;
	}

	reset_control_assert(cedar_devp->rstc);
#else /*LINUX_VERSION_CODE >= KERNEL_VERSION(4,11,0)*/
	sunxi_periph_reset_assert(ve_moduleclk);
	clk_prepare(ve_moduleclk);
#endif /*LINUX_VERSION_CODE >= KERNEL_VERSION(4,11,0)*/

	/* Create char device */
	devno = MKDEV(g_dev_major, g_dev_minor);	
	cdev_init(&cedar_devp->cdev, &cedardev_fops);
	cedar_devp->cdev.owner = THIS_MODULE;
	//cedar_devp->cdev.ops = &cedardev_fops;
	ret = cdev_add(&cedar_devp->cdev, devno, 1);
	if (ret) {
		cedar_ve_printk(KERN_WARNING, "Err:%d add cedardev", ret);
	}
	cedar_devp->class = class_create(THIS_MODULE, "cedar_dev");
	cedar_devp->odev  = device_create(cedar_devp->class, NULL, devno, NULL, "cedar_dev");
#ifdef HAVE_TIMER_SETUP
	timer_setup(&cedar_devp->cedar_engine_timer, &cedar_engine_for_events, 0);
	timer_setup(&cedar_devp->cedar_engine_timer_rel, &cedar_engine_for_timer_rel, 0);
#else
	setup_timer(&cedar_devp->cedar_engine_timer, cedar_engine_for_events, (ulong)cedar_devp);
	setup_timer(&cedar_devp->cedar_engine_timer_rel, cedar_engine_for_timer_rel, (ulong)cedar_devp);
#endif
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4,11,0)
	of_node_put(np);
#endif /*LINUX_VERSION_CODE >= KERNEL_VERSION(4,11,0)*/

#if !defined(USE_ION)
	cedar_devp->ve_size = 80 * SZ_1M;
	dma_set_coherent_mask(cedar_devp->dev, 0xFFFFFFFF);
	cedar_devp->ve_start_virt = dma_alloc_coherent(cedar_devp->dev, cedar_devp->ve_size,
												   &cedar_devp->ve_start_pa,
												   GFP_KERNEL | GFP_DMA);

	if (!cedar_devp->ve_start_virt) {
		dev_err(cedar_devp->dev, "cedar: failed to allocate memory buffer\n");
		return -ENODEV;
	}
	cedar_devp->ve_start = cedar_devp->ve_start_pa;

	printk("[cedar]: memory allocated at address %08lX\n", cedar_devp->ve_start);
#endif

	printk("[cedar]: install end!!!\n");
	return 0;
}


static void cedardev_exit(void)
{
	dev_t dev;
	dev = MKDEV(g_dev_major, g_dev_minor);

	free_irq(cedar_devp->irq, NULL);
	iounmap(cedar_devp->iomap_addrs.regs_macc);
	//	iounmap(cedar_devp->iomap_addrs.regs_avs);

	if (cedar_devp->sram_bass_vir) iounmap(cedar_devp->sram_bass_vir);
        if (cedar_devp->clk_bass_vir)  iounmap(cedar_devp->clk_bass_vir);

#if !defined(USE_ION)
    if (cedar_devp->ve_start_virt)
    	dma_free_coherent(NULL, cedar_devp->ve_size, cedar_devp->ve_start_virt, cedar_devp->ve_start);      	 

# if defined(CONFIG_OF)
	of_reserved_mem_device_release(cedar_devp->dev);
# endif	
#endif	

	/* Destroy char device */
	if (cedar_devp) {
		cdev_del(&cedar_devp->cdev);
		device_destroy(cedar_devp->class, dev);
		class_destroy(cedar_devp->class);
	}

#if LINUX_VERSION_CODE >= KERNEL_VERSION(4,11,0)
	if (NULL == cedar_devp->mod_clk || IS_ERR(cedar_devp->mod_clk)) { cedar_ve_printk(KERN_WARNING, "ve_moduleclk handle is invalid,just return! (%d)\n", __LINE__); }
	if (NULL == cedar_devp->ram_clk || IS_ERR(cedar_devp->ram_clk)) { cedar_ve_printk(KERN_WARNING, "ve_moduleclk handle is invalid,just return! (%d)\n", __LINE__); }
	clk_disable_unprepare(cedar_devp->mod_clk);
	clk_disable_unprepare(cedar_devp->ram_clk);
	cedar_devp->mod_clk = NULL;
	cedar_devp->ram_clk = NULL;
#else /*LINUX_VERSION_CODE >= KERNEL_VERSION(4,11,0)*/
	if (NULL == ve_moduleclk || IS_ERR(ve_moduleclk)) {
		cedar_ve_printk(KERN_WARNING, "ve_moduleclk handle "
			"is invalid,just return!\n");
	} else {
		clk_disable_unprepare(ve_moduleclk);
		clk_put(ve_moduleclk);
		ve_moduleclk = NULL;
	}
#endif /*LINUX_VERSION_CODE >= KERNEL_VERSION(4,11,0)*/

#if LINUX_VERSION_CODE >= KERNEL_VERSION(4,11,0)
	if (NULL == cedar_devp->ahb_clk || IS_ERR(cedar_devp->ahb_clk)) {
	  cedar_ve_printk(KERN_WARNING, "ve_parent_pll_clk handle is invalid,just return!\n");
	} else {	
	  clk_disable_unprepare(cedar_devp->ahb_clk);
	  cedar_devp->ahb_clk = NULL;
	}
#else /*LINUX_VERSION_CODE >= KERNEL_VERSION(4,11,0)*/
	if (NULL == ve_parent_pll_clk || IS_ERR(ve_parent_pll_clk)) {
		cedar_ve_printk(KERN_WARNING, "ve_parent_pll_clk "
			"handle is invalid,just return!\n");
	} else {	
		clk_put(ve_parent_pll_clk);
	}
#endif /*LINUX_VERSION_CODE >= KERNEL_VERSION(4,11,0)*/

#if defined CONFIG_ARCH_SUN9IW1P1
	//put regulator when module exit
	regulator_put(regu);

	if (NULL == ve_power_gating || IS_ERR(ve_power_gating)) {
		cedar_ve_printk(KERN_WARNING, "ve_power_gating "
			"handle is invalid,just return!\n");
	} else {
		clk_disable_unprepare(ve_power_gating);
		clk_put(ve_power_gating);
		ve_power_gating = NULL;
	}
#endif

	unregister_chrdev_region(dev, 1);	
	//  platform_driver_unregister(&sw_cedar_driver);
	if (cedar_devp) {
		kfree(cedar_devp);
	}
}

static int  sunxi_cedar_remove(struct platform_device *pdev)
{
	cedardev_exit();
	return 0;
}

static int  sunxi_cedar_probe(struct platform_device *pdev)
{
	cedardev_init(pdev);
	return 0;
}

/*share the irq no. with timer2*/
/*
static struct resource sunxi_cedar_resource[] = {
	[0] = {
		.start = SUNXI_IRQ_VE,
		.end   = SUNXI_IRQ_VE,
		.flags = IORESOURCE_IRQ,
	},
};

struct platform_device sunxi_device_cedar = {
	.name		= "sunxi-cedar",
	.id		= -1,
	.num_resources	= ARRAY_SIZE(sunxi_cedar_resource),
	.resource	= sunxi_cedar_resource,
};
*/

static struct platform_driver sunxi_cedar_driver = {
	.probe		= sunxi_cedar_probe,
	.remove		= sunxi_cedar_remove,
#ifdef CONFIG_PM
	.suspend	= snd_sw_cedar_suspend,
	.resume		= snd_sw_cedar_resume,
#endif
	.driver		= {
		.name	= "sunxi-cedar",
		.owner	= THIS_MODULE,

#if defined(CONFIG_OF)
		.of_match_table = sunxi_cedar_ve_match,
#endif
	},
};

static int __init sunxi_cedar_init(void)
{
	//need not to gegister device here,because the device is registered by device tree 
	//platform_device_register(&sunxi_device_cedar);
	printk("sunxi cedar version 0.1 \n");
	return platform_driver_register(&sunxi_cedar_driver);
}

static void __exit sunxi_cedar_exit(void)
{
	platform_driver_unregister(&sunxi_cedar_driver);
}

module_init(sunxi_cedar_init);
module_exit(sunxi_cedar_exit);


MODULE_AUTHOR("Soft-Reuuimlla");
MODULE_DESCRIPTION("User mode CEDAR device interface");
MODULE_LICENSE("GPL");
MODULE_VERSION(DRV_VERSION);
MODULE_ALIAS("platform:cedarx-sunxi");
