/*
 * Tegra Video Input 4 device common APIs
 *
 * Copyright (c) 2016-2017, NVIDIA CORPORATION.  All rights reserved.
 *
 * Author: Frank Chen <frank@nvidia.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */
#include <linux/freezer.h>
#include <linux/kthread.h>
#include <linux/nvhost.h>
#include <linux/tegra-powergate.h>
#include <media/capture.h>
#include <media/tegra_camera_platform.h>
#include "linux/nvhost_ioctl.h"
#include "mc_common.h"
#include "nvhost_acm.h"
#include "vi4_formats.h"
#include "vi4_registers.h"
#include "vi/vi4.h"
#include "vi/vi_notify.h"

#define DEFAULT_FRAMERATE	30
#define DEFAULT_CSI_FREQ	204000000
#define BPP_MEM		2
#define MAX_VI_CHANNEL 12
#define NUM_PPC		8
#define VI_CSI_CLK_SCALE	110
#define SOF_SYNCPT_IDX	0
#define FE_SYNCPT_IDX	1

void tegra_channel_queued_buf_done(struct tegra_channel *chan,
					  enum vb2_buffer_state state);
int tegra_channel_set_stream(struct tegra_channel *chan, bool on);
void tegra_channel_ring_buffer(struct tegra_channel *chan,
		struct vb2_v4l2_buffer *vb,
		struct timespec *ts, int state);
struct tegra_channel_buffer *dequeue_buffer(struct tegra_channel *chan);
void tegra_channel_init_ring_buffer(struct tegra_channel *chan);
void free_ring_buffers(struct tegra_channel *chan, int frames);
int tegra_channel_set_power(struct tegra_channel *chan, bool on);
static void tegra_channel_stop_kthreads(struct tegra_channel *chan);
static int tegra_channel_stop_increments(struct tegra_channel *chan);
static void tegra_channel_notify_status_callback(
				struct vi_notify_channel *,
				const struct vi_capture_status *,
				void *);
static void tegra_channel_error_worker(struct work_struct *status_work);
static void tegra_channel_notify_error_callback(void *);

u32 csimux_config_stream[] = {
	CSIMUX_CONFIG_STREAM_0,
	CSIMUX_CONFIG_STREAM_1,
	CSIMUX_CONFIG_STREAM_2,
	CSIMUX_CONFIG_STREAM_3,
	CSIMUX_CONFIG_STREAM_4,
	CSIMUX_CONFIG_STREAM_5
};

static void vi4_write(struct tegra_channel *chan, unsigned int addr, u32 val)
{
	writel(val, chan->vi->iomem + addr);
}

static u32 vi4_read(struct tegra_channel *chan, unsigned int addr)
{
	return readl(chan->vi->iomem + addr);
}

static void vi4_channel_write(struct tegra_channel *chan,
		unsigned int index, unsigned int addr, u32 val)
{
	writel(val,
		chan->vi->iomem + VI4_CHANNEL_OFFSET * (index + 1) + addr);
}

void vi4_init_video_formats(struct tegra_channel *chan)
{
	int i;

	chan->num_video_formats = ARRAY_SIZE(vi4_video_formats);
	for (i = 0; i < chan->num_video_formats; i++)
		chan->video_formats[i] = &vi4_video_formats[i];
}

long vi4_default_ioctl(struct file *file, void *fh,
			bool use_prio, unsigned int cmd, void *arg)
{
	struct v4l2_fh *vfh = file->private_data;
	struct tegra_channel *chan = to_tegra_channel(vfh->vdev);
	long err = 0;

	switch (_IOC_NR(cmd)) {
	case _IOC_NR(VIDIOC_CAPTURE_SETUP):
		if (chan->bypass)
			err = vi_capture_setup(chan,
					(struct vi_capture_setup *)arg);
		else {
			dev_err(&chan->video.dev, "not in bypass mode\n");
			err = -ENODEV;
		}
		if (err)
			dev_err(&chan->video.dev, "capture setup failed\n");
		break;
	case _IOC_NR(VIDIOC_CAPTURE_RESET):
		if (chan->bypass)
			err = vi_capture_reset(chan, *(uint32_t *)arg);
		else {
			dev_err(&chan->video.dev, "not in bypass mode\n");
			err = -ENODEV;
		}
		if (err)
			dev_err(&chan->video.dev, "capture reset failed\n");
		break;
	case _IOC_NR(VIDIOC_CAPTURE_RELEASE):
		if (chan->bypass)
			err = vi_capture_release(chan, *(uint32_t *)arg);
		else {
			dev_err(&chan->video.dev, "not in bypass mode\n");
			err = -ENODEV;
		}
		if (err)
			dev_err(&chan->video.dev, "capture release failed\n");
		break;
	case _IOC_NR(VIDIOC_CAPTURE_GET_INFO):
		if (chan->bypass)
			err = vi_capture_get_info(chan,
					(struct vi_capture_info *)arg);
		else {
			dev_err(&chan->video.dev, "not in bypass mode\n");
			err = -ENODEV;
		}
		if (err)
			dev_err(&chan->video.dev, "capture get info failed\n");
		break;
	case _IOC_NR(VIDIOC_CAPTURE_SET_CONFIG):
		if (chan->bypass)
			err = vi_capture_control_message(chan,
					(struct vi_capture_control_msg *)arg);
		else {
			dev_err(&chan->video.dev, "not in bypass mode\n");
			err = -ENODEV;
		}
		if (err)
			dev_err(&chan->video.dev, "capture config failed\n");
		break;
	case _IOC_NR(VIDIOC_CAPTURE_REQUEST):
		if (chan->bypass)
			err = vi_capture_request(chan,
					(struct vi_capture_req *)arg);
		else {
			dev_err(&chan->video.dev, "not in bypass mode\n");
			err = -ENODEV;
		}
		if (err)
			dev_err(&chan->video.dev,
				"capture request submit failed\n");
		break;
	case _IOC_NR(VIDIOC_CAPTURE_STATUS):
		if (chan->bypass)
			err = vi_capture_status(chan, *(uint32_t *)arg);
		else {
			dev_err(&chan->video.dev, "not in bypass mode\n");
			err = -ENODEV;
		}
		if (err)
			dev_err(&chan->video.dev,
				"capture get status failed\n");
		break;
	default:
		dev_err(&chan->video.dev, "%s:Unknown ioctl\n", __func__);
		return -ENOIOCTLCMD;
	}

	return err;
}


int tegra_vi4_s_ctrl(struct v4l2_ctrl *ctrl)
{
	struct tegra_channel *chan = container_of(ctrl->handler,
				struct tegra_channel, ctrl_handler);
	int err = 0;

	switch (ctrl->id) {
	case V4L2_CID_WRITE_ISPFORMAT:
		chan->write_ispformat = ctrl->val;
		break;
	default:
		dev_err(&chan->video.dev, "%s:Not valid ctrl\n", __func__);
		return -EINVAL;
	}

	return err;
}

static const struct v4l2_ctrl_ops vi4_ctrl_ops = {
	.s_ctrl	= tegra_vi4_s_ctrl,
};

static const struct v4l2_ctrl_config vi4_custom_ctrls[] = {
	{
		.ops = &vi4_ctrl_ops,
		.id = V4L2_CID_WRITE_ISPFORMAT,
		.name = "Write ISP format",
		.type = V4L2_CTRL_TYPE_INTEGER,
		.def = 1,
		.min = 1,
		.max = 1,
		.step = 1,
	},
};

int vi4_add_ctrls(struct tegra_channel *chan)
{
	int i;

	/* Add vi4 custom controls */
	for (i = 0; i < ARRAY_SIZE(vi4_custom_ctrls); i++) {
		v4l2_ctrl_new_custom(&chan->ctrl_handler,
			&vi4_custom_ctrls[i], NULL);
		if (chan->ctrl_handler.error) {
			dev_err(chan->vi->dev,
				"Failed to add %s ctrl\n",
				vi4_custom_ctrls[i].name);
			return chan->ctrl_handler.error;
		}
	}

	return 0;
}

static bool vi4_init(struct tegra_channel *chan)
{
	vi4_write(chan, CFG_INTERRUPT_MASK, 0x3f0000f9);
	vi4_write(chan, CFG_INTERRUPT_STATUS, 0x3f000001);
	vi4_write(chan, NOTIFY_ERROR, 0x1);
	vi4_write(chan, NOTIFY_TAG_CLASSIFY_0, 0xe39c08e3);
	return true;
}

static bool vi4_check_status(struct tegra_channel *chan)
{
	int status;

	/* check interrupt status error */
	status = vi4_read(chan, CFG_INTERRUPT_STATUS);
	if (status & 0x1)
		dev_err(chan->vi->dev,
			"VI_CFG_INTERRUPT_STATUS_0: MASTER_ERR_STATUS error!\n");

	/* Check VI NOTIFY input FIFO error */
	status = vi4_read(chan, NOTIFY_ERROR);
	if (status & 0x1)
		dev_err(chan->vi->dev,
			"VI_NOTIFY_ERROR_0: NOTIFY_FIFO_OVERFLOW error!\n");

	return true;
}

static bool vi_notify_wait(struct tegra_channel *chan,
		struct timespec *ts)
{
	int i, err;
	u32 thresh[TEGRA_CSI_BLOCKS], temp;

	/*
	 * Increment syncpt for ATOMP_FE
	 *
	 * This is needed in order to keep the syncpt max up to date,
	 * even if we are not waiting for ATOMP_FE here
	 */
	for (i = 0; i < chan->valid_ports; i++)
		temp = nvhost_syncpt_incr_max_ext(chan->vi->ndev,
					chan->syncpt[i][FE_SYNCPT_IDX], 1);

	/*
	 * Increment syncpt for PXL_SOF
	 *
	 * Increment and retrieve PXL_SOF syncpt max value.
	 * This value will be used to wait for next syncpt
	 */
	for (i = 0; i < chan->valid_ports; i++)
		thresh[i] = nvhost_syncpt_incr_max_ext(chan->vi->ndev,
					chan->syncpt[i][SOF_SYNCPT_IDX], 1);

	/*
	 * Wait for PXL_SOF syncpt
	 *
	 * Use the syncpt max value we just set as threshold
	 */
	for (i = 0; i < chan->valid_ports; i++) {
		err = nvhost_syncpt_wait_timeout_ext(chan->vi->ndev,
				chan->syncpt[i][SOF_SYNCPT_IDX], thresh[i],
				250, NULL, NULL);
		if (unlikely(err))
			dev_err(chan->vi->dev,
				"PXL_SOF syncpt timeout! err = %d\n", err);
		else {
			struct vi_capture_status status;

			err = vi_notify_get_capture_status(chan->vnc[i],
					chan->vnc_id[i],
					thresh[i], &status);
			if (unlikely(err))
				dev_err(chan->vi->dev,
					"no capture status! err = %d\n", err);
			else
				*ts = ns_to_timespec((s64)status.sof_ts);
		}
	}
	return true;
}

static void tegra_channel_surface_setup(
	struct tegra_channel *chan, struct tegra_channel_buffer *buf, int index)
{
	int vnc_id = chan->vnc_id[index];
	unsigned int offset = chan->buffer_offset[index];

	vi4_channel_write(chan, vnc_id, ATOMP_EMB_SURFACE_OFFSET0, 0x0);
	vi4_channel_write(chan, vnc_id, ATOMP_EMB_SURFACE_OFFSET0_H, 0x0);
	vi4_channel_write(chan, vnc_id, ATOMP_EMB_SURFACE_STRIDE0, 0x0);
	vi4_channel_write(chan, vnc_id,
		ATOMP_SURFACE_OFFSET0, buf->addr + offset);
	vi4_channel_write(chan, vnc_id,
		ATOMP_SURFACE_STRIDE0, chan->format.bytesperline);
	vi4_channel_write(chan, vnc_id, ATOMP_SURFACE_OFFSET0_H, 0x0);

	if (chan->fmtinfo->fourcc == V4L2_PIX_FMT_NV16) {
		vi4_channel_write(chan, vnc_id,
			ATOMP_SURFACE_OFFSET1, buf->addr + offset +
			chan->format.sizeimage / 2);
		vi4_channel_write(chan, vnc_id,
			ATOMP_SURFACE_OFFSET1_H, 0x0);
		vi4_channel_write(chan, vnc_id,
			ATOMP_SURFACE_STRIDE1, chan->format.bytesperline);

	} else {
		vi4_channel_write(chan, vnc_id, ATOMP_SURFACE_OFFSET1, 0x0);
		vi4_channel_write(chan, vnc_id, ATOMP_SURFACE_OFFSET1_H, 0x0);
		vi4_channel_write(chan, vnc_id, ATOMP_SURFACE_STRIDE1, 0x0);
	}

	vi4_channel_write(chan, vnc_id, ATOMP_SURFACE_OFFSET2, 0x0);
	vi4_channel_write(chan, vnc_id, ATOMP_SURFACE_OFFSET2_H, 0x0);
	vi4_channel_write(chan, vnc_id, ATOMP_SURFACE_STRIDE2, 0x0);
}

static void tegra_channel_handle_error(struct tegra_channel *chan)
{
	struct v4l2_subdev *sd_on_csi = chan->subdev_on_csi;
	static const struct v4l2_event source_ev_fmt = {
		.type = V4L2_EVENT_SOURCE_CHANGE,
		.u.src_change.changes = V4L2_EVENT_SRC_ERROR,
	};

	tegra_channel_stop_increments(chan);
	vb2_queue_error(&chan->queue);

	/* Application gets notified after CSI Tx's are reset */
	if (sd_on_csi->devnode)
		v4l2_subdev_notify_event(sd_on_csi, &source_ev_fmt);
}

static void tegra_channel_status_worker(struct work_struct *status_work)
{
	struct tegra_channel *chan;

	chan = container_of(status_work, struct tegra_channel, status_work);

	tegra_channel_handle_error(chan);
}

static void tegra_channel_notify_status_callback(
				struct vi_notify_channel *vnc,
				const struct vi_capture_status *status,
				void *client_data)
{
	struct tegra_channel *chan = (struct tegra_channel *)client_data;
	int i;

	spin_lock(&chan->capture_state_lock);
	if (chan->capture_state == CAPTURE_GOOD)
		chan->capture_state = CAPTURE_ERROR;
	else {
		spin_unlock(&chan->capture_state_lock);
		return;
	}
	spin_unlock(&chan->capture_state_lock);

	for (i = 0; i < chan->valid_ports; i++)
		dev_err(chan->vi->dev, "Status: %2u channel:%02X frame:%04X\n",
			status->status, chan->vnc_id[i], status->frame);
	dev_err(chan->vi->dev, "     timestamp sof %llu eof %llu data 0x%08x\n",
		status->sof_ts, status->eof_ts, status->data);
	dev_err(chan->vi->dev, "     capture_id %u stream %2u vchan %2u\n",
		status->capture_id, status->st, status->vc);

	schedule_work(&chan->status_work);
}

static int tegra_channel_notify_enable(
	struct tegra_channel *chan, unsigned int index)
{
	struct tegra_vi4_syncpts_req req;
	int i, err;

	chan->vnc_id[index] = -1;
	for (i = 0; i < MAX_VI_CHANNEL; i++) {
		chan->vnc[index] = vi_notify_channel_open(i);
		if (!IS_ERR(chan->vnc[index])) {
			chan->vnc_id[index] = i;
			break;
		}
	}
	if (chan->vnc_id[index] < 0) {
		dev_err(chan->vi->dev, "No VI channel available!\n");
		return -EFAULT;
	}

	vi_notify_channel_set_notify_funcs(chan->vnc[index],
			&tegra_channel_notify_status_callback,
			&tegra_channel_notify_error_callback,
			(void *)chan);

	/* get PXL_SOF syncpt id */
	chan->syncpt[index][SOF_SYNCPT_IDX] =
		nvhost_get_syncpt_client_managed(chan->vi->ndev, "tegra-vi4");
	if (chan->syncpt[index][SOF_SYNCPT_IDX] == 0) {
		dev_err(chan->vi->dev, "Failed to get PXL_SOF syncpt!\n");
		return -EFAULT;
	}

	/* get ATOMP_FE syncpt id */
	chan->syncpt[index][FE_SYNCPT_IDX] =
		nvhost_get_syncpt_client_managed(chan->vi->ndev, "tegra-vi4");
	if (chan->syncpt[index][FE_SYNCPT_IDX] == 0) {
		dev_err(chan->vi->dev, "Failed to get ATOMP_FE syncpt!\n");
		nvhost_syncpt_put_ref_ext(
			chan->vi->ndev, chan->syncpt[index][SOF_SYNCPT_IDX]);
		return -EFAULT;
	}

	nvhost_syncpt_set_min_eq_max_ext(
		chan->vi->ndev, chan->syncpt[index][SOF_SYNCPT_IDX]);
	nvhost_syncpt_set_min_eq_max_ext(
		chan->vi->ndev, chan->syncpt[index][FE_SYNCPT_IDX]);

	/* enable VI Notify report */
	req.syncpt_ids[0] = chan->syncpt[index][SOF_SYNCPT_IDX]; /* PXL_SOF */
	req.syncpt_ids[1] = chan->syncpt[index][FE_SYNCPT_IDX]; /* ATOMP_FE */
	req.syncpt_ids[2] = 0xffffffff;
	req.stream = chan->port[index];
	req.vc = 0;
	req.pad = 0;

	err = vi_notify_channel_enable_reports(
		chan->vnc_id[index], chan->vnc[index], &req);
	if (err < 0)
		dev_err(chan->vi->dev,
			"Failed to enable report for VI Notify, err = %d\n",
			err);

	return err;
}

static int tegra_channel_notify_disable(
	struct tegra_channel *chan, unsigned int index)
{
	int err;
	int ret = 0;
	struct tegra_vi4_syncpts_req req;

	/* free syncpts */
	nvhost_syncpt_put_ref_ext(
		chan->vi->ndev, chan->syncpt[index][SOF_SYNCPT_IDX]);
	nvhost_syncpt_put_ref_ext(
		chan->vi->ndev, chan->syncpt[index][FE_SYNCPT_IDX]);

	/* close vi-notifier */
	req.syncpt_ids[0] = 0xffffffff;
	req.syncpt_ids[1] = 0xffffffff;
	req.syncpt_ids[2] = 0xffffffff;
	req.stream = chan->port[index];
	req.vc = 0;
	req.pad = 0;

	err = vi_notify_channel_reset(
		chan->vnc_id[index], chan->vnc[index], &req);
	if (err < 0) {
		dev_err(chan->vi->dev,
			"VI Notify channel reset failed, err = %d\n", err);
		if (!ret)
			ret = err;
	}

	err = vi_notify_channel_close(chan->vnc_id[index], chan->vnc[index]);
	if (err < 0) {
		dev_err(chan->vi->dev,
			"VI Notify channel close failed, err = %d\n", err);
		if (!ret)
			ret = err;
	}

	return ret;
}

static int tegra_channel_capture_setup(struct tegra_channel *chan,
		unsigned int index)
{
	u32 height = chan->format.height;
	u32 width = chan->format.width;
	u32 format = chan->fmtinfo->img_fmt;
	u32 data_type = chan->fmtinfo->img_dt;
	u32 csi_port = chan->port[index];
	u32 stream = 1U << csi_port;
	u32 virtual_ch = 1U << 0;
	u32 vnc_id;
	int err;

	if (chan->valid_ports > 1) {
		height = chan->gang_height;
		width = chan->gang_width;
	}

	err = tegra_channel_notify_enable(chan, index);
	if (err < 0) {
		dev_err(chan->vi->dev,
			"Failed to setup VI Notifier, err = %d\n", err);
		return err;
	}

	vnc_id = chan->vnc_id[index];

	vi4_write(chan, csimux_config_stream[csi_port], 0x1);

	vi4_channel_write(chan, vnc_id, MATCH,
			((stream << STREAM_SHIFT) & STREAM) |
			STREAM_MASK |
			((virtual_ch << VIRTUAL_CHANNEL_SHIFT) &
			VIRTUAL_CHANNEL)  |
			VIRTUAL_CHANNEL_MASK);

	vi4_channel_write(chan, vnc_id, MATCH_DATATYPE,
			((data_type << DATATYPE_SHIFT) & DATATYPE) |
			DATATYPE_MASK);

	vi4_channel_write(chan, vnc_id, DT_OVERRIDE, 0x0);

	vi4_channel_write(chan, vnc_id, MATCH_FRAMEID,
			((0 << FRAMEID_SHIFT) & FRAMEID) | 0);

	vi4_channel_write(chan, vnc_id, FRAME_X, width);
	vi4_channel_write(chan, vnc_id, FRAME_Y, height);
	vi4_channel_write(chan, vnc_id, SKIP_X, 0x0);
	vi4_channel_write(chan, vnc_id, CROP_X, width);
	vi4_channel_write(chan, vnc_id, OUT_X, width);
	vi4_channel_write(chan, vnc_id, SKIP_Y, 0x0);
	vi4_channel_write(chan, vnc_id, CROP_Y, height);
	vi4_channel_write(chan, vnc_id, OUT_Y, height);
	vi4_channel_write(chan, vnc_id, PIXFMT_ENABLE, PIXFMT_EN);
	vi4_channel_write(chan, vnc_id, PIXFMT_WIDE, 0x0);
	vi4_channel_write(chan, vnc_id, PIXFMT_FORMAT, format);
	vi4_channel_write(chan, vnc_id, DPCM_STRIP, 0x0);
	vi4_channel_write(chan, vnc_id, ATOMP_DPCM_CHUNK, 0x0);
	vi4_channel_write(chan, vnc_id, ISPBUFA, 0x0);
	vi4_channel_write(chan, vnc_id, LINE_TIMER, 0x1000000);
	vi4_channel_write(chan, vnc_id, EMBED_X, 0x0);
	vi4_channel_write(chan, vnc_id, EMBED_Y, 0x0);
	/*
	 * Set ATOMP_RESERVE to 0 so rctpu won't increment syncpt
	 * for captureInfo. This is copied from nvvi driver.
	 *
	 * If we don't set this register to 0, ATOMP_FE syncpt
	 * will be increment by 2 for each frame
	 */
	vi4_channel_write(chan, vnc_id, ATOMP_RESERVE, 0x0);
	dev_dbg(chan->vi->dev,
		"Create Surface with imgW=%d, imgH=%d, memFmt=%d\n",
		width, height, format);

	return 0;
}

static int tegra_channel_capture_frame(struct tegra_channel *chan,
				       struct tegra_channel_buffer *buf)
{
	struct vb2_v4l2_buffer *vb = &buf->buf;
	struct timespec ts;
	int state = VB2_BUF_STATE_DONE;
	unsigned long flags;
	int err = false;
	int i;

	for (i = 0; i < chan->valid_ports; i++)
		tegra_channel_surface_setup(chan, buf, i);

	if (!chan->bfirst_fstart) {
		err = tegra_channel_set_stream(chan, true);
		if (err < 0)
			return err;
	}

	for (i = 0; i < chan->valid_ports; i++) {
		vi4_channel_write(chan, chan->vnc_id[i], CHANNEL_COMMAND, LOAD);
		vi4_channel_write(chan, chan->vnc_id[i],
			CONTROL, SINGLESHOT | MATCH_STATE_EN);
	}

	/* wait for vi notifier events */
	vi_notify_wait(chan, &ts);

	vi4_check_status(chan);

	spin_lock_irqsave(&chan->capture_state_lock, flags);
	if (chan->capture_state != CAPTURE_ERROR)
		chan->capture_state = CAPTURE_GOOD;
	spin_unlock_irqrestore(&chan->capture_state_lock, flags);

	tegra_channel_ring_buffer(chan, vb, &ts, state);

	return 0;
}

static int tegra_channel_stop_increments(struct tegra_channel *chan)
{
	int i;
	struct tegra_vi4_syncpts_req req = {
		.syncpt_ids = {
			0xffffffff,
			0xffffffff,
			0xffffffff,
		},
		.stream = chan->port[0],
		.vc = 0,
	};

	/* No need to check errors. There's nothing we could do. */
	for (i = 0; i < chan->valid_ports; i++)
		vi_notify_channel_reset(chan->vnc_id[i], chan->vnc[i], &req);

	return 0;
}

static void tegra_channel_capture_done(struct tegra_channel *chan)
{
	struct timespec ts;
	struct tegra_channel_buffer *buf;
	int state = VB2_BUF_STATE_DONE;
	u32 thresh[TEGRA_CSI_BLOCKS];
	int i, err;

	/* dequeue buffer and return if no buffer exists */
	buf = dequeue_buffer(chan);
	if (!buf)
		return;

	/* make sure to read the last frame out before exit */
	for (i = 0; i < chan->valid_ports; i++) {
		tegra_channel_surface_setup(chan, buf, i);
		vi4_channel_write(chan, chan->vnc_id[i], CHANNEL_COMMAND, LOAD);
		vi4_channel_write(chan, chan->vnc_id[i],
			CONTROL, SINGLESHOT | MATCH_STATE_EN);
	}

	for (i = 0; i < chan->valid_ports; i++) {
		err = nvhost_syncpt_read_ext_check(chan->vi->ndev,
				chan->syncpt[i][FE_SYNCPT_IDX], &thresh[i]);
		/* Get current ATOMP_FE syncpt min value */
		if (!err) {
			struct vi_capture_status status;
			u32 index = thresh[i] + 1;
			/* Wait for ATOMP_FE syncpt
			 *
			 * This is to make sure we don't exit the capture thread
			 * before the last frame is done writing to memory
			 */
			err = nvhost_syncpt_wait_timeout_ext(chan->vi->ndev,
					chan->syncpt[i][FE_SYNCPT_IDX],
					index,
					250, NULL, NULL);
			if (unlikely(err))
				dev_err(chan->vi->dev,
					"ATOMP_FE syncpt timeout!\n");
			else {
				err = vi_notify_get_capture_status(chan->vnc[i],
						chan->vnc_id[i],
						index, &status);
				if (unlikely(err))
					dev_err(chan->vi->dev,
						"no capture status! err = %d\n",
						err);
				else
					ts = ns_to_timespec((s64)status.eof_ts);
			}
		}
	}

	/* Mark capture state to IDLE as capture is finished */
	chan->capture_state = CAPTURE_IDLE;

	tegra_channel_ring_buffer(chan, &buf->buf, &ts, state);
}

static int tegra_channel_kthread_capture_start(void *data)
{
	struct tegra_channel *chan = data;
	struct tegra_channel_buffer *buf;
	int err = 0;

	set_freezable();

	while (1) {

		try_to_freeze();

		wait_event_interruptible(chan->start_wait,
					 !list_empty(&chan->capture) ||
					 kthread_should_stop());

		if (kthread_should_stop())
			break;

		/* source is not streaming if error is non-zero */
		/* wait till kthread stop and dont DeQ buffers */
		if (err)
			continue;

		buf = dequeue_buffer(chan);
		if (!buf)
			continue;

		err = tegra_channel_capture_frame(chan, buf);
	}

	return 0;
}

static void tegra_channel_stop_kthreads(struct tegra_channel *chan)
{
	mutex_lock(&chan->stop_kthread_lock);
	/* Stop the kthread for capture */
	if (chan->kthread_capture_start) {
		kthread_stop(chan->kthread_capture_start);
		chan->kthread_capture_start = NULL;
	}
	mutex_unlock(&chan->stop_kthread_lock);
}

static int tegra_channel_update_clknbw(struct tegra_channel *chan, u8 on)
{
	int ret = 0;
	unsigned long request_pixelrate;
	struct v4l2_subdev_frame_interval fie;
	unsigned long csi_freq = 0;

	fie.interval.denominator = DEFAULT_FRAMERATE;
	fie.interval.numerator = 1;

	if (v4l2_subdev_has_op(chan->subdev_on_csi,
				video, g_frame_interval))
		v4l2_subdev_call(chan->subdev_on_csi, video,
				g_frame_interval, &fie);
	if (on) {
		/* for PG, using default frequence */
		if (chan->pg_mode) {
			csi_freq = DEFAULT_CSI_FREQ;
			request_pixelrate = csi_freq * NUM_PPC;
		} else {
			/**
			 * TODO: use real sensor pixelrate
			 * See PowerService code
			 */
			request_pixelrate = (long long)(chan->format.width
				* chan->format.height
				* fie.interval.denominator / 100)
				* VI_CSI_CLK_SCALE;
			csi_freq = ((long long)chan->format.width
				* chan->format.height
				* fie.interval.denominator) / NUM_PPC;
		}

		/* VI clk should be slightly faster than CSI clk*/
		ret = nvhost_module_set_rate(chan->vi->ndev, &chan->video,
				request_pixelrate, 0, NVHOST_PIXELRATE);
		if (ret) {
			dev_err(chan->vi->dev, "Fail to update vi clk\n");
			return ret;
		}
	} else {
		csi_freq = DEFAULT_CSI_FREQ;
		ret = nvhost_module_set_rate(chan->vi->ndev, &chan->video, 0, 0,
				NVHOST_PIXELRATE);
		if (ret) {
			dev_err(chan->vi->dev, "Fail to update vi clk\n");
			return ret;
		}
	}
	if (chan->pg_mode)
		chan->requested_kbyteps = (on > 0 ? 1 : -1) *
			((long long)csi_freq * BPP_MEM * 110 / 100) / 1000;
	else
		chan->requested_kbyteps = (on > 0 ? 1 : -1) *
		(((long long) chan->format.width * chan->format.height
		* fie.interval.denominator * BPP_MEM) * 115 / 100) / 1000;

	mutex_lock(&chan->vi->bw_update_lock);
	chan->vi->aggregated_kbyteps += chan->requested_kbyteps;
	ret = vi_v4l2_update_isobw(chan->vi->aggregated_kbyteps, 0);
	mutex_unlock(&chan->vi->bw_update_lock);
	if (ret)
		dev_info(chan->vi->dev,
		"WAR:Calculation not precise.Ignore BW request failure\n");
	ret = vi4_v4l2_set_la(chan->vi->ndev, 0, 0);
	if (ret)
		dev_info(chan->vi->dev,
		"WAR:Calculation not precise.Ignore LA failure\n");
	return 0;
}

int vi4_channel_start_streaming(struct vb2_queue *vq, u32 count)
{
	struct tegra_channel *chan = vb2_get_drv_priv(vq);
	struct media_pipeline *pipe = chan->video.entity.pipe;
	int ret = 0, i;
	unsigned long flags;
	struct v4l2_ctrl *override_ctrl;

	vi4_init(chan);
	ret = media_entity_pipeline_start(&chan->video.entity, pipe);
	if (ret < 0)
		goto error_pipeline_start;

	if (chan->bypass) {
		ret = tegra_channel_set_stream(chan, true);
		if (ret < 0)
			goto error_set_stream;
		return ret;
	}

	spin_lock_irqsave(&chan->capture_state_lock, flags);
	chan->capture_state = CAPTURE_IDLE;
	spin_unlock_irqrestore(&chan->capture_state_lock, flags);

	for (i = 0; i < chan->valid_ports; i++) {
		ret = tegra_channel_capture_setup(chan, i);
		if (ret < 0)
			goto error_capture_setup;
	}

	chan->sequence = 0;
	tegra_channel_init_ring_buffer(chan);

	/* disable override for vi mode */
	override_ctrl = v4l2_ctrl_find(
		&chan->ctrl_handler, V4L2_CID_OVERRIDE_ENABLE);
	if (!chan->pg_mode) {
		if (override_ctrl) {
			ret = v4l2_ctrl_s_ctrl(override_ctrl, false);
			if (ret < 0)
				dev_err(&chan->video.dev,
					"failed to disable override control\n");
		} else
			dev_err(&chan->video.dev,
				"No override control\n");
	}

	/* Update clock and bandwidth based on the format */
	ret = tegra_channel_update_clknbw(chan, 1);
	if (ret)
		goto error_capture_setup;

	INIT_WORK(&chan->error_work, tegra_channel_error_worker);
	INIT_WORK(&chan->status_work, tegra_channel_status_worker);

	/* Start kthread to capture data to buffer */
	chan->kthread_capture_start = kthread_run(
					tegra_channel_kthread_capture_start,
					chan, chan->video.name);
	if (IS_ERR(chan->kthread_capture_start)) {
		dev_err(&chan->video.dev,
			"failed to run kthread for capture start\n");
		ret = PTR_ERR(chan->kthread_capture_start);
		goto error_capture_setup;
	}

	return 0;

error_capture_setup:
	if (!chan->pg_mode)
		tegra_channel_set_stream(chan, false);
error_set_stream:
	media_entity_pipeline_stop(&chan->video.entity);
error_pipeline_start:
	vq->start_streaming_called = 0;
	tegra_channel_queued_buf_done(chan, VB2_BUF_STATE_QUEUED);

	return ret;
}

int vi4_channel_stop_streaming(struct vb2_queue *vq)
{
	struct tegra_channel *chan = vb2_get_drv_priv(vq);
	bool is_streaming = atomic_read(&chan->is_streaming);
	int i;

	for (i = 0; i < chan->valid_ports; i++) {
		if (chan->vnc_id[i] == -1)
			return 0;
	}

	cancel_work_sync(&chan->status_work);
	cancel_work_sync(&chan->error_work);

	if (!chan->bypass) {
		tegra_channel_stop_kthreads(chan);
		/* wait for last frame memory write ack */
		if (is_streaming)
			tegra_channel_capture_done(chan);
		for (i = 0; i < chan->valid_ports; i++)
			tegra_channel_notify_disable(chan, i);
		/* free all the ring buffers */
		free_ring_buffers(chan, chan->num_buffers);
		/* dequeue buffers back to app which are in capture queue */
		tegra_channel_queued_buf_done(chan, VB2_BUF_STATE_ERROR);
	}

	tegra_channel_set_stream(chan, false);
	media_entity_pipeline_stop(&chan->video.entity);

	if (!chan->bypass)
		tegra_channel_update_clknbw(chan, 0);

	return 0;
}

int tegra_vi4_power_on(struct tegra_mc_vi *vi)
{
	int ret;

	ret = nvhost_module_busy(vi->ndev);
	if (ret) {
		dev_err(vi->dev, "%s:nvhost module is busy\n", __func__);
		return ret;
	}

	ret = tegra_camera_emc_clk_enable();
	if (ret)
		goto err_emc_enable;

	return 0;

err_emc_enable:
	nvhost_module_idle(vi->ndev);

	return ret;
}

void tegra_vi4_power_off(struct tegra_mc_vi *vi)
{
	tegra_channel_ec_close(vi);
	tegra_camera_emc_clk_disable();
	nvhost_module_idle(vi->ndev);
}

int vi4_power_on(struct tegra_channel *chan)
{
	int ret = 0;
	struct tegra_mc_vi *vi;
	struct tegra_csi_device *csi;

	vi = chan->vi;
	csi = vi->csi;

	/* Use chan->video as identifier of vi4 nvhost_module client
	 * since they are unique per channel
	 */
	ret = nvhost_module_add_client(vi->ndev, &chan->video);
	if (ret < 0)
		return ret;

	ret = tegra_vi4_power_on(vi);
	if (ret < 0)
		return ret;

	if (atomic_add_return(1, &chan->power_on_refcnt) == 1) {
		ret = tegra_channel_set_power(chan, 1);
		if (ret < 0) {
			dev_err(vi->dev, "Failed to power on subdevices\n");
			return ret;
		}
	}

	ret = vi_capture_init(chan);
	if (ret < 0)
		return ret;

	return 0;
}

void vi4_power_off(struct tegra_channel *chan)
{
	int ret = 0;
	struct tegra_mc_vi *vi;
	struct tegra_csi_device *csi;

	vi = chan->vi;
	csi = vi->csi;

	vi_capture_shutdown(chan);

	if (atomic_dec_and_test(&chan->power_on_refcnt)) {
		ret = tegra_channel_set_power(chan, 0);
		if (ret < 0)
			dev_err(vi->dev, "Failed to power off subdevices\n");
	}

	tegra_vi4_power_off(vi);
	nvhost_module_remove_client(vi->ndev, &chan->video);
}

static void tegra_channel_error_worker(struct work_struct *error_work)
{
	struct tegra_channel *chan;

	chan = container_of(error_work, struct tegra_channel, error_work);

	vi4_power_off(chan);
	tegra_channel_handle_error(chan);
}

static void tegra_channel_notify_error_callback(void *client_data)
{
	struct tegra_channel *chan = (struct tegra_channel *)client_data;

	spin_lock(&chan->capture_state_lock);
	if (chan->capture_state == CAPTURE_GOOD)
		chan->capture_state = CAPTURE_ERROR;
	else {
		spin_unlock(&chan->capture_state_lock);
		return;
	}
	spin_unlock(&chan->capture_state_lock);

	schedule_work(&chan->error_work);
}
