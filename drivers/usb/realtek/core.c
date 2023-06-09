// SPDX-License-Identifier: GPL-2.0-only
/*
 *  Copyright (C) 2021 Realtek Semiconductor Corp.
 *  All Rights Reserved
 */
#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/device.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/interrupt.h>
#include <linux/reset.h>
#include <linux/clk.h>
#include <linux/types.h>
#include <linux/delay.h>
#include <linux/usb/phy.h>
#include <linux/of_device.h>
#include <linux/usb/gadget.h>
#include <linux/suspend.h>
#include "core.h"

static const char		rts_usb_driver_name[] = "rts_usb_device";
static void __iomem		*usb_base;
static void __iomem		*mc_base;
static struct rts_mcm		*mcm[RTS_MCM_MAX_COUNT];

static const struct udc_devtype_data rts3917_devtype = {
	.quirks = UDC_QUIRK_DYNAMIC_MAXPKTSIZE,
};

static const struct of_device_id rts_usb_dt_ids[] = {
	{ .compatible = "realtek,rts3917-usb-device", .data = &rts3917_devtype, },
	{ /* sentinel */ },
};

/* platform device driver */
const struct platform_device_id rts_usb_ids[] = {
	{ "rts_usb_device", },
	{ }
};

static inline u32 usb_read_reg(u32 offset)
{
	return ioread32(usb_base + offset);
}

static inline void usb_write_reg(u32 value, u32 offset)
{
	iowrite32(value, usb_base + offset);
}

static inline void usb_clr_reg_bit(int bit, u32 offset)
{
	u32 v;

	v = usb_read_reg(offset) & ~(1 << bit);
	usb_write_reg(v, offset);
}

static inline void usb_set_reg_bit(int bit, u32 offset)
{
	u32 v;

	v = usb_read_reg(offset) | (1 << bit);
	usb_write_reg(v, offset);
}

static inline void usb_write_reg_mask(u32 value, u32 offset, u32 mask)
{
	u32 v;

	v = (usb_read_reg(offset) & ~mask) | (value & mask);
	usb_write_reg(v, offset);
}

static inline u32 mc_read_reg(u32 offset)
{
	return ioread32(mc_base + offset);
}

static inline void mc_write_reg(u32 value, u32 offset)
{
	iowrite32(value, mc_base + offset);
}

static inline void mc_clr_reg_bit(int bit, u32 offset)
{
	u32 v;

	v = mc_read_reg(offset) & ~(1 << bit);
	mc_write_reg(v, offset);
}

static inline void mc_set_reg_bit(int bit, u32 offset)
{
	u32 v;

	v = mc_read_reg(offset) | (1 << bit);
	mc_write_reg(v, offset);
}

/*
 * rts_set_cxstall
 * EP0 return host stall
 */
static void rts_set_cxstall(struct rts_udc *rtsusb)
{
	RTS_DEBUG("%s()\n", __func__);
	usb_set_reg_bit(EP0_STALL_OFFSET, USB_EP_CTL0);
}

/*
 * rts_set_cxdone
 * EP0 return host ACK
 */
static void rts_set_cxdone(struct rts_udc *rtsusb)
{
	RTS_DEBUG("%s()\n", __func__);
	usb_set_reg_bit(EP0_CSH_OFFSET, USB_EP_CTL1);
}

static void rts_set_mc_disable(struct rts_endpoint *priv_ep)
{
	mc_write_reg(1, MC_FIFO0_DMA_STOP + 0x100 * priv_ep->mcnum);
	mc_write_reg(1, MC_FIFO0_CTRL + 0x100 * priv_ep->mcnum);
}

static void rts_set_epnstall(struct rts_endpoint *priv_ep)
{
	u32 epnum = priv_ep->epnum;

	RTS_DEBUG("%s() ep %d\n", __func__, priv_ep->epnum);

	priv_ep->stall = 1;
	if (priv_ep->dir_in) {
		switch (epnum) {
		case 0:
			usb_set_reg_bit(EP0_STALL_OFFSET, USB_EP_CTL0);
			break;
		case 1 ... 3:
			usb_set_reg_bit(EP_STALL_OFFSET,
					USB_BULKINEPA_CTL + 0x40 * (epnum - 1));
			break;
		case 7 ... 12:
			usb_set_reg_bit(INTEP_STALL_OFFSET,
					USB_INTEREPA_CTL + 0x80 * (epnum - 7));
			break;
		default:
			break;
		}
	} else {
		switch (epnum) {
		case 0:
			usb_set_reg_bit(EP0_STALL_OFFSET, USB_EP_CTL0);
			break;
		case 1 ... 3:
			usb_set_reg_bit(EP_STALL_OFFSET,
				USB_BULKOUTEPA_CTL + 0x40 * (epnum - 1));
			break;
		default:
			break;
		}
	}
}

static void rts_clear_epnstall(struct rts_endpoint *priv_ep)
{
	u32 epnum = priv_ep->epnum;

	RTS_DEBUG("%s() ep %d\n", __func__, priv_ep->epnum);

	priv_ep->stall = 0;
	if (priv_ep->dir_in) {
		switch (epnum) {
		case 0:
			usb_clr_reg_bit(EP0_STALL_OFFSET, USB_EP_CTL0);
			break;
		case 1 ... 3:
			usb_clr_reg_bit(EP_STALL_OFFSET,
					USB_BULKINEPA_CTL + 0x40 * (epnum - 1));
			break;
		case 7 ... 12:
			usb_clr_reg_bit(INTEP_STALL_OFFSET,
					USB_INTEREPA_CTL + 0x80 * (epnum - 7));
			break;
		default:
			break;
		}
	} else {
		switch (epnum) {
		case 0:
			usb_clr_reg_bit(EP0_STALL_OFFSET, USB_EP_CTL0);
			break;
		case 1 ... 3:
			usb_clr_reg_bit(EP_STALL_OFFSET,
				USB_BULKOUTEPA_CTL + 0x40 * (epnum - 1));
			break;
		default:
			break;
		}
	}
}

static int rts_is_epnstall(struct rts_endpoint *priv_ep)
{
	RTS_DEBUG("%s()\n", __func__);

	return priv_ep->stall;
}

static void rts_set_epnum(struct rts_endpoint *priv_ep, u32 epnum)
{
	if (priv_ep->dir_in) {
		switch (epnum) {
		case 1 ... 3:
			usb_write_reg_mask(epnum << EP_EPNUM_OFFSET,
					USB_BULKINEPA_CFG + 0x40 * (epnum - 1),
					EP_EPNUM_MASK);
			break;
		case 4:
			usb_write_reg_mask(epnum << UACEP_EPNUM_OFFSET,
					   USB_UACINEP_CFG, UACEP_EPNUM_MASK);
			break;
		case 5:
			usb_write_reg_mask(epnum << EP_EPNUM3_0_OFFSET,
					   USB_UVCINEPA_CFG, EP_EPNUM3_0_MASK);
			break;
		case 6:
			usb_write_reg_mask(epnum << EP_EPNUM3_0_OFFSET,
					   USB_UVCINEPB_CFG, EP_EPNUM3_0_MASK);
			break;
		case 7 ... 12:
			usb_write_reg_mask(epnum << INTEP_EPNUM_OFFSET,
					USB_INTEREPA_CFG + 0x80 * (epnum - 7),
					INTEP_EPNUM_MASK);
			break;
		default:
			break;
		}
	} else {
		switch (epnum) {
		case 1 ... 3:
			usb_write_reg_mask(epnum << EP_EPNUM_OFFSET,
					USB_BULKOUTEPA_CFG + 0x40 * (epnum - 1),
					EP_EPNUM_MASK);
			break;
		case 4:
			usb_write_reg_mask(epnum << UACEP_EPNUM_OFFSET,
					   USB_UACOUTEP_CFG, UACEP_EPNUM_MASK);
			break;
		default:
			break;
		}
	}
}

static void rts_set_ep_enable(struct rts_endpoint *priv_ep, u32 epnum)
{
	RTS_DEBUG("%s()\n", __func__);
	priv_ep->ep_enable = 1;

	rts_clear_epnstall(priv_ep);

	if (priv_ep->dir_in) {
		if (epnum > 0 && epnum < 7)
			rts_set_mc_disable(priv_ep);
		switch (epnum) {
		case 1 ... 3:
			usb_set_reg_bit(EP_EN_OFFSET,
					USB_BULKINEPA_CFG + 0x40 * (epnum - 1));
			break;
		case 4:
			priv_ep->is_uac_in = 1;
			priv_ep->uac_cnt = 0;
			usb_set_reg_bit(UACEP_EN_OFFSET, USB_UACINEP_CFG);
			break;
		case 5:
			usb_set_reg_bit(EP_EN_OFFSET, USB_UVCINEPA_CFG);
			break;
		case 6:
			usb_set_reg_bit(EP_EN_OFFSET, USB_UVCINEPB_CFG);
			break;
		case 7 ... 12:
			usb_write_reg(0,
				USB_INTEREPA_TOGGLE_CTRL + 0x80 * (epnum - 7));
			usb_set_reg_bit(INTEP_EN_OFFSET,
					USB_INTEREPA_CFG + 0x80 * (epnum - 7));
			break;
		default:
			break;
		}
	} else {
		switch (epnum) {
		case 1 ... 3:
			rts_set_mc_disable(priv_ep);
			usb_write_reg(0,
			USB_BULKOUTEPA_TOGGLE_CTRL + 0x40 * (epnum - 1));
			usb_set_reg_bit(EP_EN_OFFSET,
				USB_BULKOUTEPA_CFG + 0x40 * (epnum - 1));
			break;
		case 4:
			rts_set_mc_disable(priv_ep);
			usb_set_reg_bit(UACEP_EN_OFFSET, USB_UACOUTEP_CFG);
			break;
		default:
			break;
		}
	}
}

static void rts_set_ep_maxpkt(struct rts_endpoint *priv_ep, u16 max_pkt_size)
{
	u16 maxpkt = priv_ep->endpoint.maxpacket & 0x7ff;
	u32 epnum = priv_ep->epnum;

	if (max_pkt_size && max_pkt_size < maxpkt)
		maxpkt = max_pkt_size & 0x7ff;

	if (priv_ep->dir_in) {
		switch (epnum) {
		case 1 ... 3:
			usb_write_reg_mask(maxpkt << EP_MAXPKT_OFFSET,
					USB_BULKINEPA_CFG + 0x40 * (epnum - 1),
					EP_MAXPKT_MASK);
			break;
		case 5:
			usb_write_reg_mask(maxpkt << EP_MAXPKT10_0_OFFSET,
					USB_UVCINEPA_CFG, EP_MAXPKT10_0_MASK);
			break;
		case 6:
			usb_write_reg_mask(maxpkt << EP_MAXPKT10_0_OFFSET,
					USB_UVCINEPB_CFG, EP_MAXPKT10_0_MASK);
			break;
		case 7 ... 12:
			usb_write_reg_mask(maxpkt << INTEP_MAXPKT_OFFSET,
					USB_INTEREPA_CFG + 0x80 * (epnum - 7),
					INTEP_MAXPKT_MASK);
			break;
		default:
			break;
		}
	} else {
		switch (epnum) {
		case 1 ... 3:
			usb_write_reg_mask(maxpkt << EP_MAXPKT_OFFSET,
					USB_BULKOUTEPA_CFG + 0x40 * (epnum - 1),
					EP_MAXPKT_MASK);
			break;
		case 4:
			usb_write_reg_mask(maxpkt << UACOUTEP_MAXPKT_OFFSET,
					USB_UACOUTEP_CFG, UACOUTEP_MAXPKT_MASK);
			break;
		default:
			break;
		}
	}
	priv_ep->maxpkt = maxpkt;
}

static int rts_set_ep_irq_enable(struct rts_endpoint *priv_ep)
{
	u32 epnum = priv_ep->epnum;

	RTS_DEBUG("%s() ep %d\n", __func__, priv_ep->epnum);

	if (priv_ep->dir_in) {
		switch (epnum) {
		case 1 ... 6:
			mc_set_reg_bit(INT_LASTPKT_DONE_OFFSET,
				MC_FIFO0_IRQ + 0x100 * priv_ep->mcnum);
			mc_set_reg_bit(INT_LASTPKT_DONE_EN_OFFSET,
				MC_FIFO0_IRQ_EN + 0x100 * priv_ep->mcnum);
			break;
		case 7 ... 12:
			usb_set_reg_bit(IE_INTEP_IN_OFFSET,
				USB_INTEREPA_IRQ_EN + 0x80 * (epnum - 7));
			break;
		default:
			break;
		}
	} else {
		if (epnum) {
			mc_set_reg_bit(INT_DMA_DONE_OFFSET,
				MC_FIFO0_IRQ + 0x100 * priv_ep->mcnum);
			mc_set_reg_bit(INT_DMA_DONE_EN_OFFSET,
				MC_FIFO0_IRQ_EN + 0x100 * priv_ep->mcnum);
		}
	}

	RTS_DEBUG("EP%d_MC_IRQ_EN = %#x\n", priv_ep->mcnum,
		  mc_read_reg(MC_FIFO0_IRQ_EN + 0x100 * priv_ep->mcnum));
	return 0;
}

static int rts_set_ep_irq_disable(struct rts_endpoint *priv_ep)
{
	u32 epnum = priv_ep->epnum;

	RTS_DEBUG("%s() ep %d\n", __func__, priv_ep->epnum);

	if (priv_ep->dir_in) {
		switch (epnum) {
		case 1 ... 6:
			mc_clr_reg_bit(INT_LASTPKT_DONE_EN_OFFSET,
				MC_FIFO0_IRQ_EN + 0x100 * priv_ep->mcnum);
			break;
		case 7 ... 12:
			usb_clr_reg_bit(IE_INTEP_IN_OFFSET,
				USB_INTEREPA_IRQ_EN + 0x80 * (epnum - 7));
			break;
		default:
			break;
		}
	} else {
		if (epnum)
			mc_clr_reg_bit(INT_DMA_DONE_EN_OFFSET,
				MC_FIFO0_IRQ_EN + 0x100 * priv_ep->mcnum);
	}
	return 0;
}

static int mcm_uvc[2] = {0, 1};
static int mcm_in[3] = {2, 3, 4};
static int mcm_out[3] = {5, 6, 7};
static int rts_set_ep_fifo_mapping(struct rts_endpoint *priv_ep, u32 epnum)
{
	if (priv_ep->dir_in) {
		switch (epnum) {
		case 1 ... 3:
			priv_ep->mcnum = mcm_in[epnum - 1];
			if (mcm[priv_ep->mcnum]->claimed)
				return -EINVAL;
			usb_write_reg_mask((1 << priv_ep->mcnum) <<
					EP_BUF_EN_SEL_OFFSET,
					USB_BULKINEPA_CFG + 0x40 * (epnum - 1),
					EP_BUF_EN_SEL_MASK);
			break;
		case 4:
			priv_ep->mcnum = mcm_in[epnum - 2];
			if (mcm[priv_ep->mcnum]->claimed)
				return -EINVAL;
			usb_write_reg_mask((1 << priv_ep->mcnum) <<
					UACEP_BUF_EN_SEL_OFFSET,
					USB_UACINEP_CFG, UACEP_BUF_EN_SEL_MASK);
			break;
		case 5:
			priv_ep->mcnum = mcm_uvc[epnum - 5];
			usb_write_reg_mask((1 << priv_ep->mcnum) <<
					EP_BUF_EN_SEL_OFFSET,
					USB_UVCINEPA_CFG, EP_BUF_EN_SEL_MASK);
			break;
		case 6:
			priv_ep->mcnum = mcm_uvc[epnum - 5];
			usb_write_reg_mask((1 << priv_ep->mcnum) <<
					EP_BUF_EN_SEL_OFFSET,
					USB_UVCINEPB_CFG, EP_BUF_EN_SEL_MASK);
			break;
		default:
			break;
		}
		mc_set_reg_bit(priv_ep->mcnum, R_MC_DUMMY0);
	} else {
		switch (epnum) {
		case 1 ... 3:
			priv_ep->mcnum = mcm_out[epnum - 1];
			if (mcm[priv_ep->mcnum]->claimed)
				return -EINVAL;
			usb_write_reg_mask((1 << priv_ep->mcnum) <<
					EP_BUF_EN_SEL_OFFSET,
					USB_BULKOUTEPA_CFG + 0x40 * (epnum - 1),
					EP_BUF_EN_SEL_MASK);
			break;
		case 4:
			priv_ep->mcnum = mcm_out[epnum - 2];
			if (mcm[priv_ep->mcnum]->claimed)
				return -EINVAL;
			usb_write_reg_mask((1 << priv_ep->mcnum) <<
					   UACEP_BUF_EN_SEL_OFFSET,
					   USB_UACOUTEP_CFG,
					   UACEP_BUF_EN_SEL_MASK);
			break;
		default:
			break;
		}
		mc_clr_reg_bit(priv_ep->mcnum, R_MC_DUMMY0);
	}

	mcm[priv_ep->mcnum]->epnum = epnum;
	mcm[priv_ep->mcnum]->dir_in = priv_ep->dir_in;
	mcm[priv_ep->mcnum]->dir_out = priv_ep->dir_out;
	mcm[priv_ep->mcnum]->claimed = 1;

	return 0;
}

static int rts_config_ep(struct rts_endpoint *priv_ep,
			 const struct usb_endpoint_descriptor *desc)
{
	struct rts_udc *rtsusb = priv_ep->rts_dev;
	int ret = 0;
	int pid = (desc->wMaxPacketSize >> 11) & 0x3;

	RTS_DEBUG("%s() -> ep%d in %d out %d\n", __func__, priv_ep->epnum,
					priv_ep->dir_in, priv_ep->dir_out);

	rts_set_epnum(priv_ep, priv_ep->epnum);
	rts_set_ep_maxpkt(priv_ep, 0);

	if ((priv_ep->epnum > 0 && priv_ep->epnum < 7 && priv_ep->dir_in)
				|| (priv_ep->epnum && priv_ep->dir_out)) {
		ret = rts_set_ep_fifo_mapping(priv_ep, priv_ep->epnum);
		if (ret)
			return ret;
	}

	if (priv_ep->epnum == 5) {
		usb_write_reg(UVC_HEAD, USB_UVCINEPA_PHINF0);
		usb_write_reg_mask(pid << EP_ISOTYPE1_0_OFFSET,
					USB_UVCINEPA_CFG5,
					EP_ISOTYPE1_0_MASK);
		priv_ep->pid = pid;
	} else if (priv_ep->epnum == 6) {
		usb_write_reg(UVC_HEAD, USB_UVCINEPB_PHINF0);
		usb_write_reg_mask(pid << EP_ISOTYPE1_0_OFFSET,
					USB_UVCINEPB_CFG5,
					EP_ISOTYPE1_0_MASK);
		priv_ep->pid = pid;
	}

	rts_set_ep_enable(priv_ep, priv_ep->epnum);
	if (priv_ep->dir_in)
		rtsusb->ep_in[priv_ep->epnum] = priv_ep;
	else
		rtsusb->ep_out[priv_ep->epnum] = priv_ep;

	return 0;
}

static void rts_set_ep_disable(struct rts_endpoint *priv_ep, u32 epnum)
{
	RTS_DEBUG("%s()\n", __func__);
	priv_ep->ep_enable = 0;

	if (priv_ep->dir_in) {
		if (epnum > 0 && epnum < 7)
			rts_set_mc_disable(priv_ep);
		switch (epnum) {
		case 1 ... 3:
			usb_clr_reg_bit(EP_EN_OFFSET,
					USB_BULKINEPA_CFG + 0x40 * (epnum - 1));
			break;
		case 4:
			usb_clr_reg_bit(UACEP_EN_OFFSET, USB_UACINEP_CFG);
			break;
		case 5:
			usb_clr_reg_bit(EP_EN_OFFSET, USB_UVCINEPA_CFG);
			break;
		case 6:
			usb_clr_reg_bit(EP_EN_OFFSET, USB_UVCINEPB_CFG);
			break;
		case 7 ... 12:
			usb_clr_reg_bit(INTEP_EN_OFFSET,
					USB_INTEREPA_CFG + 0x80 * (epnum - 7));
			break;
		default:
			break;
		}
	} else {
		switch (epnum) {
		case 1 ... 3:
			rts_set_mc_disable(priv_ep);
			usb_clr_reg_bit(EP_EN_OFFSET,
				USB_BULKOUTEPA_CFG + 0x40 * (epnum - 1));
			break;
		case 4:
			rts_set_mc_disable(priv_ep);
			usb_clr_reg_bit(UACEP_EN_OFFSET, USB_UACOUTEP_CFG);
			break;
		default:
			break;
		}
	}
}

/**
 * rts_ep_enable
 */
static int rts_ep_enable(struct usb_ep *ep,
			 const struct usb_endpoint_descriptor *desc)
{
	struct rts_endpoint *priv_ep = ep_to_rts_ep(ep);

	RTS_DEBUG("%s()\n", __func__);

	priv_ep->desc = desc;
	priv_ep->epnum = usb_endpoint_num(desc);
	if (!priv_ep->epnum)
		return 0;
	priv_ep->type = usb_endpoint_type(desc);
	priv_ep->dir_in = usb_endpoint_dir_in(desc);
	priv_ep->dir_out = usb_endpoint_dir_out(desc);
	priv_ep->endpoint.maxpacket = usb_endpoint_maxp(desc);

	return rts_config_ep(priv_ep, desc);
}

static void rts_done_wq(struct rts_endpoint *priv_ep,
			struct rts_request *priv_req, int status)
{
	bool stopped = priv_ep->stopped;

	RTS_DEBUG("%s() ep %d\n", __func__, priv_ep->epnum);

	list_del_init(&priv_req->queue);

	priv_ep->stopped = 1;

	/* don't modify queue heads during completion callback */
	if (priv_ep->rts_dev->gadget.speed == USB_SPEED_UNKNOWN)
		priv_req->request.status = -ESHUTDOWN;
	else
		priv_req->request.status = status;

	if (list_empty(&priv_ep->queue)) {
		RTS_DEBUG("ep%d->queue is empty", priv_ep->epnum);
		rts_set_ep_irq_disable(priv_ep);
	}

	usb_gadget_unmap_request(&priv_ep->rts_dev->gadget,
				 &priv_req->request, priv_ep->dir_in);

	usb_gadget_giveback_request(&priv_ep->endpoint, &priv_req->request);

	if (priv_ep->rts_dev->request_pending > 0)
		--priv_ep->rts_dev->request_pending;
	priv_ep->stopped = stopped;
}

static int __set_filterout(struct rts_endpoint *priv_ep,
		struct rts_request *priv_req)
{
	struct rts_udc *rtsusb = priv_ep->rts_dev;
	int retval = 0;

	if (!rtsusb || !rtsusb->gadget_driver ||
			!rtsusb->gadget_driver->filteroutdata)
		return 0;

	spin_unlock(&rtsusb->lock);
	retval = rtsusb->gadget_driver->filteroutdata(&rtsusb->gadget,
		rtsusb->setup_buf, priv_req->request.buf,
		priv_req->request.length);
	if (retval)
		rts_set_cxstall(rtsusb);

	spin_lock(&rtsusb->lock);

	return retval;
}

static void rts_done(struct rts_endpoint *priv_ep, struct rts_request *priv_req,
		     int status)
{
	bool stopped = priv_ep->stopped;

	RTS_DEBUG("%s() ep %d\n", __func__, priv_ep->epnum);

	list_del_init(&priv_req->queue);

	priv_ep->stopped = 1;

	/* don't modify queue heads during completion callback */
	if (priv_ep->rts_dev->gadget.speed == USB_SPEED_UNKNOWN)
		priv_req->request.status = -ESHUTDOWN;
	else
		priv_req->request.status = status;

	if (priv_ep->epnum) { /* not ep0 */
		if (list_empty(&priv_ep->queue)) {
			RTS_DEBUG("ep%d->queue is empty", priv_ep->epnum);
			rts_set_ep_irq_disable(priv_ep);
		}
	} else { /* ep0 */
		if (priv_ep->dir_in) {
			usb_clr_reg_bit(IE_EP0IN_OFFSET, USB_IRQ_EN);
			mc_clr_reg_bit(U_BUF0_EP0_TX_EN_OFFSET,
				       R_EP0_MC_BUF_CTL);
			rts_set_cxdone(priv_ep->rts_dev);
		} else {
			usb_clr_reg_bit(IE_EP0OUT_OFFSET, USB_IRQ_EN);
			mc_clr_reg_bit(U_BUF0_EP0_RX_EN_OFFSET,
				       R_EP0_MC_BUF_CTL);
			if (!__set_filterout(priv_ep, priv_req))
				rts_set_cxdone(priv_ep->rts_dev);
		}
	}

	if ((priv_ep->epnum > 0 && priv_ep->epnum < 7 && priv_ep->dir_in)
					|| (priv_ep->epnum && priv_ep->dir_out))
		usb_gadget_unmap_request(&priv_ep->rts_dev->gadget,
					 &priv_req->request, priv_ep->dir_in);

	spin_unlock(&priv_ep->rts_dev->lock);
	usb_gadget_giveback_request(&priv_ep->endpoint, &priv_req->request);
	spin_lock(&priv_ep->rts_dev->lock);

	if (priv_ep->rts_dev->request_pending > 0)
		--priv_ep->rts_dev->request_pending;
	priv_ep->stopped = stopped;
}

static void rts_done_uvc_in_disable(struct rts_endpoint *priv_ep)
{
	struct rts_request *priv_req;
	unsigned long flags;

	spin_lock_irqsave(&priv_ep->rts_dev->lock, flags);
	while (!list_empty(&priv_ep->queue)) {
		priv_req = list_entry(priv_ep->queue.next,
				      struct rts_request, queue);
		rts_done_wq(priv_ep, priv_req, -ECONNRESET);
	}
	spin_unlock_irqrestore(&priv_ep->rts_dev->lock, flags);

	priv_ep->epnum = 0;
	priv_ep->mcnum = 0;
	priv_ep->stall = 0;
	priv_ep->wedged = 0;
	priv_ep->stopped = 0;
	priv_ep->ep_enable = 0;
	priv_ep->uac_cnt = 0;
	priv_ep->is_uac_in = 0;
}

static void rts_done_uvc_in_disable_ep5(struct work_struct *work)
{
	struct rts_udc *rtsusb = container_of(work, struct rts_udc,
					      uvcin_done_work[0]);
	struct rts_endpoint *priv_ep = rtsusb->ep_in[5];

	rts_done_uvc_in_disable(priv_ep);
}

static void rts_done_uvc_in_disable_ep6(struct work_struct *work)
{
	struct rts_udc *rtsusb = container_of(work, struct rts_udc,
					      uvcin_done_work[1]);
	struct rts_endpoint *priv_ep = rtsusb->ep_in[6];

	rts_done_uvc_in_disable(priv_ep);
}

/**
 * rts_ep_disable
 */
static int rts_ep_disable(struct usb_ep *ep)
{
	struct rts_endpoint *priv_ep;
	struct rts_request *priv_req;
	unsigned long flags;
	int i;

	RTS_DEBUG("%s()\n", __func__);

	WARN_ON(!ep);

	priv_ep = ep_to_rts_ep(ep);

	rts_set_ep_disable(priv_ep, priv_ep->epnum);

	for (i = 0; i < RTS_MCM_MAX_COUNT; i++) {
		if (mcm[i]->epnum == priv_ep->epnum &&
		    mcm[i]->dir_in == priv_ep->dir_in &&
		    mcm[i]->dir_out == priv_ep->dir_out) {
			mcm[i]->epnum = 0;
			mcm[i]->claimed = 0;
			mcm[i]->dir_in = 0;
			mcm[i]->dir_out = 0;
		}
	}

	if (priv_ep->epnum == 5 || priv_ep->epnum == 6) {
		struct rts_udc *rtsusb = priv_ep->rts_dev;

		queue_work(rtsusb->udc_wq,
			   &rtsusb->uvcin_done_work[priv_ep->epnum - 5]);
		return 0;
	}

	while (!list_empty(&priv_ep->queue)) {
		priv_req = list_entry(priv_ep->queue.next,
					struct rts_request, queue);
		spin_lock_irqsave(&priv_ep->rts_dev->lock, flags);
		rts_done(priv_ep, priv_req, -ECONNRESET);
		spin_unlock_irqrestore(&priv_ep->rts_dev->lock, flags);
	}

	if (!priv_ep->epnum)
		return 0;
	priv_ep->epnum = 0;
	priv_ep->mcnum = 0;
	priv_ep->stall = 0;
	priv_ep->wedged = 0;
	priv_ep->stopped = 0;
	priv_ep->ep_enable = 0;
	priv_ep->uac_cnt = 0;
	priv_ep->is_uac_in = 0;
	return 0;
}

/**
 * rts_ep_alloc_request Allocates request
 * @ep: endpoint object associated with request
 * @gfp_flags: gfp flags
 *
 * Returns allocated request address, NULL on allocation error
 */
static struct usb_request *rts_ep_alloc_request(struct usb_ep *ep,
						gfp_t gfp_flags)
{
	struct rts_request *priv_req;

	RTS_DEBUG("%s()\n", __func__);

	priv_req = kzalloc(sizeof(*priv_req), gfp_flags);
	if (!priv_req)
		return NULL;

	INIT_LIST_HEAD(&priv_req->queue);
	return &priv_req->request;
}

/**
 * rts_ep_free_request Free memory occupied by request
 * @ep: endpoint object associated with request
 * @request: request to free memory
 */
static void rts_ep_free_request(struct usb_ep *ep, struct usb_request *request)
{
	struct rts_request *priv_req = to_rts_request(request);

	RTS_DEBUG("%s()\n", __func__);
	kfree(priv_req);
}

static void rts_ep0_req_complete(struct usb_ep *ep, struct usb_request *request)
{
}

static void rts_start_ep0_transfer(struct rts_endpoint *priv_ep,
				   struct rts_request *priv_req)
{
	u32 *buffer;
	u16 length;
	int i;

	RTS_DEBUG("%s() -> ep%d\n", __func__, priv_ep->epnum);

	if (priv_ep->dir_in) {
		usb_set_reg_bit(IE_EP0IN_OFFSET, USB_IRQ_EN);
		RTS_DEBUG("%s() ep0 in request.length %d\n", __func__,
			  priv_req->request.length);
		buffer = priv_req->request.buf + priv_req->request.actual;
		length = priv_req->request.length - priv_req->request.actual;
		if (length > priv_ep->endpoint.maxpacket)
			length = priv_ep->endpoint.maxpacket;
		RTS_DEBUG("ep0 in length %d\n", length);
		for (i = 0; i <= (length - 1) / 4; i++, buffer++)
			mc_write_reg(*buffer, EP0_BASE + i * 4);

		mc_write_reg(length & 0xff, R_EP0_MC_BUF_BC);
		priv_req->ep0_in_last_length = length;
		mc_set_reg_bit(U_BUF0_EP0_TX_EN_OFFSET, R_EP0_MC_BUF_CTL);
	} else {
		usb_set_reg_bit(IE_EP0OUT_OFFSET, USB_IRQ_EN);
		RTS_DEBUG("%s() ep0 out\n", __func__);
		mc_set_reg_bit(U_BUF0_EP0_RX_EN_OFFSET, R_EP0_MC_BUF_CTL);
	}
}

static int rts_ep0_queue(struct rts_endpoint *priv_ep,
			 struct rts_request *priv_req)
{
	int ret = 0;

	RTS_DEBUG("%s()\n", __func__);

	if (!priv_req->request.length) {
		rts_done(priv_ep, priv_req, 0);
		return ret;
	}

	rts_start_ep0_transfer(priv_ep, priv_req);

	return ret;
}

static void rts_uvc_quirks(struct rts_endpoint *priv_ep,
			struct rts_request *priv_req)
{
	int pid = priv_ep->pid;
	u16 maxpid = (priv_ep->desc->wMaxPacketSize >> 11) & 0x3;
	int maxpkt = priv_ep->maxpkt;
	int ep_maxpkt = priv_ep->desc->wMaxPacketSize & 0x7FF;

	if (priv_ep->epnum != 5 && priv_ep->epnum != 6)
		return;

	if (pid > maxpid)
		pid = maxpid;
	if (priv_req->request.length > (ep_maxpkt - 12) * (maxpid + 1)) {
		pid = maxpid;
		maxpkt = ep_maxpkt;
	} else {
		pid = 0;
		if (priv_req->request.length < (priv_ep->maxpkt - 12)
			* (pid + 1))
			maxpkt = 12 + rounddown(
				priv_req->request.length - 1, 4);
	}
	if (maxpkt != priv_ep->maxpkt)
		rts_set_ep_maxpkt(priv_ep, maxpkt);

	if (pid != priv_ep->pid) {
		if (priv_ep->epnum == 5)
			usb_write_reg_mask(pid << EP_ISOTYPE1_0_OFFSET,
						USB_UVCINEPA_CFG5,
						EP_ISOTYPE1_0_MASK);
		else if (priv_ep->epnum == 6)
			usb_write_reg_mask(pid << EP_ISOTYPE1_0_OFFSET,
						USB_UVCINEPB_CFG5,
						EP_ISOTYPE1_0_MASK);
		priv_ep->pid = pid;
	}
}

static inline void rts_usb_write_header_len(u32 len, unsigned char epnum)
{
	u32 val, reg_read;

	if (epnum == 5 || epnum == 6) {
		reg_read = usb_read_reg(USB_DUMMY0);
		if (len > 0) {
			if (epnum == 5) {
				reg_read &= 0xFF7FFF;
				val = ((len + 12) << 24) | 0x8000;
			} else {
				reg_read &= 0xFF00BFFF;
				val = ((len + 12) << 16) | 0x4000;
			}
			val |= reg_read;
		} else {
			if (epnum == 5)
				val = reg_read & 0xFFFF7FFF;
			else
				val = reg_read & 0xFFFFBFFF;
		}
		usb_write_reg(val, USB_DUMMY0);
	}
}

static void rts_start_dma_transfer(struct rts_endpoint *priv_ep,
				   struct rts_request *priv_req)
{
	struct rts_udc *rtsusb = priv_ep->rts_dev;

	RTS_DEBUG("%s() -> ep%d mc%d\n", __func__, priv_ep->epnum,
			priv_ep->mcnum);

	if (rtsusb->devtype_data->quirks & UDC_QUIRK_DYNAMIC_MAXPKTSIZE)
		rts_uvc_quirks(priv_ep, priv_req);

	if (priv_ep->is_uac_in) {
		if (priv_ep->uac_cnt % 2 == 0)
			usb_write_reg_mask((priv_req->request.length & 0x7ff) <<
				UACIN_PKTSIZE010_0_OFFSET,
				USB_UACINEP_PKTSIZE0 + priv_ep->uac_cnt * 2,
				UACIN_PKTSIZE010_0_MASK);
		else if (priv_ep->uac_cnt % 2 == 1)
			usb_write_reg_mask((priv_req->request.length & 0x7ff) <<
				UACIN_PKTSIZE110_0_OFFSET,
				USB_UACINEP_PKTSIZE0 +
				(priv_ep->uac_cnt - 1) * 2,
				UACIN_PKTSIZE110_0_MASK);
		priv_ep->uac_cnt++;
		if (priv_ep->uac_cnt == 20)
			priv_ep->uac_cnt = 0;
	}

	mc_write_reg(1, MC_FIFO0_CTRL + 0x100 * priv_ep->mcnum);
	mc_write_reg(priv_req->request.dma,
		     MC_FIFO0_DMA_ADDR + 0x100 * priv_ep->mcnum);
	mc_write_reg(priv_req->request.length,
		     MC_FIFO0_DMA_LENGTH + 0x100 * priv_ep->mcnum);

	if (priv_ep->dir_in)
		mc_set_reg_bit(U_PE_TRANS_DIR_OFFSET,
			       MC_FIFO0_DMA_CTRL + 0x100 * priv_ep->mcnum);
	else
		mc_clr_reg_bit(U_PE_TRANS_DIR_OFFSET,
			       MC_FIFO0_DMA_CTRL + 0x100 * priv_ep->mcnum);

	rts_usb_write_header_len(priv_req->request.meta_len, priv_ep->epnum);
	mc_set_reg_bit(U_PE_TRANS_EN_OFFSET,
		       MC_FIFO0_DMA_CTRL + 0x100 * priv_ep->mcnum);

	RTS_DEBUG("EP%d_MC_DMA_ADDR %#x ", priv_ep->mcnum,
		  mc_read_reg(MC_FIFO0_DMA_ADDR + 0x100 * priv_ep->mcnum));
	RTS_DEBUG("EP%d_MC_DMA_LENGTH %#x ", priv_ep->mcnum,
		  mc_read_reg(MC_FIFO0_DMA_LENGTH + 0x100 * priv_ep->mcnum));
	RTS_DEBUG("EP%d_MC_DMA_CTRL %#x\n", priv_ep->mcnum,
		  mc_read_reg(MC_FIFO0_DMA_CTRL + 0x100 * priv_ep->mcnum));
}

static void rts_start_intr_transfer(struct rts_endpoint *priv_ep,
				    struct rts_request *priv_req)
{
	u32 *buffer;
	u16 length;
	int i;

	RTS_DEBUG("%s() -> ep%d\n", __func__, priv_ep->epnum);

	if (priv_ep->dir_in) {
		buffer = priv_req->request.buf + priv_req->request.actual;
		length = priv_req->request.length - priv_req->request.actual;
		if (length > priv_ep->endpoint.maxpacket)
			length = priv_ep->endpoint.maxpacket;
		RTS_DEBUG("intr ep in length %d\n", length);
		for (i = 0; i <= (length - 1) / 4; i++, buffer++)
			usb_write_reg(*buffer, USB_INTEREPA_DAT0 +
				      0x80 * (priv_ep->epnum - 7) + i * 4);

		usb_write_reg(length, USB_INTEREPA_BC +
			      0x80 * (priv_ep->epnum - 7));
		usb_set_reg_bit(INT_BUF_EN_OFFSET, USB_INTEREPA_CTL +
				0x80 * (priv_ep->epnum - 7));
		priv_req->intr_in_last_length = length;
		RTS_DEBUG("USB_INTEREP_BC %#x, USB_INTEREP_CTL %#x\n",
		usb_read_reg(USB_INTEREPA_BC + 0x80 * (priv_ep->epnum - 7)),
		usb_read_reg(USB_INTEREPA_CTL + 0x80 * (priv_ep->epnum - 7)));
	}
}

/**
 * rts_ep_queue Transfer data on endpoint
 * @ep: pointer to endpoint zero object
 * @request: pointer to request object
 * @gfp_flags: gfp flags
 *
 * Returns 0 on success, error code elsewhere
 */
static int rts_ep_queue(struct usb_ep *ep, struct usb_request *request,
			gfp_t gfp_flags)
{
	struct rts_endpoint *priv_ep = ep_to_rts_ep(ep);
	struct rts_request *priv_req = to_rts_request(request);
	unsigned long flags;
	int req = 0, ret = 0;

	RTS_DEBUG("%s() ep%d in %d out %d\n", __func__, priv_ep->epnum,
		  priv_ep->dir_in, priv_ep->dir_out);

	spin_lock_irqsave(&priv_ep->rts_dev->lock, flags);

	if (list_empty(&priv_ep->queue)) {
		RTS_DEBUG("ep %d list empty\n", priv_ep->epnum);
		req = 1;
	}

	list_add_tail(&priv_req->queue, &priv_ep->queue);
	++priv_ep->rts_dev->request_pending;

	priv_req->request.actual = 0;
	priv_req->request.status = -EINPROGRESS;

	if (!priv_ep->epnum) /* ep0 */
		ret = rts_ep0_queue(priv_ep, priv_req);
	else if (req && !priv_ep->stall)
		rts_set_ep_irq_enable(priv_ep);

	if ((priv_ep->epnum > 0 && priv_ep->epnum < 7 && priv_ep->dir_in)
			|| (priv_ep->epnum && priv_ep->dir_out)) {
		ret = usb_gadget_map_request(&priv_ep->rts_dev->gadget, request,
					     priv_ep->dir_in);
		if (req && !priv_ep->stall && !priv_ep->stopped)
			rts_start_dma_transfer(priv_ep, priv_req);
	} else if (priv_ep->epnum > 6 && priv_ep->dir_in) { //intr ep
		if (req && !priv_ep->stall && !priv_ep->stopped)
			rts_start_intr_transfer(priv_ep, priv_req);
	}

	spin_unlock_irqrestore(&priv_ep->rts_dev->lock, flags);

	return ret;
}

static int rts_ep_dequeue(struct usb_ep *ep, struct usb_request *request)
{
	struct rts_endpoint *priv_ep = ep_to_rts_ep(ep);
	struct rts_request *priv_req = to_rts_request(request);
	unsigned long flags;

	RTS_DEBUG("%s()\n", __func__);

	spin_lock_irqsave(&priv_ep->rts_dev->lock, flags);
	if (!list_empty(&priv_ep->queue))
		rts_done(priv_ep, priv_req, -ECONNRESET);
	spin_unlock_irqrestore(&priv_ep->rts_dev->lock, flags);

	return 0;
}

static int rts_set_halt_and_wedge(struct usb_ep *ep, int value, int wedge)
{
	struct rts_endpoint *priv_ep = ep_to_rts_ep(ep);
	struct rts_udc *rtsusb = priv_ep->rts_dev;
	unsigned long flags;

	RTS_DEBUG("%s()\n", __func__);

	spin_lock_irqsave(&rtsusb->lock, flags);

	if (value) {
		rts_set_epnstall(priv_ep);
		priv_ep->stall = 1;
		priv_ep->stopped = 1;
		if (wedge)
			priv_ep->wedged = 1;
	} else {
		rts_clear_epnstall(priv_ep);
		priv_ep->stall = 0;
		priv_ep->wedged = 0;
		priv_ep->stopped = 0;
		if (!list_empty(&priv_ep->queue))
			rts_set_ep_irq_enable(priv_ep);
	}

	spin_unlock_irqrestore(&rtsusb->lock, flags);

	return 0;
}

static int rts_ep_set_halt(struct usb_ep *ep, int value)
{
	RTS_DEBUG("%s()\n", __func__);
	return rts_set_halt_and_wedge(ep, value, 0);
}

static int rts_ep_set_wedge(struct usb_ep *ep)
{
	RTS_DEBUG("%s()\n", __func__);
	return rts_set_halt_and_wedge(ep, 1, 1);
}

static const struct usb_ep_ops rts_ep_ops = {
	.enable		= rts_ep_enable,
	.disable	= rts_ep_disable,

	.alloc_request	= rts_ep_alloc_request,
	.free_request	= rts_ep_free_request,

	.queue		= rts_ep_queue,
	.dequeue	= rts_ep_dequeue,

	.set_halt	= rts_ep_set_halt,
	.set_wedge	= rts_ep_set_wedge,
};

static void rts_usb_connect(struct rts_udc *rtsusb)
{
	RTS_DEBUG("%s()\n", __func__);
	usb_set_reg_bit(CONNECT_EN_OFFSET, USB_CTRL);
}

static void rts_usb_disconnect(struct rts_udc *rtsusb)
{
	RTS_DEBUG("%s()\n", __func__);
	usb_clr_reg_bit(CONNECT_EN_OFFSET, USB_CTRL);
}

static void rts_usb_enable_phy_int(struct rts_udc *rtsusb)
{
	/* Enable vbus interrupt */
	RTS_DEBUG("%s()\n", __func__);
	usb_set_reg_bit(VBUS_INT_EN_OFFSET, USB_DPHYCFG2);
	usb_read_reg(USB_DPHYCFG2);
}

static void rts_usb_disable_phy_int(struct rts_udc *rtsusb)
{
	/* Disable vbus interrupt */
	RTS_DEBUG("%s()\n", __func__);
	usb_write_reg_mask((VBUS_DOWN_INTR_MASK | VBUS_ON_INTR_MASK),
		USB_DPHYCFG2, (VBUS_DOWN_INTR_MASK | VBUS_ON_INTR_MASK));
}

static uint32_t rts_usb_read_phy_int(void)
{
	uint32_t int_val;

	RTS_DEBUG("%s()\n", __func__);

	int_val = usb_read_reg(USB_DPHYCFG2);
	return int_val;
}

static void rts_usb_clear_phy_int(uint32_t clr_msk)
{
	uint32_t int_val;

	RTS_DEBUG("%s()\n", __func__);

	int_val = usb_read_reg(USB_DPHYCFG2);
	int_val &= ~clr_msk;
	usb_write_reg(int_val, USB_DPHYCFG2);
}

static void rts_usb_req_ep0_get_status(struct rts_udc *rtsusb,
				       struct usb_ctrlrequest *ctrl_req)
{
	u8 epnum;

	RTS_DEBUG("%s()\n", __func__);

	switch (ctrl_req->bRequestType & USB_RECIP_MASK) {
	case USB_RECIP_DEVICE:
		rtsusb->ep0_data = rtsusb->devstatus;
		break;
	case USB_RECIP_INTERFACE:
		rtsusb->ep0_data = 0;
		break;
	case USB_RECIP_ENDPOINT:
		epnum = ctrl_req->wIndex & USB_ENDPOINT_NUMBER_MASK;
		if (epnum) {
			if (ctrl_req->wIndex & USB_ENDPOINT_DIR_MASK)
				rtsusb->ep0_data =
				rts_is_epnstall(rtsusb->ep_in[epnum])
						<< USB_ENDPOINT_HALT;
			else
				rtsusb->ep0_data =
				rts_is_epnstall(rtsusb->ep_out[epnum])
						<< USB_ENDPOINT_HALT;
		} else {
			rts_set_cxstall(rtsusb);
		}
		break;
	default:
		rts_set_cxstall(rtsusb);
		return;
	}
	rtsusb->ep0_req->buf = &rtsusb->ep0_data;
	rtsusb->ep0_req->length = 2;
	rtsusb->ep0_req->zero = 0;
	rtsusb->ep0_req->complete = rts_ep0_req_complete;

	spin_unlock(&rtsusb->lock);
	rts_ep_queue(rtsusb->gadget.ep0, rtsusb->ep0_req, GFP_ATOMIC);
	spin_lock(&rtsusb->lock);
}

static void rts_usb_req_ep0_clear_feature(struct rts_udc *rtsusb,
					  struct usb_ctrlrequest *ctrl_req)
{
	struct rts_endpoint *ep;

	RTS_DEBUG("%s()\n", __func__);

	if (ctrl_req->wIndex & USB_ENDPOINT_DIR_MASK)
		ep = rtsusb->ep_in[ctrl_req->wIndex & USB_ENDPOINT_NUMBER_MASK];
	else
		ep =
		rtsusb->ep_out[ctrl_req->wIndex & USB_ENDPOINT_NUMBER_MASK];

	switch (ctrl_req->bRequestType & USB_RECIP_MASK) {
	case USB_RECIP_DEVICE:
		rts_set_cxdone(rtsusb);
		break;
	case USB_RECIP_INTERFACE:
		rts_set_cxdone(rtsusb);
		break;
	case USB_RECIP_ENDPOINT:
		if (ctrl_req->wIndex & USB_ENDPOINT_NUMBER_MASK) {
			if (ep->wedged) {
				rts_set_cxdone(rtsusb);
				break;
			}
			if (ep->stall)
				rts_set_halt_and_wedge(&ep->endpoint, 0, 0);
		}
		rts_set_cxdone(rtsusb);
		break;
	default:
		rts_set_cxstall(rtsusb);
		break;
	}
}

static void rts_usb_test_packet(void)
{
	int i;
	/* 53 bytes (for USB 2.0 Test Mode) */
	const uint32_t usb_test_packet_array[] = {
		0x00000000, 0x00000000, 0xAAAAAA00, 0xAAAAAAAA,
		0xEEEEEEAA, 0xEEEEEEEE, 0xFFFFFEEE, 0xFFFFFFFF,
		0xFFFFFFFF, 0xDFBF7FFF, 0xFDFBF7EF, 0xDFBF7EFC,
		0xFDFBF7EF, 0x7E
	};
	u32 *buffer = usb_test_packet_array;

	for (i = 0; i < 14; i++, buffer++)
		mc_write_reg(*buffer, EP0_BASE + i * 4);

	mc_write_reg(0x35 & 0xff, R_EP0_MC_BUF_BC);
	usb_write_reg_mask(0x4 << USBTMOD_OFFSET, USB_SLBTEST,  USBTMOD_MASK);
	mc_set_reg_bit(U_BUF0_EP0_TX_EN_OFFSET, R_EP0_MC_BUF_CTL);
}

static void rts_usb_test_mode(struct work_struct *work)
{
	struct rts_udc *rtsusb = container_of(work, struct rts_udc, test_work);

	switch (rtsusb->test_mode) {
	case 0x01:	/* test J */
		usb_write_reg_mask(0x2 << USBTMOD_OFFSET,
				   USB_SLBTEST, USBTMOD_MASK);
		break;
	case 0x02:	/* test K */
		usb_write_reg_mask(0x3 << USBTMOD_OFFSET,
				   USB_SLBTEST, USBTMOD_MASK);
		break;
	case 0x03:	/* test SE0 NAK */
		usb_write_reg_mask(0x1 << USBTMOD_OFFSET,
				   USB_SLBTEST, USBTMOD_MASK);
		break;
	case 0x04:	/* test Packet */
		rts_usb_test_packet();
		break;
	default:
		break;
	}
}

static void rts_usb_req_ep0_set_feature(struct rts_udc *rtsusb,
					struct usb_ctrlrequest *ctrl_req)
{
	RTS_DEBUG("%s()\n", __func__);

	switch (ctrl_req->bRequestType & USB_RECIP_MASK) {
	case USB_RECIP_DEVICE:
		rts_set_cxdone(rtsusb);
		if (ctrl_req->wValue == USB_DEVICE_TEST_MODE) {
			rtsusb->test_mode = ctrl_req->wIndex >> 8;
			queue_work(rtsusb->udc_wq, &rtsusb->test_work);
		}
		break;
	case USB_RECIP_INTERFACE:
		rts_set_cxdone(rtsusb);
		break;
	case USB_RECIP_ENDPOINT: {
		u8 epnum;
		struct rts_endpoint *ep;

		epnum = ctrl_req->wIndex & USB_ENDPOINT_NUMBER_MASK;
		if (ctrl_req->wIndex & USB_ENDPOINT_DIR_MASK)
			ep = rtsusb->ep_in[epnum];
		else
			ep = rtsusb->ep_out[epnum];

		if (epnum)
			rts_set_epnstall(ep);
		else
			rts_set_cxstall(rtsusb);
		rts_set_cxdone(rtsusb);
		}
		break;
	default:
		rts_set_cxstall(rtsusb);
		break;
	}
}

static void rts_set_dev_addr(struct rts_udc *rtsusb, u32 addr)
{
	u32 value = usb_read_reg(USB_ADDR);

	RTS_DEBUG("%s()\n", __func__);

	value |= (addr & 0x7f);
	if (usb_read_reg(USB_ADDR) & 0x7f) {
		rts_set_cxdone(rtsusb);
		usb_write_reg(value, USB_ADDR);
	} else {
		usb_write_reg(value, USB_ADDR);
		rts_set_cxdone(rtsusb);
	}
}

static void rts_usb_req_ep0_set_address(struct rts_udc *rtsusb,
					struct usb_ctrlrequest *ctrl_req)
{
	u32 int_val;

	RTS_DEBUG("%s()\n", __func__);

	int_val = usb_read_reg(USB_CTRL);
	if (int_val & BIT(4)) {
		if (abs(clk_get_rate(rtsusb->bus_clk) - USB_HS_CLK) > 10000)
			clk_set_rate(rtsusb->bus_clk, USB_HS_CLK);
		rtsusb->gadget.speed = USB_SPEED_HIGH;
	} else {
		if (abs(clk_get_rate(rtsusb->bus_clk) - USB_FS_CLK) > 10000)
			clk_set_rate(rtsusb->bus_clk, USB_FS_CLK);
		rtsusb->gadget.speed = USB_SPEED_FULL;
	}

	if (ctrl_req->wValue >= 0x100)
		rts_set_cxstall(rtsusb);
	else
		rts_set_dev_addr(rtsusb, ctrl_req->wValue);
}

static void rts_usb_req_ep0_set_configuration(struct rts_udc *rtsusb,
					      struct usb_ctrlrequest *ctrl_req)
{
	RTS_DEBUG("%s()\n", __func__);
	// usb_set_reg_bit(FORCE_DEVADDR_OFFSET, USB_ADDR);
}

static int rts_usb_ep0_standard_request(struct rts_udc *rtsusb,
					struct usb_ctrlrequest *ctrl_req)
{
	int ret = 0;

	RTS_DEBUG("%s()\n", __func__);

	switch (ctrl_req->bRequest) {
	case USB_REQ_GET_STATUS:
		RTS_DEBUG("\nget_status\n");
		rts_usb_req_ep0_get_status(rtsusb, ctrl_req);
		break;
	case USB_REQ_CLEAR_FEATURE:
		RTS_DEBUG("\nclear_feature\n");
		rts_usb_req_ep0_clear_feature(rtsusb, ctrl_req);
		break;
	case USB_REQ_SET_FEATURE:
		RTS_DEBUG("\nset_feature\n");
		rts_usb_req_ep0_set_feature(rtsusb, ctrl_req);
		break;
	case USB_REQ_SET_ADDRESS:
		RTS_DEBUG("\nset_address\n");
		rts_usb_req_ep0_set_address(rtsusb, ctrl_req);
		break;
	case USB_REQ_SET_CONFIGURATION:
		RTS_DEBUG("\nset_configuration\n");
		rts_usb_req_ep0_set_configuration(rtsusb, ctrl_req);
		ret = 1;
		break;
	default:
		ret = 1;
		break;
	}
	return ret;
}

static int rts_usb_setup_process(struct rts_udc *rtsusb)
{
	struct usb_ctrlrequest *ctrl = rtsusb->setup_buf;
	int ret = 1;

	RTS_DEBUG("%s()\n", __func__);

	rtsusb->ep_in[0]->dir_in = ctrl->bRequestType & USB_DIR_IN;
	switch (ctrl->bRequestType & USB_TYPE_MASK) {
	case USB_TYPE_STANDARD:
		ret = rts_usb_ep0_standard_request(rtsusb, ctrl);
		break;
	default:
		ret = 1;
		break;
	}
	return ret;
}

static void rts_ep0_transfer_process(struct rts_endpoint *priv_ep)
{
	struct rts_request *priv_req = NULL;
	u32 *buffer;
	u16 length;
	int i;

	RTS_DEBUG("%s() -> ep%d\n", __func__, priv_ep->epnum);

	if (!priv_ep || list_empty(&priv_ep->queue))
		return;

	priv_req = list_entry(priv_ep->queue.next, struct rts_request, queue);
	if (priv_req->request.length != priv_req->request.actual) {
		if (priv_ep->dir_in) {
			priv_req->request.actual +=
			priv_req->ep0_in_last_length;
		} else {
			buffer = priv_req->request.buf +
				 priv_req->request.actual;
			length = priv_req->request.length -
				 priv_req->request.actual;
			if (length > priv_ep->endpoint.maxpacket)
				length = priv_ep->endpoint.maxpacket;
			for (i = 0; i <= (length - 1) / 4; i++) {
				*buffer = mc_read_reg(EP0_BASE + i * 4);
				buffer++;
			}
			priv_req->request.actual += length;
			RTS_DEBUG("continue actual %d request.length %d\n",
			priv_req->request.actual, priv_req->request.length);
		}
	}

	if ((priv_req->request.length == priv_req->request.actual)
	    || (priv_req->request.actual < priv_ep->endpoint.maxpacket))
		rts_done(priv_ep, priv_req, 0);
	else
		rts_start_ep0_transfer(priv_ep, priv_req);
}

static int rts_usb_ep0_irq(struct rts_udc *rtsusb)
{
	int ret = 0;
	u32 int_val;

	RTS_DEBUG("%s()\n", __func__);

	int_val = (usb_read_reg(USB_IRQ_EN) & usb_read_reg(USB_IRQ_STATUS));
	RTS_DEBUG("interrupt_val %#x\n", int_val);
	if (int_val & BIT(I_SETUPF_OFFSET)) {
		usb_set_reg_bit(I_EP0OUTF_OFFSET, USB_IRQ_STATUS);
		RTS_DEBUG("\nrecieve setup irq\n");

		rtsusb->setup_buf->bRequestType =
			usb_read_reg(USB_EP0_SETUP_DATA0) & 0xff;
		rtsusb->setup_buf->bRequest =
			(usb_read_reg(USB_EP0_SETUP_DATA0) >> 8) & 0xff;
		rtsusb->setup_buf->wValue =
			(usb_read_reg(USB_EP0_SETUP_DATA0) >> 16) & 0xffff;
		rtsusb->setup_buf->wIndex =
			usb_read_reg(USB_EP0_SETUP_DATA1) & 0xffff;
		rtsusb->setup_buf->wLength =
			(usb_read_reg(USB_EP0_SETUP_DATA1) >> 16) & 0xffff;

		usb_set_reg_bit(I_SETUPF_OFFSET, USB_IRQ_STATUS);
		ret = rts_usb_setup_process(rtsusb);
		if (ret) {
			spin_unlock(&rtsusb->lock);
			if (rtsusb->gadget_driver->setup(&rtsusb->gadget,
							 rtsusb->setup_buf) < 0)
				rts_set_cxstall(rtsusb);
			spin_lock(&rtsusb->lock);
		}
	}

	if (int_val & BIT(I_EP0INF_OFFSET)) {
		RTS_DEBUG("\nrecieve ep0 in transmitted irq\n");
		usb_set_reg_bit(I_EP0INF_OFFSET, USB_IRQ_STATUS);
		rts_ep0_transfer_process(rtsusb->ep_in[0]);
	}

	if (int_val & BIT(I_EP0OUTF_OFFSET)) {
		RTS_DEBUG("\nrecieve ep0 out received irq\n");
		usb_set_reg_bit(I_EP0OUTF_OFFSET, USB_IRQ_STATUS);
		rts_ep0_transfer_process(rtsusb->ep_in[0]);
	}

	return ret;
}

static void rts_transfer_complete(struct rts_endpoint *priv_ep,
				  struct rts_request *priv_req)
{
	RTS_DEBUG("%s() -> ep%d mc%d\n", __func__,
		  priv_ep->epnum, priv_ep->mcnum);

	if (priv_ep->dir_in) {
		priv_req->request.actual += priv_req->request.length;
	} else {
		RTS_DEBUG("dma done! dmaout_length %d\n",
		mc_read_reg(MC_FIFO0_DMAOUT_LENGTH + 0x100 * priv_ep->mcnum));
		priv_req->request.actual +=
		mc_read_reg(MC_FIFO0_DMAOUT_LENGTH + 0x100 * priv_ep->mcnum);
	}
}

static void rts_start_next_request(struct rts_endpoint *priv_ep)
{
	struct rts_request *priv_req;

	RTS_DEBUG("%s() -> ep%d mc%d\n", __func__,
		  priv_ep->epnum, priv_ep->mcnum);

	if (!list_empty(&priv_ep->queue)) {
		priv_req = list_entry(priv_ep->queue.next,
				      struct rts_request, queue);
		if (priv_ep->epnum > 6 && priv_ep->dir_in) //intr ep
			rts_start_intr_transfer(priv_ep, priv_req);
		else
			rts_start_dma_transfer(priv_ep, priv_req);
	}
}

static void rts_intr_transfer_process(struct rts_endpoint *priv_ep,
				      struct rts_request *priv_req)
{
	RTS_DEBUG("%s() -> ep%d\n", __func__, priv_ep->epnum);

	if (priv_req->request.length != priv_req->request.actual)
		if (priv_ep->dir_in)
			priv_req->request.actual +=
						priv_req->intr_in_last_length;

	if ((priv_req->request.length == priv_req->request.actual)
	    || (priv_req->request.actual < priv_ep->endpoint.maxpacket)) {
		rts_done(priv_ep, priv_req, 0);
		rts_start_next_request(priv_ep);
	} else
		rts_start_intr_transfer(priv_ep, priv_req);
}

static void rts_usb_intr_in_process(struct rts_endpoint *priv_ep)
{
	struct rts_request *priv_req = NULL;

	if (!priv_ep || list_empty(&priv_ep->queue))
		return;

	priv_req = list_entry(priv_ep->queue.next, struct rts_request, queue);

	RTS_DEBUG("%s() -> ep%d length %d\n", __func__,
		  priv_ep->epnum, priv_req->request.length);

	if (priv_req->request.length)
		rts_intr_transfer_process(priv_ep, priv_req);
}

static int rts_usb_intrep_irq(struct rts_udc *rtsusb)
{
	u32 int_val;
	int epnum;

	RTS_DEBUG("%s()\n", __func__);

	for (epnum = 7; epnum < 13; epnum++) {
		if (rtsusb->ep_in[epnum]->ep_enable) {
			int_val = usb_read_reg(USB_INTEREPA_IRQ_STATUS +
					       (epnum - 7) * 0x80);
			if (int_val & BIT(I_INTEP_INF_OFFSET)) {
				RTS_DEBUG("irq: intr ep%d irq val %#x\n",
				epnum, int_val);
				rts_usb_intr_in_process(rtsusb->ep_in[epnum]);
				usb_set_reg_bit(I_INTEP_INF_OFFSET,
				USB_INTEREPA_IRQ_STATUS + (epnum - 7) * 0x80);
			}
		}
	}

	return 0;
}

static void rts_usb_bulk_in_process(struct rts_endpoint *priv_ep)
{
	struct rts_request *priv_req = NULL;
	int cnt;

	RTS_DEBUG("%s() -> ep%d\n", __func__, priv_ep->epnum);

	if (!priv_ep || list_empty(&priv_ep->queue))
		return;

	priv_req = list_entry(priv_ep->queue.next, struct rts_request, queue);
	if (priv_req->request.length)
		rts_transfer_complete(priv_ep, priv_req);
	rts_done_wq(priv_ep, priv_req, 0);
	cnt = 10000;
	while (cnt--) {
		if (mc_read_reg(MC_FIFO0_BC + 0x100 * priv_ep->mcnum) == 0)
			break;
	}
	rts_start_next_request(priv_ep);
}

static void rts_usb_bulk_in_process_ep1(struct work_struct *work)
{
	struct rts_udc *rtsusb = container_of(work, struct rts_udc,
					      bulkin_work[0]);
	struct rts_endpoint *priv_ep = rtsusb->ep_in[1];

	rts_usb_bulk_in_process(priv_ep);
}

static void rts_usb_bulk_in_process_ep2(struct work_struct *work)
{
	struct rts_udc *rtsusb = container_of(work, struct rts_udc,
					      bulkin_work[1]);
	struct rts_endpoint *priv_ep = rtsusb->ep_in[2];

	rts_usb_bulk_in_process(priv_ep);
}

static void rts_usb_bulk_in_process_ep3(struct work_struct *work)
{
	struct rts_udc *rtsusb = container_of(work, struct rts_udc,
					      bulkin_work[2]);
	struct rts_endpoint *priv_ep = rtsusb->ep_in[3];

	rts_usb_bulk_in_process(priv_ep);
}

static int rts_usb_bulkinep_irq(struct rts_udc *rtsusb)
{
	u32 int_val;
	int epnum;

	RTS_DEBUG("%s()\n", __func__);

	for (epnum = 1; epnum < 4; epnum++) {
		if (rtsusb->ep_in[epnum]->ep_enable) {
			int_val = mc_read_reg(MC_FIFO0_IRQ + 0x100 *
				  rtsusb->ep_in[epnum]->mcnum);
			if (int_val & BIT(INT_LASTPKT_DONE_OFFSET)) {
				RTS_DEBUG("irq: bulk in ep%d lastpkt done\n",
					  epnum);
				mc_set_reg_bit(INT_LASTPKT_DONE_OFFSET,
				MC_FIFO0_IRQ +
				0x100 * rtsusb->ep_in[epnum]->mcnum);
				queue_work(rtsusb->udc_wq,
					   &rtsusb->bulkin_work[epnum - 1]);
			}
		}
	}

	return 0;
}

static void rts_usb_bulk_out_process(struct rts_endpoint *priv_ep)
{
	struct rts_request *priv_req = NULL;

	RTS_DEBUG("%s() -> ep%d\n", __func__, priv_ep->epnum);

	if (!priv_ep || list_empty(&priv_ep->queue))
		return;

	priv_req = list_entry(priv_ep->queue.next, struct rts_request, queue);
	rts_transfer_complete(priv_ep, priv_req);
	rts_done(priv_ep, priv_req, 0);
	rts_start_next_request(priv_ep);
}

static int rts_usb_bulkoutep_irq(struct rts_udc *rtsusb)
{
	u32 int_val;
	int epnum;

	RTS_DEBUG("%s()\n", __func__);

	for (epnum = 1; epnum < 4; epnum++) {
		if (rtsusb->ep_out[epnum]->ep_enable) {
			int_val = mc_read_reg(MC_FIFO0_IRQ + 0x100 *
					      rtsusb->ep_out[epnum]->mcnum);
			if (int_val & BIT(INT_DMA_DONE_OFFSET)) {
				RTS_DEBUG("irq: bulk out ep%d dma done\n",
					  epnum);
				usb_write_reg(0xe, USB_BULKOUTEPA_IRQ_STATUS +
					      (epnum - 1) * 0x40);
				mc_set_reg_bit(INT_DMA_DONE_OFFSET, MC_FIFO0_IRQ
					+ 0x100 * rtsusb->ep_out[epnum]->mcnum);
				rts_usb_bulk_out_process(rtsusb->ep_out[epnum]);
			}
		}
	}

	return 0;
}

static void rts_usb_uac_in_process(struct rts_endpoint *priv_ep)
{
	struct rts_request *priv_req = NULL;

	RTS_DEBUG("%s()\n", __func__);

	if (!priv_ep || list_empty(&priv_ep->queue))
		return;

	priv_req = list_entry(priv_ep->queue.next, struct rts_request, queue);
	if (priv_req->request.length)
		rts_transfer_complete(priv_ep, priv_req);
	rts_done(priv_ep, priv_req, 0);
	rts_start_next_request(priv_ep);
}

static int rts_usb_uacinep_irq(struct rts_udc *rtsusb)
{
	u32 int_val;

	RTS_DEBUG("%s()\n", __func__);

	if (rtsusb->ep_in[4]->ep_enable) {
		int_val = mc_read_reg(MC_FIFO0_IRQ + 0x100 *
				      rtsusb->ep_in[4]->mcnum);
		if (int_val & BIT(INT_LASTPKT_DONE_OFFSET)) {
			RTS_DEBUG("irq: uac in ep4 lastpkt done\n");
			mc_set_reg_bit(INT_LASTPKT_DONE_OFFSET, MC_FIFO0_IRQ +
				       0x100 * rtsusb->ep_in[4]->mcnum);
			rts_usb_uac_in_process(rtsusb->ep_in[4]);
		}
	}

	return 0;
}

static void rts_usb_uac_out_process(struct rts_endpoint *priv_ep)
{
	struct rts_request *priv_req = NULL;

	RTS_DEBUG("%s()\n", __func__);

	if (!priv_ep || list_empty(&priv_ep->queue))
		return;

	priv_req = list_entry(priv_ep->queue.next, struct rts_request, queue);
	rts_transfer_complete(priv_ep, priv_req);
	rts_done(priv_ep, priv_req, 0);
	rts_start_next_request(priv_ep);
}

static int rts_usb_uacoutep_irq(struct rts_udc *rtsusb)
{
	u32 int_val;

	RTS_DEBUG("%s()\n", __func__);

	if (rtsusb->ep_out[4]->ep_enable) {
		int_val = mc_read_reg(MC_FIFO0_IRQ + 0x100 *
				      rtsusb->ep_out[4]->mcnum);
		if (int_val & BIT(INT_DMA_DONE_OFFSET)) {
			RTS_DEBUG("irq: uac out ep4 dma done\n");
			mc_set_reg_bit(INT_DMA_DONE_OFFSET, MC_FIFO0_IRQ +
				       0x100 * rtsusb->ep_out[4]->mcnum);
			rts_usb_uac_out_process(rtsusb->ep_out[4]);
		}
	}

	return 0;
}

static void rts_usb_uvc_in_process(struct rts_endpoint *priv_ep)
{
	struct rts_request *priv_req = NULL;

	RTS_DEBUG("%s()\n", __func__);

	if (!priv_ep || list_empty(&priv_ep->queue))
		return;

	priv_req = list_entry(priv_ep->queue.next, struct rts_request, queue);
	if (priv_req->request.length)
		rts_transfer_complete(priv_ep, priv_req);
	rts_done_wq(priv_ep, priv_req, 0);
	rts_start_next_request(priv_ep);
}

static void rts_usb_uvc_in_process_ep5(struct work_struct *work)
{
	struct rts_udc *rtsusb = container_of(work, struct rts_udc,
					      uvcin_work[0]);
	struct rts_endpoint *priv_ep = rtsusb->ep_in[5];

	rts_usb_uvc_in_process(priv_ep);
}

static void rts_usb_uvc_in_process_ep6(struct work_struct *work)
{
	struct rts_udc *rtsusb = container_of(work, struct rts_udc,
					      uvcin_work[1]);
	struct rts_endpoint *priv_ep = rtsusb->ep_in[6];

	rts_usb_uvc_in_process(priv_ep);
}

static int rts_usb_uvcinep_irq(struct rts_udc *rtsusb)
{
	u32 int_val;
	int epnum;

	RTS_DEBUG("%s()\n", __func__);

	for (epnum = 5; epnum < 7; epnum++) {
		if (rtsusb->ep_in[epnum]->ep_enable) {
			int_val = mc_read_reg(MC_FIFO0_IRQ + 0x100 *
					      rtsusb->ep_in[epnum]->mcnum);
			if (int_val & BIT(INT_LASTPKT_DONE_OFFSET)) {
				RTS_DEBUG("irq: uvc in ep%d lastpkt done\n",
					  epnum);
				mc_set_reg_bit(INT_LASTPKT_DONE_OFFSET,
					       MC_FIFO0_IRQ + 0x100 *
					       rtsusb->ep_in[epnum]->mcnum);
				mc_set_reg_bit(INT_DMA_DONE_OFFSET,
					       MC_FIFO0_IRQ + 0x100 *
					       rtsusb->ep_in[epnum]->mcnum);
				rtsusb->uvcin_epnum = epnum;
				queue_work(rtsusb->udc_wq,
					   &rtsusb->uvcin_work[epnum - 5]);
			}
		}
	}

	return 0;
}

static void rts_usb_se0_irq(struct rts_udc *rtsusb)
{
	u32 int_val;

	RTS_DEBUG("%s()\n", __func__);

	int_val = (usb_read_reg(USB_IRQ_EN) & usb_read_reg(USB_IRQ_STATUS));
	if (int_val & BIT(I_SE0RSTF_OFFSET)) {
		usb_set_reg_bit(I_SE0RSTF_OFFSET, USB_IRQ_STATUS);
		RTS_DEBUG("\nrecieve se0 irq\n");
		spin_unlock(&rtsusb->lock);
		if (rtsusb->gadget_driver)
			usb_gadget_udc_reset(&rtsusb->gadget,
					     rtsusb->gadget_driver);
		spin_lock(&rtsusb->lock);
	}
}

static int rts_usb_ep_irq(void *dev)
{
	struct rts_udc *rtsusb = (struct rts_udc *)dev;

	RTS_DEBUG("%s()\n", __func__);

	spin_lock(&rtsusb->lock);

	rts_usb_se0_irq(rtsusb);

	rts_usb_ep0_irq(rtsusb);

	rts_usb_intrep_irq(rtsusb);

	rts_usb_bulkinep_irq(rtsusb);

	rts_usb_bulkoutep_irq(rtsusb);

	rts_usb_uacinep_irq(rtsusb);

	rts_usb_uacoutep_irq(rtsusb);

	rts_usb_uvcinep_irq(rtsusb);

	spin_unlock(&rtsusb->lock);

	return IRQ_HANDLED;
}

static irqreturn_t rts_usb_common_irq(int irq, void *dev)
{
	int ret = IRQ_NONE;
	struct rts_udc *rtsusb = (struct rts_udc *)dev;

	RTS_DEBUG("%s() ~~start~~\n", __func__);
#ifdef CONFIG_RTS3917_SUSPEND_TO_RAM
	if (rtsusb->suspend_work.work.func) {
		cancel_delayed_work(&rtsusb->suspend_work);
		schedule_delayed_work(&rtsusb->suspend_work,
					msecs_to_jiffies(1000));
	}
#endif

	ret = rts_usb_ep_irq(dev);

	RTS_DEBUG("%s() ~~end~~\n", __func__);
	RTS_DEBUG("\n\n\n");
	return IRQ_RETVAL(ret);
}

static irqreturn_t rts_usb_phy_irq(int irq, void *dev)
{
	int ret = IRQ_NONE;
	struct rts_udc *rtsusb = (struct rts_udc *)dev;
	u32 int_val;

	RTS_DEBUG("%s() ~~start~~\n", __func__);
	int_val = rts_usb_read_phy_int();
	switch (int_val & UPHY_DEV_PORT_VBUS_INT_MSK) {
	case UPHY_DEV_PORT_VBUS_ON_INT:
		pr_debug("clear usb phy power on intp 0x1824001c=0x%-8x\n",
			 int_val);
		rts_usb_clear_phy_int(UPHY_DEV_PORT_VBUS_DOWN_INT);
		usb_gadget_vbus_connect(&rtsusb->gadget);
		ret = IRQ_HANDLED;
		break;
	case UPHY_DEV_PORT_VBUS_DOWN_INT:
		pr_debug("clear usb phy power down intp 0x1824001c=0x%-8x\n",
			 int_val);
		rts_usb_clear_phy_int(UPHY_DEV_PORT_VBUS_ON_INT);
		usb_gadget_vbus_disconnect(&rtsusb->gadget);
		ret = IRQ_HANDLED;
		break;
	case UPHY_DEV_PORT_VBUS_INT_MSK:
		pr_debug("usb phy power on/down intp ");
		pr_debug("change VBUS deglich time!\n");
		rts_usb_clear_phy_int(UPHY_DEV_PORT_VBUS_NULL_MSK);
		usb_gadget_vbus_disconnect(&rtsusb->gadget);
		ret = IRQ_HANDLED;
		break;
	}

	return ret;
}

/**
 * rts_usb_init
 * init usb irq, configs
 */
static int rts_usb_init(struct rts_udc *rtsusb)
{
	RTS_DEBUG("%s()\n", __func__);

	usb_write_reg(0, USB_IRQ_EN);
	mc_write_reg(0, R_EP0_MC_BUF_CTL);
	mc_write_reg(0x1, R_MC_DEV_CFG); /// select OTG???
	usb_set_reg_bit(IE_SETUP_OFFSET, USB_IRQ_EN);
	usb_set_reg_bit(IE_SE0RST_OFFSET, USB_IRQ_EN);
	usb_write_reg(0xFFFFFFFF, USB_IRQ_STATUS);
	usb_set_reg_bit(EP0_RESET_OFFSET, USB_EP_CTL0);
	usb_write_reg(USB_EP0_MAX_PKT_SIZE, USB_EP_MAXPKT0);
	usb_clr_reg_bit(EP0_NAKOUT_MODE_OFFSET, USB_EP_CFG0);

	return 0;
}

/**
 * rts_gadget_init_endpoints
 * Initialize the EP structure.
 */
static int rts_gadget_init_endpoints(struct rts_udc *rtsusb)
{
	static const char * const ep_in_names[] = {
		"ep0",
		"ep1in",
		"ep2in",
		"ep3in",
		"ep4in",
		"ep5in",
		"ep6in",
		"ep7in",
		"ep8in",
		"ep9in",
		"ep10in",
		"ep11in",
		"ep12in",
	};

	static const char * const ep_out_names[] = {
		"ep0",
		"ep1out",
		"ep2out",
		"ep3out",
		"ep4out",
	};
	int i;

	RTS_DEBUG("%s()\n", __func__);

	for (i = 0; i < RTS_EP_IN_MAX_COUNT; i++) {
		struct rts_endpoint *ep = rtsusb->ep_in[i];

		if (i)
			list_add_tail(&ep->endpoint.ep_list,
				      &rtsusb->gadget.ep_list);

		ep->rts_dev = rtsusb;
		INIT_LIST_HEAD(&ep->queue);
		ep->endpoint.name = ep_in_names[i];
		ep->endpoint.ops = &rts_ep_ops;
		usb_ep_set_maxpacket_limit(&ep->endpoint, (unsigned short) ~0);

		switch (i) {
		case 0: /// control ep
			ep->endpoint.caps.type_control = true;
			ep->endpoint.caps.dir_in = true; /// ep0支持in and out
			ep->endpoint.caps.dir_out = true;
			usb_ep_set_maxpacket_limit(&ep->endpoint, 0x40); /// 64 bytes
			break;
		case 1 ... 3: /// bulk in ep
			ep->endpoint.caps.type_bulk = true;
			ep->endpoint.caps.dir_in = true;
			usb_ep_set_maxpacket_limit(&ep->endpoint, 0x200); /// 512 bytes
			break;
		case 4: /// uac isochronous ep
			ep->endpoint.caps.type_iso = true;
			ep->endpoint.caps.dir_in = true;
			usb_ep_set_maxpacket_limit(&ep->endpoint, 0x400); /// 1024 bytes
			break;
		case 5 ... 6: /// uvc isochronous ep
			ep->endpoint.caps.type_iso = true;
			ep->endpoint.caps.dir_in = true;
			usb_ep_set_maxpacket_limit(&ep->endpoint, 0x400); /// 1024 bytes
			break;
		case 7 ... 12: /// interrupt ep
			ep->endpoint.caps.type_int = true;
			ep->endpoint.caps.dir_in = true;
			usb_ep_set_maxpacket_limit(&ep->endpoint, 0x40); /// 64 bytes
			break;
		default:
			break;
		}

	}

	for (i = 1; i < RTS_EP_OUT_MAX_COUNT; i++) {
		struct rts_endpoint *ep = rtsusb->ep_out[i];

		list_add_tail(&ep->endpoint.ep_list,
			      &rtsusb->gadget.ep_list);
		ep->rts_dev = rtsusb;
		INIT_LIST_HEAD(&ep->queue);
		ep->endpoint.name = ep_out_names[i];
		ep->endpoint.ops = &rts_ep_ops;
		usb_ep_set_maxpacket_limit(&ep->endpoint, (unsigned short) ~0);

		switch (i) {
		case 1 ... 3: /// bulk out
			ep->endpoint.caps.type_bulk = true;
			ep->endpoint.caps.dir_out = true;
			usb_ep_set_maxpacket_limit(&ep->endpoint, 0x200);
			break;
		case 4: /// uac isochronous out
			ep->endpoint.caps.type_iso = true;
			ep->endpoint.caps.dir_out = true;
			usb_ep_set_maxpacket_limit(&ep->endpoint, 0x400);
			break;
		default:
			break;
		}

	}

	rtsusb->gadget.ep0 = &rtsusb->ep_in[0]->endpoint;
	INIT_LIST_HEAD(&rtsusb->gadget.ep0->ep_list);

	return 0;
}

static int rts_gadget_free_endpoints(struct rts_udc *rtsusb)
{
	u8 i;

	RTS_DEBUG("%s()\n", __func__);

	for (i = 0; i < RTS_EP_IN_MAX_COUNT; i++)
		list_del(&rtsusb->ep_in[i]->queue);
	for (i = 1; i < RTS_EP_OUT_MAX_COUNT; i++)
		list_del(&rtsusb->ep_out[i]->queue);
	return 0;
}

static int rts_gadget_get_frame(struct usb_gadget *g)
{
	RTS_DEBUG("%s()\n", __func__);

	return -EOPNOTSUPP;
}

static int rts_gadget_wakeup(struct usb_gadget *g)
{
	struct rts_udc *rtsusb = gadget_to_rts(g);
	unsigned long flags;

	RTS_DEBUG("%s()\n", __func__);

	spin_lock_irqsave(&rtsusb->lock, flags);
	usb_set_reg_bit(WAKEUP_EN_OFFSET, USB_CTRL);
	spin_unlock_irqrestore(&rtsusb->lock, flags);

	return 0;
}

static int rts_gadget_set_selfpowered(struct usb_gadget *g, int is_selfpowered)
{
	struct rts_udc *rtsusb = gadget_to_rts(g);
	unsigned long flags;

	RTS_DEBUG("%s()\n", __func__);

	spin_lock_irqsave(&rtsusb->lock, flags);
	g->is_selfpowered = !!is_selfpowered;
	if (is_selfpowered)
		rtsusb->devstatus |= (1 << USB_DEVICE_SELF_POWERED);
	else
		rtsusb->devstatus &= ~(1 << USB_DEVICE_SELF_POWERED);
	spin_unlock_irqrestore(&rtsusb->lock, flags);

	return 0;
}

static int rts_gadget_vbus_session(struct usb_gadget *g, int is_active)
{
	struct rts_udc *rtsusb = gadget_to_rts(g);
	unsigned long flags;

	RTS_DEBUG("%s()\n", __func__);

	spin_lock_irqsave(&rtsusb->lock, flags);
	rtsusb->vbuson = (is_active != 0);
	if (rtsusb->gadgetstart && rtsusb->vbuson)
		rts_usb_connect(rtsusb);
	else if (rtsusb->gadgetstart == 0 || !rtsusb->vbuson)
		rts_usb_disconnect(rtsusb);
	spin_unlock_irqrestore(&rtsusb->lock, flags);

	return 0;
}

static int rts_gadget_pullup(struct usb_gadget *g, int is_on)
{
	struct rts_udc *rtsusb = gadget_to_rts(g);
	unsigned long flags;

	RTS_DEBUG("%s()\n", __func__);

	spin_lock_irqsave(&rtsusb->lock, flags);

	rtsusb->gadgetstart = is_on = !!is_on;
	if (rtsusb->gadgetstart && rtsusb->vbuson)
		rts_usb_connect(rtsusb);
	else if (rtsusb->gadgetstart == 0)
		rts_usb_disconnect(rtsusb);

	spin_unlock_irqrestore(&rtsusb->lock, flags);

	return 0;
}

static int rts_gadget_udc_start(struct usb_gadget *g,
				struct usb_gadget_driver *gadget_driver)
{
	struct rts_udc *rtsusb = gadget_to_rts(g);
	unsigned long flags;

	RTS_DEBUG("%s()\n", __func__);

	spin_lock_irqsave(&rtsusb->lock, flags);
	/*
	 * Enable usb phy vbus interrupt.
	 */
	rts_usb_enable_phy_int(rtsusb);

	if (rtsusb->gadget_driver) {
		dev_err(rtsusb->dev, "%s is already bound to %s\n",
			rtsusb->gadget.name,
			rtsusb->gadget_driver->driver.name);
		return -EBUSY;
	}
	rtsusb->gadget_driver = gadget_driver;
	rtsusb->gadget.dev.driver = &gadget_driver->driver;
	rtsusb->gadgetstart = 1;

	spin_unlock_irqrestore(&rtsusb->lock, flags);

	return 0;
}

static int rts_gadget_udc_stop(struct usb_gadget *g)
{
	struct rts_udc *rtsusb = gadget_to_rts(g);
	unsigned long flags;

	RTS_DEBUG("%s()\n", __func__);

	spin_lock_irqsave(&rtsusb->lock, flags);

	rtsusb->gadget_driver = NULL;
	rtsusb->gadget.dev.driver = NULL;
	rtsusb->gadgetstart = 0;

	spin_unlock_irqrestore(&rtsusb->lock, flags);

	return 0;
}

static struct usb_ep *_get_iso_in_ep(struct usb_gadget *g)
{
	struct usb_ep *ep = NULL;

	RTS_DEBUG("%s()\n", __func__);

	ep = gadget_find_ep_by_name(g, "ep4in");
	if (ep && !ep->claimed)
		return ep;
	return NULL;
}

static struct usb_ep *_get_iso_out_ep(struct usb_gadget *g)
{
	struct usb_ep *ep = NULL;

	RTS_DEBUG("%s()\n", __func__);

	ep = gadget_find_ep_by_name(g, "ep4out");
	if (ep && !ep->claimed)
		return ep;
	return NULL;
}

static struct usb_ep *_get_bulk_in_ep(struct usb_gadget *g)
{
	struct usb_ep *ep = NULL;

	RTS_DEBUG("%s()\n", __func__);

	ep = gadget_find_ep_by_name(g, "ep1in");
	if (ep && !ep->claimed)
		return ep;
	ep = gadget_find_ep_by_name(g, "ep2in");
	if (ep && !ep->claimed)
		return ep;
	ep = gadget_find_ep_by_name(g, "ep3in");
	if (ep && !ep->claimed)
		return ep;
	return NULL;
}

static struct usb_ep *_get_bulk_out_ep(struct usb_gadget *g)
{
	struct usb_ep *ep = NULL;

	RTS_DEBUG("%s()\n", __func__);

	ep = gadget_find_ep_by_name(g, "ep1out");
	if (ep && !ep->claimed)
		return ep;
	ep = gadget_find_ep_by_name(g, "ep2out");
	if (ep && !ep->claimed)
		return ep;
	ep = gadget_find_ep_by_name(g, "ep3out");
	if (ep && !ep->claimed)
		return ep;
	return NULL;
}

static struct usb_ep *_get_intr_in_ep(struct usb_gadget *g)
{
	struct usb_ep *ep = NULL;

	RTS_DEBUG("%s()\n", __func__);

	ep = gadget_find_ep_by_name(g, "ep7in");
	if (ep && !ep->claimed)
		return ep;
	ep = gadget_find_ep_by_name(g, "ep8in");
	if (ep && !ep->claimed)
		return ep;
	ep = gadget_find_ep_by_name(g, "ep9in");
	if (ep && !ep->claimed)
		return ep;
	ep = gadget_find_ep_by_name(g, "ep10in");
	if (ep && !ep->claimed)
		return ep;
	ep = gadget_find_ep_by_name(g, "ep11in");
	if (ep && !ep->claimed)
		return ep;
	ep = gadget_find_ep_by_name(g, "ep12in");
	if (ep && !ep->claimed)
		return ep;
	return NULL;
}

static struct usb_ep *rts_gadget_match_ep(struct usb_gadget *g,
				struct usb_endpoint_descriptor *desc,
				struct usb_ss_ep_comp_descriptor *ep_comp)
{
	u8 type;
	struct usb_ep *ep = NULL;

	RTS_DEBUG("%s()\n", __func__);

	type = usb_endpoint_type(desc);

	/*
	 * ep0 - CONTROL
	 *
	 * ep1in - BULK IN
	 * ep2in - BULK IN
	 * ep3in - BULK IN
	 * ep4in - UAC ISO IN
	 * ep5in - UVC ISO IN
	 * ep6in - UVC ISO IN
	 * ep7in - INTERRUPT IN
	 * ep8in - INTERRUPT IN
	 * ep9in - INTERRUPT IN
	 * ep10in - INTERRUPT IN
	 * ep11in - INTERRUPT IN
	 * ep12in - INTERRUPT IN
	 *
	 * ep1out - BULK OUT
	 * ep2out - BULK OUT
	 * ep3out - BULK OUT
	 * ep4out - UAC ISO OUT
	 */

	switch (type) {
	case USB_ENDPOINT_XFER_ISOC:
		if (USB_DIR_IN & desc->bEndpointAddress)
			ep = _get_iso_in_ep(g);
		else
			ep = _get_iso_out_ep(g);
		break;
	case USB_ENDPOINT_XFER_BULK:
		if (USB_DIR_IN & desc->bEndpointAddress)
			ep = _get_bulk_in_ep(g);
		else
			ep = _get_bulk_out_ep(g);
		break;
	case USB_ENDPOINT_XFER_INT:
		if (USB_DIR_IN & desc->bEndpointAddress)
			ep = _get_intr_in_ep(g);
		break;
	default:
		break;
	}

	if (ep)
		return ep;

	return NULL;
}
#define RTSX_DFU_IOC_MAGIC	0x77
#define DFU_IOC_SOFT_RESET	_IOW(RTSX_DFU_IOC_MAGIC, 0x04, __u32)

void rts_usb_device_soft_reset(struct usb_gadget *gadget)
{
	struct rts_udc *rtsusb = container_of(gadget,
			struct rts_udc, gadget);
	if (!rtsusb)
		return;

	rts_usb_disconnect(rtsusb);
	mdelay(5);
	rts_usb_connect(rtsusb);
}

static int rts_gadget_ioctl(struct usb_gadget *gadget,
			unsigned int code, unsigned long param)
{
	switch (code) {
	case DFU_IOC_SOFT_RESET:
		rts_usb_device_soft_reset(gadget);
		break;
	default:
		return -1;
	}
	return 0;
}

static const struct usb_gadget_ops rts_gadget_ops = {
	.get_frame		= rts_gadget_get_frame,
	.wakeup			= rts_gadget_wakeup,
	.set_selfpowered	= rts_gadget_set_selfpowered,
	.vbus_session		= rts_gadget_vbus_session,
	.pullup			= rts_gadget_pullup,
	.udc_start		= rts_gadget_udc_start,
	.udc_stop		= rts_gadget_udc_stop,
	.match_ep		= rts_gadget_match_ep,
	.ioctl			= rts_gadget_ioctl,
};

static void rts_usb_suspend(struct work_struct *work)
{
	pm_suspend(PM_SUSPEND_MEM);
}

static int rts_usb_driver_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct resource *res;
	struct reset_control *sysmem_sd;
	struct reset_control *rst;
	struct clk *phy_clk;
	struct rts_udc *rtsusb;
	struct rts_endpoint *ep_in[RTS_EP_IN_MAX_COUNT];
	struct rts_endpoint *ep_out[RTS_EP_OUT_MAX_COUNT];
	const struct of_device_id *of_id;
	int ret = 0, i;
	int val, wakeup;

	RTS_DEBUG("%s()\n", __func__);

	rtsusb = devm_kzalloc(dev, sizeof(*rtsusb), GFP_KERNEL);
	// if (!rtsusb)
	// 	return -ENOMEM;
	rtsusb->setup_buf = devm_kzalloc(dev, sizeof(struct usb_ctrlrequest),
					 GFP_KERNEL);
	// if (!rtsusb->setup_buf)
	// 	return -ENOMEM;

	for (i = 0; i < RTS_EP_IN_MAX_COUNT; i++) {
		ep_in[i] = devm_kzalloc(dev, sizeof(struct rts_endpoint),
					GFP_KERNEL);
		// if (!ep_in[i])
		// 	return -ENOMEM;
		rtsusb->ep_in[i] = ep_in[i];
	}

	rtsusb->ep_out[0] = rtsusb->ep_in[0];

	for (i = 1; i < RTS_EP_OUT_MAX_COUNT; i++) {
		ep_out[i] = devm_kzalloc(dev, sizeof(struct rts_endpoint),
					 GFP_KERNEL);
		// if (!ep_out[i])
		// 	return -ENOMEM;
		rtsusb->ep_out[i] = ep_out[i];
	}

	for (i = 0; i < RTS_MCM_MAX_COUNT; i++) {
		mcm[i] = devm_kzalloc(dev, sizeof(struct rts_mcm), GFP_KERNEL);
		// if (!mcm[i])
		// 	return -ENOMEM;
	}

	/*
	 * enable usb phy clk
	 */
	phy_clk = devm_clk_get(dev, "usbphy_dev_ck");
	// if (!phy_clk) {
	// 	dev_err(dev, "get clk usbphy dev clk fail\n");
	// 	return -EINVAL;
	// }
	clk_prepare(phy_clk);
	clk_enable(phy_clk);

	rtsusb->bus_clk = devm_clk_get(dev, "bus_ck");
	// if (IS_ERR(rtsusb->bus_clk)) {
	// 	dev_err(dev, "get bus clk failed\n");
	// 	return -EINVAL;
	// }

	sysmem_sd = devm_reset_control_get(dev, "u2dev-sysmem-up");
	// if (!sysmem_sd) {
	// 	dev_err(dev, "not find u2dev sysmem sd control\n");
	// 	return -EINVAL;
	// }

	/*
	 * turn on mem of usb device
	 */
	reset_control_deassert(sysmem_sd);

	mdelay(5);

	rst = devm_reset_control_get(dev, "reset-usb-device");
	// if (!rst) {
	// 	dev_err(dev, "not find reset u2dev control\n");
	// 	return -EINVAL;
	// }

	/*
	 * Reset USB Dev
	 */
	reset_control_reset(rst);

	mdelay(5);

	rtsusb->irq = platform_get_irq(pdev, 0);
	// if (rtsusb->irq <= 0) {
	// 	dev_err(dev, "no device irq\n");
	// 	return -EINVAL;
	// }
	// dev_info(dev, "usb_dev->irq = %d\n", rtsusb->irq);

	rtsusb->phy_irq = platform_get_irq(pdev, 1);
	// if (rtsusb->phy_irq <= 0) {
	// 	dev_err(dev, "no device irq\n");
	// 	return -EINVAL;
	// }
	// dev_info(dev, "phy_irq = %d\n", rtsusb->phy_irq);

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	// if (!res)
	// 	return -EINVAL;

	usb_base = devm_ioremap_resource(dev, res);
	// if (!usb_base)
	// 	return -ENOMEM;

	res = platform_get_resource(pdev, IORESOURCE_MEM, 1);
	// if (!res)
	// 	return -EINVAL;

	mc_base = devm_ioremap_resource(dev, res);
	// if (!mc_base)
	// 	return -ENOMEM;

	of_id = of_match_device(rts_usb_dt_ids, &pdev->dev);
	if (of_id) {
		rtsusb->devtype_data = of_id->data;
	} else if (platform_get_device_id(pdev)->driver_data) {
		rtsusb->devtype_data = (struct udc_devtype_data *)
			platform_get_device_id(pdev)->driver_data;
	} else {
		return -ENODEV;
	}

	/*
	 * Disable usb phy vbus interrupt.
	 */
	val = rts_usb_read_phy_int();
	rts_usb_disable_phy_int(rtsusb);

	spin_lock_init(&rtsusb->lock);

	platform_set_drvdata(pdev, rtsusb);
	rtsusb->dev = dev;

	rtsusb->gadget.ops = &rts_gadget_ops;
	rtsusb->gadget.name = "rts_gadget";
	rtsusb->gadget.max_speed = USB_SPEED_HIGH;
	rtsusb->gadget.dev.parent = dev;
	rtsusb->gadget.speed = USB_SPEED_UNKNOWN;

	/*
	 * initialize endpoint container
	 */
	INIT_LIST_HEAD(&rtsusb->gadget.ep_list);

	ret = rts_gadget_init_endpoints(rtsusb);
	// if (ret) {
	// 	dev_err(dev, "gadget init endpoints failed\n");
	// 	goto err_init_ep;
	// }

	rtsusb->ep0_req = rts_ep_alloc_request(&rtsusb->ep_in[0]->endpoint,
					       GFP_KERNEL);
	// if (rtsusb->ep0_req == NULL)
	// 	goto err_request;

	/*
	 * initialize usb register
	 */
	ret = rts_usb_init(rtsusb);
	// if (ret) {
	// 	dev_err(dev, "rts usb init failed\n");
	// 	goto err_init_usb;
	// }

	ret = devm_request_irq(dev, rtsusb->irq, rts_usb_common_irq,
			       IRQF_SHARED, "rts_usb", (void *)rtsusb);
	// if (ret) {
	// 	dev_err(dev, "request usb irq failed\n");
	// 	goto err;
	// }

	ret = devm_request_irq(dev, rtsusb->phy_irq, rts_usb_phy_irq,
					IRQF_SHARED, "rts_phy", (void *)rtsusb);
	// if (ret) {
	// 	dev_err(dev, "request phy irq failed\n");
	// 	goto err;
	// }

	ret = usb_add_gadget_udc(dev, &rtsusb->gadget);
	// if (ret) {
	// 	dev_err(dev, "register udc failed\n");
	// 	goto err;
	// }

	rtsusb->udc_wq = create_workqueue("udc_wq");
	// if (!rtsusb->udc_wq) {
	// 	ret = -ENOMEM;
	// 	goto err;
	// }

	INIT_WORK(&rtsusb->uvcin_work[0], rts_usb_uvc_in_process_ep5);
	INIT_WORK(&rtsusb->uvcin_work[1], rts_usb_uvc_in_process_ep6);
	INIT_WORK(&rtsusb->uvcin_done_work[0], rts_done_uvc_in_disable_ep5);
	INIT_WORK(&rtsusb->uvcin_done_work[1], rts_done_uvc_in_disable_ep6);
	INIT_WORK(&rtsusb->test_work, rts_usb_test_mode);
	INIT_WORK(&rtsusb->bulkin_work[0], rts_usb_bulk_in_process_ep1);
	INIT_WORK(&rtsusb->bulkin_work[1], rts_usb_bulk_in_process_ep2);
	INIT_WORK(&rtsusb->bulkin_work[2], rts_usb_bulk_in_process_ep3);

	dev_info(dev, "Initialized Realtek IPCam USB Device module\n");

	if ((val & UPHY_DEV_PORT_VBUS_INT_MSK) == UPHY_DEV_PORT_VBUS_ON_INT)
		usb_gadget_vbus_connect(&rtsusb->gadget);

	wakeup = of_property_read_bool(dev->of_node, "wakeup-source");
	device_init_wakeup(dev, wakeup);
	if (wakeup)
		INIT_DELAYED_WORK(&rtsusb->suspend_work, rts_usb_suspend);

	return ret;
err:
err_init_usb:
	rts_ep_free_request(&rtsusb->ep_in[0]->endpoint, rtsusb->ep0_req);
err_request:
	rts_gadget_free_endpoints(rtsusb);
err_init_ep:
	return ret;
}

static int rts_usb_driver_remove(struct platform_device *pdev)
{
	struct rts_udc *rtsusb = platform_get_drvdata(pdev);
	struct device *dev = &pdev->dev;

	usb_del_gadget_udc(&rtsusb->gadget);
	rts_ep_free_request(&rtsusb->ep_in[0]->endpoint, rtsusb->ep0_req);
	rts_gadget_free_endpoints(rtsusb);
	rts_gadget_pullup(&rtsusb->gadget, 0);
	destroy_workqueue(rtsusb->udc_wq);
	device_init_wakeup(dev, 0);

	return 0;
}

#ifdef CONFIG_PM_SLEEP
static int rts_usb_driver_suspend(struct device *dev)
{
	struct platform_device *pdev = to_platform_device(dev);
	struct rts_udc *rtsusb = platform_get_drvdata(pdev);

	if (device_may_wakeup(dev))
		enable_irq_wake(rtsusb->irq);

	return 0;
}

static int rts_usb_driver_resume(struct device *dev)
{
	struct platform_device *pdev = to_platform_device(dev);
	struct rts_udc *rtsusb = platform_get_drvdata(pdev);

	if (device_may_wakeup(dev))
		disable_irq_wake(rtsusb->irq);

	return 0;
}
#endif

static SIMPLE_DEV_PM_OPS(rts_usb_pm_ops,
			rts_usb_driver_suspend, rts_usb_driver_resume);

static struct platform_driver rts_usb_driver = {
	.driver = {
		.name = (char *)rts_usb_driver_name,
		.owner = THIS_MODULE,
		.of_match_table = of_match_ptr(rts_usb_dt_ids),
		.pm = &rts_usb_pm_ops,
	},
	.probe = rts_usb_driver_probe,
	.remove = rts_usb_driver_remove,
	.id_table = rts_usb_ids,
};

static int __init rts_usb_driver_init(void)
{
	return platform_driver_register(&rts_usb_driver);
}

module_init(rts_usb_driver_init);

static void __exit rts_usb_driver_cleanup(void)
{
	platform_driver_unregister(&rts_usb_driver);
}

module_exit(rts_usb_driver_cleanup);

MODULE_LICENSE("GPL v2");
MODULE_DESCRIPTION("Realtek USB dev driver");
