/* SPDX-License-Identifier: GPL-2.0-only */
/*
 *  Copyright (C) 2021 Realtek Semiconductor Corp.
 *  All Rights Reserved
 */
#include "regs.h"
#include "mc_regs.h"

#define RTS_USB_DRIVER_VERSION "v0.0"

// #define RTS_USB_DEBUG
#define RTS_USB "---RTS_USB: "
#ifdef RTS_USB_DEBUG
#define RTS_DEBUG(x...) \
	printk(KERN_DEBUG RTS_USB x)
#else
#define RTS_DEBUG(x...)
#endif

#define gadget_to_rts(g) (container_of(g, struct rts_udc, gadget))
#define ep_to_rts_ep(ep) (container_of(ep, struct rts_endpoint, endpoint))
#define to_rts_request(r) (container_of(r, struct rts_request, request))

#define RTS_EP_IN_MAX_COUNT				13
#define RTS_EP_OUT_MAX_COUNT				5
#define RTS_MCM_MAX_COUNT				8
#define USB_EP0_MAX_PKT_SIZE				0x40
#define UVC_HEAD					0x8c

#define UPHY_DEV_PORT_VBUS_INT_MSK	0x03
#define UPHY_DEV_PORT_VBUS_ON_INT	0x02
#define UPHY_DEV_PORT_VBUS_DOWN_INT	0x01
#define UPHY_DEV_PORT_VBUS_NULL_MSK	0x01

#define UDC_QUIRK_DYNAMIC_MAXPKTSIZE	BIT(1)
/*-------------------------------------------------------------------------*/
/* Used structs */
struct udc_devtype_data {
	u32 quirks;
};

struct rts_udc;

struct rts_endpoint {
	struct usb_ep				endpoint;
	struct rts_udc				*rts_dev;

	struct list_head			queue;
	unsigned				stall:1;
	unsigned				wedged:1;
	bool					stopped;
	bool					ep_enable;
	unsigned char				epnum;
	unsigned char				mcnum;
	unsigned char				type;
	unsigned char				dir_in;
	unsigned char				dir_out;
	const struct usb_endpoint_descriptor	*desc;
	/* for usc in ep */
	int					uac_cnt;
	bool					is_uac_in;
	u16					maxpkt;
	int					pid;
};

struct rts_mcm {
	unsigned char				epnum;
	unsigned char				dir_in;
	unsigned char				dir_out;
	bool					claimed;
};

struct rts_request {
	struct usb_request			request;
	struct list_head			queue;
	u16					ep0_in_last_length;
	u16					intr_in_last_length;
};

struct rts_udc {
	struct device			*dev;

	struct usb_gadget		gadget;
	struct usb_gadget_driver	*gadget_driver;

	/* device lock */
	spinlock_t			lock;

	struct usb_ctrlrequest		*setup_buf;

	unsigned int			irq;
	unsigned int			phy_irq;

	struct rts_endpoint		*ep_in[RTS_EP_IN_MAX_COUNT];
	struct rts_endpoint		*ep_out[RTS_EP_OUT_MAX_COUNT];

	struct usb_request		*ep0_req; /* for internal request */
	__le16				ep0_data;

	u8				devstatus;

	/* gadget start */
	u8				gadgetstart;
	/* vbus on */
	u8				vbuson;
	u32				request_pending;

	unsigned char			uvcin_epnum;
	struct workqueue_struct		*udc_wq;
	struct work_struct		uvcin_work[2];
	struct work_struct		uvcin_done_work[2];
	struct work_struct		test_work;
	int				test_mode;

	const struct udc_devtype_data	*devtype_data;
	struct delayed_work		suspend_work;
	struct work_struct		bulkin_work[3];
#define USB_FS_CLK			2140000
#define USB_HS_CLK			125000000
	struct clk			*bus_clk;
};
