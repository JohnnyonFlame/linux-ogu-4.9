/*
 * drivers/amlogic/media/vin/tvin/tvafe/tvafe_avin_detect.c
 *
 * Copyright (C) 2017 Amlogic, Inc. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 */

/* Standard Linux headers */
#include <linux/types.h>
#include <linux/errno.h>
#include <linux/init.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/interrupt.h>
#include <linux/fs.h>
#include <linux/device.h>
#include <linux/of_device.h>
#include <linux/cdev.h>
#include <linux/platform_device.h>
#include <linux/mm.h>
#include <linux/kthread.h>
#include <linux/vmalloc.h>
#include <linux/irq.h>
#include <linux/poll.h>
#include <linux/of_irq.h>
#include <linux/of.h>
#include <linux/uaccess.h>
#include <linux/delay.h>
#include "tvafe_avin_detect.h"
#include "tvafe.h"
#include "tvafe_regs.h"
#include "tvafe_debug.h"
#include "../tvin_global.h"


#define TVAFE_AVIN_CH1_MASK  (1 << 0)
#define TVAFE_AVIN_CH2_MASK  (1 << 1)
#define TVAFE_AVIN_MASK  (TVAFE_AVIN_CH1_MASK | TVAFE_AVIN_CH2_MASK)
#define TVAFE_MAX_AVIN_DEVICE_NUM  2
#define TVAFE_AVIN_NAME  "avin_detect"
#define TVAFE_AVIN_NAME_CH1  "tvafe_avin_detect_ch1"
#define TVAFE_AVIN_NAME_CH2  "tvafe_avin_detect_ch2"

static unsigned int avin_detect_debug_print;

#define AVIN_DETECT_INIT          (1 << 0)
#define AVIN_DETECT_OPEN          (1 << 1)
#define AVIN_DETECT_CH1_MASK      (1 << 2)
#define AVIN_DETECT_CH2_MASK      (1 << 3)
#define AVIN_DETECT_CH1_EN        (1 << 4)   /* 0x2e[0] */
#define AVIN_DETECT_CH1_SYNC_TIP  (1 << 5)   /* 0x2e[3] */
#define AVIN_DETECT_CH2_EN        (1 << 6)   /* 0x2e[15] */
#define AVIN_DETECT_CH2_SYNC_TIP  (1 << 7)   /* 0x2e[18] */
static unsigned int avin_detect_flag;

/*0:670mv; 1:727mv; 2:777mv; 3:823mv; 4:865mv; 5:904mv; 6:940mv; 7:972mv*/
static unsigned int dc_level_adj = 4;

/*0:635mv; 1:686mv; 2:733mv; 3:776mv; 4:816mv; 5:853mv; 6:887mv; 7:919mv*/
static unsigned int comp_level_adj;

/*0:use internal VDC to bias CVBS_in*/
/*1:use ground to bias CVBS_in*/
static unsigned int detect_mode;

/*0:460mv; 1:0.225mv*/
static unsigned int vdc_level = 1;

/*0:50mv; 1:100mv; 2:150mv; 3:200mv; 4:250mv; 6:300mv; 7:310mv*/
static unsigned int sync_level = 1;

static unsigned int avplay_sync_level = 6;

/*0:50mv; 1:100mv; 2:150mv; 3:200mv*/
static unsigned int sync_hys_adj = 1;

static unsigned int irq_mode = 5;

static unsigned int trigger_sel = 1;

static unsigned int irq_edge_en = 1;

static unsigned int irq_filter;

static unsigned int irq_pol;

static unsigned int avin_count_times = 5;

static unsigned int avin_timer_time = 10;/*100ms*/

#define TVAFE_AVIN_INTERVAL (HZ/100)/*10ms*/
static struct timer_list avin_detect_timer;
static unsigned int s_irq_counter0;
static unsigned int s_irq_counter1;
static unsigned int s_irq_counter0_time;
static unsigned int s_irq_counter1_time;
static unsigned int s_counter0_last_state;
static unsigned int s_counter1_last_state;

static struct meson_avin_data *meson_data;
static DECLARE_WAIT_QUEUE_HEAD(tvafe_avin_waitq);

static int tvafe_avin_dts_parse(struct platform_device *pdev)
{
	int ret;
	int i;
	int value;
	struct resource *res;
	struct tvafe_avin_det_s *avdev;

	avdev = platform_get_drvdata(pdev);

	ret = of_property_read_u32(pdev->dev.of_node,
		"device_mask", &value);
	if (ret) {
		tvafe_pr_info("Failed to get device_mask.\n");
		goto get_avin_param_failed;
	} else {
		avdev->dts_param.device_mask = value;
		tvafe_pr_info("avin device_mask is 0x%x\n",
			avdev->dts_param.device_mask);
		if (avdev->dts_param.device_mask == TVAFE_AVIN_MASK) {
			avin_detect_flag |=
				(AVIN_DETECT_CH1_MASK | AVIN_DETECT_CH2_MASK);
			avdev->device_num = 2;
			ret = of_property_read_u32(pdev->dev.of_node,
				"device_sequence", &value);
			if (ret) {
				avdev->dts_param.device_sequence = 0;
			} else {
				tvafe_pr_info("find device_sequence: %d\n",
					value);
				avdev->dts_param.device_sequence = value;
			}
			if (avdev->dts_param.device_sequence == 0) {
				avdev->report_data_s[0].channel =
						TVAFE_AVIN_CHANNEL1;
				avdev->report_data_s[1].channel =
						TVAFE_AVIN_CHANNEL2;
			} else {
				avdev->report_data_s[0].channel =
						TVAFE_AVIN_CHANNEL2;
				avdev->report_data_s[1].channel =
						TVAFE_AVIN_CHANNEL1;
			}
			tvafe_pr_info("avin ch1:%d, ch2:%d\n",
				avdev->report_data_s[0].channel,
				avdev->report_data_s[1].channel);
		} else if (avdev->dts_param.device_mask ==
			TVAFE_AVIN_CH1_MASK) {
			avin_detect_flag |= AVIN_DETECT_CH1_MASK;
			avdev->device_num = 1;
			avdev->report_data_s[0].channel = TVAFE_AVIN_CHANNEL1;
			avdev->report_data_s[1].channel =
				TVAFE_AVIN_CHANNEL_MAX;
		} else if (avdev->dts_param.device_mask ==
			TVAFE_AVIN_CH2_MASK) {
			avin_detect_flag |= AVIN_DETECT_CH2_MASK;
			avdev->device_num = 1;
			avdev->report_data_s[0].channel = TVAFE_AVIN_CHANNEL2;
			avdev->report_data_s[1].channel =
				TVAFE_AVIN_CHANNEL_MAX;
		} else {
			avdev->device_num = 0;
			avdev->report_data_s[0].channel =
				TVAFE_AVIN_CHANNEL_MAX;
			avdev->report_data_s[1].channel =
				TVAFE_AVIN_CHANNEL_MAX;
			tvafe_pr_info("device_mask 0x%x invalid\n",
				avdev->dts_param.device_mask);
			goto get_avin_param_failed;
		}
		avdev->report_data_s[0].status = TVAFE_AVIN_STATUS_UNKNOWN;
		avdev->report_data_s[1].status = TVAFE_AVIN_STATUS_UNKNOWN;
	}
	/* get irq no*/
	for (i = 0; i < avdev->device_num; i++) {
		res = platform_get_resource(pdev, IORESOURCE_IRQ, i);
		if (!res) {
			tvafe_pr_err("%s: can't get avin(%d) irq resource\n",
				__func__, i);
			goto fail_get_resource_irq;
		}
		avdev->dts_param.irq[i] = res->start;
	}
	return 0;
get_avin_param_failed:
fail_get_resource_irq:
	return -EINVAL;
}

void tvafe_avin_detect_ch1_anlog_enable(bool enable)
{
	if ((avin_detect_flag & AVIN_DETECT_OPEN) == 0) {
		if (avin_detect_debug_print) {
			tvafe_pr_info("%s: avin_detect is not opened\n",
				__func__);
		}
		return;
	}
	if ((avin_detect_flag & AVIN_DETECT_CH1_MASK) == 0) {
		if (avin_detect_debug_print) {
			tvafe_pr_info("%s: avin_detect ch1 is inactive\n",
				__func__);
		}
		return;
	}
	if (avin_detect_debug_print)
		tvafe_pr_info("%s: %d\n", __func__, enable);

	/*txlx,txhd,tl1 the same bit:0 for ch1 en detect*/
	if (enable) {
		W_HIU_BIT(HHI_CVBS_DETECT_CNTL, 1,
			AFE_CH1_EN_DETECT_BIT, AFE_CH1_EN_DETECT_WIDTH);
		avin_detect_flag |= AVIN_DETECT_CH1_EN;
	} else {
		W_HIU_BIT(HHI_CVBS_DETECT_CNTL, 0,
			AFE_CH1_EN_DETECT_BIT, AFE_CH1_EN_DETECT_WIDTH);
		avin_detect_flag &= ~AVIN_DETECT_CH1_EN;
	}
}

void tvafe_avin_detect_ch2_anlog_enable(bool enable)
{
	if ((avin_detect_flag & AVIN_DETECT_OPEN) == 0) {
		if (avin_detect_debug_print) {
			tvafe_pr_info("%s: avin_detect is not opened\n",
				__func__);
		}
		return;
	}
	if ((avin_detect_flag & AVIN_DETECT_CH2_MASK) == 0) {
		if (avin_detect_debug_print) {
			tvafe_pr_info("%s: avin_detect ch2 is inactive\n",
				__func__);
		}
		return;
	}
	if (avin_detect_debug_print)
		tvafe_pr_info("%s: %d\n", __func__, enable);

	if (enable) {
		if (meson_data)
			W_HIU_BIT(HHI_CVBS_DETECT_CNTL, 1,
			AFE_TL_CH2_EN_DETECT_BIT, AFE_TL_CH2_EN_DETECT_WIDTH);
		else
			W_HIU_BIT(HHI_CVBS_DETECT_CNTL, 1,
			AFE_CH2_EN_DETECT_BIT, AFE_CH2_EN_DETECT_WIDTH);
		avin_detect_flag |= AVIN_DETECT_CH2_EN;
	} else {
		if (meson_data)
			W_HIU_BIT(HHI_CVBS_DETECT_CNTL, 0,
			AFE_TL_CH2_EN_DETECT_BIT, AFE_TL_CH2_EN_DETECT_WIDTH);
		else
			W_HIU_BIT(HHI_CVBS_DETECT_CNTL, 0,
			AFE_CH2_EN_DETECT_BIT, AFE_CH2_EN_DETECT_WIDTH);
		avin_detect_flag &= ~AVIN_DETECT_CH2_EN;
	}
}

/*after adc and afe is enable,this TIP bit must be set to "0"*/
void tvafe_cha1_SYNCTIP_close_config(void)
{
	if ((avin_detect_flag & AVIN_DETECT_OPEN) == 0) {
		if (avin_detect_debug_print) {
			tvafe_pr_info("%s: avin_detect is not opened\n",
				__func__);
		}
		return;
	}
	if ((avin_detect_flag & AVIN_DETECT_CH1_MASK) == 0) {
		if (avin_detect_debug_print) {
			tvafe_pr_info("%s: avin_detect ch1 is inactive\n",
				__func__);
		}
		return;
	}
	if (avin_detect_debug_print)
		tvafe_pr_info("%s\n", __func__);

	if (meson_data) {
		W_HIU_BIT(HHI_CVBS_DETECT_CNTL, 0, AFE_CH1_EN_DC_BIAS_BIT,
			AFE_CH1_EN_DC_BIAS_WIDTH);
	} else {
		W_HIU_BIT(HHI_CVBS_DETECT_CNTL, 0, AFE_CH1_EN_SYNC_TIP_BIT,
			AFE_CH1_EN_SYNC_TIP_WIDTH);
		W_HIU_BIT(HHI_CVBS_DETECT_CNTL, avplay_sync_level,
		AFE_CH1_SYNC_LEVEL_ADJ_BIT, AFE_CH1_SYNC_LEVEL_ADJ_WIDTH);
		W_HIU_BIT(HHI_CVBS_DETECT_CNTL, 1,
			AFE_CH1_SYNC_HYS_ADJ_BIT, AFE_CH1_SYNC_HYS_ADJ_WIDTH);
	}
	avin_detect_flag &= ~AVIN_DETECT_CH1_SYNC_TIP;
}

void tvafe_cha2_SYNCTIP_close_config(void)
{
	if ((avin_detect_flag & AVIN_DETECT_OPEN) == 0) {
		if (avin_detect_debug_print) {
			tvafe_pr_info("%s: avin_detect is not opened\n",
				__func__);
		}
		return;
	}
	if ((avin_detect_flag & AVIN_DETECT_CH2_MASK) == 0) {
		if (avin_detect_debug_print) {
			tvafe_pr_info("%s: avin_detect ch2 is inactive\n",
				__func__);
		}
		return;
	}
	if (avin_detect_debug_print)
		tvafe_pr_info("%s\n", __func__);

	if (meson_data) {
		if (meson_data->cpu_id >= AVIN_CPU_TYPE_T5)
			W_HIU_BIT(HHI_CVBS_DETECT_CNTL, 0,
				  AFE_T5_CH2_EN_DC_BIAS_BIT,
				  AFE_T5_CH2_EN_DC_BIAS_WIDTH);
		else
			W_HIU_BIT(HHI_CVBS_DETECT_CNTL, 0,
				  AFE_CH2_EN_DC_BIAS_BIT,
				  AFE_CH2_EN_DC_BIAS_WIDTH);
	} else {
		W_HIU_BIT(HHI_CVBS_DETECT_CNTL, 0, AFE_CH2_EN_SYNC_TIP_BIT,
			AFE_CH2_EN_SYNC_TIP_WIDTH);
		W_HIU_BIT(HHI_CVBS_DETECT_CNTL, avplay_sync_level,
		AFE_CH2_SYNC_LEVEL_ADJ_BIT, AFE_CH2_SYNC_LEVEL_ADJ_WIDTH);
		W_HIU_BIT(HHI_CVBS_DETECT_CNTL, 0,
			AFE_CH2_SYNC_HYS_ADJ_BIT, AFE_CH2_SYNC_HYS_ADJ_WIDTH);
	}
	avin_detect_flag &= ~AVIN_DETECT_CH2_SYNC_TIP;
}

/*After the CVBS is unplug,the EN_SYNC_TIP need be set to "1"*/
/*to sense the plug in operation*/
void tvafe_cha1_detect_restart_config(void)
{
	if ((avin_detect_flag & AVIN_DETECT_OPEN) == 0) {
		if (avin_detect_debug_print) {
			tvafe_pr_info("%s: avin_detect is not opened\n",
				__func__);
		}
		return;
	}
	if ((avin_detect_flag & AVIN_DETECT_CH1_MASK) == 0) {
		if (avin_detect_debug_print) {
			tvafe_pr_info("%s: avin_detect ch1 is inactive\n",
				__func__);
		}
		return;
	}
	if (avin_detect_debug_print)
		tvafe_pr_info("%s\n", __func__);

	if (meson_data) {
		W_HIU_BIT(HHI_CVBS_DETECT_CNTL, 1, AFE_CH1_EN_DC_BIAS_BIT,
			AFE_CH1_EN_DC_BIAS_WIDTH);
	} else {
		W_HIU_BIT(HHI_CVBS_DETECT_CNTL, 1, AFE_CH1_EN_SYNC_TIP_BIT,
			AFE_CH1_EN_SYNC_TIP_WIDTH);
		W_HIU_BIT(HHI_CVBS_DETECT_CNTL, sync_level,
		AFE_CH1_SYNC_LEVEL_ADJ_BIT, AFE_CH1_SYNC_LEVEL_ADJ_WIDTH);
		W_HIU_BIT(HHI_CVBS_DETECT_CNTL, sync_hys_adj,
			AFE_CH1_SYNC_HYS_ADJ_BIT, AFE_CH1_SYNC_HYS_ADJ_WIDTH);
	}
	avin_detect_flag |= AVIN_DETECT_CH1_SYNC_TIP;
}

void tvafe_cha2_detect_restart_config(void)
{
	if ((avin_detect_flag & AVIN_DETECT_OPEN) == 0) {
		if (avin_detect_debug_print) {
			tvafe_pr_info("%s: avin_detect is not opened\n",
				__func__);
		}
		return;
	}
	if ((avin_detect_flag & AVIN_DETECT_CH2_MASK) == 0) {
		if (avin_detect_debug_print) {
			tvafe_pr_info("%s: avin_detect ch2 is inactive\n",
				__func__);
		}
		return;
	}
	if (avin_detect_debug_print)
		tvafe_pr_info("%s\n", __func__);

	if (meson_data) {
		if (meson_data->cpu_id >= AVIN_CPU_TYPE_T5)
			W_HIU_BIT(HHI_CVBS_DETECT_CNTL, 1,
				  AFE_T5_CH2_EN_DC_BIAS_BIT,
				  AFE_T5_CH2_EN_DC_BIAS_WIDTH);
		else
			W_HIU_BIT(HHI_CVBS_DETECT_CNTL, 1,
				  AFE_CH2_EN_DC_BIAS_BIT,
				  AFE_CH2_EN_DC_BIAS_WIDTH);
	} else {
		W_HIU_BIT(HHI_CVBS_DETECT_CNTL, 1, AFE_CH2_EN_SYNC_TIP_BIT,
			AFE_CH2_EN_SYNC_TIP_WIDTH);
		W_HIU_BIT(HHI_CVBS_DETECT_CNTL, sync_level,
		AFE_CH2_SYNC_LEVEL_ADJ_BIT, AFE_CH2_SYNC_LEVEL_ADJ_WIDTH);
		W_HIU_BIT(HHI_CVBS_DETECT_CNTL, sync_hys_adj,
			AFE_CH2_SYNC_HYS_ADJ_BIT, AFE_CH2_SYNC_HYS_ADJ_WIDTH);
	}
	avin_detect_flag |= AVIN_DETECT_CH2_SYNC_TIP;
}

static void tvafe_avin_detect_enable(struct tvafe_avin_det_s *avin_data)
{
	if ((avin_detect_flag & AVIN_DETECT_OPEN) == 0) {
		if (avin_detect_debug_print) {
			tvafe_pr_info("%s: avin_detect is not opened\n",
				__func__);
		}
		return;
	}
	/*enable anlog config*/
	tvafe_pr_info("%s\n", __func__);
	if (avin_data->dts_param.device_mask & TVAFE_AVIN_CH1_MASK)
		tvafe_avin_detect_ch1_anlog_enable(1);
	if (avin_data->dts_param.device_mask & TVAFE_AVIN_CH2_MASK)
		tvafe_avin_detect_ch2_anlog_enable(1);
}

static void tvafe_avin_detect_disable(struct tvafe_avin_det_s *avin_data)
{
	if ((avin_detect_flag & AVIN_DETECT_OPEN) == 0) {
		if (avin_detect_debug_print) {
			tvafe_pr_info("%s: avin_detect is not opened\n",
				__func__);
		}
		return;
	}
	/*disable anlog config*/
	tvafe_pr_info("%s\n", __func__);
	if (avin_data->dts_param.device_mask & TVAFE_AVIN_CH1_MASK)
		tvafe_avin_detect_ch1_anlog_enable(0);
	if (avin_data->dts_param.device_mask & TVAFE_AVIN_CH2_MASK)
		tvafe_avin_detect_ch2_anlog_enable(0);
}

static void tvafe_avin_detect_anlog_config(void)
{
	if (meson_data) {
		/*ch1 config*/
		W_HIU_BIT(HHI_CVBS_DETECT_CNTL, dc_level_adj,
			AFE_CH1_DC_LEVEL_ADJ_BIT, AFE_CH1_DC_LEVEL_ADJ_WIDTH);
		W_HIU_BIT(HHI_CVBS_DETECT_CNTL, comp_level_adj,
		AFE_CH1_COMP_LEVEL_ADJ_BIT, AFE_CH1_COMP_LEVEL_ADJ_WIDTH);
		W_HIU_BIT(HHI_CVBS_DETECT_CNTL, 0,
			AFE_CH1_COMP_HYS_ADJ_BIT, AFE_CH1_COMP_HYS_ADJ_WIDTH);
		W_HIU_BIT(HHI_CVBS_DETECT_CNTL, 1, AFE_CH1_EN_DC_BIAS_BIT,
			AFE_CH1_EN_DC_BIAS_WIDTH);

		/*ch2 config*/
		W_HIU_BIT(HHI_CVBS_DETECT_CNTL, dc_level_adj,
			AFE_CH2_DC_LEVEL_ADJ_BIT, AFE_CH2_DC_LEVEL_ADJ_WIDTH);
		W_HIU_BIT(HHI_CVBS_DETECT_CNTL, comp_level_adj,
		AFE_CH2_COMP_LEVEL_ADJ_BIT, AFE_CH2_COMP_LEVEL_ADJ_WIDTH);
		W_HIU_BIT(HHI_CVBS_DETECT_CNTL, 0,
			AFE_CH2_COMP_HYS_ADJ_BIT, AFE_CH2_COMP_HYS_ADJ_WIDTH);
		if (meson_data->cpu_id >= AVIN_CPU_TYPE_T5)
			W_HIU_BIT(HHI_CVBS_DETECT_CNTL, 1,
				  AFE_T5_CH2_EN_DC_BIAS_BIT,
				  AFE_T5_CH2_EN_DC_BIAS_WIDTH);
		else
			W_HIU_BIT(HHI_CVBS_DETECT_CNTL, 1,
				  AFE_CH2_EN_DC_BIAS_BIT,
				  AFE_CH2_EN_DC_BIAS_WIDTH);
	} else {
		if (detect_mode == 0) {
			/*for ch1*/
			W_HIU_BIT(HHI_CVBS_DETECT_CNTL, 0,
				AFE_DETECT_RSV1_BIT, AFE_DETECT_RSV1_WIDTH);
			/*for ch2*/
			W_HIU_BIT(HHI_CVBS_DETECT_CNTL, 0,
				AFE_DETECT_RSV3_BIT, AFE_DETECT_RSV3_WIDTH);
		} else {
			W_HIU_BIT(HHI_CVBS_DETECT_CNTL, 1,
				AFE_DETECT_RSV1_BIT, AFE_DETECT_RSV1_WIDTH);
			W_HIU_BIT(HHI_CVBS_DETECT_CNTL, 1,
				AFE_DETECT_RSV3_BIT, AFE_DETECT_RSV3_WIDTH);
		}

		/*ch1 config*/
		W_HIU_BIT(HHI_CVBS_DETECT_CNTL, sync_level,
		AFE_CH1_SYNC_LEVEL_ADJ_BIT, AFE_CH1_SYNC_LEVEL_ADJ_WIDTH);
		W_HIU_BIT(HHI_CVBS_DETECT_CNTL, sync_hys_adj,
			AFE_CH1_SYNC_HYS_ADJ_BIT, AFE_CH1_SYNC_HYS_ADJ_WIDTH);
		W_HIU_BIT(HHI_CVBS_DETECT_CNTL, vdc_level,
			AFE_DETECT_RSV0_BIT, AFE_DETECT_RSV0_WIDTH);
		/*after adc and afe is enable,this bit must be set to "0"*/
		W_HIU_BIT(HHI_CVBS_DETECT_CNTL, 1, AFE_CH1_EN_SYNC_TIP_BIT,
			AFE_CH1_EN_SYNC_TIP_WIDTH);

		/***ch2 config***/
		W_HIU_BIT(HHI_CVBS_DETECT_CNTL, sync_level,
		AFE_CH2_SYNC_LEVEL_ADJ_BIT, AFE_CH2_SYNC_LEVEL_ADJ_WIDTH);
		W_HIU_BIT(HHI_CVBS_DETECT_CNTL, sync_hys_adj,
			AFE_CH2_SYNC_HYS_ADJ_BIT, AFE_CH2_SYNC_HYS_ADJ_WIDTH);
		W_HIU_BIT(HHI_CVBS_DETECT_CNTL, vdc_level,
			AFE_DETECT_RSV2_BIT, AFE_DETECT_RSV2_WIDTH);
		W_HIU_BIT(HHI_CVBS_DETECT_CNTL, 1, AFE_CH2_EN_SYNC_TIP_BIT,
			AFE_CH2_EN_SYNC_TIP_WIDTH);
	}
	avin_detect_flag |= AVIN_DETECT_CH1_SYNC_TIP;
	avin_detect_flag |= AVIN_DETECT_CH2_SYNC_TIP;
}

static void tvafe_avin_detect_digital_config(void)
{
	aml_cbus_update_bits(CVBS_IRQ0_CNTL,
		CVBS_IRQ_MODE_MASK << CVBS_IRQ_MODE_BIT,
		irq_mode << CVBS_IRQ_MODE_BIT);
	aml_cbus_update_bits(CVBS_IRQ0_CNTL,
		CVBS_IRQ_TRIGGER_SEL_MASK << CVBS_IRQ_TRIGGER_SEL_BIT,
		trigger_sel << CVBS_IRQ_TRIGGER_SEL_BIT);
	aml_cbus_update_bits(CVBS_IRQ0_CNTL,
		CVBS_IRQ_EDGE_EN_MASK << CVBS_IRQ_EDGE_EN_BIT,
		irq_edge_en << CVBS_IRQ_EDGE_EN_BIT);
	aml_cbus_update_bits(CVBS_IRQ0_CNTL,
		CVBS_IRQ_FILTER_MASK << CVBS_IRQ_FILTER_BIT,
		irq_filter << CVBS_IRQ_FILTER_BIT);
	aml_cbus_update_bits(CVBS_IRQ0_CNTL,
		CVBS_IRQ_POL_MASK << CVBS_IRQ_POL_BIT,
		irq_pol << CVBS_IRQ_POL_BIT);
}

static int tvafe_avin_open(struct inode *inode, struct file *file)
{
	struct tvafe_avin_det_s *avin_data;

	tvafe_pr_info("tvafe_avin_open.\n");
	avin_data = container_of(inode->i_cdev,
		struct tvafe_avin_det_s, avin_cdev);
	file->private_data = avin_data;
	/*enable irq */
	avin_detect_flag |= AVIN_DETECT_OPEN;
	tvafe_avin_detect_enable(avin_data);
	return 0;
}

static ssize_t tvafe_avin_read(struct file *file, char __user *buf,
	size_t count, loff_t *ppos)
{
	unsigned long ret;
	struct tvafe_avin_det_s *avin_data =
		(struct tvafe_avin_det_s *)file->private_data;

	ret = copy_to_user(buf,
		(void *)(&avin_data->report_data_s[0]),
		sizeof(struct tvafe_report_data_s)
		* avin_data->device_num);

	return 0;
}

static int tvafe_avin_release(struct inode *inode, struct file *file)
{
	struct tvafe_avin_det_s *avin_data =
		(struct tvafe_avin_det_s *)file->private_data;

	tvafe_avin_detect_disable(avin_data);
	avin_detect_flag &= ~AVIN_DETECT_OPEN;
	file->private_data = NULL;
	return 0;
}

static unsigned int tvafe_avin_poll(struct file *file, poll_table *wait)
{
	unsigned int mask = 0;

	poll_wait(file, &tvafe_avin_waitq, wait);
	mask |= POLLIN | POLLRDNORM;

	return mask;
}

static const struct file_operations tvafe_avin_fops = {
	.owner      = THIS_MODULE,
	.open       = tvafe_avin_open,
	.read       = tvafe_avin_read,
	.poll       = tvafe_avin_poll,
	.release    = tvafe_avin_release,
};

static int tvafe_register_avin_dev(struct tvafe_avin_det_s *avin_data)
{
	int ret = 0;

	ret = alloc_chrdev_region(&avin_data->avin_devno,
		0, 1, TVAFE_AVIN_NAME);
	if (ret < 0) {
		tvafe_pr_err("failed to allocate major number\n");
		return -ENODEV;
	}

	/* connect the file operations with cdev */
	cdev_init(&avin_data->avin_cdev, &tvafe_avin_fops);
	avin_data->avin_cdev.owner = THIS_MODULE;
	/* connect the major/minor number to the cdev */
	ret = cdev_add(&avin_data->avin_cdev, avin_data->avin_devno, 1);
	if (ret) {
		tvafe_pr_err("failed to add device\n");
		unregister_chrdev_region(avin_data->avin_devno, 1);
		return -ENODEV;
	}

	strcpy(avin_data->config_name, TVAFE_AVIN_NAME);
	avin_data->config_class = class_create(THIS_MODULE,
		avin_data->config_name);
	avin_data->config_dev = device_create(avin_data->config_class, NULL,
		avin_data->avin_devno, NULL, avin_data->config_name);
	if (IS_ERR(avin_data->config_dev)) {
		tvafe_pr_err("failed to create device node\n");
		cdev_del(&avin_data->avin_cdev);
		unregister_chrdev_region(avin_data->avin_devno, 1);
		ret = PTR_ERR(avin_data->config_dev);
		return ret;
	}

	return ret;
}

static void tvafe_avin_detect_state(struct tvafe_avin_det_s *avdev)
{
	tvafe_pr_info("device_num: %d\n", avdev->device_num);
	tvafe_pr_info("\t*****dts param*****\n");
	tvafe_pr_info("device_mask: %d\n", avdev->dts_param.device_mask);
	tvafe_pr_info("irq0: %d\n", avdev->dts_param.irq[0]);
	tvafe_pr_info("irq1: %d\n", avdev->dts_param.irq[1]);
	tvafe_pr_info("irq_counter[0]: 0x%x\n", avdev->irq_counter[0]);
	tvafe_pr_info("irq_counter[1]: 0x%x\n", avdev->irq_counter[1]);
	tvafe_pr_info("\t*****channel status:0->in;1->out*****\n");
	tvafe_pr_info("channel[%d] status: %d\n",
		avdev->report_data_s[0].channel,
		avdev->report_data_s[0].status);
	tvafe_pr_info("channel[%d] status: %d\n",
		avdev->report_data_s[1].channel,
		avdev->report_data_s[1].status);
	tvafe_pr_info("avin_detect_flag: 0x%02x\n", avin_detect_flag);
	tvafe_pr_info("avin_detect_reg:  0x%02x=0x%08x\n",
		HHI_CVBS_DETECT_CNTL, R_HIU_REG(HHI_CVBS_DETECT_CNTL));
	tvafe_pr_info("\t*****global param*****\n");
	tvafe_pr_info("dc_level_adj: %d\n", dc_level_adj);
	tvafe_pr_info("comp_level_adj: %d\n", comp_level_adj);
	tvafe_pr_info("detect_mode: %d\n", detect_mode);
	tvafe_pr_info("vdc_level: %d\n", vdc_level);
	tvafe_pr_info("sync_level: %d\n", sync_level);
	tvafe_pr_info("sync_hys_adj: %d\n", sync_hys_adj);
	tvafe_pr_info("irq_mode: %d\n", irq_mode);
	tvafe_pr_info("trigger_sel: %d\n", trigger_sel);
	tvafe_pr_info("irq_edge_en: %d\n", irq_edge_en);
	tvafe_pr_info("irq_filter: %d\n", irq_filter);
	tvafe_pr_info("irq_pol: %d\n", irq_pol);
}

static void tvafe_avin_detect_parse_param(char *buf_orig, char **parm)
{
	char *ps, *token;
	char delim1[3] = " ";
	char delim2[2] = "\n";
	unsigned int n = 0;

	ps = buf_orig;
	strcat(delim1, delim2);
	while (1) {
		token = strsep(&ps, delim1);
		if (token == NULL)
			break;
		if (*token == '\0')
			continue;
		parm[n++] = token;
	}
}

static ssize_t tvafe_avin_detect_show(struct device *dev,
	struct device_attribute *attr, char *buf)
{
	ssize_t len = 0;

	len += sprintf(buf+len,
		"\t*****usage:*****\n");
	len += sprintf(buf+len,
		"echo dc_level_adj val(D) > debug\n");
	len += sprintf(buf+len,
		"echo comp_level_adj val(D) > debug\n");
	len += sprintf(buf+len,
		"echo detect_mode val(D) > debug\n");
	len += sprintf(buf+len,
		"echo vdc_level val(D) > debug\n");
	len += sprintf(buf+len,
		"echo sync_level val(D) > debug\n");
	len += sprintf(buf+len,
		"echo sync_hys_adj val(D) > debug\n");
	len += sprintf(buf+len,
		"echo irq_mode val(D) > debug\n");
	len += sprintf(buf+len,
		"echo trigger_sel val(D) > debug\n");
	len += sprintf(buf+len,
		"echo irq_edge_en val(D) > debug\n");
	len += sprintf(buf+len,
		"echo irq_filter val(D) > debug\n");
	len += sprintf(buf+len,
		"echo irq_pol val(D) > debug\n");
	len += sprintf(buf+len,
		"echo ch1_enable val(D) > debug\n");
	len += sprintf(buf+len,
		"echo ch2_enable val(D) > debug\n");
	len += sprintf(buf+len,
		"echo enable > debug\n");
	len += sprintf(buf+len,
		"echo disable > debug\n");
	len += sprintf(buf+len,
		"echo status > debug\n");
	return len;
}


static ssize_t tvafe_avin_detect_store(struct device *dev,
	struct device_attribute *attr, const char *buff, size_t count)
{
	struct tvafe_avin_det_s *avdev;
	unsigned int val;
	char *buf_orig, *parm[10] = {NULL};

	avdev = dev_get_drvdata(dev);
	if (!buff)
		return count;
	buf_orig = kstrdup(buff, GFP_KERNEL);
	tvafe_avin_detect_parse_param(buf_orig, (char **)&parm);

	/*tvafe_pr_info("[%s]:param0:%s.\n", __func__, parm[0]);*/
	if (!strcmp(parm[0], "dc_level_adj")) {
		if (parm[1]) {
			if (kstrtouint(parm[1], 10, &dc_level_adj)) {
				tvafe_pr_info("[%s]:invaild parameter\n",
					__func__);
				goto tvafe_avin_detect_store_err;
			} else {
				W_HIU_BIT(HHI_CVBS_DETECT_CNTL, dc_level_adj,
					  AFE_CH1_DC_LEVEL_ADJ_BIT,
					  AFE_CH1_DC_LEVEL_ADJ_WIDTH);
				W_HIU_BIT(HHI_CVBS_DETECT_CNTL, dc_level_adj,
					  AFE_CH2_DC_LEVEL_ADJ_BIT,
					  AFE_CH2_DC_LEVEL_ADJ_WIDTH);
			}
		}
		tvafe_pr_info("[%s]: dc_level_adj: %d\n",
			__func__, dc_level_adj);
	} else if (!strcmp(parm[0], "comp_level_adj")) {
		if (parm[1]) {
			if (kstrtouint(parm[1], 10, &comp_level_adj)) {
				tvafe_pr_info("[%s]:invaild parameter\n",
					__func__);
				goto tvafe_avin_detect_store_err;
			} else {
				W_HIU_BIT(HHI_CVBS_DETECT_CNTL, comp_level_adj,
					  AFE_CH1_COMP_LEVEL_ADJ_BIT,
					  AFE_CH1_COMP_LEVEL_ADJ_WIDTH);
				W_HIU_BIT(HHI_CVBS_DETECT_CNTL, comp_level_adj,
					  AFE_CH2_COMP_LEVEL_ADJ_BIT,
					  AFE_CH2_COMP_LEVEL_ADJ_WIDTH);
			}
		}
		tvafe_pr_info("[%s]: comp_level_adj: %d\n",
			__func__, comp_level_adj);
	} else if (!strcmp(parm[0], "detect_mode")) {
		if (parm[1]) {
			if (kstrtouint(parm[1], 10, &detect_mode)) {
				tvafe_pr_info("[%s]:invaild parameter\n",
					__func__);
				goto tvafe_avin_detect_store_err;
			}
		}
		tvafe_pr_info("[%s]: detect_mode: %d\n",
			__func__, detect_mode);
	} else if (!strcmp(parm[0], "vdc_level")) {
		if (parm[1]) {
			if (kstrtouint(parm[1], 10, &vdc_level)) {
				tvafe_pr_info("[%s]:invaild parameter\n",
					__func__);
				goto tvafe_avin_detect_store_err;
			}
		}
		tvafe_pr_info("[%s]: vdc_level: %d\n",
			__func__, vdc_level);
	} else if (!strcmp(parm[0], "sync_level")) {
		if (parm[1]) {
			if (kstrtouint(parm[1], 10, &sync_level)) {
				tvafe_pr_info("[%s]:invaild parameter\n",
					__func__);
				goto tvafe_avin_detect_store_err;
			}
		}
		tvafe_pr_info("[%s]: sync_level: %d\n",
			__func__, sync_level);
	} else if (!strcmp(parm[0], "sync_hys_adj")) {
		if (parm[1]) {
			if (kstrtouint(parm[1], 10, &sync_hys_adj)) {
				tvafe_pr_info("[%s]:invaild parameter\n",
					__func__);
				goto tvafe_avin_detect_store_err;
			}
		}
		tvafe_pr_info("[%s]: sync_hys_adj: %d\n",
			__func__, sync_hys_adj);
	} else if (!strcmp(parm[0], "irq_mode")) {
		if (parm[1]) {
			if (kstrtouint(parm[1], 10, &irq_mode)) {
				tvafe_pr_info("[%s]:invaild parameter\n",
					__func__);
				goto tvafe_avin_detect_store_err;
			}
		}
		tvafe_pr_info("[%s]: irq_mode: %d\n",
			__func__, irq_mode);
	} else if (!strcmp(parm[0], "trigger_sel")) {
		if (parm[1]) {
			if (kstrtouint(parm[1], 10, &trigger_sel)) {
				tvafe_pr_info("[%s]:invaild parameter\n",
					__func__);
				goto tvafe_avin_detect_store_err;
			}
		}
		tvafe_pr_info("[%s]: trigger_sel: %d\n",
			__func__, trigger_sel);
	} else if (!strcmp(parm[0], "irq_edge_en")) {
		if (parm[1]) {
			if (kstrtouint(parm[1], 10, &irq_edge_en)) {
				tvafe_pr_info("[%s]:invaild parameter\n",
					__func__);
				goto tvafe_avin_detect_store_err;
			}
		}
		tvafe_pr_info("[%s]: irq_edge_en: %d\n",
			__func__, irq_edge_en);
	} else if (!strcmp(parm[0], "irq_filter")) {
		if (parm[1]) {
			if (kstrtouint(parm[1], 10, &irq_filter)) {
				tvafe_pr_info("[%s]:invaild parameter\n",
					__func__);
				goto tvafe_avin_detect_store_err;
			}
		}
		tvafe_pr_info("[%s]: irq_filter: %d\n",
			__func__, irq_filter);
	} else if (!strcmp(parm[0], "irq_pol")) {
		if (parm[1]) {
			if (kstrtouint(parm[1], 10, &irq_pol)) {
				tvafe_pr_info("[%s]:invaild parameter\n",
					__func__);
				goto tvafe_avin_detect_store_err;
			}
		}
		tvafe_pr_info("[%s]: irq_pol: %d\n",
			__func__, irq_pol);
	} else if (!strcmp(parm[0], "ch1_enable")) {
		if (parm[1]) {
			if (kstrtouint(parm[1], 10, &val)) {
				tvafe_pr_info("[%s]:invaild parameter\n",
					__func__);
				goto tvafe_avin_detect_store_err;
			}
			tvafe_pr_info("[%s]: ch1_enable: %d\n",
				__func__, val);
			if (val)
				tvafe_avin_detect_ch1_anlog_enable(1);
			else
				tvafe_avin_detect_ch1_anlog_enable(0);
		}
	} else if (!strcmp(parm[0], "ch2_enable")) {
		if (parm[1]) {
			if (kstrtouint(parm[1], 10, &val)) {
				tvafe_pr_info("[%s]:invaild parameter\n",
					__func__);
				goto tvafe_avin_detect_store_err;
			}
			tvafe_pr_info("[%s]: ch2_enable: %d\n",
				__func__, val);
			if (val)
				tvafe_avin_detect_ch2_anlog_enable(1);
			else
				tvafe_avin_detect_ch2_anlog_enable(0);
		}
	} else if (!strcmp(parm[0], "enable")) {
		avin_detect_flag |= AVIN_DETECT_OPEN;
		tvafe_avin_detect_enable(avdev);
	} else if (!strcmp(parm[0], "disable")) {
		tvafe_avin_detect_disable(avdev);
		avin_detect_flag &= ~AVIN_DETECT_OPEN;
		av1_plugin_state = 0;
		av2_plugin_state = 0;
	} else if (!strcmp(parm[0], "status")) {
		tvafe_avin_detect_state(avdev);
	} else if (!strcmp(parm[0], "print")) {
		if (parm[1]) {
			if (kstrtouint(parm[1], 10, &avin_detect_debug_print)) {
				tvafe_pr_info("[%s]:invaild parameter\n",
					__func__);
				goto tvafe_avin_detect_store_err;
			}
		}
		tvafe_pr_info("[%s]: avin_detect_debug_print: %d\n",
			__func__, avin_detect_debug_print);
	} else
		tvafe_pr_info("[%s]:invaild command.\n", __func__);

	kfree(buf_orig);
	return count;

tvafe_avin_detect_store_err:
	kfree(buf_orig);
	return -EINVAL;
}

static void tvafe_avin_detect_timer_handler(unsigned long arg)
{
	unsigned int state_changed = 0;
	struct tvafe_avin_det_s *avdev = (struct tvafe_avin_det_s *)arg;

	if (avdev->dts_param.device_mask == TVAFE_AVIN_CH1_MASK) {
		avdev->irq_counter[0] = aml_read_cbus(CVBS_IRQ0_COUNTER);
		if (!R_HIU_BIT(HHI_CVBS_DETECT_CNTL,
			AFE_CH1_EN_DETECT_BIT, AFE_CH1_EN_DETECT_WIDTH)) {
			goto TIMER;
		}
	} else if (avdev->dts_param.device_mask == TVAFE_AVIN_CH2_MASK) {
		avdev->irq_counter[0] = aml_read_cbus(CVBS_IRQ1_COUNTER);
		if (meson_data) {
			if (!R_HIU_BIT(HHI_CVBS_DETECT_CNTL,
			AFE_TL_CH2_EN_DETECT_BIT, AFE_TL_CH2_EN_DETECT_WIDTH))
				goto TIMER;
		} else {
			if (!R_HIU_BIT(HHI_CVBS_DETECT_CNTL,
				AFE_CH2_EN_DETECT_BIT, AFE_CH2_EN_DETECT_WIDTH))
				goto TIMER;
		}
	} else if (avdev->dts_param.device_mask == TVAFE_AVIN_MASK) {
		avdev->irq_counter[0] = aml_read_cbus(CVBS_IRQ0_COUNTER);
		avdev->irq_counter[1] = aml_read_cbus(CVBS_IRQ1_COUNTER);
		if (meson_data) {
			if (!R_HIU_BIT(HHI_CVBS_DETECT_CNTL,
			AFE_CH1_EN_DETECT_BIT, AFE_CH1_EN_DETECT_WIDTH) ||
				!R_HIU_BIT(HHI_CVBS_DETECT_CNTL,
			AFE_TL_CH2_EN_DETECT_BIT, AFE_TL_CH2_EN_DETECT_WIDTH))
				goto TIMER;
		} else {
			if (!R_HIU_BIT(HHI_CVBS_DETECT_CNTL,
			AFE_CH1_EN_DETECT_BIT, AFE_CH1_EN_DETECT_WIDTH) ||
				!R_HIU_BIT(HHI_CVBS_DETECT_CNTL,
				AFE_CH2_EN_DETECT_BIT, AFE_CH2_EN_DETECT_WIDTH))
				goto TIMER;
		}
		if (avdev->irq_counter[1] != s_irq_counter1) {
			if (s_counter1_last_state != 1)
				s_irq_counter1_time = 0;
			s_irq_counter1_time++;
			if (s_irq_counter1_time >= avin_count_times) {
				if (avdev->report_data_s[1].status !=
							TVAFE_AVIN_STATUS_IN) {
					avdev->report_data_s[1].status =
							TVAFE_AVIN_STATUS_IN;
					state_changed = 1;
					av2_plugin_state = 0;
					tvafe_pr_info("avin[1].status IN.\n");
				/*port opened and plug in,enable clamp*/
				/*sync tip close*/
				if (avport_opened == TVAFE_PORT_AV2)
					tvafe_cha2_SYNCTIP_close_config();
				}
				s_irq_counter1_time = 0;
			}
			s_counter1_last_state = 1;
		} else {
			if (s_counter1_last_state != 0)
				s_irq_counter1_time = 0;
			s_irq_counter1_time++;
			if (s_irq_counter1_time >= avin_count_times) {
				if (avdev->report_data_s[1].status !=
						TVAFE_AVIN_STATUS_OUT) {
					avdev->report_data_s[1].status =
						TVAFE_AVIN_STATUS_OUT;
					state_changed = 1;
					av2_plugin_state = 1;
					tvafe_pr_info("avin[1].status OUT.\n");
				/*port opened but plug out,need disable clamp*/
				if (avport_opened == TVAFE_PORT_AV2) {
					W_APB_BIT(TVFE_CLAMP_INTF, 0,
						  CLAMP_EN_BIT, CLAMP_EN_WID);
					tvafe_cha2_detect_restart_config();
				}
				}
				s_irq_counter1_time = 0;
			}
			s_counter1_last_state = 0;
		}
		s_irq_counter1 = avdev->irq_counter[1];
	}
		/*tvafe_pr_info("irq_counter[0]:%u, last_count:%u\n"*/
		/*	avdev->irq_counter[0], s_irq_counter0);*/
	if (avdev->irq_counter[0] != s_irq_counter0) {
		if (s_counter0_last_state != 1)
			s_irq_counter0_time = 0;
		s_irq_counter0_time++;
		if (s_irq_counter0_time >= avin_count_times) {
			if (avdev->report_data_s[0].status !=
					TVAFE_AVIN_STATUS_IN) {
				avdev->report_data_s[0].status =
					TVAFE_AVIN_STATUS_IN;
				state_changed = 1;
				av1_plugin_state = 0;
				tvafe_pr_info("avin[0].status IN.\n");
				/*port opened and plug in then enable clamp*/
				/*sync tip close*/
				if (avport_opened == TVAFE_PORT_AV1)
					tvafe_cha1_SYNCTIP_close_config();
			}
			s_irq_counter0_time = 0;
		}
		s_counter0_last_state = 1;
	} else {
		if (s_counter0_last_state != 0)
			s_irq_counter0_time = 0;
		s_irq_counter0_time++;
		if (s_irq_counter0_time >= avin_count_times) {
			if (avdev->report_data_s[0].status !=
						TVAFE_AVIN_STATUS_OUT) {
				avdev->report_data_s[0].status =
						TVAFE_AVIN_STATUS_OUT;
				state_changed = 1;
				av1_plugin_state = 1;
				tvafe_pr_info("avin[0].status OUT.\n");

			/*After the CVBS is unplug,*/
			/*the EN_SYNC_TIP need be set to "1"*/
			/*to sense the plug in operation*/
			/*port opened but plug out,need disable clamp*/
			if (avport_opened == TVAFE_PORT_AV1) {
				W_APB_BIT(TVFE_CLAMP_INTF, 0,
					  CLAMP_EN_BIT, CLAMP_EN_WID);
				tvafe_cha1_detect_restart_config();
			}
			}
			s_irq_counter0_time = 0;
		}
		s_counter0_last_state = 0;
	}
	s_irq_counter0 = avdev->irq_counter[0];

	if (state_changed)
		wake_up_interruptible(&tvafe_avin_waitq);

TIMER:
	avin_detect_timer.expires = jiffies +
				(TVAFE_AVIN_INTERVAL * avin_timer_time);
	add_timer(&avin_detect_timer);
}

static int tvafe_avin_init_resource(struct tvafe_avin_det_s *avdev)
{
	/* add timer for avin detect*/
	init_timer(&avin_detect_timer);
	avin_detect_timer.data = (unsigned long)avdev;
	avin_detect_timer.function = tvafe_avin_detect_timer_handler;
	avin_detect_timer.expires = jiffies +
				(TVAFE_AVIN_INTERVAL * avin_timer_time);
	add_timer(&avin_detect_timer);

	return 0;
}

static DEVICE_ATTR(debug, 0644, tvafe_avin_detect_show,
	tvafe_avin_detect_store);

static int tvafe_avin_detect_probe(struct platform_device *pdev)
{
	int ret;
	int state = 0;
	struct tvafe_avin_det_s *avdev = NULL;

	meson_data = (struct meson_avin_data *)
		of_device_get_match_data(&pdev->dev);
	if (meson_data)
		tvafe_pr_info("tvafe_avin_detect_probe cpuid:%d,%s.\n",
			meson_data->cpu_id, meson_data->name);

	avdev = kzalloc(sizeof(struct tvafe_avin_det_s), GFP_KERNEL);
	if (!avdev) {
		state = -ENOMEM;
		goto get_param_mem_fail;
	}

	platform_set_drvdata(pdev, avdev);

	ret = tvafe_avin_dts_parse(pdev);
	if (ret) {
		state = ret;
		goto get_dts_dat_fail;
	}

	/* register char device */
	ret = tvafe_register_avin_dev(avdev);
	/* create class attr file */
	ret = device_create_file(avdev->config_dev, &dev_attr_debug);
	if (ret < 0) {
		tvafe_pr_err("fail to create dbg attribute file\n");
		goto fail_create_dbg_file;
	}
	dev_set_drvdata(avdev->config_dev, avdev);

	/*config analog part setting*/
	tvafe_avin_detect_anlog_config();
	/*config digital part setting*/
	tvafe_avin_detect_digital_config();

	/* add timer for avin detect*/
	init_timer(&avin_detect_timer);
	avin_detect_timer.data = (unsigned long)avdev;
	avin_detect_timer.function = tvafe_avin_detect_timer_handler;
	avin_detect_timer.expires = jiffies +
				(TVAFE_AVIN_INTERVAL * avin_timer_time);
	add_timer(&avin_detect_timer);

	avin_detect_flag |= AVIN_DETECT_INIT;
	tvafe_pr_info("tvafe_avin_detect_probe ok.\n");

	return 0;

fail_create_dbg_file:
get_dts_dat_fail:
	kfree(avdev);
get_param_mem_fail:
	tvafe_pr_info("tvafe_avin_detect: kzalloc error\n");
	return state;
}

static int tvafe_avin_detect_suspend(struct platform_device *pdev,
	pm_message_t state)
{
	struct tvafe_avin_det_s *avdev = platform_get_drvdata(pdev);

	del_timer_sync(&avin_detect_timer);
	tvafe_avin_detect_disable(avdev);
	tvafe_pr_info("tvafe_avin_detect_suspend ok.\n");
	return 0;
}

static int tvafe_avin_detect_resume(struct platform_device *pdev)
{
	struct tvafe_avin_det_s *avdev = platform_get_drvdata(pdev);

	tvafe_avin_init_resource(avdev);
	tvafe_avin_detect_enable(avdev);
	tvafe_pr_info("tvafe_avin_detect_resume ok.\n");
	return 0;
}

static void tvafe_avin_detect_shutdown(struct platform_device *pdev)
{
	struct tvafe_avin_det_s *avdev = platform_get_drvdata(pdev);

	cdev_del(&avdev->avin_cdev);
	del_timer_sync(&avin_detect_timer);
	tvafe_avin_detect_disable(avdev);
	device_remove_file(avdev->config_dev, &dev_attr_debug);
	tvafe_pr_info("tvafe_avin_detect_shutdown ok.\n");
	kfree(avdev);
}

int tvafe_avin_detect_remove(struct platform_device *pdev)
{
	struct tvafe_avin_det_s *avdev = platform_get_drvdata(pdev);

	cdev_del(&avdev->avin_cdev);
	del_timer_sync(&avin_detect_timer);
	tvafe_avin_detect_disable(avdev);
	device_remove_file(avdev->config_dev, &dev_attr_debug);
	kfree(avdev);

	avin_detect_flag = 0;

	return 0;
}

#ifdef CONFIG_OF
struct meson_avin_data tl1_data = {
	.cpu_id = AVIN_CPU_TYPE_TL1,
	.name = "meson-tl1-avin-detect",
};

struct meson_avin_data tm2_data = {
	.cpu_id = AVIN_CPU_TYPE_TM2,
	.name = "meson-tm2-avin-detect",
};

struct meson_avin_data t5_data = {
	.cpu_id = AVIN_CPU_TYPE_T5,
	.name = "meson-t5-avin-detect",
};

struct meson_avin_data t5d_data = {
	.cpu_id = AVIN_CPU_TYPE_T5D,
	.name = "meson-t5d-avin-detect",
};

static const struct of_device_id tvafe_avin_dt_match[] = {
	{	.compatible = "amlogic, tvafe_avin_detect",
	},
	{	.compatible = "amlogic, tl1_tvafe_avin_detect",
		.data = &tl1_data,
	},
	{	.compatible = "amlogic, tm2_tvafe_avin_detect",
		.data = &tm2_data,
	},
	{	.compatible = "amlogic, t5_tvafe_avin_detect",
		.data = &t5_data,
	},
	{	.compatible = "amlogic, t5d_tvafe_avin_detect",
		.data = &t5d_data,
	},
	{},
};
#else
#define tvafe_avin_dt_match NULL
#endif

static struct platform_driver tvafe_avin_driver = {
	.probe      = tvafe_avin_detect_probe,
	.remove     = tvafe_avin_detect_remove,
	.suspend    = tvafe_avin_detect_suspend,
	.resume     = tvafe_avin_detect_resume,
	.shutdown   = tvafe_avin_detect_shutdown,
	.driver     = {
		.name   = "tvafe_avin_detect",
		.of_match_table = tvafe_avin_dt_match,
	},
};

static int __init tvafe_avin_detect_init(void)
{
	return platform_driver_register(&tvafe_avin_driver);
}

static void __exit tvafe_avin_detect_exit(void)
{
	platform_driver_unregister(&tvafe_avin_driver);
}

module_init(tvafe_avin_detect_init);
module_exit(tvafe_avin_detect_exit);

MODULE_DESCRIPTION("Meson TVAFE AVIN detect Driver");
MODULE_LICENSE("GPL");
MODULE_AUTHOR("Amlogic, Inc.");



