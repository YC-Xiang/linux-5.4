// SPDX-License-Identifier: GPL-2.0
/*
 * Synopsys DesignWare I2C adapter driver (slave only).
 *
 * Based on the Synopsys DesignWare I2C adapter driver (master).
 *
 * Copyright (C) 2016 Synopsys Inc.
 */
#define DEBUG
#include <linux/delay.h>
#include <linux/err.h>
#include <linux/errno.h>
#include <linux/i2c.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/pm_runtime.h>
#include <linux/regmap.h>

#include "i2c-designware-core.h"

static void i2c_dw_configure_fifo_slave(struct dw_i2c_dev *dev)
{
	/* Configure Tx/Rx FIFO threshold levels. */
	regmap_write(dev->map, DW_IC_TX_TL, 0);
	regmap_write(dev->map, DW_IC_RX_TL, 0);

	/* Configure the I2C slave. */
	regmap_write(dev->map, DW_IC_CON, dev->slave_cfg);
	regmap_write(dev->map, DW_IC_INTR_MASK, DW_IC_INTR_SLAVE_MASK);
}

/**
 * i2c_dw_init_slave() - Initialize the designware i2c slave hardware
 * @dev: device private data
 *
 * This function configures and enables the I2C in slave mode.
 * This function is called during I2C init function, and in case of timeout at
 * run time.
 */
static int i2c_dw_init_slave(struct dw_i2c_dev *dev)
{
	int ret;

	ret = i2c_dw_acquire_lock(dev);
	if (ret)
		return ret;

	/* Disable the adapter. */
	__i2c_dw_disable(dev);

	/* Write SDA hold time if supported */
	if (dev->sda_hold_time)
		regmap_write(dev->map, DW_IC_SDA_HOLD, dev->sda_hold_time);

	regmap_write(dev->map, DW_IC_SDA_HOLD, 20);

	i2c_dw_configure_fifo_slave(dev);
	i2c_dw_release_lock(dev);

	return 0;
}

static int i2c_dw_reg_slave(struct i2c_client *slave)
{
	struct dw_i2c_dev *dev = i2c_get_adapdata(slave->adapter);

	if (dev->slave)
		return -EBUSY;
	if (slave->flags & I2C_CLIENT_TEN)
		return -EAFNOSUPPORT;
	pm_runtime_get_sync(dev->dev);

	/*
	 * Set slave address in the IC_SAR register,
	 * the address to which the DW_apb_i2c responds.
	 */
	__i2c_dw_disable_nowait(dev);
	regmap_write(dev->map, DW_IC_SAR, slave->addr);
	dev->slave = slave;

	__i2c_dw_enable(dev);

	dev->cmd_err = 0;
	dev->msg_write_idx = 0;
	dev->msg_read_idx = 0;
	dev->msg_err = 0;
	dev->status = STATUS_IDLE;
	dev->abort_source = 0;
	dev->rx_outstanding = 0;

	return 0;
}

static int i2c_dw_unreg_slave(struct i2c_client *slave)
{
	struct dw_i2c_dev *dev = i2c_get_adapdata(slave->adapter);

	dev->disable_int(dev);
	dev->disable(dev);
	synchronize_irq(dev->irq);
	dev->slave = NULL;
	pm_runtime_put(dev->dev);

	return 0;
}

static u32 i2c_dw_read_clear_intrbits_slave(struct dw_i2c_dev *dev)
{
	u32 stat, dummy;

	/*
	 * The IC_INTR_STAT register just indicates "enabled" interrupts.
	 * The unmasked raw version of interrupt status bits is available
	 * in the IC_RAW_INTR_STAT register.
	 *
	 * That is,
	 *   stat = readl(IC_INTR_STAT);
	 * equals to,
	 *   stat = readl(IC_RAW_INTR_STAT) & readl(IC_INTR_MASK);
	 *
	 * The raw version might be useful for debugging purposes.
	 */
	regmap_read(dev->map, DW_IC_INTR_STAT, &stat);

	/*
	 * Do not use the IC_CLR_INTR register to clear interrupts, or
	 * you'll miss some interrupts, triggered during the period from
	 * readl(IC_INTR_STAT) to readl(IC_CLR_INTR).
	 *
	 * Instead, use the separately-prepared IC_CLR_* registers.
	 */
	if (stat & DW_IC_INTR_TX_ABRT)
		regmap_read(dev->map, DW_IC_CLR_TX_ABRT, &dummy);
	if (stat & DW_IC_INTR_RX_UNDER)
		regmap_read(dev->map, DW_IC_CLR_RX_UNDER, &dummy);
	if (stat & DW_IC_INTR_RX_OVER)
		regmap_read(dev->map, DW_IC_CLR_RX_OVER, &dummy);
	if (stat & DW_IC_INTR_TX_OVER)
		regmap_read(dev->map, DW_IC_CLR_TX_OVER, &dummy);
	if (stat & DW_IC_INTR_RX_DONE)
		regmap_read(dev->map, DW_IC_CLR_RX_DONE, &dummy);
	if (stat & DW_IC_INTR_ACTIVITY)
		regmap_read(dev->map, DW_IC_CLR_ACTIVITY, &dummy);
	if (stat & DW_IC_INTR_STOP_DET)
		regmap_read(dev->map, DW_IC_CLR_STOP_DET, &dummy);
	if (stat & DW_IC_INTR_START_DET)
		regmap_read(dev->map, DW_IC_CLR_START_DET, &dummy);
	if (stat & DW_IC_INTR_GEN_CALL)
		regmap_read(dev->map, DW_IC_CLR_GEN_CALL, &dummy);

	return stat;
}

/*
 * Interrupt service routine. This gets called whenever an I2C slave interrupt
 * occurs.
 */

/*
	testi2c_device.c，抓取log。

	写12bytes
	INTR_STAT=0x4 DW_IC_INTR_RX_FULL /// master write，触发rx_full中断，在这个中断里处理完所有写操作
	INTR_STAT=0x200 DW_IC_INTR_STOP_DET /// master发出stop信号

	读12bytes
	INTR_STAT=0x224 DW_IC_INTR_RX_FULL DW_IC_INTR_STOP_DET DW_IC_INTR_RD_REQ 这里不知道为什么DW_IC_INTR_RX_FULL DW_IC_INTR_STOP_DET没被清掉，好像不清掉也没影响
	INTR_STAT=0x20
	INTR_STAT=0x20
	INTR_STAT=0x20
	INTR_STAT=0x20
	INTR_STAT=0x20
	INTR_STAT=0x20
	INTR_STAT=0x20
	INTR_STAT=0x20
	INTR_STAT=0x20
	INTR_STAT=0x20
	INTR_STAT=0x20

	写12bytes
	INTR_STAT=0x200 DW_IC_INTR_STOP_DET
	INTR_STAT=0x204 DW_IC_INTR_STOP_DET DW_IC_INTR_RX_FULL

	读12bytes
	INTR_STAT=0x224
	INTR_STAT=0x20
	INTR_STAT=0x20
	INTR_STAT=0x20
	INTR_STAT=0x20
	INTR_STAT=0x20
	INTR_STAT=0x20
	INTR_STAT=0x20
	INTR_STAT=0x20
	INTR_STAT=0x20
	INTR_STAT=0x20
	INTR_STAT=0x20

	写12bytes
	INTR_STAT=0x200
	INTR_STAT=0x204
	...

	代码流程:
	0x04中断：
	if (stat & DW_IC_INTR_RX_FULL) {
		if (!(dev->status & STATUS_WRITE_IN_PROGRESS)) {
			dev->status |= STATUS_WRITE_IN_PROGRESS;
			dev->status &= ~STATUS_READ_IN_PROGRESS;
			i2c_slave_event(dev->slave, I2C_SLAVE_WRITE_REQUESTED,
					&val);
		}

		do {
			regmap_read(dev->map, DW_IC_DATA_CMD, &tmp);
			val = tmp;
			i2c_slave_event(dev->slave, I2C_SLAVE_WRITE_RECEIVED,
					&val);
			regmap_read(dev->map, DW_IC_STATUS, &tmp);
		} while (tmp & DW_IC_STATUS_RFNE);
	}

	0x200:
	if (stat & DW_IC_INTR_STOP_DET)
		i2c_slave_event(dev->slave, I2C_SLAVE_STOP, &val); /// eeprom->idx_write_cnt = 0;

	0x204:
	下面
	// if (stat & DW_IC_INTR_RX_FULL) {
	// 	if (!(dev->status & STATUS_WRITE_IN_PROGRESS)) {/// dev->status & STATUS_WRITE_IN_PROGRESS == 1 跳过
	// 		dev->status |= STATUS_WRITE_IN_PROGRESS;
	// 		dev->status &= ~STATUS_READ_IN_PROGRESS;
	// 		i2c_slave_event(dev->slave, I2C_SLAVE_WRITE_REQUESTED,
	// 				&val);
	// 	}

	// 	do {
	// 		regmap_read(dev->map, DW_IC_DATA_CMD, &tmp); /// tmp=0?
	// 		val = tmp;
	// 		i2c_slave_event(dev->slave, I2C_SLAVE_WRITE_RECEIVED,
	// 				&val);
	// 		regmap_read(dev->map, DW_IC_STATUS, &tmp);
	// 	} while (tmp & DW_IC_STATUS_RFNE);
	// }

	第一个0x20:
	if (stat & DW_IC_INTR_RD_REQ) { /// master read slave，threshold为0，触发中断0x20
		if (slave_activity) {
			regmap_read(dev->map, DW_IC_CLR_RD_REQ, &tmp);

			if (!(dev->status & STATUS_READ_IN_PROGRESS)) {
				i2c_slave_event(dev->slave,
						I2C_SLAVE_READ_REQUESTED,
						&val);
				dev->status |= STATUS_READ_IN_PROGRESS;
				dev->status &= ~STATUS_WRITE_IN_PROGRESS;
			}
			regmap_write(dev->map, DW_IC_DATA_CMD, val);
		}
	}

	第二个0x20:
	if (slave_activity) {
			regmap_read(dev->map, DW_IC_CLR_RD_REQ, &tmp);
			else {
				i2c_slave_event(dev->slave,
						I2C_SLAVE_READ_PROCESSED,
						&val);
			}
			regmap_write(dev->map, DW_IC_DATA_CMD, val);
		}
*/

static int i2c_dw_irq_handler_slave(struct dw_i2c_dev *dev)
{
	u32 raw_stat, stat, enabled, tmp;
	u8 val = 0, slave_activity;

	regmap_read(dev->map, DW_IC_ENABLE, &enabled);
	regmap_read(dev->map, DW_IC_RAW_INTR_STAT, &raw_stat);
	regmap_read(dev->map, DW_IC_STATUS, &tmp);
	slave_activity = ((tmp & DW_IC_STATUS_SLAVE_ACTIVITY) >> 6);

	if (!enabled || !(raw_stat & ~DW_IC_INTR_ACTIVITY) || !dev->slave)
		return IRQ_NONE;

	stat = i2c_dw_read_clear_intrbits_slave(dev);
	dev_dbg(dev->dev,
		"%#x STATUS SLAVE_ACTIVITY=%#x : RAW_INTR_STAT=%#x : INTR_STAT=%#x\n",
		enabled, slave_activity, raw_stat, stat);

	if (stat & DW_IC_INTR_RX_FULL) {  /// master write slave，threshold为0，触发中断0x20
		if (!(dev->status & STATUS_WRITE_IN_PROGRESS)) {
			dev->status |= STATUS_WRITE_IN_PROGRESS;
			dev->status &= ~STATUS_READ_IN_PROGRESS;
			i2c_slave_event(dev->slave, I2C_SLAVE_WRITE_REQUESTED,
					&val);
		}

		do {
			regmap_read(dev->map, DW_IC_DATA_CMD, &tmp);
			val = tmp;
			i2c_slave_event(dev->slave, I2C_SLAVE_WRITE_RECEIVED,
					&val);
			regmap_read(dev->map, DW_IC_STATUS, &tmp);
		} while (tmp & DW_IC_STATUS_RFNE); /// rx fifo不是空的就一直读完
	}

	if (stat & DW_IC_INTR_RD_REQ) { /// master read slave，threshold为0，触发中断0x20
		if (slave_activity) {
			regmap_read(dev->map, DW_IC_CLR_RD_REQ, &tmp);

			if (!(dev->status & STATUS_READ_IN_PROGRESS)) {
				i2c_slave_event(dev->slave,
						I2C_SLAVE_READ_REQUESTED,
						&val);
				dev->status |= STATUS_READ_IN_PROGRESS;
				dev->status &= ~STATUS_WRITE_IN_PROGRESS;
			} else {
				i2c_slave_event(dev->slave,
						I2C_SLAVE_READ_PROCESSED,
						&val);
			}
			regmap_write(dev->map, DW_IC_DATA_CMD, val);
		}
	}

	if (stat & DW_IC_INTR_STOP_DET)
		i2c_slave_event(dev->slave, I2C_SLAVE_STOP, &val);

	return IRQ_HANDLED;
}

static irqreturn_t i2c_dw_isr_slave(int this_irq, void *dev_id)
{
	struct dw_i2c_dev *dev = dev_id;
	int ret;

	ret = i2c_dw_irq_handler_slave(dev);
	if (ret > 0)
		complete(&dev->cmd_complete);

	return IRQ_RETVAL(ret);
}

static const struct i2c_algorithm i2c_dw_algo = {
	.functionality = i2c_dw_func,
	.reg_slave = i2c_dw_reg_slave,
	.unreg_slave = i2c_dw_unreg_slave,
};

void i2c_dw_configure_slave(struct dw_i2c_dev *dev)
{
	dev->functionality = I2C_FUNC_SLAVE | DW_IC_DEFAULT_FUNCTIONALITY;

	dev->slave_cfg = DW_IC_CON_RX_FIFO_FULL_HLD_CTRL |
			 DW_IC_CON_RESTART_EN | DW_IC_CON_STOP_DET_IFADDRESSED;

	dev->mode = DW_IC_SLAVE;
}
EXPORT_SYMBOL_GPL(i2c_dw_configure_slave);

int i2c_dw_probe_slave(struct dw_i2c_dev *dev)
{
	struct i2c_adapter *adap = &dev->adapter;
	int ret;

	init_completion(&dev->cmd_complete);

	dev->init = i2c_dw_init_slave;
	dev->disable = i2c_dw_disable;
	dev->disable_int = i2c_dw_disable_int;

	ret = i2c_dw_init_regmap(dev);
	if (ret)
		return ret;

	ret = i2c_dw_set_sda_hold(dev);
	if (ret)
		return ret;

	ret = i2c_dw_set_fifo_size(dev);
	if (ret)
		return ret;

	ret = dev->init(dev);
	if (ret)
		return ret;

	snprintf(adap->name, sizeof(adap->name),
		 "Synopsys DesignWare I2C Slave adapter");
	adap->retries = 3;
	adap->algo = &i2c_dw_algo;
	adap->dev.parent = dev->dev;
	i2c_set_adapdata(adap, dev);

	ret = devm_request_irq(dev->dev, dev->irq, i2c_dw_isr_slave,
			       IRQF_SHARED, dev_name(dev->dev), dev);
	if (ret) {
		dev_err(dev->dev, "failure requesting irq %i: %d\n",
			dev->irq, ret);
		return ret;
	}

	ret = i2c_add_numbered_adapter(adap);
	if (ret)
		dev_err(dev->dev, "failure adding adapter: %d\n", ret);

	return ret;
}
EXPORT_SYMBOL_GPL(i2c_dw_probe_slave);

MODULE_AUTHOR("Luis Oliveira <lolivei@synopsys.com>");
MODULE_DESCRIPTION("Synopsys DesignWare I2C bus slave adapter");
MODULE_LICENSE("GPL v2");
