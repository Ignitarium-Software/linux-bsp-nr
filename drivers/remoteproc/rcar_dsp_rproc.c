// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2026 Renesas Electronics Corporation
 */

#include <linux/limits.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/of_device.h>
#include <linux/of_reserved_mem.h>
#include <linux/remoteproc.h>
#include <linux/delay.h>
#include <linux/platform_device.h>
#include <linux/interrupt.h>
#include <linux/dma-mapping.h>
#include <linux/dma-direct.h>

#include "remoteproc_internal.h"

#define INFO_OFFSET(n) (0x0200 + n * 0x4)
#define MIRROR_OFFSET(n) (0x0280 + n * 0x4)

struct rcar_rproc {
	struct rproc *rproc;
	struct work_struct workqueue;
	void __iomem *base;
	struct atomic_notifier_head notifier_head;
	void *notifier_data;
};

static int rcar_dsp_rproc_mem_alloc(struct rproc *rproc,
		struct rproc_mem_entry *mem)
{
	struct device *dev = &rproc->dev;
	void *va;

	dev_info(dev, "map memory: %pa+%zx\n", &mem->dma, mem->len);
	va = ioremap_wc(mem->dma, mem->len);
	if (!va) {
		dev_err(dev, "Unable to map memory region: %pa+%zx\n",
			&mem->dma, mem->len);
		return -ENOMEM;
	}

	/* Update memory entry va */
	mem->va = va;

	return 0;
}

static int rcar_dsp_rproc_mem_release(struct rproc *rproc,
		struct rproc_mem_entry *mem)
{
	dev_info(&rproc->dev, "unmap memory: %pa\n", &mem->dma);
	iounmap(mem->va);

	return 0;
}

static void handle_event(struct work_struct *work)
{
	struct rcar_rproc *priv =
		container_of(work, struct rcar_rproc, workqueue);

	rproc_vq_interrupt(priv->rproc, 0);
	rproc_vq_interrupt(priv->rproc, 1);
}

static int rcar_dsp_rproc_prepare(struct rproc *rproc)
{
	struct device *dev = rproc->dev.parent;
	struct device_node *np = dev->of_node;
	struct of_phandle_iterator it;
	struct rproc_mem_entry *mem;
	struct reserved_mem *rmem;
	u32 da;

	/* Register associated reserved carveout regions */
	of_phandle_iterator_init(&it, np, "carveout-region", NULL, 0);
	while (of_phandle_iterator_next(&it) == 0) {
		rmem = of_reserved_mem_lookup(it.node);
		if (!rmem) {
			of_node_put(it.node);
			dev_err(&rproc->dev,
				"unable to acquire carveout-region\n");
			return -EINVAL;
		}

		if (rmem->base > U32_MAX) {
			of_node_put(it.node);
			return -EINVAL;
		}

		dev_info(&rproc->dev, "%s: base(0x%llx), size(0x%llx).\n",
				__func__, rmem->base, rmem->size);

		/* No need to translate pa to da, R-Car use same map */
		da = rmem->base;
		mem = rproc_mem_entry_init(dev, NULL,
					   rmem->base,
					   rmem->size, da,
					   rcar_dsp_rproc_mem_alloc,
					   rcar_dsp_rproc_mem_release,
					   it.node->name);

		if (!mem) {
			of_node_put(it.node);
			return -ENOMEM;
		}

		rproc_add_carveout(rproc, mem);
	}

	return 0;
}

static int rcar_dsp_rproc_parse_fw(struct rproc *rproc,
		const struct firmware *fw)
{
	int ret;

	ret = rproc_elf_load_rsc_table(rproc, fw);
	if (ret)
		dev_info(&rproc->dev, "No resource table in elf\n");

	return 0;
}

static int rcar_dsp_rproc_start(struct rproc *rproc)
{
	if (!rproc->bootaddr)
		return -EINVAL;

	return 0;
}

static int rcar_dsp_rproc_stop(struct rproc *rproc)
{
	return 0;
}

static int rcar_dsp_trigger_interrupt(struct rcar_rproc *priv,
		int count, int id)
{
	uint8_t *addr = priv->base + INFO_OFFSET(0);
	uint8_t *mirror = priv->base + MIRROR_OFFSET(0);
	uint32_t value;

	value = ioread32(mirror);
	if ((value >> 8) & 0xFF)
		return -EBUSY;

	value = (1 << 17) | (1 << 16) | (count << 8) | id;
	iowrite32(value, addr);
	return 0;
}

static void rcar_dsp_rproc_kick(struct rproc *rproc, int vqid)
{
	struct device *dev = rproc->dev.parent;
	struct rcar_rproc *priv = rproc->priv;
	unsigned int n_tries = 3;
	int ret;

	do {
		ret = rcar_dsp_trigger_interrupt(priv, 1, 1);
		if (ret)
			usleep_range(500, 510);

	} while (ret && n_tries--);

	if (ret)
		dev_info(dev, "%s failed\n", __func__);
}

static int rcar_dsp_rproc_elf_load_segments(struct rproc *rproc,
		const struct firmware *fw)
{
	return 0;
}

static struct resource_table *rcar_dsp_rproc_elf_find_loaded_rsc_table(
		struct rproc *rproc, const struct firmware *fw)
{
	struct resource_table *table;

	table = rproc_elf_find_loaded_rsc_table(rproc, fw);
	return table;
}

static struct rproc_ops rcar_rproc_ops = {
	.prepare		= rcar_dsp_rproc_prepare,
	.start			= rcar_dsp_rproc_start,
	.stop			= rcar_dsp_rproc_stop,
	.kick			= rcar_dsp_rproc_kick,
	.load			= rcar_dsp_rproc_elf_load_segments,
	.parse_fw		= rcar_dsp_rproc_parse_fw,
	.find_loaded_rsc_table	= rcar_dsp_rproc_elf_find_loaded_rsc_table,
	.sanity_check		= rproc_elf_sanity_check,
	.get_boot_addr		= rproc_elf_get_boot_addr,

};

static irqreturn_t dsp_irq_handler(int irq, void *data)
{
	struct rcar_rproc *priv = (struct rcar_rproc *)data;

	schedule_work(&priv->workqueue);
	return IRQ_HANDLED;
}

static int rcar_dsp_rproc_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct device_node *np = dev->of_node;
	struct rcar_rproc *priv;
	struct rproc *rproc;
	struct resource *res;
	int ret, irq;

	rproc = devm_rproc_alloc(dev, np->name, &rcar_rproc_ops,
			NULL, sizeof(*priv));
	if (!rproc)
		return -ENOMEM;

	priv = rproc->priv;
	dev_set_drvdata(dev, rproc);

	priv->rproc = rproc;
	INIT_WORK(&priv->workqueue, handle_event);

	/* Attach this device to the reserved-memory pool from DT */
	ret = of_reserved_mem_device_init(dev);
	if (ret) {
		dev_err(dev, "of_reserved_mem_device_init failed: %d\n", ret);
		goto flush_wq;
	}

	ret = dma_set_coherent_mask(dev, DMA_BIT_MASK(32));
	if (ret) {
		dev_err(dev, "dma_set_coherent_mask failed: %d\n", ret);
		goto release_reserved_mem;
	}

	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "ch0");
	if (!res) {
		dev_err(dev, "Failed to get base address.\n");
		ret = -EINVAL;
		goto release_reserved_mem;
	}

	priv->base = devm_ioremap_resource(dev, res);
	if (!priv->base) {
		dev_err(dev, "failed to map IO memory.\n.");
		ret = -ENOMEM;
		goto release_reserved_mem;
	}

	/* Get IRQ resource */
	irq = platform_get_irq(pdev, 0);
	if (!irq) {
		dev_err(dev, "missing IRQ.\n");
		ret = -EINVAL;
		goto release_reserved_mem;
	}

	ret = devm_request_irq(dev, irq, dsp_irq_handler, IRQF_SHARED,
			"dsp-doorbell", priv);
	if (ret < 0) {
		dev_err(dev, "failed to request IRQ.\n");
		ret = -EINVAL;
		goto release_reserved_mem;
	}

	/* Manually start the rproc */
	rproc->auto_boot = false;

	ret = devm_rproc_add(dev, rproc);
	if (ret) {
		dev_err(dev, "rproc_add failed\n");
		goto release_reserved_mem;
	}

	return 0;

release_reserved_mem:
	of_reserved_mem_device_release(dev);
flush_wq:
	flush_work(&priv->workqueue);

	return ret;
}

static int rcar_dsp_rproc_remove(struct platform_device *pdev)
{
	struct rcar_rproc *priv =  platform_get_drvdata(pdev);
	struct device *dev = &pdev->dev;

	of_reserved_mem_device_release(dev);
	flush_work(&priv->workqueue);
	rproc_del(priv->rproc);

	return 0;
}

static const struct of_device_id rcar_dsp_rproc_of_match[] = {
	{ .compatible = "renesas,rcar-dsp-rproc" },
	{},
};

MODULE_DEVICE_TABLE(of, rcar_dsp_rproc_of_match);

static struct platform_driver rcar_dsp_rproc_driver = {
	.probe = rcar_dsp_rproc_probe,
	.remove = rcar_dsp_rproc_remove,
	.driver = {
		.name = "rcar-dsp-rproc",
		.of_match_table = rcar_dsp_rproc_of_match,
	},
};

module_platform_driver(rcar_dsp_rproc_driver);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Renesas R-Car DSP remote processor control driver");
