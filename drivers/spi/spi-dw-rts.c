/*
 * SSI interface driver for DW SPI Core
 *
 * Copyright (c) 2018, Realtek semiconductor.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2, as published by the Free Software Foundation.
 */

#include <linux/clk.h>
#include <linux/err.h>
#include <linux/interrupt.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/spi/spi.h>
#include <linux/scatterlist.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_gpio.h>
#include <linux/of_platform.h>
#include <linux/property.h>
#include <linux/dma-mapping.h>

#include "spi-dw.h"

#define DRIVER_NAME "dw_spi_rts"

struct dw_spi_rts {
	struct dw_spi  dws;
	struct clk     *clk;
};


static int rts_spi_dma_init(struct dw_spi *dws)
{
	dws->dma_inited = 1;
	return 0;
}

static void rts_spi_dma_exit(struct dw_spi *dws) {}

static int rts_spi_dma_setup(struct dw_spi *dws, struct spi_transfer *xfer)
{
	struct device dev;

	dev = dws->master->dev;
	if (xfer->tx_buf) {
		/* set TX addr */
		dws->dma_tx = (void *)dma_map_single(&dev, (void *)xfer->tx_buf, /// streaming dma mapping. xfer->tx_buf:虚拟地址 dws->dma_tx:总线地址
			xfer->len, DMA_FROM_DEVICE); /// ??? 应该是DMA_TO_DEVICE
		dw_writel(dws, SSI_RD_ADDR, (u32)dws->dma_tx);
	}
	if (xfer->rx_buf) {
		/* set RX addr */
		dws->dma_rx = (void *)dma_map_single(&dev, xfer->rx_buf, /// ??? 只有dma_map_single, 没有dma_unmap_single? 有些平台dma_unmap_single定义为NULL，这边不清楚。
			xfer->len, DMA_FROM_DEVICE);
		dw_writel(dws, SSI_WR_ADDR, (u32)dws->dma_rx);
	}
	dw_writel(dws, SSI_DATA_LEN, xfer->len);

	/* Set the interrupt mask */
	dw_writel(dws, SSI_IRQ_ENABLE, DONE_INT_EN); ///enable transfer done interrupt

	return 0;
}

static bool rts_spi_can_dma(struct spi_controller *master,
		struct spi_device *spi, struct spi_transfer *xfer)
{
	struct dw_spi *dws = spi_controller_get_devdata(master);

	if (!dws->dma_inited)
		return false;

	return xfer->len > dws->fifo_len;
}

static int rts_spi_dma_transfer(struct dw_spi *dws, struct spi_transfer *xfer)
{
	/* start */
	dw_writel(dws, SSI_START, SPI_SSI_START);

	return 1;
}

static void rts_spi_dma_stop(struct dw_spi *dws)
{
	/* stop */
	dw_writel(dws, SSI_STOP, SPI_SSI_STOP);
}

static const struct dw_spi_dma_ops rts_dma_ops = {
	.dma_init	= rts_spi_dma_init,
	.dma_exit	= rts_spi_dma_exit,
	.dma_setup	= rts_spi_dma_setup,
	.can_dma	= rts_spi_can_dma,
	.dma_transfer	= rts_spi_dma_transfer,
	.dma_stop	= rts_spi_dma_stop,
};

static int dw_spi_rts_probe(struct platform_device *pdev)
{
	struct dw_spi_rts *dwsrts;
	struct dw_spi *dws;
	struct resource *mem, *mem1;
	int ret;
	int num_cs;

	dwsrts = devm_kzalloc(&pdev->dev, sizeof(struct dw_spi_rts),
			GFP_KERNEL);
	if (!dwsrts)
		return -ENOMEM;

	dws = &dwsrts->dws;

	/* Get basic io resource and map it */
	mem = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!mem) {
		dev_err(&pdev->dev, "get resource failed!\n");
		return -EINVAL;
	}

	mem = devm_request_mem_region(&pdev->dev,
		mem->start, resource_size(mem), pdev->name);
	if (!mem) {
		dev_err(&pdev->dev, "request memory region failed!\n");
		return -ENOMEM;
	}

	dws->regs = devm_ioremap(&pdev->dev, mem->start, resource_size(mem));
	if (!dws->regs) {
		dev_err(&pdev->dev, "ioremap failed, phy address:%x\n",
				mem->start);
		return -ENOMEM;
	}

	dev_dbg(&pdev->dev, "Realtek SSI Controller at 0x%08lx phy address %x\n",
			(unsigned long)dws->regs, mem->start);

	/* Get basic io resource and map it */
	mem1 = platform_get_resource(pdev, IORESOURCE_MEM, 1);

	mem1 = devm_request_mem_region(&pdev->dev,
		mem1->start, resource_size(mem1), pdev->name);
	if (!mem1) {
		dev_err(&pdev->dev, "request memory1 region failed!\n");
		return -ENOMEM;
	}
	dws->reg_pad = devm_ioremap(&pdev->dev, mem1->start,
			resource_size(mem1));
	if (!dws->reg_pad) {
		dev_err(&pdev->dev, "ioremap failed, phy address:%x\n",
				mem1->start);
		return -ENOMEM;
	}

	dev_dbg(&pdev->dev, "Realtek SSI pad Controller at 0x%08lx phy address %x\n",
			(unsigned long)dws->reg_pad, mem1->start);

	dws->irq = platform_get_irq(pdev, 0);
	if (dws->irq < 0) {
		dev_dbg(&pdev->dev, "no irq resource?\n");
		return dws->irq; /* -ENXIO */
	}

	dwsrts->clk = devm_clk_get(&pdev->dev, "ssi_ck");
	if (IS_ERR(dwsrts->clk))
		return PTR_ERR(dwsrts->clk);
	ret = clk_prepare_enable(dwsrts->clk);
	if (ret)
		return ret;

	dws->bus_num = pdev->id;

	dws->max_freq = clk_get_rate(dwsrts->clk);

	device_property_read_u32(&pdev->dev, "reg-io-width", /// dts里没有reg-io-width
		&dws->reg_io_width);

	num_cs = 4;

	device_property_read_u32(&pdev->dev, "num-cs", &num_cs); /// dts num-cs = <2>; cs片选引脚支持几路。

	dws->num_cs = num_cs;

	dws->dma_ops = &rts_dma_ops;

	device_property_read_u32(&pdev->dev, "dma_transfer", &dws->dma_mapped); /// dma_transfer = <1>;

	ret = dw_spi_add_host(&pdev->dev, dws); /// 进入spi-dw.c
	if (ret)
		goto out;

	platform_set_drvdata(pdev, dwsrts);
	dev_dbg(&pdev->dev, "Realtek SSI Controller at 0x%08lx (irq %d)\n",
			(unsigned long)mem->start, dws->irq);
	return 0;

out:
	clk_disable_unprepare(dwsrts->clk);
	return ret;
}

static int dw_spi_rts_remove(struct platform_device *pdev)
{
	struct dw_spi_rts *dwsrts = platform_get_drvdata(pdev);

	clk_disable_unprepare(dwsrts->clk);
	dw_spi_remove_host(&dwsrts->dws);

	return 0;
}

static const struct of_device_id dw_spi_rts_of_match[] = {
	{ .compatible = "realtek,dw-apb-ssi", },
	{ /* end of table */}
};
MODULE_DEVICE_TABLE(of, dw_spi_rts_of_match);

static struct platform_driver dw_spi_rts_driver = {
	.probe		= dw_spi_rts_probe,
	.remove		= dw_spi_rts_remove,
	.driver		= {
		.name	= DRIVER_NAME,
		.of_match_table = dw_spi_rts_of_match,
	},
};
module_platform_driver(dw_spi_rts_driver);

MODULE_AUTHOR("Keent <keent_zhuo@realsil.com.cn>");
MODULE_DESCRIPTION("SSI interface driver for DW SPI Core");
MODULE_LICENSE("GPL v2");
