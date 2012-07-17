/*
 * arch/arm/mach-s5pv210/cpuidle.c
 *
 * Copyright (c) Samsung Electronics Co. Ltd
 * Copyright (c) 2012 - Will Tisdale <willtisdale@gmail.com>
 *
 * CPU idle driver for S5PV210
 *
 * This file is licensed under the terms of the GNU General Public
 * License version 2.  This program is licensed "as is" without any
 * warranty of any kind, whether express or implied.
 */

#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/platform_device.h>
#include <linux/cpuidle.h>
#include <linux/io.h>
#include <asm/proc-fns.h>
#include <asm/cacheflush.h>
#include <linux/dma-mapping.h>

#include <mach/map.h>
#include <mach/regs-irq.h>
#include <mach/regs-clock.h>
#include <plat/pm.h>
#ifdef CONFIG_S5P_IDLE2
#include <plat/regs-serial.h>
#include <plat/regs-otg.h>
#include <asm/hardware/pl330.h>
#endif
#include <mach/cpuidle.h>
#include <plat/devs.h>

#include <mach/dma.h>

#include <mach/regs-gpio.h>

#define S5PC110_MAX_STATES	1

inline static void s5p_enter_idle(void)
{
	unsigned long tmp;

	tmp = __raw_readl(S5P_IDLE_CFG);
	tmp &= ~((3<<30)|(3<<28)|(1<<0));
	tmp |= ((2<<30)|(2<<28));
	__raw_writel(tmp, S5P_IDLE_CFG);

	tmp = __raw_readl(S5P_PWR_CFG);
	tmp &= S5P_CFG_WFI_CLEAN;
	__raw_writel(tmp, S5P_PWR_CFG);

	cpu_do_idle();
}

/* Actual code that puts the SoC in different idle states */
inline static int s5p_enter_idle_normal(struct cpuidle_device *dev,
				struct cpuidle_state *state)
{
	struct timeval before, after;
	int idle_time;

	local_irq_disable();
	do_gettimeofday(&before);

	s5p_enter_idle();

	do_gettimeofday(&after);
	local_irq_enable();
	idle_time = (after.tv_sec - before.tv_sec) * USEC_PER_SEC +
			(after.tv_usec - before.tv_usec);
	return idle_time;
}

static DEFINE_PER_CPU(struct cpuidle_device, s5p_cpuidle_device);

static struct cpuidle_driver s5p_idle_driver = {
	.name =         "s5p_idle",
	.owner =        THIS_MODULE,
};

#ifdef CONFIG_S5P_IDLE2
int previous_idle_mode = NORMAL_MODE;
extern void s5p_idle2(void);

/* For saving & restoring VIC register before entering
 * idle2 mode
 **/
static unsigned long vic_regs[4];
static unsigned long *regs_save;
static dma_addr_t phy_regs_save;

#define MAX_CHK_DEV	0xf

/* Specific device list for checking before entering
 * idle2 mode
 **/
struct check_device_op {
	void __iomem		*base;
	struct platform_device	*pdev;
};

/* Array of checking devices list */
static struct check_device_op chk_dev_op[] = {
#if defined(CONFIG_S3C_DEV_HSMMC)
	{.base = 0, .pdev = &s3c_device_hsmmc0},
#endif
#if defined(CONFIG_S3C_DEV_HSMMC1)
	{.base = 0, .pdev = &s3c_device_hsmmc1},
#endif
#if defined(CONFIG_S3C_DEV_HSMMC2)
	{.base = 0, .pdev = &s3c_device_hsmmc2},
#endif
#if defined(CONFIG_S3C_DEV_HSMMC3)
	{.base = 0, .pdev = &s3c_device_hsmmc3},
#endif
	{.base = 0, .pdev = &s5p_device_onenand},
	{.base = 0, .pdev = &s3c_device_i2c0},
	{.base = 0, .pdev = &s3c_device_i2c1},
	{.base = 0, .pdev = &s3c_device_i2c2},
	{.base = 0, .pdev = NULL},
};

/* Check 3D */
inline static bool check_g3d_op(void)
{
	unsigned long val;

	val    = __raw_readl(S5P_CLKGATE_IP0);

	if(val & S5P_CLKGATE_IP0_G3D) {
#ifdef CONFIG_S5P_IDLE2_DEBUG
		printk(KERN_INFO "%s: returns true\n", __func__);
#endif
		return true;
	}
	else
		return false;
}

#define S3C_HSMMC_PRNSTS	(0x24)
#define S3C_HSMMC_CLKCON	(0x2c)
#define S3C_HSMMC_CMD_INHIBIT	0x00000001
#define S3C_HSMMC_DATA_INHIBIT	0x00000002
#define S3C_HSMMC_CLOCK_CARD_EN	0x0004


/* If SD/MMC interface is working: return = true */
inline static bool check_sdmmc_op(unsigned int ch)
{
	unsigned int reg1, reg2;
	void __iomem *base_addr;

	if (unlikely(ch > 2)) {
		printk(KERN_ERR "Invalid ch[%d] for SD/MMC \n", ch);
		return false;
	}

	base_addr = chk_dev_op[ch].base;
	/* Check CMDINHDAT[1] and CMDINHCMD [0] */
	reg1 = readl(base_addr + S3C_HSMMC_PRNSTS);
	/* Check CLKCON [2]: ENSDCLK */
	reg2 = readl(base_addr + S3C_HSMMC_CLKCON);

	if ((reg1 & (S3C_HSMMC_CMD_INHIBIT | S3C_HSMMC_DATA_INHIBIT)) ||
			(reg2 & (S3C_HSMMC_CLOCK_CARD_EN))) {
#ifdef CONFIG_S5P_IDLE2_DEBUG
		printk(KERN_INFO "%s: returns true\n", __func__);
#endif
		return true;
	} else {
		return false;
	}
}

/* Check all sdmmc controller */
inline static bool loop_sdmmc_check(void)
{
	unsigned int iter;

	for (iter = 0; iter < 3; iter++) {
		if (check_sdmmc_op(iter))
			return true;
	}
	return false;
}

/* Check onenand is working or not */

/* ONENAND_IF_STATUS(0xB060010C)
 * ORWB[0] = 	1b : busy
 * 		0b : Not busy
 **/
inline static bool check_onenand_op(void)
{
	unsigned int val;
	void __iomem *base_addr;

	base_addr = chk_dev_op[3].base;

	val = __raw_readl(base_addr + 0x0000010c);

	if (val & 0x1) {
#ifdef CONFIG_S5P_IDLE2_DEBUG
		printk(KERN_INFO "%s: returns true\n", __func__);
#endif
		return true;
	}
	return false;
}

inline static bool check_dma_op(void)
{
	unsigned long val;

	val = __raw_readl(S5P_CLKGATE_IP0);
	if (val & (S5P_CLKGATE_IP0_MDMA | S5P_CLKGATE_IP0_PDMA0
					| S5P_CLKGATE_IP0_PDMA1)) {
#ifdef CONFIG_S5P_IDLE2_DEBUG
		printk(KERN_INFO "%s: returns true\n", __func__);
#endif
		return true;
	}
	return false;
}

extern void i2sdma_getpos(dma_addr_t *src);
inline static bool check_idmapos(void)
{
	dma_addr_t src;
	i2sdma_getpos(&src);
	src = src & 0x3FFF;
	src = 0x4000 - src;
	if(src < 0x150){
#ifdef CONFIG_S5P_IDLE2_DEBUG
		printk(KERN_INFO "%s: returns true\n", __func__);
#endif
		return true;
	}
	else
		return false;
}
extern unsigned int get_rtc_cnt(void);
inline static bool check_rtcint(void)
{
	unsigned int current_cnt = get_rtc_cnt();
	if(current_cnt < 0x40){
#ifdef CONFIG_S5P_IDLE2_DEBUG
		printk(KERN_INFO "%s: returns true\n", __func__);
#endif
		return true;
	}
	else
		return false;
}


/* Before entering, idle2 mode GPIO Powe Down Mode
 * Configuration register has to be set with same state
 * in Normal Mode
 **/
#define GPIO_OFFSET		0x20
#define GPIO_CON_PDN_OFFSET	0x10
#define GPIO_PUD_PDN_OFFSET	0x14
#define GPIO_PUD_OFFSET		0x08

inline static void s5p_gpio_pdn_conf(void)
{
	void __iomem *gpio_base = S5PV210_GPA0_BASE;
	unsigned int val;

	do {
		/* Keep the previous state in idle2 mode */
		__raw_writel(0xffff, gpio_base + GPIO_CON_PDN_OFFSET);

		/* Pull up-down state in idle2 is same as normal */
		val = __raw_readl(gpio_base + GPIO_PUD_OFFSET);
		__raw_writel(val, gpio_base + GPIO_PUD_PDN_OFFSET);

		gpio_base += GPIO_OFFSET;

	} while (gpio_base <= S5PV210_MP28_BASE);
}

inline static void s5p_enter_idle2(void)
{
	unsigned long tmp;
	unsigned long save_eint_mask;

	/* store the physical address of the register recovery block */
	__raw_writel(phy_regs_save, S5P_INFORM2);

	__raw_writel(IDLE2_MODE, S5P_INFORM1);

	/* ensure at least INFORM0 has the resume address */
	__raw_writel(virt_to_phys(s5p_idle2_resume), S5P_INFORM0);

	/* Save current VIC_INT_ENABLE register*/
	vic_regs[0] = __raw_readl(S5P_VIC0REG(VIC_INT_ENABLE));
	vic_regs[1] = __raw_readl(S5P_VIC1REG(VIC_INT_ENABLE));
	vic_regs[2] = __raw_readl(S5P_VIC2REG(VIC_INT_ENABLE));
	vic_regs[3] = __raw_readl(S5P_VIC3REG(VIC_INT_ENABLE));

	/* Disable all interrupt through VIC */
	__raw_writel(0xffffffff, S5P_VIC0REG(VIC_INT_ENABLE_CLEAR));
	__raw_writel(0xffffffff, S5P_VIC1REG(VIC_INT_ENABLE_CLEAR));
	__raw_writel(0xffffffff, S5P_VIC2REG(VIC_INT_ENABLE_CLEAR));
	__raw_writel(0xffffffff, S5P_VIC3REG(VIC_INT_ENABLE_CLEAR));

	/* GPIO Power Down Control */
	s5p_gpio_pdn_conf();
        save_eint_mask = __raw_readl(S5P_EINT_WAKEUP_MASK);
        __raw_writel(0xFFFFFFFF, S5P_EINT_WAKEUP_MASK);

	/* Wakeup source configuration for idle2 */
	tmp = __raw_readl(S5P_WAKEUP_MASK);

	tmp |= 0xffff;
        // Key interrupt mask idma & RTC tic only
        tmp &= ~((1<<2) | (1<<13));

	__raw_writel(tmp, S5P_WAKEUP_MASK);

	/* Clear wakeup status register */
	tmp = __raw_readl(S5P_WAKEUP_STAT);
	__raw_writel(tmp, S5P_WAKEUP_STAT);

	/* IDLE config register set */
	/* TOP Memory retention off */
	/* TOP Memory LP mode       */
	/* ARM_L2_Cacheret on       */
	tmp = __raw_readl(S5P_IDLE_CFG);
	tmp &= ~(0x3f << 26);
	tmp |= ((1<<30) | (1<<28) | (1<<26) | (1<<0));
	__raw_writel(tmp, S5P_IDLE_CFG);

	/* Power mode Config setting */
	tmp = __raw_readl(S5P_PWR_CFG);
	tmp &= S5P_CFG_WFI_CLEAN;
	tmp |= S5P_CFG_WFI_IDLE;
	__raw_writel(tmp, S5P_PWR_CFG);

	/* To check VIC Status register before enter idle2 mode */
	if (__raw_readl(S5P_VIC2REG(VIC_RAW_STATUS)) & 0x10000) {
		printk(KERN_WARNING "%s: Skipping IDLE2\n", __func__);
		goto skipped_idle2;
	}

	/* SYSCON_INT_DISABLE */
	tmp = __raw_readl(S5P_OTHERS);
	tmp |= S5P_OTHER_SYSC_INTOFF;
	__raw_writel(tmp, S5P_OTHERS);

	/* Entering idle2 mode with WFI instruction */
	if (s5p_idle2_save(regs_save) == 0) {
#ifdef CONFIG_S5P_IDLE2_DEBUG
		printk(KERN_INFO "*** Entering IDLE2 mode\n");
#endif
		s5p_idle2();
	}

skipped_idle2:
	__raw_writel(save_eint_mask, S5P_EINT_WAKEUP_MASK);

	tmp = __raw_readl(S5P_IDLE_CFG);
	tmp &= ~((3<<30)|(3<<28)|(3<<26)|(1<<0));
	tmp |= ((2<<30)|(2<<28));
	__raw_writel(tmp, S5P_IDLE_CFG);

	/* Power mode Config setting */
	tmp = __raw_readl(S5P_PWR_CFG);
	tmp &= S5P_CFG_WFI_CLEAN;
	__raw_writel(tmp, S5P_PWR_CFG);

	/* Release retention GPIO/MMC/UART IO */
	tmp = __raw_readl(S5P_OTHERS);
	tmp |= ((1<<31) | (1<<30) | (1<<29) | (1<<28));
	__raw_writel(tmp, S5P_OTHERS);

	__raw_writel(vic_regs[0], S5P_VIC0REG(VIC_INT_ENABLE));
	__raw_writel(vic_regs[1], S5P_VIC1REG(VIC_INT_ENABLE));
	__raw_writel(vic_regs[2], S5P_VIC2REG(VIC_INT_ENABLE));
	__raw_writel(vic_regs[3], S5P_VIC3REG(VIC_INT_ENABLE));
}

static spinlock_t idle2_lock;
int idle2_lock_count = 0;
/*
 * flag = 0 : allow to enter idle2
 * flag = 1 : don't allow to enter idle2
 */
void s5p_set_idle2_lock(int flag)
{
	spin_lock(&idle2_lock);

	if (flag == 1)
		idle2_lock_count++;
	else
		idle2_lock_count--;

	if (idle2_lock_count < 0)
		idle2_lock_count = 0;
	printk(KERN_INFO "idle2: %d locks enabled\n", idle2_lock_count);

	spin_unlock(&idle2_lock);
}
EXPORT_SYMBOL(s5p_set_idle2_lock);

inline int s5p_get_idle2_lock(void)
{
	return idle2_lock_count;
}

inline static bool s5p_idle_bm_check(void)
{
	if (likely(has_audio_wake_lock() && s5p_get_idle2_lock() == 0)) {
		if (unlikely(loop_sdmmc_check() || check_onenand_op()
			|| check_dma_op() || check_g3d_op()
			|| check_idmapos() || check_rtcint()))
			return true;
		else
			return false;
	}
	return true;
}

/* Actual code that puts the SoC in different idle states */
inline static int s5p_enter_idle_idle2(struct cpuidle_device *dev,
				struct cpuidle_state *state)
{
	struct timeval before, after;
	int idle_time;

	local_irq_disable();
	do_gettimeofday(&before);

	s5p_enter_idle2();
	do_gettimeofday(&after);
	local_irq_enable();
	idle_time = (after.tv_sec - before.tv_sec) * USEC_PER_SEC +
			(after.tv_usec - before.tv_usec);
	return idle_time;
}

inline static int s5p_enter_idle_bm(struct cpuidle_device *dev,
				struct cpuidle_state *state)
{
	if (unlikely(s5p_idle_bm_check())) {
#ifdef CONFIG_S5P_IDLE2_DEBUG
		printk("%s: s5p_idle_bm_check() true - Entering normal IDLE\n", __func__);
#endif
		return s5p_enter_idle_normal(dev, state);
	}
	else
		return s5p_enter_idle_idle2(dev, state);
}

int s5p_setup_idle2(unsigned int mode)
{
	struct cpuidle_device *device;

	int ret = 0;

	cpuidle_pause_and_lock();
	device = &per_cpu(s5p_cpuidle_device, smp_processor_id());
	cpuidle_disable_device(device);

	switch (mode) {
	case NORMAL_MODE:
		device->state_count = 1;
		/* Wait for interrupt state */
		device->states[0].enter = s5p_enter_idle_normal;
		device->states[0].exit_latency = 1;	/* uS */
		device->states[0].target_residency = 10000;
		device->states[0].flags = CPUIDLE_FLAG_TIME_VALID;
		strcpy(device->states[0].name, "IDLE");
		strcpy(device->states[0].desc, "ARM clock gating - WFI");
		break;
	case IDLE2_MODE:
		device->state_count = 1;
		/* Wait for interrupt state */
		device->states[0].enter = s5p_enter_idle_bm;
		device->states[0].exit_latency = 300;	/* uS */
		device->states[0].target_residency = 5000;
		device->states[0].flags = CPUIDLE_FLAG_TIME_VALID |
						CPUIDLE_FLAG_CHECK_BM;
		strcpy(device->states[0].name, "IDLE2");
		strcpy(device->states[0].desc, "S5PC110 idle2");

		break;
	default:
		printk(KERN_ERR "Can't find cpuidle mode %d\n", mode);
		device->state_count = 1;

		/* Wait for interrupt state */
		device->states[0].enter = s5p_enter_idle_normal;
		device->states[0].exit_latency = 1;	/* uS */
		device->states[0].target_residency = 10000;
		device->states[0].flags = CPUIDLE_FLAG_TIME_VALID;
		strcpy(device->states[0].name, "IDLE");
		strcpy(device->states[0].desc, "ARM clock gating - WFI");
		break;
	}
	cpuidle_enable_device(device);
	cpuidle_resume_and_unlock();

	return ret;
}
EXPORT_SYMBOL(s5p_setup_idle2);
#endif /* CONFIG_S5P_IDLE2 */

/* Initialize CPU idle by registering the idle states */
static int s5p_init_cpuidle(void)
{
	struct cpuidle_device *device;
#ifdef CONFIG_S5P_IDLE2
	struct platform_device *pdev;
	struct resource *res;
	int i = 0;
#endif /* CONFIG_S5P_IDLE2 */

	cpuidle_register_driver(&s5p_idle_driver);

	device = &per_cpu(s5p_cpuidle_device, smp_processor_id());
	device->state_count = 1;

	/* Wait for interrupt state */
	device->states[0].enter = s5p_enter_idle_normal;
	device->states[0].exit_latency = 1;	/* uS */
	device->states[0].target_residency = 10000;
	device->states[0].flags = CPUIDLE_FLAG_TIME_VALID;
	strcpy(device->states[0].name, "IDLE");
	strcpy(device->states[0].desc, "ARM clock gating - WFI");

	if (cpuidle_register_device(device)) {
		printk(KERN_ERR "s5p_init_cpuidle: Failed registering\n");
		BUG();
		return -EIO;
	}
#ifdef CONFIG_S5P_IDLE2
	regs_save = dma_alloc_coherent(NULL, 4096, &phy_regs_save, GFP_KERNEL);
	if (regs_save == NULL) {
		printk(KERN_ERR "DMA alloc error\n");
		BUG();
		return -ENOMEM;
	}
	printk(KERN_INFO "cpuidle: IDLE2 support enabled - version 0.110 by <willtisdale@gmail.com>\n");
	printk(KERN_INFO "cpuidle: phy_regs_save:0x%x\n", phy_regs_save);

	spin_lock_init(&idle2_lock);

	/* Allocate memory region to access IP's directly */
	for (i = 0 ; i < MAX_CHK_DEV ; i++) {

		pdev = chk_dev_op[i].pdev;

		if (pdev == NULL)
			break;

		res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
		if (!res) {
			printk(KERN_ERR "failed to get io memory region\n");
			return -EINVAL;
		}
		/* ioremap for register block */
		if(pdev == &s5p_device_onenand)
			chk_dev_op[i].base = ioremap(res->start+0x00600000, 4096);
		else
			chk_dev_op[i].base = ioremap(res->start, 4096);

		if (!chk_dev_op[i].base) {
			printk(KERN_ERR "failed to remap io region\n");
			return -EINVAL;
		}
	}
#endif /* CONFIG_S5P_IDLE2 */

	return 0;
}

device_initcall(s5p_init_cpuidle);
