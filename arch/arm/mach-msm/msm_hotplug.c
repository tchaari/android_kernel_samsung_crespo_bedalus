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
 * and do away with the userspace junk Qualcomm and HTC implemented
 */

#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/module.h>
#include <linux/cpu.h>
#include <linux/workqueue.h>
#include <linux/sched.h>
#include <linux/earlysuspend.h>

#define DEBUG 0

#define SAMPLING_PERIODS 5
#define SAMPLING_RATE msecs_to_jiffies(10)
#define ENABLE_RUNNING_THRESHOLD 450
#define DISABLE_RUNNING_THRESHOLD 125

struct delayed_work msm_hotplug_work;
struct work_struct cpu_up_work;
struct work_struct cpu_down_work;
static int enabled = 1;
static int boost = 0;
static int suspended = 0;
static unsigned int index = (SAMPLING_PERIODS - 1);
static unsigned int history[SAMPLING_PERIODS];
/*
 * Initialize avg_running with a high value to avoid
 * offlining as soon as the averaging starts
 * This high value will be averaged down after a few iterations
 */
static unsigned int avg_running = 1000 * SAMPLING_PERIODS;

static DEFINE_MUTEX(msm_hotplug_lock);
static struct workqueue_struct *msm_hotplug_wq;
extern int board_mfg_mode(void);


static int msm_hotplug_running(void)
{
	int i,j,running;
#if DEBUG
	int k;
#endif
	/*
	 * Multiply nr_running() by 100 so we don't have to
	 * use float division to get the average
	 */
	mutex_lock(&msm_hotplug_lock);
	running = nr_running() * 100;
	history[index] = running;
#if DEBUG
	printk(KERN_DEBUG "index is: %d\n", index);
	printk(KERN_DEBUG "running is: %d\n", running);
#endif
	/*
	 * Use a circular buffer to calculate the average load
	 * over the sampling periods.
	 * This will absorb load spikes of short
	 * duration where we don't want the second core to be onlined.
	 * By the time it is onlined, the work would have been processed by
	 * the first core anyway, so the whole expensive operation would have
	 * been in vain.
	 */
	for (i = 0, j = index; i < SAMPLING_PERIODS; i++, j--) {
		avg_running += history[j];
		if (j == 0)
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
	if (++index >= SAMPLING_PERIODS)
		index = 0;


#if DEBUG
	printk(KERN_DEBUG "average_running is: %d\n", avg_running);
#endif

	mutex_unlock(&msm_hotplug_lock);
	return avg_running;
}

static void msm_hotplug_work_fn(struct work_struct *work)
{
	int rate;

	/*
	 * Reduce sampling rate when second core is online
	 * there is no rush to offline the second core
	 */
	if (cpu_online(1) == 1)
		rate = SAMPLING_RATE * 2;
	else
		rate = SAMPLING_RATE;

	avg_running = msm_hotplug_running();

	if((avg_running > ENABLE_RUNNING_THRESHOLD) && (cpu_online(1) == 0)) {
		printk(KERN_INFO
			"msm_hotplug: Onlining CPU1, avg running: %d\n", avg_running);
		queue_work_on(0, msm_hotplug_wq,&cpu_up_work);
		goto out;
	}
	if((avg_running <= DISABLE_RUNNING_THRESHOLD) && (cpu_online(1) == 1)) {
		printk(KERN_INFO
			"msm_hotplug: Offlining CPU1, avg running: %d\n", avg_running);
		queue_work_on(0, msm_hotplug_wq,&cpu_down_work);
		goto out;
	}

out:
	queue_delayed_work_on(0, msm_hotplug_wq, &msm_hotplug_work,
		rate);
}

static int set_msm_hotplug_enabled(const char *val, struct kernel_param *kp)
{
	param_set_int(val, kp);
	printk(KERN_INFO "msm_hotplug enabled: %d\n",enabled);
	if (enabled) {
		if (boost) {
			if (cpu_online(1) == 0) {
				printk(KERN_WARNING
					"msm_hotplug: Onlining CPU1, boost enabled\n");
				queue_work_on(0,msm_hotplug_wq,&cpu_up_work);
			}
			return 0;
		}
		queue_delayed_work_on(0, msm_hotplug_wq, &msm_hotplug_work,
			SAMPLING_RATE);
	} else {
		cancel_rearming_delayed_work(&msm_hotplug_work);
		if (cpu_online(1) == 1) {
			printk(KERN_WARNING
				"msm_hotplug: Offlining CPU1, module disabled\n");
			queue_work_on(0,msm_hotplug_wq,&cpu_down_work);
		}
	}
	return 0;
}

static int set_msm_hotplug_boost(const char *val, struct kernel_param *kp)
{
	param_set_int(val, kp);
	printk(KERN_INFO "msm_hotplug boost: %d\n",boost);
	if (enabled) {
		if (boost) {
			cancel_rearming_delayed_work(&msm_hotplug_work);
			if (cpu_online(1) == 0) {
				printk(KERN_WARNING
					"msm_hotplug: Onlining CPU1, boost enabled\n");
				queue_work_on(0,msm_hotplug_wq,&cpu_up_work);
			}
		} else {
			printk(KERN_INFO "msm_hotplug: Setting CPU1 back to auto\n");
			queue_delayed_work_on(0, msm_hotplug_wq, &msm_hotplug_work,
			SAMPLING_RATE);
		}
	}
	return 0;
}

module_param_call(enabled, set_msm_hotplug_enabled, param_get_int, &enabled, 00644);
module_param_call(boost, set_msm_hotplug_boost, param_get_int, &boost, 00644);

static void do_cpu_up(struct work_struct *work)
{
	cpu_up(1);
}

static void do_cpu_down(struct work_struct *work)
{
	cpu_down(1);
}

static void msm_hotplug_early_suspend(struct early_suspend *handler)
{
	if (suspended)
		return;
	suspended = 1;
	printk(KERN_DEBUG "msm_hotplug: early suspend handler\n");
	if (enabled) {
		if (boost) {
			printk(KERN_WARNING
				"msm_hotplug: Not offlining CPU1 due to boost\n");
			return;
		}
		cancel_rearming_delayed_work(&msm_hotplug_work);
		if (num_online_cpus() == 2) {
			printk(KERN_INFO
				"msm_hotplug: Offlining CPU1 for early suspend\n");
			queue_work_on(0,msm_hotplug_wq,&cpu_down_work);
		}
	}
}

static void msm_hotplug_late_resume(struct early_suspend *handler)
{
	if (!suspended)
		return;
	suspended = 0;
	printk(KERN_DEBUG "msm_hotplug: late resume handler\n");
	if (enabled) {
		if (boost) {
			if (cpu_online(1) == 0) {
				printk(KERN_WARNING
					"msm_hotplug: Restoring boost after resume\n");
				queue_work_on(0,msm_hotplug_wq,&cpu_up_work);
			}
		} else
			queue_delayed_work_on(0, msm_hotplug_wq, &msm_hotplug_work,
				SAMPLING_RATE);
	}
}

static struct early_suspend msm_hotplug_suspend = {
	.suspend = msm_hotplug_early_suspend,
	.resume = msm_hotplug_late_resume,
};

static int __init msm_hotplug_init(void)
{
	printk(KERN_INFO "msm_hotplug v0.172 by _thalamus init()");
	msm_hotplug_wq = create_singlethread_workqueue("msm_hotplug");
	BUG_ON(!msm_hotplug_wq);
	INIT_DELAYED_WORK_DEFERRABLE(&msm_hotplug_work, msm_hotplug_work_fn);
	INIT_WORK(&cpu_up_work, do_cpu_up);
	INIT_WORK(&cpu_down_work, do_cpu_down);
	register_early_suspend(&msm_hotplug_suspend);
	if (enabled)
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
			queue_delayed_work_on(0, msm_hotplug_wq,
				&msm_hotplug_work, 60 * HZ);
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
				"msm_hotplug: Booted into charge mode, disabling CPU1\n");
			queue_work_on(0,msm_hotplug_wq,&cpu_down_work);
			break;
		}
	return 0;
}
late_initcall(msm_hotplug_init);
