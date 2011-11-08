/* Copyright (c) 2011, Will Tisdale <willtisdale@gmail.com>. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 and
 * only version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
 * 02110-1301, USA.
 *
 */

/*
 * MSM CPU hotplug driver to control CPU1 on the MSM8x60 platform
 * and do away with the expensive userspace junk from Qualcomm.
 */

#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/module.h>
#include <linux/cpu.h>
#include <linux/workqueue.h>
#include <linux/sched.h>
#include <linux/earlysuspend.h>
#include <linux/spinlock.h>

/*
 * Enable debug output to dump the average
 * calculations and ring buffer array values
 */
#define DEBUG 0

/*
 * Set ENABLED to 1 to enable automatic hotplug
 * Set to 0 to only use CPU0 and leave CPU1 disabled
 */
#define ENABLED 1

/*
 * Set BOOST to 1 to permanently online CPU1
 * WARNING: This may stop power collapse suspend
 * working correctly.
 */
#define BOOST 0

#define SAMPLING_PERIODS 6
#define SAMPLING_RATE msecs_to_jiffies(10)
#define ENABLE_RUNNING_THRESHOLD 400
#define DISABLE_RUNNING_THRESHOLD 200

struct delayed_work msm_hotplug_work;
struct delayed_work hotplug_online_work;
struct delayed_work hotplug_offline_work;

static int enabled = ENABLED;
static int boost = BOOST;
static int suspended = 0;
static unsigned int index = (SAMPLING_PERIODS - 1);
static unsigned int history[SAMPLING_PERIODS];
/*
 * Initialize avg_running with a high value to avoid
 * offlining as soon as the averaging starts
 * This high value will be averaged down after a few iterations
 */
static unsigned int avg_running = 1000 * SAMPLING_PERIODS;

static struct workqueue_struct *msm_hotplug_wq;
extern int board_mfg_mode(void);
static DEFINE_SPINLOCK(hotplug_lock);


static void msm_hotplug_work_fn(struct work_struct *work)
{
	unsigned long flags;
	int i,j,running,rate;
#if DEBUG
	int k;
#endif

	spin_lock_irqsave(&hotplug_lock, flags);
	/*
	 * Reduce sampling rate when second core is online
	 * there is no rush to offline the second core
	 */
	if (unlikely(cpu_online(1)))
		rate = SAMPLING_RATE * 2;
	else
		rate = SAMPLING_RATE;

	/*
	 * Multiply nr_running() by 100 so we don't have to
	 * use float division to get the average
	 */
	running = nr_running() * 100;

	history[index] = running;

#if DEBUG
	printk(KERN_DEBUG "index is: %d\n", index);
	printk(KERN_DEBUG "running is: %d\n", running);
#endif

	/*
	 * Use a circular buffer to calculate the average load
	 * over the sampling periods.
	 * This will absorb load spikes of short duration where
	 * we don't want the second core to be onlined.
	 */
	for (i = 0, j = index; i < SAMPLING_PERIODS; i++, j--) {
		avg_running += history[j];
		if (unlikely(j == 0))
			j = SAMPLING_PERIODS;
	}

#if DEBUG
	printk(KERN_DEBUG "array contents: ");
	for (k = 0; k < SAMPLING_PERIODS; k++) {
		 printk("%d, ", history[k]);
	}
	printk(KERN_DEBUG "\n");
	printk(KERN_DEBUG "avg_running before division: %d\n", avg_running);
#endif

	avg_running = avg_running / (SAMPLING_PERIODS + 1);

	/*
	 * If we are at the end of the buffer, return to the beginning.
	 */
	if (unlikely(++index >= SAMPLING_PERIODS))
		index = 0;

#if DEBUG
	printk(KERN_DEBUG "average_running is: %d\n", avg_running);
#endif

	if(unlikely((avg_running >= ENABLE_RUNNING_THRESHOLD) && (!cpu_online(1)))) {
		printk(KERN_INFO
			"msm_hotplug: Onlining CPU1, avg running: %d\n", avg_running);
		queue_delayed_work(msm_hotplug_wq, &hotplug_online_work, 0);
		goto out;
	}
	if(unlikely((avg_running < DISABLE_RUNNING_THRESHOLD) && (cpu_online(1)))) {
		printk(KERN_INFO
			"msm_hotplug: Offlining CPU1, avg running: %d\n", avg_running);
		queue_delayed_work(msm_hotplug_wq, &hotplug_offline_work, 0);
		goto out;
	}

out:
	queue_delayed_work(msm_hotplug_wq, &msm_hotplug_work, rate);

	spin_unlock_irqrestore(&hotplug_lock, flags);
	return;
}

static int set_msm_hotplug_enabled(const char *val, struct kernel_param *kp)
{
	unsigned long flags;

	param_set_int(val, kp);
	printk(KERN_INFO "msm_hotplug enabled: %d\n",enabled);
	if (enabled) {
		spin_lock_irqsave(&hotplug_lock, flags);
		if (boost) {
			if (!cpu_online(1)) {
				printk(KERN_WARNING
					"msm_hotplug: Onlining CPU1, boost enabled\n");
				queue_delayed_work(msm_hotplug_wq, &hotplug_online_work, HZ / 10);
			}
			return 0;
		}
		queue_delayed_work(msm_hotplug_wq,&msm_hotplug_work,SAMPLING_RATE);
		spin_unlock_irqrestore(&hotplug_lock, flags);
	} else {
		cancel_rearming_delayed_work(&msm_hotplug_work);
		if (cpu_online(1)) {
			spin_lock_irqsave(&hotplug_lock, flags);
			printk(KERN_WARNING
				"msm_hotplug: Offlining CPU1, module disabled\n");
			queue_delayed_work(msm_hotplug_wq, &hotplug_offline_work, HZ / 10);
			spin_unlock_irqrestore(&hotplug_lock, flags);
		}
	}
	return 0;
}

static int set_msm_hotplug_boost(const char *val, struct kernel_param *kp)
{
	unsigned long flags;

	param_set_int(val, kp);
	printk(KERN_INFO "msm_hotplug boost: %d\n",boost);
	if (enabled) {
		if (boost) {
			cancel_rearming_delayed_work(&msm_hotplug_work);
			if (!cpu_online(1)) {
				spin_lock_irqsave(&hotplug_lock, flags);
				printk(KERN_WARNING
					"msm_hotplug: Onlining CPU1, boost enabled\n");
				queue_delayed_work(msm_hotplug_wq, &hotplug_online_work, 0);
				spin_unlock_irqrestore(&hotplug_lock, flags);
			}
		} else {
			spin_lock_irqsave(&hotplug_lock, flags);
			printk(KERN_INFO "msm_hotplug: Setting CPU1 back to auto\n");
			queue_delayed_work(msm_hotplug_wq, &msm_hotplug_work, SAMPLING_RATE);
			spin_unlock_irqrestore(&hotplug_lock, flags);
		}
	}
	return 0;
}

module_param_call(enabled, set_msm_hotplug_enabled, param_get_int, &enabled, 00644);
module_param_call(boost, set_msm_hotplug_boost, param_get_int, &boost, 00644);

static void hotplug_online(struct work_struct *work)
{
	cpu_up(1);
}

static void hotplug_offline(struct work_struct *work)
{
	cpu_down(1);
}

static void msm_hotplug_early_suspend(struct early_suspend *handler)
{
	unsigned long flags;

	if (unlikely(suspended))
		return;
	suspended = 1;
	printk(KERN_DEBUG "msm_hotplug: early suspend handler\n");
	if (likely(enabled)) {
		if (unlikely(boost)) {
			printk(KERN_WARNING
				"msm_hotplug: Not offlining CPU1 due to boost\n");
			return;
		}
		cancel_rearming_delayed_work(&msm_hotplug_work);
		if (cpu_online(1)) {
			spin_lock_irqsave(&hotplug_lock, flags);
			printk(KERN_INFO
				"msm_hotplug: Offlining CPU1 for early suspend\n");
			queue_delayed_work(msm_hotplug_wq, &hotplug_offline_work, HZ / 10);
			spin_unlock_irqrestore(&hotplug_lock, flags);
		}
	}
	return;
}

static void msm_hotplug_late_resume(struct early_suspend *handler)
{
	unsigned long flags;

	if (unlikely(!suspended))
		return;
	suspended = 0;
	printk(KERN_DEBUG "msm_hotplug: late resume handler\n");
	if (likely(enabled)) {
		spin_lock_irqsave(&hotplug_lock, flags);
		if (unlikely(boost)) {
			if (!cpu_online(1)) {
				printk(KERN_WARNING
					"msm_hotplug: Restoring boost after resume\n");
				queue_delayed_work(msm_hotplug_wq, &hotplug_online_work, 0);
			}
		} else {
			queue_delayed_work(msm_hotplug_wq, &msm_hotplug_work, SAMPLING_RATE);
		}
		spin_unlock_irqrestore(&hotplug_lock, flags);
	}
	return;
}

static struct early_suspend msm_hotplug_suspend = {
	.suspend = msm_hotplug_early_suspend,
	.resume = msm_hotplug_late_resume,
};

static int __init msm_hotplug_init(void)
{
	printk(KERN_INFO "msm_hotplug v0.192 by _thalamus init()");
	msm_hotplug_wq = create_singlethread_workqueue("msm_hotplug");
	BUG_ON(!msm_hotplug_wq);
	INIT_DELAYED_WORK_DEFERRABLE(&msm_hotplug_work, msm_hotplug_work_fn);
	INIT_DELAYED_WORK_DEFERRABLE(&hotplug_online_work, hotplug_online);
	INIT_DELAYED_WORK_DEFERRABLE(&hotplug_offline_work, hotplug_offline);
	register_early_suspend(&msm_hotplug_suspend);
	if (likely(enabled)) {
		switch (board_mfg_mode()) {
		case 0: /* normal */
		case 1: /* factory2 */
		case 2: /* recovery */
			/*
			 * 60 second delay before hotplugging starts
			 * to allow the system to fully boot.
			 * Code borrowed from pm-8x60.c
			 */
			printk(KERN_INFO
				"msm_hotplug: boot time 60 second delay begin\n");
			queue_delayed_work(msm_hotplug_wq, &msm_hotplug_work, 60 * HZ);
			break;
		case 3: /* charge */
		case 4: /* power_test */
		case 5: /* offmode_charge */
		default:
			/*
			 * Disable second core when not booted into
			 * Android OS or recovery to save power.
			 */
			printk(KERN_INFO
				"msm_hotplug: Booted into charge mode, disabling CPU1 in 10 seconds\n");
			queue_delayed_work(msm_hotplug_wq, &hotplug_offline_work, 10 * HZ);
			break;
		}
	} else {
		printk(KERN_INFO
			"msm_hotplug: Disabled by default. CPU1 will not be used.\n");
		if (cpu_online(1))
			queue_delayed_work(msm_hotplug_wq, &hotplug_offline_work, 10 * HZ);
	}
	return 0;
}
late_initcall(msm_hotplug_init);
