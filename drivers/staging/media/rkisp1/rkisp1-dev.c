// SPDX-License-Identifier: (GPL-2.0+ OR MIT)
/*
 * Rockchip ISP1 Driver - Base driver
 *
 * Copyright (C) 2019 Collabora, Ltd.
 *
 * Based on Rockchip ISP1 driver by Rockchip Electronics Co., Ltd.
 * Copyright (C) 2017 Rockchip Electronics Co., Ltd.
 */

#include <linux/clk.h>
#include <linux/debugfs.h>
#include <linux/interrupt.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_graph.h>
#include <linux/of_platform.h>
#include <linux/pinctrl/consumer.h>
#include <linux/phy/phy.h>
#include <linux/phy/phy-mipi-dphy.h>
#include <media/v4l2-fwnode.h>

#include "rkisp1-common.h"

/*
 * ISP Details
 * -----------
 *
 * ISP Comprises with:
 *	MIPI serial camera interface
 *	Image Signal Processing
 *	Many Image Enhancement Blocks
 *	Crop
 *	Resizer
 *	RBG display ready image
 *	Image Rotation
 *
 * ISP Block Diagram
 * -----------------
 *                                                             rkisp1-resizer.c          rkisp1-capture.c
 *                                                          |====================|  |=======================|
 *                                rkisp1-isp.c                              Main Picture Path
 *                        |==========================|      |===============================================|
 *                        +-----------+  +--+--+--+--+      +--------+  +--------+              +-----------+
 *                        |           |  |  |  |  |  |      |        |  |        |              |           |
 * +--------+    |\       |           |  |  |  |  |  |   -->|  Crop  |->|  RSZ   |------------->|           |
 * |  MIPI  |--->|  \     |           |  |  |  |  |  |   |  |        |  |        |              |           |
 * +--------+    |   |    |           |  |IE|IE|IE|IE|   |  +--------+  +--------+              |  Memory   |
 *               |MUX|--->|    ISP    |->|0 |1 |2 |3 |---+                                      | Interface |
 * +--------+    |   |    |           |  |  |  |  |  |   |  +--------+  +--------+  +--------+  |           |
 * |Parallel|--->|  /     |           |  |  |  |  |  |   |  |        |  |        |  |        |  |           |
 * +--------+    |/       |           |  |  |  |  |  |   -->|  Crop  |->|  RSZ   |->|  RGB   |->|           |
 *                        |           |  |  |  |  |  |      |        |  |        |  | Rotate |  |           |
 *                        +-----------+  +--+--+--+--+      +--------+  +--------+  +--------+  +-----------+
 *                                               ^
 * +--------+                                    |          |===============================================|
 * |  DMA   |------------------------------------+                          Self Picture Path
 * +--------+
 *
 *         rkisp1-stats.c        rkisp1-params.c
 *       |===============|      |===============|
 *       +---------------+      +---------------+
 *       |               |      |               |
 *       |      ISP      |      |      ISP      |
 *       |               |      |               |
 *       +---------------+      +---------------+
 *
 *
 * Media Topology
 * --------------
 *      +----------+     +----------+
 *      | Sensor 2 |     | Sensor X |
 *      ------------ ... ------------
 *      |    0     |     |    0     |
 *      +----------+     +----------+      +-----------+
 *                  \      |               |  params   |
 *                   \     |               | (output)  |
 *    +----------+    \    |               +-----------+
 *    | Sensor 1 |     v   v                     |
 *    ------------      +------+------+          |
 *    |    0     |----->|  0   |  1   |<---------+
 *    +----------+      |------+------|
 *                      |     ISP     |
 *                      |------+------|
 *        +-------------|  2   |  3   |----------+
 *        |             +------+------+          |
 *        |                |                     |
 *        v                v                     v
 *  +- ---------+    +-----------+         +-----------+
 *  |     0     |    |     0     |         |   stats   |
 *  -------------    -------------         | (capture) |
 *  |  Resizer  |    |  Resizer  |         +-----------+
 *  ------------|    ------------|
 *  |     1     |    |     1     |
 *  +-----------+    +-----------+
 *        |                |
 *        v                v
 *  +-----------+    +-----------+
 *  | selfpath  |    | mainpath  |
 *  | (capture) |    | (capture) |
 *  +-----------+    +-----------+
 */

struct rkisp1_match_data {
	const char * const *clks;
	unsigned int size;
};

/* ----------------------------------------------------------------------------
 * Sensor DT bindings
 */

static int rkisp1_create_links(struct rkisp1_device *rkisp1)
{
	struct media_entity *source, *sink;
	unsigned int flags, source_pad;
	struct v4l2_subdev *sd;
	unsigned int i;
	int ret;

	/* sensor links */
	flags = MEDIA_LNK_FL_ENABLED;
	list_for_each_entry(sd, &rkisp1->v4l2_dev.subdevs, list) {
		if (sd == &rkisp1->isp.sd ||
		    sd == &rkisp1->resizer_devs[RKISP1_MAINPATH].sd ||
		    sd == &rkisp1->resizer_devs[RKISP1_SELFPATH].sd)
			continue;

		ret = media_entity_get_fwnode_pad(&sd->entity, sd->fwnode,
						  MEDIA_PAD_FL_SOURCE);
		if (ret) {
			dev_err(sd->dev, "failed to find src pad for %s\n",
				sd->name);
			return ret;
		}
		source_pad = ret;

		ret = media_create_pad_link(&sd->entity, source_pad,
					    &rkisp1->isp.sd.entity,
					    RKISP1_ISP_PAD_SINK_VIDEO,
					    flags);
		if (ret)
			return ret;

		flags = 0;
	}

	flags = MEDIA_LNK_FL_ENABLED;

	/* create ISP->RSZ->CAP links */
	for (i = 0; i < 2; i++) {
		source = &rkisp1->isp.sd.entity;
		sink = &rkisp1->resizer_devs[i].sd.entity;
		ret = media_create_pad_link(source, RKISP1_ISP_PAD_SOURCE_VIDEO,
					    sink, RKISP1_RSZ_PAD_SINK, flags);
		if (ret)
			return ret;

		source = sink;
		sink = &rkisp1->capture_devs[i].vnode.vdev.entity;
		ret = media_create_pad_link(source, RKISP1_RSZ_PAD_SRC,
					    sink, 0, flags);
		if (ret)
			return ret;
	}

	/* params links */
	source = &rkisp1->params.vnode.vdev.entity;
	sink = &rkisp1->isp.sd.entity;
	ret = media_create_pad_link(source, 0, sink,
				    RKISP1_ISP_PAD_SINK_PARAMS, flags);
	if (ret)
		return ret;

	/* 3A stats links */
	source = &rkisp1->isp.sd.entity;
	sink = &rkisp1->stats.vnode.vdev.entity;
	return media_create_pad_link(source, RKISP1_ISP_PAD_SOURCE_STATS,
				     sink, 0, flags);
}

static int rkisp1_subdev_notifier_bound(struct v4l2_async_notifier *notifier,
					struct v4l2_subdev *sd,
					struct v4l2_async_subdev *asd)
{
	struct rkisp1_device *rkisp1 =
		container_of(notifier, struct rkisp1_device, notifier);
	struct rkisp1_sensor_async *s_asd =
		container_of(asd, struct rkisp1_sensor_async, asd);

	s_asd->pixel_rate_ctrl = v4l2_ctrl_find(sd->ctrl_handler,
						V4L2_CID_PIXEL_RATE);
	s_asd->sd = sd;
	s_asd->dphy = devm_phy_get(rkisp1->dev, "dphy");
	if (IS_ERR(s_asd->dphy)) {
		if (PTR_ERR(s_asd->dphy) != -EPROBE_DEFER)
			dev_err(rkisp1->dev, "Couldn't get the MIPI D-PHY\n");
		return PTR_ERR(s_asd->dphy);
	}

	phy_init(s_asd->dphy);

	return 0;
}

static void rkisp1_subdev_notifier_unbind(struct v4l2_async_notifier *notifier,
					  struct v4l2_subdev *sd,
					  struct v4l2_async_subdev *asd)
{
	struct rkisp1_sensor_async *s_asd =
		container_of(asd, struct rkisp1_sensor_async, asd);

	phy_exit(s_asd->dphy);
}

static int rkisp1_subdev_notifier_complete(struct v4l2_async_notifier *notifier)
{
	struct rkisp1_device *rkisp1 =
		container_of(notifier, struct rkisp1_device, notifier);
	int ret;

	mutex_lock(&rkisp1->media_dev.graph_mutex);
	ret = rkisp1_create_links(rkisp1);
	if (ret)
		goto unlock;
	ret = v4l2_device_register_subdev_nodes(&rkisp1->v4l2_dev);
	if (ret)
		goto unlock;

	dev_dbg(rkisp1->dev, "Async subdev notifier completed\n");

unlock:
	mutex_unlock(&rkisp1->media_dev.graph_mutex);
	return ret;
}

static int rkisp1_fwnode_parse(struct device *dev,
			       struct v4l2_fwnode_endpoint *vep,
			       struct v4l2_async_subdev *asd)
{
	struct rkisp1_sensor_async *s_asd =
			container_of(asd, struct rkisp1_sensor_async, asd);

	if (vep->bus_type != V4L2_MBUS_CSI2_DPHY) {
		dev_err(dev, "Only CSI2 bus type is currently supported\n");
		return -EINVAL;
	}

	if (vep->base.port != 0) {
		dev_err(dev, "The ISP has only port 0\n");
		return -EINVAL;
	}

	s_asd->mbus.type = vep->bus_type;
	s_asd->mbus.flags = vep->bus.mipi_csi2.flags;
	s_asd->lanes = vep->bus.mipi_csi2.num_data_lanes;

	switch (vep->bus.mipi_csi2.num_data_lanes) {
	case 1:
		s_asd->mbus.flags |= V4L2_MBUS_CSI2_1_LANE;
		break;
	case 2:
		s_asd->mbus.flags |= V4L2_MBUS_CSI2_2_LANE;
		break;
	case 3:
		s_asd->mbus.flags |= V4L2_MBUS_CSI2_3_LANE;
		break;
	case 4:
		s_asd->mbus.flags |= V4L2_MBUS_CSI2_4_LANE;
		break;
	default:
		return -EINVAL;
	}

	return 0;
}

static const struct v4l2_async_notifier_operations rkisp1_subdev_notifier_ops = {
	.bound = rkisp1_subdev_notifier_bound,
	.unbind = rkisp1_subdev_notifier_unbind,
	.complete = rkisp1_subdev_notifier_complete,
};

static int rkisp1_subdev_notifier(struct rkisp1_device *rkisp1)
{
	struct v4l2_async_notifier *ntf = &rkisp1->notifier;
	struct device *dev = rkisp1->dev;
	int ret;

	v4l2_async_notifier_init(ntf);

	ret = v4l2_async_notifier_parse_fwnode_endpoints_by_port(dev, ntf,
					sizeof(struct rkisp1_sensor_async),
					0, rkisp1_fwnode_parse);
	if (ret)
		return ret;

	if (list_empty(&ntf->asd_list))
		return -ENODEV;

	ntf->ops = &rkisp1_subdev_notifier_ops;

	return v4l2_async_notifier_register(&rkisp1->v4l2_dev, ntf);
}

/* ----------------------------------------------------------------------------
 * Power
 */

static int __maybe_unused rkisp1_runtime_suspend(struct device *dev)
{
	struct rkisp1_device *rkisp1 = dev_get_drvdata(dev);

	clk_bulk_disable_unprepare(rkisp1->clk_size, rkisp1->clks);
	return pinctrl_pm_select_sleep_state(dev);
}

static int __maybe_unused rkisp1_runtime_resume(struct device *dev)
{
	struct rkisp1_device *rkisp1 = dev_get_drvdata(dev);
	int ret;

	ret = pinctrl_pm_select_default_state(dev);
	if (ret)
		return ret;
	ret = clk_bulk_prepare_enable(rkisp1->clk_size, rkisp1->clks);
	if (ret)
		return ret;

	return 0;
}

static const struct dev_pm_ops rkisp1_pm_ops = {
	SET_SYSTEM_SLEEP_PM_OPS(pm_runtime_force_suspend,
				pm_runtime_force_resume)
	SET_RUNTIME_PM_OPS(rkisp1_runtime_suspend, rkisp1_runtime_resume, NULL)
};

/* ----------------------------------------------------------------------------
 * Core
 */

static int rkisp1_entities_register(struct rkisp1_device *rkisp1)
{
	int ret;

	ret = rkisp1_isp_register(rkisp1, &rkisp1->v4l2_dev);
	if (ret)
		return ret;

	ret = rkisp1_resizer_devs_register(rkisp1);
	if (ret)
		goto err_unreg_isp_subdev;

	ret = rkisp1_capture_devs_register(rkisp1);
	if (ret)
		goto err_unreg_resizer_devs;

	ret = rkisp1_stats_register(&rkisp1->stats, &rkisp1->v4l2_dev, rkisp1);
	if (ret)
		goto err_unreg_capture_devs;

	ret = rkisp1_params_register(&rkisp1->params,
				     &rkisp1->v4l2_dev, rkisp1);
	if (ret)
		goto err_unreg_stats;

	ret = rkisp1_subdev_notifier(rkisp1);
	if (ret) {
		dev_err(rkisp1->dev,
			"Failed to register subdev notifier(%d)\n", ret);
		goto err_unreg_params;
	}

	return 0;
err_unreg_params:
	rkisp1_params_unregister(&rkisp1->params);
err_unreg_stats:
	rkisp1_stats_unregister(&rkisp1->stats);
err_unreg_capture_devs:
	rkisp1_capture_devs_unregister(rkisp1);
err_unreg_resizer_devs:
	rkisp1_resizer_devs_unregister(rkisp1);
err_unreg_isp_subdev:
	rkisp1_isp_unregister(rkisp1);
	return ret;
}

irqreturn_t rkisp1_isr(int irq, void *ctx)
{
	struct device *dev = ctx;
	struct rkisp1_device *rkisp1 = dev_get_drvdata(dev);

	/*
	 * Call rkisp1_capture_isr() first to handle the frame that
	 * potentially completed using the current frame_sequence number before
	 * it is potentially incremented by rkisp1_isp_isr() in the vertical
	 * sync.
	 */
	if (rkisp1_capture_isr(rkisp1) == IRQ_NONE &&
	    rkisp1_isp_isr(rkisp1) == IRQ_NONE &&
	    rkisp1_mipi_isr(rkisp1) == IRQ_NONE)
		return IRQ_NONE;

	return IRQ_HANDLED;
}

static const char * const rk3399_isp_clks[] = {
	"clk_isp",
	"aclk_isp",
	"hclk_isp",
	"aclk_isp_wrap",
	"hclk_isp_wrap",
};

static const struct rkisp1_match_data rk3399_isp_clk_data = {
	.clks = rk3399_isp_clks,
	.size = ARRAY_SIZE(rk3399_isp_clks),
};

static const struct of_device_id rkisp1_of_match[] = {
	{
		.compatible = "rockchip,rk3399-cif-isp",
		.data = &rk3399_isp_clk_data,
	},
	{},
};
MODULE_DEVICE_TABLE(of, rkisp1_of_match);

static void rkisp1_debug_init(struct rkisp1_device *rkisp1)
{
	struct rkisp1_debug *debug = &rkisp1->debug;

	debug->debugfs_dir = debugfs_create_dir(RKISP1_DRIVER_NAME, NULL);
	if (!debug->debugfs_dir) {
		dev_dbg(rkisp1->dev, "failed to create debugfs directory\n");
		return;
	}
	debugfs_create_ulong("data_loss", 0444, debug->debugfs_dir,
			     &debug->data_loss);
	debugfs_create_ulong("pic_size_error", 0444,  debug->debugfs_dir,
			     &debug->pic_size_error);
	debugfs_create_ulong("mipi_error", 0444, debug->debugfs_dir,
			     &debug->mipi_error);
	debugfs_create_ulong("stats_error", 0444, debug->debugfs_dir,
			     &debug->stats_error);
	debugfs_create_ulong("mp_stop_timeout", 0444, debug->debugfs_dir,
			     &debug->stop_timeout[RKISP1_MAINPATH]);
	debugfs_create_ulong("sp_stop_timeout", 0444, debug->debugfs_dir,
			     &debug->stop_timeout[RKISP1_SELFPATH]);
	debugfs_create_ulong("mp_frame_drop", 0444, debug->debugfs_dir,
			     &debug->frame_drop[RKISP1_MAINPATH]);
	debugfs_create_ulong("sp_frame_drop", 0444, debug->debugfs_dir,
			     &debug->frame_drop[RKISP1_SELFPATH]);
}

static int rkisp1_probe(struct platform_device *pdev)
{
	struct device_node *node = pdev->dev.of_node;
	const struct rkisp1_match_data *clk_data;
	const struct of_device_id *match;
	struct device *dev = &pdev->dev;
	struct rkisp1_device *rkisp1;
	struct v4l2_device *v4l2_dev;
	unsigned int i;
	int ret, irq;

	match = of_match_node(rkisp1_of_match, node);
	rkisp1 = devm_kzalloc(dev, sizeof(*rkisp1), GFP_KERNEL);
	if (!rkisp1)
		return -ENOMEM;

	dev_set_drvdata(dev, rkisp1);
	rkisp1->dev = dev;

	rkisp1->base_addr = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(rkisp1->base_addr))
		return PTR_ERR(rkisp1->base_addr);

	irq = platform_get_irq(pdev, 0);
	if (irq < 0)
		return irq;

	ret = devm_request_irq(dev, irq, rkisp1_isr, IRQF_SHARED,
			       dev_driver_string(dev), dev);
	if (ret) {
		dev_err(dev, "request irq failed: %d\n", ret);
		return ret;
	}

	rkisp1->irq = irq;
	clk_data = match->data;

	for (i = 0; i < clk_data->size; i++)
		rkisp1->clks[i].id = clk_data->clks[i];
	ret = devm_clk_bulk_get(dev, clk_data->size, rkisp1->clks);
	if (ret)
		return ret;
	rkisp1->clk_size = clk_data->size;

	pm_runtime_enable(&pdev->dev);

	strscpy(rkisp1->media_dev.model, RKISP1_DRIVER_NAME,
		sizeof(rkisp1->media_dev.model));
	rkisp1->media_dev.dev = &pdev->dev;
	strscpy(rkisp1->media_dev.bus_info, RKISP1_BUS_INFO,
		sizeof(rkisp1->media_dev.bus_info));
	media_device_init(&rkisp1->media_dev);

	v4l2_dev = &rkisp1->v4l2_dev;
	v4l2_dev->mdev = &rkisp1->media_dev;
	strscpy(v4l2_dev->name, RKISP1_DRIVER_NAME, sizeof(v4l2_dev->name));

	ret = v4l2_device_register(rkisp1->dev, &rkisp1->v4l2_dev);
	if (ret)
		return ret;

	ret = media_device_register(&rkisp1->media_dev);
	if (ret) {
		dev_err(dev, "Failed to register media device: %d\n", ret);
		goto err_unreg_v4l2_dev;
	}

	ret = rkisp1_entities_register(rkisp1);
	if (ret)
		goto err_unreg_media_dev;

	rkisp1_debug_init(rkisp1);

	return 0;

err_unreg_media_dev:
	media_device_unregister(&rkisp1->media_dev);
err_unreg_v4l2_dev:
	v4l2_device_unregister(&rkisp1->v4l2_dev);
	pm_runtime_disable(&pdev->dev);
	return ret;
}

static int rkisp1_remove(struct platform_device *pdev)
{
	struct rkisp1_device *rkisp1 = platform_get_drvdata(pdev);

	v4l2_async_notifier_unregister(&rkisp1->notifier);
	v4l2_async_notifier_cleanup(&rkisp1->notifier);

	rkisp1_params_unregister(&rkisp1->params);
	rkisp1_stats_unregister(&rkisp1->stats);
	rkisp1_capture_devs_unregister(rkisp1);
	rkisp1_resizer_devs_unregister(rkisp1);
	rkisp1_isp_unregister(rkisp1);

	media_device_unregister(&rkisp1->media_dev);
	v4l2_device_unregister(&rkisp1->v4l2_dev);

	pm_runtime_disable(&pdev->dev);

	debugfs_remove_recursive(rkisp1->debug.debugfs_dir);
	return 0;
}

static struct platform_driver rkisp1_drv = {
	.driver = {
		.name = RKISP1_DRIVER_NAME,
		.of_match_table = of_match_ptr(rkisp1_of_match),
		.pm = &rkisp1_pm_ops,
	},
	.probe = rkisp1_probe,
	.remove = rkisp1_remove,
};

module_platform_driver(rkisp1_drv);
MODULE_DESCRIPTION("Rockchip ISP1 platform driver");
MODULE_LICENSE("Dual MIT/GPL");
