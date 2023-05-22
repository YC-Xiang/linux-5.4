// SPDX-License-Identifier: GPL-2.0+
/*
 *  Base port operations for 8250/16550-type serial ports
 *
 *  Based on drivers/char/serial.c, by Linus Torvalds, Theodore Ts'o.
 *  Split from 8250_core.c, Copyright (C) 2001 Russell King.
 *
 * A note about mapbase / membase
 *
 *  mapbase is the physical address of the IO port.
 *  membase is an 'ioremapped' cookie.
 */

#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/ioport.h>
#include <linux/init.h>
#include <linux/console.h>
#include <linux/gpio/consumer.h>
#include <linux/sysrq.h>
#include <linux/delay.h>
#include <linux/platform_device.h>
#include <linux/tty.h>
#include <linux/ratelimit.h>
#include <linux/tty_flip.h>
#include <linux/serial.h>
#include <linux/serial_8250.h>
#include <linux/nmi.h>
#include <linux/mutex.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/pm_runtime.h>
#include <linux/ktime.h>

#include <asm/io.h>
#include <asm/irq.h>

#include "8250.h"

/* Nuvoton NPCM timeout register */
#define UART_NPCM_TOR          7
#define UART_NPCM_TOIE         BIT(7)  /* Timeout Interrupt Enable */

/*
 * Debugging.
 */
#if 0
#define DEBUG_AUTOCONF(fmt...)	printk(fmt)
#else
#define DEBUG_AUTOCONF(fmt...)	do { } while (0)
#endif

#define BOTH_EMPTY	(UART_LSR_TEMT | UART_LSR_THRE)

/*
 * Here we define the default xmit fifo size used for each type of UART.
 */
static const struct serial8250_config uart_config[] = {
	[PORT_UNKNOWN] = {
		.name		= "unknown",
		.fifo_size	= 1,
		.tx_loadsz	= 1,
	},
	[PORT_8250] = {
		.name		= "8250",
		.fifo_size	= 1,
		.tx_loadsz	= 1,
	},
	[PORT_16450] = {
		.name		= "16450",
		.fifo_size	= 1,
		.tx_loadsz	= 1,
	},
	[PORT_16550] = {
		.name		= "16550",
		.fifo_size	= 1,
		.tx_loadsz	= 1,
	},
	[PORT_16550A] = {
		.name		= "16550A",
		.fifo_size	= 32,
		.tx_loadsz	= 1,
		.fcr		= UART_FCR_ENABLE_FIFO | UART_FCR_R_TRIG_10,
		.rxtrig_bytes	= {1, 4, 8, 14},
		.flags		= UART_CAP_FIFO,
	},
	/// 删了很多别的port types
};

/* Uart divisor latch read */
static int default_serial_dl_read(struct uart_8250_port *up)
{
	/* Assign these in pieces to truncate any bits above 7.  */
	unsigned char dll = serial_in(up, UART_DLL);
	unsigned char dlm = serial_in(up, UART_DLM);

	return dll | dlm << 8;
}

/* Uart divisor latch write */
static void default_serial_dl_write(struct uart_8250_port *up, int value)
{
	serial_out(up, UART_DLL, value & 0xff);
	serial_out(up, UART_DLM, value >> 8 & 0xff);
}

// static unsigned int hub6_serial_in(struct uart_port *p, int offset)
// {
// 	offset = offset << p->regshift;
// 	outb(p->hub6 - 1 + offset, p->iobase);
// 	return inb(p->iobase + 1);
// }

// static void hub6_serial_out(struct uart_port *p, int offset, int value)
// {
// 	offset = offset << p->regshift;
// 	outb(p->hub6 - 1 + offset, p->iobase);
// 	outb(value, p->iobase + 1);
// }

static unsigned int mem_serial_in(struct uart_port *p, int offset)
{
	offset = offset << p->regshift;
	return readb(p->membase + offset);
}

static void mem_serial_out(struct uart_port *p, int offset, int value)
{
	offset = offset << p->regshift;
	writeb(value, p->membase + offset);
}

// static void mem16_serial_out(struct uart_port *p, int offset, int value)
// {
// 	offset = offset << p->regshift;
// 	writew(value, p->membase + offset);
// }

// static unsigned int mem16_serial_in(struct uart_port *p, int offset)
// {
// 	offset = offset << p->regshift;
// 	return readw(p->membase + offset);
// }

static void mem32_serial_out(struct uart_port *p, int offset, int value)
{
	offset = offset << p->regshift;
	writel(value, p->membase + offset);
}

static unsigned int mem32_serial_in(struct uart_port *p, int offset)
{
	offset = offset << p->regshift;
	return readl(p->membase + offset);
}

// static void mem32be_serial_out(struct uart_port *p, int offset, int value)
// {
// 	offset = offset << p->regshift;
// 	iowrite32be(value, p->membase + offset);
// }

// static unsigned int mem32be_serial_in(struct uart_port *p, int offset)
// {
// 	offset = offset << p->regshift;
// 	return ioread32be(p->membase + offset);
// }

static unsigned int io_serial_in(struct uart_port *p, int offset)
{
	offset = offset << p->regshift;
	return inb(p->iobase + offset);
}

static void io_serial_out(struct uart_port *p, int offset, int value)
{
	offset = offset << p->regshift;
	outb(value, p->iobase + offset);
}

static int serial8250_default_handle_irq(struct uart_port *port);

static void set_io_from_upio(struct uart_port *p)
{
	struct uart_8250_port *up = up_to_u8250p(p);

	up->dl_read = default_serial_dl_read;
	up->dl_write = default_serial_dl_write;

	switch (p->iotype) {
	case UPIO_HUB6:
		p->serial_in = hub6_serial_in;
		p->serial_out = hub6_serial_out;
		break;

	case UPIO_MEM:
		p->serial_in = mem_serial_in;
		p->serial_out = mem_serial_out;
		break;

	case UPIO_MEM16:
		p->serial_in = mem16_serial_in;
		p->serial_out = mem16_serial_out;
		break;

	case UPIO_MEM32: /// 8250_dw.c 注册uart_port. this way
		p->serial_in = mem32_serial_in;
		p->serial_out = mem32_serial_out;
		break;

	case UPIO_MEM32BE:
		p->serial_in = mem32be_serial_in;
		p->serial_out = mem32be_serial_out;
		break;

	default:
		p->serial_in = io_serial_in; /// 前三次默认注册uart_port走 this way p->iotype=0
		p->serial_out = io_serial_out;
		break;
	}
	/* Remember loaded iotype */
	up->cur_iotype = p->iotype; /// 前三次8250_core.c p->iotype=0 第四次8250_dw.c p->iotype=UPIO_MEM32
	p->handle_irq = serial8250_default_handle_irq;
}

static void
serial_port_out_sync(struct uart_port *p, int offset, int value)
{
	switch (p->iotype) {
	case UPIO_MEM:
	case UPIO_MEM16:
	case UPIO_MEM32:
	case UPIO_MEM32BE:
	case UPIO_AU:
		p->serial_out(p, offset, value);
		p->serial_in(p, UART_LCR);	/* safe, no side-effects */
		break;
	default:
		p->serial_out(p, offset, value);
	}
}

/*
 * FIFO support.
 */
static void serial8250_clear_fifos(struct uart_8250_port *p)
{
	if (p->capabilities & UART_CAP_FIFO) {
		serial_out(p, UART_FCR, UART_FCR_ENABLE_FIFO);
		serial_out(p, UART_FCR, UART_FCR_ENABLE_FIFO |
			       UART_FCR_CLEAR_RCVR | UART_FCR_CLEAR_XMIT);
		serial_out(p, UART_FCR, 0);
	}
}

static enum hrtimer_restart serial8250_em485_handle_start_tx(struct hrtimer *t);
static enum hrtimer_restart serial8250_em485_handle_stop_tx(struct hrtimer *t);

void serial8250_clear_and_reinit_fifos(struct uart_8250_port *p)
{
	serial8250_clear_fifos(p);
	serial_out(p, UART_FCR, p->fcr);
}
EXPORT_SYMBOL_GPL(serial8250_clear_and_reinit_fifos);

void serial8250_rpm_get(struct uart_8250_port *p)
{
	if (!(p->capabilities & UART_CAP_RPM))
		return;
	pm_runtime_get_sync(p->port.dev);
}
EXPORT_SYMBOL_GPL(serial8250_rpm_get);

void serial8250_rpm_put(struct uart_8250_port *p)
{
	if (!(p->capabilities & UART_CAP_RPM))
		return;
	pm_runtime_mark_last_busy(p->port.dev);
	pm_runtime_put_autosuspend(p->port.dev);
}
EXPORT_SYMBOL_GPL(serial8250_rpm_put);

/**
 *	serial8250_em485_init() - put uart_8250_port into rs485 emulating
 *	@p:	uart_8250_port port instance
 *
 *	The function is used to start rs485 software emulating on the
 *	&struct uart_8250_port* @p. Namely, RTS is switched before/after
 *	transmission. The function is idempotent, so it is safe to call it
 *	multiple times.
 *
 *	The caller MUST enable interrupt on empty shift register before
 *	calling serial8250_em485_init(). This interrupt is not a part of
 *	8250 standard, but implementation defined.
 *
 *	The function is supposed to be called from .rs485_config callback
 *	or from any other callback protected with p->port.lock spinlock.
 *
 *	See also serial8250_em485_destroy()
 *
 *	Return 0 - success, -errno - otherwise
 */
// static int serial8250_em485_init(struct uart_8250_port *p)
// {
// 	if (p->em485)
// 		goto deassert_rts;

// 	p->em485 = kmalloc(sizeof(struct uart_8250_em485), GFP_ATOMIC);
// 	if (!p->em485)
// 		return -ENOMEM;

// 	hrtimer_init(&p->em485->stop_tx_timer, CLOCK_MONOTONIC,
// 		     HRTIMER_MODE_REL);
// 	hrtimer_init(&p->em485->start_tx_timer, CLOCK_MONOTONIC,
// 		     HRTIMER_MODE_REL);
// 	p->em485->stop_tx_timer.function = &serial8250_em485_handle_stop_tx;
// 	p->em485->start_tx_timer.function = &serial8250_em485_handle_start_tx;
// 	p->em485->port = p;
// 	p->em485->active_timer = NULL;
// 	p->em485->tx_stopped = true;

// deassert_rts:
// 	if (p->em485->tx_stopped)
// 		p->rs485_stop_tx(p);

// 	return 0;
// }

/**
 *	serial8250_em485_destroy() - put uart_8250_port into normal state
 *	@p:	uart_8250_port port instance
 *
 *	The function is used to stop rs485 software emulating on the
 *	&struct uart_8250_port* @p. The function is idempotent, so it is safe to
 *	call it multiple times.
 *
 *	The function is supposed to be called from .rs485_config callback
 *	or from any other callback protected with p->port.lock spinlock.
 *
 *	See also serial8250_em485_init()
 */
// void serial8250_em485_destroy(struct uart_8250_port *p)
// {
// 	if (!p->em485)
// 		return;

// 	hrtimer_cancel(&p->em485->start_tx_timer);
// 	hrtimer_cancel(&p->em485->stop_tx_timer);

// 	kfree(p->em485);
// 	p->em485 = NULL;
// }
// EXPORT_SYMBOL_GPL(serial8250_em485_destroy);

/**
 * serial8250_em485_config() - generic ->rs485_config() callback
 * @port: uart port
 * @rs485: rs485 settings
 *
 * Generic callback usable by 8250 uart drivers to activate rs485 settings
 * if the uart is incapable of driving RTS as a Transmit Enable signal in
 * hardware, relying on software emulation instead.
 */
// int serial8250_em485_config(struct uart_port *port, struct serial_rs485 *rs485) /// 8250_dw.c 没有设置.rs485_config
// {
// 	struct uart_8250_port *up = up_to_u8250p(port);

// 	/* pick sane settings if the user hasn't */
// 	if (!!(rs485->flags & SER_RS485_RTS_ON_SEND) ==
// 	    !!(rs485->flags & SER_RS485_RTS_AFTER_SEND)) {
// 		rs485->flags |= SER_RS485_RTS_ON_SEND;
// 		rs485->flags &= ~SER_RS485_RTS_AFTER_SEND;
// 	}

// 	gpiod_set_value(port->rs485_term_gpio,
// 			rs485->flags & SER_RS485_TERMINATE_BUS);

// 	/*
// 	 * Both serial8250_em485_init() and serial8250_em485_destroy()
// 	 * are idempotent.
// 	 */
// 	if (rs485->flags & SER_RS485_ENABLED)
// 		return serial8250_em485_init(up);

// 	serial8250_em485_destroy(up);
// 	return 0;
// }
// EXPORT_SYMBOL_GPL(serial8250_em485_config);

/*
 * These two wrappers ensure that enable_runtime_pm_tx() can be called more than
 * once and disable_runtime_pm_tx() will still disable RPM because the fifo is
 * empty and the HW can idle again.
 */
void serial8250_rpm_get_tx(struct uart_8250_port *p)
{
	unsigned char rpm_active;

	if (!(p->capabilities & UART_CAP_RPM))
		return;

	rpm_active = xchg(&p->rpm_tx_active, 1);
	if (rpm_active)
		return;
	pm_runtime_get_sync(p->port.dev);
}
EXPORT_SYMBOL_GPL(serial8250_rpm_get_tx);

void serial8250_rpm_put_tx(struct uart_8250_port *p)
{
	unsigned char rpm_active;

	if (!(p->capabilities & UART_CAP_RPM))
		return;

	rpm_active = xchg(&p->rpm_tx_active, 0);
	if (!rpm_active)
		return;
	pm_runtime_mark_last_busy(p->port.dev);
	pm_runtime_put_autosuspend(p->port.dev);
}
EXPORT_SYMBOL_GPL(serial8250_rpm_put_tx);

/*
 * IER sleep support.  UARTs which have EFRs need the "extended
 * capability" bit enabled.  Note that on XR16C850s, we need to
 * reset LCR to write to IER.
 */
static void serial8250_set_sleep(struct uart_8250_port *p, int sleep)
{
	unsigned char lcr = 0, efr = 0;

	serial8250_rpm_get(p);

	if (p->capabilities & UART_CAP_SLEEP) {
		if (p->capabilities & UART_CAP_EFR) {
			lcr = serial_in(p, UART_LCR);
			efr = serial_in(p, UART_EFR);
			serial_out(p, UART_LCR, UART_LCR_CONF_MODE_B);
			serial_out(p, UART_EFR, UART_EFR_ECB);
			serial_out(p, UART_LCR, 0);
		}
		serial_out(p, UART_IER, sleep ? UART_IERX_SLEEP : 0);
		if (p->capabilities & UART_CAP_EFR) {
			serial_out(p, UART_LCR, UART_LCR_CONF_MODE_B);
			serial_out(p, UART_EFR, efr);
			serial_out(p, UART_LCR, lcr);
		}
	}

	serial8250_rpm_put(p);
}

/*
 * This is a quickie test to see how big the FIFO is.
 * It doesn't work at all the time, more's the pity.
 */
static int size_fifo(struct uart_8250_port *up)
{
	unsigned char old_fcr, old_mcr, old_lcr;
	unsigned short old_dl;
	int count;

	old_lcr = serial_in(up, UART_LCR);
	serial_out(up, UART_LCR, 0);
	old_fcr = serial_in(up, UART_FCR);
	old_mcr = serial8250_in_MCR(up);
	serial_out(up, UART_FCR, UART_FCR_ENABLE_FIFO |
		    UART_FCR_CLEAR_RCVR | UART_FCR_CLEAR_XMIT);
	serial8250_out_MCR(up, UART_MCR_LOOP);
	serial_out(up, UART_LCR, UART_LCR_CONF_MODE_A);
	old_dl = serial_dl_read(up);
	serial_dl_write(up, 0x0001);
	serial_out(up, UART_LCR, 0x03);
	for (count = 0; count < 256; count++)
		serial_out(up, UART_TX, count);
	mdelay(20);/* FIXME - schedule_timeout */
	for (count = 0; (serial_in(up, UART_LSR) & UART_LSR_DR) &&
	     (count < 256); count++)
		serial_in(up, UART_RX);
	serial_out(up, UART_FCR, old_fcr);
	serial8250_out_MCR(up, old_mcr);
	serial_out(up, UART_LCR, UART_LCR_CONF_MODE_A);
	serial_dl_write(up, old_dl);
	serial_out(up, UART_LCR, old_lcr);

	return count;
}

/*
 * We detected a chip without a FIFO.  Only two fall into
 * this category - the original 8250 and the 16450.  The
 * 16450 has a scratch register (accessible with LCR=0)
 */
// static void autoconfig_8250(struct uart_8250_port *up)
// {
// 	unsigned char scratch, status1, status2;

// 	up->port.type = PORT_8250;

// 	scratch = serial_in(up, UART_SCR);
// 	serial_out(up, UART_SCR, 0xa5);
// 	status1 = serial_in(up, UART_SCR);
// 	serial_out(up, UART_SCR, 0x5a);
// 	status2 = serial_in(up, UART_SCR);
// 	serial_out(up, UART_SCR, scratch);

// 	if (status1 == 0xa5 && status2 == 0x5a)
// 		up->port.type = PORT_16450;
// }

/*
 * We know that the chip has FIFOs.  Does it have an EFR?  The
 * EFR is located in the same register position as the IIR and
 * we know the top two bits of the IIR are currently set.  The
 * EFR should contain zero.  Try to read the EFR.
 */
static void autoconfig_16550a(struct uart_8250_port *up)
{
	unsigned char status1, status2;
	unsigned int iersave;

	up->port.type = PORT_16550A;
	up->capabilities |= UART_CAP_FIFO;

	if (!IS_ENABLED(CONFIG_SERIAL_8250_16550A_VARIANTS) && /// 5.10新增的，应该会直接return，下面都是variants uart
	    !(up->port.flags & UPF_FULL_PROBE))
		return;

	// 删了很多specfic 16550A uarts detect
}

/*
 * This routine is called by rs_init() to initialize a specific serial
 * port.  It determines what type of UART chip this serial port is
 * using: 8250, 16450, 16550, 16550A.  The important question is
 * whether or not this UART is a 16550A or not, since this will
 * determine whether or not we can use its FIFO features or not.
 */
static void autoconfig(struct uart_8250_port *up)
{
	unsigned char status1, scratch, scratch2, scratch3;
	unsigned char save_lcr, save_mcr;
	struct uart_port *port = &up->port;
	unsigned long flags;
	unsigned int old_capabilities;

	if (!port->iobase && !port->mapbase && !port->membase)
		return;

	DEBUG_AUTOCONF("%s: autoconf (0x%04lx, 0x%p): ",
		       port->name, port->iobase, port->membase);

	/*
	 * We really do need global IRQs disabled here - we're going to
	 * be frobbing the chips IRQ enable register to see if it exists.
	 */
	spin_lock_irqsave(&port->lock, flags);

	up->capabilities = 0;
	up->bugs = 0;

	if (!(port->flags & UPF_BUGGY_UART)) {
		/*
		 * Do a simple existence test first; if we fail this,
		 * there's no point trying anything else.
		 *
		 * 0x80 is used as a nonsense port to prevent against
		 * false positives due to ISA bus float.  The
		 * assumption is that 0x80 is a non-existent port;
		 * which should be safe since include/asm/io.h also
		 * makes this assumption.
		 *
		 * Note: this is safe as long as MCR bit 4 is clear
		 * and the device is in "PC" mode.
		 */
		scratch = serial_in(up, UART_IER);
		serial_out(up, UART_IER, 0);

		/*
		 * Mask out IER[7:4] bits for test as some UARTs (e.g. TL
		 * 16C754B) allow only to modify them if an EFR bit is set.
		 */
		scratch2 = serial_in(up, UART_IER) & 0x0f;
		serial_out(up, UART_IER, 0x0F);

		scratch3 = serial_in(up, UART_IER) & 0x0f;
		serial_out(up, UART_IER, scratch);
		if (scratch2 != 0 || scratch3 != 0x0F) {
			/*
			 * We failed; there's nothing here
			 */
			spin_unlock_irqrestore(&port->lock, flags);
			DEBUG_AUTOCONF("IER test failed (%02x, %02x) ",
				       scratch2, scratch3);
			goto out;
		}
	}

	save_mcr = serial8250_in_MCR(up);
	save_lcr = serial_in(up, UART_LCR);

	/*
	 * Check to see if a UART is really there.  Certain broken
	 * internal modems based on the Rockwell chipset fail this
	 * test, because they apparently don't implement the loopback
	 * test mode.  So this test is skipped on the COM 1 through
	 * COM 4 ports.  This *should* be safe, since no board
	 * manufacturer would be stupid enough to design a board
	 * that conflicts with COM 1-4 --- we hope!
	 */
	if (!(port->flags & UPF_SKIP_TEST)) {
		serial8250_out_MCR(up, UART_MCR_LOOP | 0x0A);
		status1 = serial_in(up, UART_MSR) & 0xF0;
		serial8250_out_MCR(up, save_mcr);
		if (status1 != 0x90) {
			spin_unlock_irqrestore(&port->lock, flags);
			DEBUG_AUTOCONF("LOOP test failed (%02x) ",
				       status1);
			goto out;
		}
	}

	/*
	 * We're pretty sure there's a port here.  Lets find out what
	 * type of port it is.  The IIR top two bits allows us to find
	 * out if it's 8250 or 16450, 16550, 16550A or later.  This
	 * determines what we test for next.
	 *
	 * We also initialise the EFR (if any) to zero for later.  The
	 * EFR occupies the same register location as the FCR and IIR.
	 */
	serial_out(up, UART_LCR, UART_LCR_CONF_MODE_B);
	serial_out(up, UART_EFR, 0);
	serial_out(up, UART_LCR, 0);

	serial_out(up, UART_FCR, UART_FCR_ENABLE_FIFO);

	/* Assign this as it is to truncate any bits above 7.  */
	scratch = serial_in(up, UART_IIR);

	switch (scratch >> 6) {
	case 0:
		autoconfig_8250(up);
		break;
	case 1:
		port->type = PORT_UNKNOWN;
		break;
	case 2:
		port->type = PORT_16550;
		break;
	case 3:
		autoconfig_16550a(up); /// this way
		break;
	}

	serial_out(up, UART_LCR, save_lcr);

	port->fifosize = uart_config[up->port.type].fifo_size; /// 32
	old_capabilities = up->capabilities; /// UART_CAP_FIFO(256) line943
	up->capabilities = uart_config[port->type].flags; /// UART_CAP_FIFO(256)
	up->tx_loadsz = uart_config[port->type].tx_loadsz; /// 1

	// if (port->type == PORT_UNKNOWN)
	// 	goto out_lock;

	/*
	 * Reset the UART.
	 */

	serial8250_out_MCR(up, save_mcr);
	serial8250_clear_fifos(up);
	serial_in(up, UART_RX);
	// if (up->capabilities & UART_CAP_UUE) /// skip
	// 	serial_out(up, UART_IER, UART_IER_UUE);
	// else
	serial_out(up, UART_IER, 0);

out_lock:
	spin_unlock_irqrestore(&port->lock, flags);

	if (up->capabilities != old_capabilities) { /// 相同 skip
		dev_warn(port->dev, "detected caps %08x should be %08x\n",
			 old_capabilities, up->capabilities);
	}
out:
	DEBUG_AUTOCONF("iir=%d ", scratch); // 193
	DEBUG_AUTOCONF("type=%s\n", uart_config[port->type].name); /// 16550A
}

// static void autoconfig_irq(struct uart_8250_port *up) /// 用不到
// {
// 	struct uart_port *port = &up->port;
// 	unsigned char save_mcr, save_ier;
// 	unsigned char save_ICP = 0;
// 	unsigned int ICP = 0;
// 	unsigned long irqs;
// 	int irq;

// 	if (port->flags & UPF_FOURPORT) {
// 		ICP = (port->iobase & 0xfe0) | 0x1f;
// 		save_ICP = inb_p(ICP);
// 		outb_p(0x80, ICP);
// 		inb_p(ICP);
// 	}

// 	if (uart_console(port))
// 		console_lock();

// 	/* forget possible initially masked and pending IRQ */
// 	probe_irq_off(probe_irq_on());
// 	save_mcr = serial8250_in_MCR(up);
// 	save_ier = serial_in(up, UART_IER);
// 	serial8250_out_MCR(up, UART_MCR_OUT1 | UART_MCR_OUT2);

// 	irqs = probe_irq_on();
// 	serial8250_out_MCR(up, 0);
// 	udelay(10);
// 	if (port->flags & UPF_FOURPORT) {
// 		serial8250_out_MCR(up, UART_MCR_DTR | UART_MCR_RTS);
// 	} else {
// 		serial8250_out_MCR(up,
// 			UART_MCR_DTR | UART_MCR_RTS | UART_MCR_OUT2);
// 	}
// 	serial_out(up, UART_IER, 0x0f);	/* enable all intrs */
// 	serial_in(up, UART_LSR);
// 	serial_in(up, UART_RX);
// 	serial_in(up, UART_IIR);
// 	serial_in(up, UART_MSR);
// 	serial_out(up, UART_TX, 0xFF);
// 	udelay(20);
// 	irq = probe_irq_off(irqs);

// 	serial8250_out_MCR(up, save_mcr);
// 	serial_out(up, UART_IER, save_ier);

// 	if (port->flags & UPF_FOURPORT)
// 		outb_p(save_ICP, ICP);

// 	if (uart_console(port))
// 		console_unlock();

// 	port->irq = (irq > 0) ? irq : 0;
// }

static void serial8250_stop_rx(struct uart_port *port)
{
	struct uart_8250_port *up = up_to_u8250p(port);

	serial8250_rpm_get(up);

	up->ier &= ~(UART_IER_RLSI | UART_IER_RDI);
	up->port.read_status_mask &= ~UART_LSR_DR;
	serial_port_out(port, UART_IER, up->ier);

	serial8250_rpm_put(up);
}

/**
 * serial8250_em485_stop_tx() - generic ->rs485_stop_tx() callback
 * @p: uart 8250 port
 *
 * Generic callback usable by 8250 uart drivers to stop rs485 transmission.
 */
// void serial8250_em485_stop_tx(struct uart_8250_port *p)
// {
// 	unsigned char mcr = serial8250_in_MCR(p);

// 	if (p->port.rs485.flags & SER_RS485_RTS_AFTER_SEND)
// 		mcr |= UART_MCR_RTS;
// 	else
// 		mcr &= ~UART_MCR_RTS;
// 	serial8250_out_MCR(p, mcr);

// 	/*
// 	 * Empty the RX FIFO, we are not interested in anything
// 	 * received during the half-duplex transmission.
// 	 * Enable previously disabled RX interrupts.
// 	 */
// 	if (!(p->port.rs485.flags & SER_RS485_RX_DURING_TX)) {
// 		serial8250_clear_and_reinit_fifos(p);

// 		p->ier |= UART_IER_RLSI | UART_IER_RDI;
// 		serial_port_out(&p->port, UART_IER, p->ier);
// 	}
// }
// EXPORT_SYMBOL_GPL(serial8250_em485_stop_tx);

// static enum hrtimer_restart serial8250_em485_handle_stop_tx(struct hrtimer *t)
// {
// 	struct uart_8250_em485 *em485;
// 	struct uart_8250_port *p;
// 	unsigned long flags;

// 	em485 = container_of(t, struct uart_8250_em485, stop_tx_timer);
// 	p = em485->port;

// 	serial8250_rpm_get(p);
// 	spin_lock_irqsave(&p->port.lock, flags);
// 	if (em485->active_timer == &em485->stop_tx_timer) {
// 		p->rs485_stop_tx(p);
// 		em485->active_timer = NULL;
// 		em485->tx_stopped = true;
// 	}
// 	spin_unlock_irqrestore(&p->port.lock, flags);
// 	serial8250_rpm_put(p);
// 	return HRTIMER_NORESTART;
// }

static void start_hrtimer_ms(struct hrtimer *hrt, unsigned long msec)
{
	long sec = msec / 1000;
	long nsec = (msec % 1000) * 1000000;
	ktime_t t = ktime_set(sec, nsec);

	hrtimer_start(hrt, t, HRTIMER_MODE_REL);
}

// static void __stop_tx_rs485(struct uart_8250_port *p)
// {
// 	struct uart_8250_em485 *em485 = p->em485;

// 	/*
// 	 * rs485_stop_tx() is going to set RTS according to config
// 	 * AND flush RX FIFO if required.
// 	 */
// 	if (p->port.rs485.delay_rts_after_send > 0) {
// 		em485->active_timer = &em485->stop_tx_timer;
// 		start_hrtimer_ms(&em485->stop_tx_timer,
// 				   p->port.rs485.delay_rts_after_send);
// 	} else {
// 		p->rs485_stop_tx(p);
// 		em485->active_timer = NULL;
// 		em485->tx_stopped = true;
// 	}
// }

static inline void __do_stop_tx(struct uart_8250_port *p)
{
	if (serial8250_clear_THRI(p))
		serial8250_rpm_put_tx(p);
}

static inline void __stop_tx(struct uart_8250_port *p)
{
	struct uart_8250_em485 *em485 = p->em485;

	// if (em485) {
	// 	unsigned char lsr = serial_in(p, UART_LSR);
	// 	p->lsr_saved_flags |= lsr & LSR_SAVE_FLAGS;

	// 	/*
	// 	 * To provide required timeing and allow FIFO transfer,
	// 	 * __stop_tx_rs485() must be called only when both FIFO and
	// 	 * shift register are empty. It is for device driver to enable
	// 	 * interrupt on TEMT.
	// 	 */
	// 	if ((lsr & BOTH_EMPTY) != BOTH_EMPTY)
	// 		return;

	// 	__stop_tx_rs485(p);
	// }
	__do_stop_tx(p);
}

static void serial8250_stop_tx(struct uart_port *port)
{
	struct uart_8250_port *up = up_to_u8250p(port);

	serial8250_rpm_get(up);
	__stop_tx(up);

	/*
	 * We really want to stop the transmitter from sending.
	 */
	// if (port->type == PORT_16C950) {
	// 	up->acr |= UART_ACR_TXDIS;
	// 	serial_icr_write(up, UART_ACR, up->acr);
	// }
	serial8250_rpm_put(up);
}

static inline void __start_tx(struct uart_port *port)
{
	struct uart_8250_port *up = up_to_u8250p(port);

	if (up->dma && !up->dma->tx_dma(up))
		return;

	if (serial8250_set_THRI(up)) {
		// if (up->bugs & UART_BUG_TXEN) {
		// 	unsigned char lsr;

		// 	lsr = serial_in(up, UART_LSR);
		// 	up->lsr_saved_flags |= lsr & LSR_SAVE_FLAGS;
		// 	if (lsr & UART_LSR_THRE)
		// 		serial8250_tx_chars(up);
		// }
	}

	/*
	 * Re-enable the transmitter if we disabled it.
	 */
	// if (port->type == PORT_16C950 && up->acr & UART_ACR_TXDIS) {
	// 	up->acr &= ~UART_ACR_TXDIS;
	// 	serial_icr_write(up, UART_ACR, up->acr);
	// }
}

/**
 * serial8250_em485_start_tx() - generic ->rs485_start_tx() callback
 * @up: uart 8250 port
 *
 * Generic callback usable by 8250 uart drivers to start rs485 transmission.
 * Assumes that setting the RTS bit in the MCR register means RTS is high.
 * (Some chips use inverse semantics.)  Further assumes that reception is
 * stoppable by disabling the UART_IER_RDI interrupt.  (Some chips set the
 * UART_LSR_DR bit even when UART_IER_RDI is disabled, foiling this approach.)
 */
// void serial8250_em485_start_tx(struct uart_8250_port *up)
// {
// 	unsigned char mcr = serial8250_in_MCR(up);

// 	if (!(up->port.rs485.flags & SER_RS485_RX_DURING_TX))
// 		serial8250_stop_rx(&up->port);

// 	if (up->port.rs485.flags & SER_RS485_RTS_ON_SEND)
// 		mcr |= UART_MCR_RTS;
// 	else
// 		mcr &= ~UART_MCR_RTS;
// 	serial8250_out_MCR(up, mcr);
// }
// EXPORT_SYMBOL_GPL(serial8250_em485_start_tx);

// static inline void start_tx_rs485(struct uart_port *port)
// {
// 	struct uart_8250_port *up = up_to_u8250p(port);
// 	struct uart_8250_em485 *em485 = up->em485;

// 	/*
// 	 * While serial8250_em485_handle_stop_tx() is a noop if
// 	 * em485->active_timer != &em485->stop_tx_timer, it might happen that
// 	 * the timer is still armed and triggers only after the current bunch of
// 	 * chars is send and em485->active_timer == &em485->stop_tx_timer again.
// 	 * So cancel the timer. There is still a theoretical race condition if
// 	 * the timer is already running and only comes around to check for
// 	 * em485->active_timer when &em485->stop_tx_timer is armed again.
// 	 */
// 	if (em485->active_timer == &em485->stop_tx_timer)
// 		hrtimer_try_to_cancel(&em485->stop_tx_timer);

// 	em485->active_timer = NULL;

// 	if (em485->tx_stopped) {
// 		em485->tx_stopped = false;

// 		up->rs485_start_tx(up);

// 		if (up->port.rs485.delay_rts_before_send > 0) {
// 			em485->active_timer = &em485->start_tx_timer;
// 			start_hrtimer_ms(&em485->start_tx_timer,
// 					 up->port.rs485.delay_rts_before_send);
// 			return;
// 		}
// 	}

// 	__start_tx(port);
// }

// static enum hrtimer_restart serial8250_em485_handle_start_tx(struct hrtimer *t)
// {
// 	struct uart_8250_em485 *em485;
// 	struct uart_8250_port *p;
// 	unsigned long flags;

// 	em485 = container_of(t, struct uart_8250_em485, start_tx_timer);
// 	p = em485->port;

// 	spin_lock_irqsave(&p->port.lock, flags);
// 	if (em485->active_timer == &em485->start_tx_timer) {
// 		__start_tx(&p->port);
// 		em485->active_timer = NULL;
// 	}
// 	spin_unlock_irqrestore(&p->port.lock, flags);
// 	return HRTIMER_NORESTART;
// }

static void serial8250_start_tx(struct uart_port *port)
{
	struct uart_8250_port *up = up_to_u8250p(port);
	struct uart_8250_em485 *em485 = up->em485;

	serial8250_rpm_get_tx(up);

	// if (em485 &&
	//     em485->active_timer == &em485->start_tx_timer)
	// 	return;

	// if (em485)
	// 	start_tx_rs485(port);
	// else
		__start_tx(port);
}

static void serial8250_throttle(struct uart_port *port)
{
	port->throttle(port);
}

static void serial8250_unthrottle(struct uart_port *port)
{
	port->unthrottle(port);
}

static void serial8250_disable_ms(struct uart_port *port)
{
	struct uart_8250_port *up = up_to_u8250p(port);

	/* no MSR capabilities */
	if (up->bugs & UART_BUG_NOMSR)
		return;

	mctrl_gpio_disable_ms(up->gpios);

	up->ier &= ~UART_IER_MSI;
	serial_port_out(port, UART_IER, up->ier);
}

static void serial8250_enable_ms(struct uart_port *port)
{
	struct uart_8250_port *up = up_to_u8250p(port);

	/* no MSR capabilities */
	if (up->bugs & UART_BUG_NOMSR)
		return;

	mctrl_gpio_enable_ms(up->gpios); /// up->gpios=NULL do nothing

	up->ier |= UART_IER_MSI;

	serial8250_rpm_get(up);
	serial_port_out(port, UART_IER, up->ier);
	serial8250_rpm_put(up);
}

void serial8250_read_char(struct uart_8250_port *up, unsigned char lsr)
{
	struct uart_port *port = &up->port;
	unsigned char ch;
	char flag = TTY_NORMAL;

	if (likely(lsr & UART_LSR_DR))
		ch = serial_in(up, UART_RX);
	else
		/*
		 * Intel 82571 has a Serial Over Lan device that will
		 * set UART_LSR_BI without setting UART_LSR_DR when
		 * it receives a break. To avoid reading from the
		 * receive buffer without UART_LSR_DR bit set, we
		 * just force the read character to be 0
		 */
		ch = 0;

	port->icount.rx++;

	lsr |= up->lsr_saved_flags;
	up->lsr_saved_flags = 0;

	if (unlikely(lsr & UART_LSR_BRK_ERROR_BITS)) {
		if (lsr & UART_LSR_BI) {
			lsr &= ~(UART_LSR_FE | UART_LSR_PE);
			port->icount.brk++;
			/*
			 * We do the SysRQ and SAK checking
			 * here because otherwise the break
			 * may get masked by ignore_status_mask
			 * or read_status_mask.
			 */
			if (uart_handle_break(port))
				return;
		} else if (lsr & UART_LSR_PE)
			port->icount.parity++;
		else if (lsr & UART_LSR_FE)
			port->icount.frame++;
		if (lsr & UART_LSR_OE)
			port->icount.overrun++;

		/*
		 * Mask off conditions which should be ignored.
		 */
		lsr &= port->read_status_mask;

		if (lsr & UART_LSR_BI) {
			dev_dbg(port->dev, "handling break\n");
			flag = TTY_BREAK;
		} else if (lsr & UART_LSR_PE)
			flag = TTY_PARITY;
		else if (lsr & UART_LSR_FE)
			flag = TTY_FRAME;
	}
	if (uart_prepare_sysrq_char(port, ch))
		return;

	uart_insert_char(port, lsr, UART_LSR_OE, ch, flag); /// 把数据存入tty_port里的tty_buffer
}
EXPORT_SYMBOL_GPL(serial8250_read_char);

/*
 * serial8250_rx_chars: processes according to the passed in LSR
 * value, and returns the remaining LSR bits not handled
 * by this Rx routine.
 */
unsigned char serial8250_rx_chars(struct uart_8250_port *up, unsigned char lsr)
{
	struct uart_port *port = &up->port;
	int max_count = 256;

	do {
		serial8250_read_char(up, lsr);
		if (--max_count == 0)
			break;
		lsr = serial_in(up, UART_LSR);
	} while (lsr & (UART_LSR_DR | UART_LSR_BI));

	tty_flip_buffer_push(&port->state->port); /// 通知行规程来处理
	return lsr;
}
EXPORT_SYMBOL_GPL(serial8250_rx_chars);

void serial8250_tx_chars(struct uart_8250_port *up)
{
	struct uart_port *port = &up->port;
	struct circ_buf *xmit = &port->state->xmit;
	int count;

	if (port->x_char) {
		uart_xchar_out(port, UART_TX);
		return;
	}
	if (uart_tx_stopped(port)) {
		serial8250_stop_tx(port);
		return;
	}
	if (uart_circ_empty(xmit)) {
		__stop_tx(up);
		return;
	}

	count = up->tx_loadsz;
	do {
		serial_out(up, UART_TX, xmit->buf[xmit->tail]);
		if (up->bugs & UART_BUG_TXRACE) {
			/*
			 * The Aspeed BMC virtual UARTs have a bug where data
			 * may get stuck in the BMC's Tx FIFO from bursts of
			 * writes on the APB interface.
			 *
			 * Delay back-to-back writes by a read cycle to avoid
			 * stalling the VUART. Read a register that won't have
			 * side-effects and discard the result.
			 */
			serial_in(up, UART_SCR);
		}
		xmit->tail = (xmit->tail + 1) & (UART_XMIT_SIZE - 1);
		port->icount.tx++;
		if (uart_circ_empty(xmit))
			break;
		// if ((up->capabilities & UART_CAP_HFIFO) &&
		//     (serial_in(up, UART_LSR) & BOTH_EMPTY) != BOTH_EMPTY)
		// 	break;
		/* The BCM2835 MINI UART THRE bit is really a not-full bit. */
		// if ((up->capabilities & UART_CAP_MINI) &&
		//     !(serial_in(up, UART_LSR) & UART_LSR_THRE))
		// 	break;
	} while (--count > 0);

	if (uart_circ_chars_pending(xmit) < WAKEUP_CHARS)
		uart_write_wakeup(port);

	/*
	 * With RPM enabled, we have to wait until the FIFO is empty before the
	 * HW can go idle. So we get here once again with empty FIFO and disable
	 * the interrupt and RPM in __stop_tx()
	 */
	if (uart_circ_empty(xmit) && !(up->capabilities & UART_CAP_RPM))
		__stop_tx(up);
}
EXPORT_SYMBOL_GPL(serial8250_tx_chars);

/* Caller holds uart port lock */
unsigned int serial8250_modem_status(struct uart_8250_port *up)
{
	struct uart_port *port = &up->port;
	unsigned int status = serial_in(up, UART_MSR);

	status |= up->msr_saved_flags;
	up->msr_saved_flags = 0;
	if (status & UART_MSR_ANY_DELTA && up->ier & UART_IER_MSI &&
	    port->state != NULL) {
		if (status & UART_MSR_TERI)
			port->icount.rng++;
		if (status & UART_MSR_DDSR)
			port->icount.dsr++;
		if (status & UART_MSR_DDCD)
			uart_handle_dcd_change(port, status & UART_MSR_DCD);
		if (status & UART_MSR_DCTS)
			uart_handle_cts_change(port, status & UART_MSR_CTS);

		wake_up_interruptible(&port->state->port.delta_msr_wait);
	}

	return status;
}
EXPORT_SYMBOL_GPL(serial8250_modem_status);

static bool handle_rx_dma(struct uart_8250_port *up, unsigned int iir)
{
	switch (iir & 0x3f) {
	case UART_IIR_RDI:
		if (!up->dma->rx_running)
			break;
		fallthrough;
	case UART_IIR_RLSI:
	case UART_IIR_RX_TIMEOUT:
		serial8250_rx_dma_flush(up);
		return true;
	}
	return up->dma->rx_dma(up);
}

/*
 * This handles the interrupt from one port.
 */
int serial8250_handle_irq(struct uart_port *port, unsigned int iir)
{
	unsigned char status;
	unsigned long flags;
	struct uart_8250_port *up = up_to_u8250p(port);
	bool skip_rx = false;

	if (iir & UART_IIR_NO_INT)
		return 0;

	spin_lock_irqsave(&port->lock, flags);

	status = serial_port_in(port, UART_LSR);

	/*
	 * If port is stopped and there are no error conditions in the
	 * FIFO, then don't drain the FIFO, as this may lead to TTY buffer
	 * overflow. Not servicing, RX FIFO would trigger auto HW flow
	 * control when FIFO occupancy reaches preset threshold, thus
	 * halting RX. This only works when auto HW flow control is
	 * available.
	 */
	if (!(status & (UART_LSR_FIFOE | UART_LSR_BRK_ERROR_BITS)) &&
	    (port->status & (UPSTAT_AUTOCTS | UPSTAT_AUTORTS)) &&
	    !(port->read_status_mask & UART_LSR_DR))
		skip_rx = true;

	if (status & (UART_LSR_DR | UART_LSR_BI) && !skip_rx) {
		if (!up->dma || handle_rx_dma(up, iir))
			status = serial8250_rx_chars(up, status); /// 读数据
	}
	serial8250_modem_status(up);
	if ((!up->dma || up->dma->tx_err) && (status & UART_LSR_THRE) &&
		(up->ier & UART_IER_THRI))
		serial8250_tx_chars(up); /// 发数据

	uart_unlock_and_check_sysrq(port, flags);
	return 1;
}
EXPORT_SYMBOL_GPL(serial8250_handle_irq);

static int serial8250_default_handle_irq(struct uart_port *port)
{
	struct uart_8250_port *up = up_to_u8250p(port);
	unsigned int iir;
	int ret;

	serial8250_rpm_get(up);

	iir = serial_port_in(port, UART_IIR);
	ret = serial8250_handle_irq(port, iir);

	serial8250_rpm_put(up);
	return ret;
}

/*
 * Newer 16550 compatible parts such as the SC16C650 & Altera 16550 Soft IP
 * have a programmable TX threshold that triggers the THRE interrupt in
 * the IIR register. In this case, the THRE interrupt indicates the FIFO
 * has space available. Load it up with tx_loadsz bytes.
 */
// static int serial8250_tx_threshold_handle_irq(struct uart_port *port)
// {
// 	unsigned long flags;
// 	unsigned int iir = serial_port_in(port, UART_IIR);

// 	/* TX Threshold IRQ triggered so load up FIFO */
// 	if ((iir & UART_IIR_ID) == UART_IIR_THRI) {
// 		struct uart_8250_port *up = up_to_u8250p(port);

// 		spin_lock_irqsave(&port->lock, flags);
// 		serial8250_tx_chars(up);
// 		spin_unlock_irqrestore(&port->lock, flags);
// 	}

// 	iir = serial_port_in(port, UART_IIR);
// 	return serial8250_handle_irq(port, iir);
// }

static unsigned int serial8250_tx_empty(struct uart_port *port)
{
	struct uart_8250_port *up = up_to_u8250p(port);
	unsigned long flags;
	unsigned int lsr;

	serial8250_rpm_get(up);

	spin_lock_irqsave(&port->lock, flags);
	lsr = serial_port_in(port, UART_LSR);
	up->lsr_saved_flags |= lsr & LSR_SAVE_FLAGS;
	spin_unlock_irqrestore(&port->lock, flags);

	serial8250_rpm_put(up);

	return (lsr & BOTH_EMPTY) == BOTH_EMPTY ? TIOCSER_TEMT : 0;
}

unsigned int serial8250_do_get_mctrl(struct uart_port *port)
{
	struct uart_8250_port *up = up_to_u8250p(port);
	unsigned int status;
	unsigned int val;

	serial8250_rpm_get(up);
	status = serial8250_modem_status(up);
	serial8250_rpm_put(up);

	val = serial8250_MSR_to_TIOCM(status);
	// if (up->gpios)
	// 	return mctrl_gpio_get(up->gpios, &val);

	return val;
}
EXPORT_SYMBOL_GPL(serial8250_do_get_mctrl);

static unsigned int serial8250_get_mctrl(struct uart_port *port)
{
	if (port->get_mctrl)
		return port->get_mctrl(port);
	return serial8250_do_get_mctrl(port);
}

void serial8250_do_set_mctrl(struct uart_port *port, unsigned int mctrl)
{
	struct uart_8250_port *up = up_to_u8250p(port);
	unsigned char mcr;

	mcr = serial8250_TIOCM_to_MCR(mctrl);

	mcr = (mcr & up->mcr_mask) | up->mcr_force | up->mcr;

	serial8250_out_MCR(up, mcr);
}
EXPORT_SYMBOL_GPL(serial8250_do_set_mctrl);

static void serial8250_set_mctrl(struct uart_port *port, unsigned int mctrl)
{
	// if (port->rs485.flags & SER_RS485_ENABLED)
	// 	return;

	if (port->set_mctrl) /// 没设置
		port->set_mctrl(port, mctrl);
	else
		serial8250_do_set_mctrl(port, mctrl);
}

static void serial8250_break_ctl(struct uart_port *port, int break_state)
{
	struct uart_8250_port *up = up_to_u8250p(port);
	unsigned long flags;

	serial8250_rpm_get(up);
	spin_lock_irqsave(&port->lock, flags);
	if (break_state == -1)
		up->lcr |= UART_LCR_SBC;
	else
		up->lcr &= ~UART_LCR_SBC;
	serial_port_out(port, UART_LCR, up->lcr);
	spin_unlock_irqrestore(&port->lock, flags);
	serial8250_rpm_put(up);
}

/*
 *	Wait for transmitter & holding register to empty
 */
static void wait_for_xmitr(struct uart_8250_port *up, int bits)
{
	unsigned int status, tmout = 10000;

	/* Wait up to 10ms for the character(s) to be sent. */
	for (;;) {
		status = serial_in(up, UART_LSR);

		up->lsr_saved_flags |= status & LSR_SAVE_FLAGS;

		if ((status & bits) == bits)
			break;
		if (--tmout == 0)
			break;
		udelay(1);
		touch_nmi_watchdog();
	}

	/* Wait up to 1s for flow control if necessary */
	if (up->port.flags & UPF_CONS_FLOW) {
		for (tmout = 1000000; tmout; tmout--) {
			unsigned int msr = serial_in(up, UART_MSR);
			up->msr_saved_flags |= msr & MSR_SAVE_FLAGS;
			if (msr & UART_MSR_CTS)
				break;
			udelay(1);
			touch_nmi_watchdog();
		}
	}
}

// #ifdef CONFIG_CONSOLE_POLL
// /*
//  * Console polling routines for writing and reading from the uart while
//  * in an interrupt or debug context.
//  */

// static int serial8250_get_poll_char(struct uart_port *port)
// {
// 	struct uart_8250_port *up = up_to_u8250p(port);
// 	unsigned char lsr;
// 	int status;

// 	serial8250_rpm_get(up);

// 	lsr = serial_port_in(port, UART_LSR);

// 	if (!(lsr & UART_LSR_DR)) {
// 		status = NO_POLL_CHAR;
// 		goto out;
// 	}

// 	status = serial_port_in(port, UART_RX);
// out:
// 	serial8250_rpm_put(up);
// 	return status;
// }


// static void serial8250_put_poll_char(struct uart_port *port,
// 			 unsigned char c)
// {
// 	unsigned int ier;
// 	struct uart_8250_port *up = up_to_u8250p(port);

// 	serial8250_rpm_get(up);
// 	/*
// 	 *	First save the IER then disable the interrupts
// 	 */
// 	ier = serial_port_in(port, UART_IER);
// 	if (up->capabilities & UART_CAP_UUE)
// 		serial_port_out(port, UART_IER, UART_IER_UUE);
// 	else
// 		serial_port_out(port, UART_IER, 0);

// 	wait_for_xmitr(up, BOTH_EMPTY);
// 	/*
// 	 *	Send the character out.
// 	 */
// 	serial_port_out(port, UART_TX, c);

// 	/*
// 	 *	Finally, wait for transmitter to become empty
// 	 *	and restore the IER
// 	 */
// 	wait_for_xmitr(up, BOTH_EMPTY);
// 	serial_port_out(port, UART_IER, ier);
// 	serial8250_rpm_put(up);
// }

// #endif /* CONFIG_CONSOLE_POLL */

int serial8250_do_startup(struct uart_port *port) /// 这边启动没跑进来
{
	struct uart_8250_port *up = up_to_u8250p(port);
	unsigned long flags;
	unsigned char lsr, iir;
	int retval;

	if (!port->fifosize)
		port->fifosize = uart_config[port->type].fifo_size;
	if (!up->tx_loadsz)
		up->tx_loadsz = uart_config[port->type].tx_loadsz;
	if (!up->capabilities)
		up->capabilities = uart_config[port->type].flags;
	up->mcr = 0;

	if (port->iotype != up->cur_iotype)
		set_io_from_upio(port);

	serial8250_rpm_get(up);

	/*
	 * Clear the FIFO buffers and disable them.
	 * (they will be reenabled in set_termios())
	 */
	serial8250_clear_fifos(up);

	/*
	 * Clear the interrupt registers.
	 */
	serial_port_in(port, UART_LSR);
	serial_port_in(port, UART_RX);
	serial_port_in(port, UART_IIR);
	serial_port_in(port, UART_MSR);

	/*
	 * At this point, there's no way the LSR could still be 0xff;
	 * if it is, then bail out, because there's likely no UART
	 * here.
	 */
	if (!(port->flags & UPF_BUGGY_UART) &&
	    (serial_port_in(port, UART_LSR) == 0xff)) {
		dev_info_ratelimited(port->dev, "LSR safety check engaged!\n");
		retval = -ENODEV;
		goto out;
	}

	/* Check if we need to have shared IRQs */
	if (port->irq && (up->port.flags & UPF_SHARE_IRQ))
		up->port.irqflags |= IRQF_SHARED;

	retval = up->ops->setup_irq(up);
	if (retval)
		goto out;

	if (port->irq && !(up->port.flags & UPF_NO_THRE_TEST)) {
		unsigned char iir1;

		if (port->irqflags & IRQF_SHARED)
			disable_irq_nosync(port->irq);

		/*
		 * Test for UARTs that do not reassert THRE when the
		 * transmitter is idle and the interrupt has already
		 * been cleared.  Real 16550s should always reassert
		 * this interrupt whenever the transmitter is idle and
		 * the interrupt is enabled.  Delays are necessary to
		 * allow register changes to become visible.
		 */
		spin_lock_irqsave(&port->lock, flags);

		wait_for_xmitr(up, UART_LSR_THRE);
		serial_port_out_sync(port, UART_IER, UART_IER_THRI);
		udelay(1); /* allow THRE to set */
		iir1 = serial_port_in(port, UART_IIR);
		serial_port_out(port, UART_IER, 0);
		serial_port_out_sync(port, UART_IER, UART_IER_THRI);
		udelay(1); /* allow a working UART time to re-assert THRE */
		iir = serial_port_in(port, UART_IIR);
		serial_port_out(port, UART_IER, 0);

		spin_unlock_irqrestore(&port->lock, flags);

		if (port->irqflags & IRQF_SHARED)
			enable_irq(port->irq);

		/*
		 * If the interrupt is not reasserted, or we otherwise
		 * don't trust the iir, setup a timer to kick the UART
		 * on a regular basis.
		 */
		if ((!(iir1 & UART_IIR_NO_INT) && (iir & UART_IIR_NO_INT)) ||
		    up->port.flags & UPF_BUG_THRE) {
			up->bugs |= UART_BUG_THRE;
		}
	}

	up->ops->setup_timer(up);

	/*
	 * Now, initialize the UART
	 */
	serial_port_out(port, UART_LCR, UART_LCR_WLEN8);

	spin_lock_irqsave(&port->lock, flags);
	if (up->port.flags & UPF_FOURPORT) {
		if (!up->port.irq)
			up->port.mctrl |= TIOCM_OUT1;
	} else
		/*
		 * Most PC uarts need OUT2 raised to enable interrupts.
		 */
		if (port->irq)
			up->port.mctrl |= TIOCM_OUT2;

	serial8250_set_mctrl(port, port->mctrl);

	/*
	 * Serial over Lan (SoL) hack:
	 * Intel 8257x Gigabit ethernet chips have a 16550 emulation, to be
	 * used for Serial Over Lan.  Those chips take a longer time than a
	 * normal serial device to signalize that a transmission data was
	 * queued. Due to that, the above test generally fails. One solution
	 * would be to delay the reading of iir. However, this is not
	 * reliable, since the timeout is variable. So, let's just don't
	 * test if we receive TX irq.  This way, we'll never enable
	 * UART_BUG_TXEN.
	 */
	if (up->port.quirks & UPQ_NO_TXEN_TEST)
		goto dont_test_tx_en;

	/*
	 * Do a quick test to see if we receive an interrupt when we enable
	 * the TX irq.
	 */
	serial_port_out(port, UART_IER, UART_IER_THRI);
	lsr = serial_port_in(port, UART_LSR);
	iir = serial_port_in(port, UART_IIR);
	serial_port_out(port, UART_IER, 0);

	if (lsr & UART_LSR_TEMT && iir & UART_IIR_NO_INT) {
		if (!(up->bugs & UART_BUG_TXEN)) {
			up->bugs |= UART_BUG_TXEN;
			dev_dbg(port->dev, "enabling bad tx status workarounds\n");
		}
	} else {
		up->bugs &= ~UART_BUG_TXEN;
	}

dont_test_tx_en:
	spin_unlock_irqrestore(&port->lock, flags);

	/*
	 * Clear the interrupt registers again for luck, and clear the
	 * saved flags to avoid getting false values from polling
	 * routines or the previous session.
	 */
	serial_port_in(port, UART_LSR);
	serial_port_in(port, UART_RX);
	serial_port_in(port, UART_IIR);
	serial_port_in(port, UART_MSR);
	up->lsr_saved_flags = 0;
	up->msr_saved_flags = 0;

	/*
	 * Request DMA channels for both RX and TX.
	 */
	if (up->dma) { /// no dma
		const char *msg = NULL;

		if (uart_console(port))
			msg = "forbid DMA for kernel console";
		else if (serial8250_request_dma(up))
			msg = "failed to request DMA";
		if (msg) {
			dev_warn_ratelimited(port->dev, "%s\n", msg);
			up->dma = NULL;
		}
	}

	/*
	 * Set the IER shadow for rx interrupts but defer actual interrupt
	 * enable until after the FIFOs are enabled; otherwise, an already-
	 * active sender can swamp the interrupt handler with "too much work".
	 */
	up->ier = UART_IER_RLSI | UART_IER_RDI;

	if (port->flags & UPF_FOURPORT) {
		unsigned int icp;
		/*
		 * Enable interrupts on the AST Fourport board
		 */
		icp = (port->iobase & 0xfe0) | 0x01f;
		outb_p(0x80, icp);
		inb_p(icp);
	}
	retval = 0;
out:
	serial8250_rpm_put(up);
	return retval;
}
EXPORT_SYMBOL_GPL(serial8250_do_startup);

static int serial8250_startup(struct uart_port *port)
{
	if (port->startup) /// 没设置
		return port->startup(port);
	return serial8250_do_startup(port);
}

void serial8250_do_shutdown(struct uart_port *port)
{
	struct uart_8250_port *up = up_to_u8250p(port);
	unsigned long flags;

	serial8250_rpm_get(up);
	/*
	 * Disable interrupts from this port
	 */
	spin_lock_irqsave(&port->lock, flags);
	up->ier = 0;
	serial_port_out(port, UART_IER, 0);
	spin_unlock_irqrestore(&port->lock, flags);

	synchronize_irq(port->irq);

	if (up->dma)
		serial8250_release_dma(up);

	spin_lock_irqsave(&port->lock, flags);
	if (port->flags & UPF_FOURPORT) {
		/* reset interrupts on the AST Fourport board */
		inb((port->iobase & 0xfe0) | 0x1f);
		port->mctrl |= TIOCM_OUT1;
	} else
		port->mctrl &= ~TIOCM_OUT2;

	serial8250_set_mctrl(port, port->mctrl);
	spin_unlock_irqrestore(&port->lock, flags);

	/*
	 * Disable break condition and FIFOs
	 */
	serial_port_out(port, UART_LCR,
			serial_port_in(port, UART_LCR) & ~UART_LCR_SBC);
	serial8250_clear_fifos(up);

// #ifdef CONFIG_SERIAL_8250_RSA
// 	/*
// 	 * Reset the RSA board back to 115kbps compat mode.
// 	 */
// 	disable_rsa(up);
// #endif

	/*
	 * Read data port to reset things, and then unlink from
	 * the IRQ chain.
	 */
	serial_port_in(port, UART_RX);
	serial8250_rpm_put(up);

	up->ops->release_irq(up);
}
EXPORT_SYMBOL_GPL(serial8250_do_shutdown);

static void serial8250_shutdown(struct uart_port *port)
{
	if (port->shutdown)
		port->shutdown(port);
	else
		serial8250_do_shutdown(port);
}

/* Nuvoton NPCM UARTs have a custom divisor calculation */
static unsigned int npcm_get_divisor(struct uart_8250_port *up,
		unsigned int baud)
{
	struct uart_port *port = &up->port;

	return DIV_ROUND_CLOSEST(port->uartclk, 16 * baud + 2) - 2;
}

static unsigned int serial8250_do_get_divisor(struct uart_port *port,
					      unsigned int baud,
					      unsigned int *frac)
{
	struct uart_8250_port *up = up_to_u8250p(port);
	unsigned int quot;

	/*
	 * Handle magic divisors for baud rates above baud_base on
	 * SMSC SuperIO chips.
	 *
	 */
	if ((port->flags & UPF_MAGIC_MULTIPLIER) &&
	    baud == (port->uartclk/4))
		quot = 0x8001;
	else if ((port->flags & UPF_MAGIC_MULTIPLIER) &&
		 baud == (port->uartclk/8))
		quot = 0x8002;
	else if (up->port.type == PORT_NPCM)
		quot = npcm_get_divisor(up, baud);
	else
		quot = uart_get_divisor(port, baud);

	/*
	 * Oxford Semi 952 rev B workaround
	 */
	if (up->bugs & UART_BUG_QUOT && (quot & 0xff) == 0)
		quot++;

	return quot;
}

static unsigned int serial8250_get_divisor(struct uart_port *port,
					   unsigned int baud,
					   unsigned int *frac)
{
	if (port->get_divisor)
		return port->get_divisor(port, baud, frac);

	return serial8250_do_get_divisor(port, baud, frac);
}

static unsigned char serial8250_compute_lcr(struct uart_8250_port *up,
					    tcflag_t c_cflag)
{
	unsigned char cval;

	switch (c_cflag & CSIZE) {
	case CS5:
		cval = UART_LCR_WLEN5;
		break;
	case CS6:
		cval = UART_LCR_WLEN6;
		break;
	case CS7:
		cval = UART_LCR_WLEN7;
		break;
	default:
	case CS8:
		cval = UART_LCR_WLEN8;
		break;
	}

	if (c_cflag & CSTOPB)
		cval |= UART_LCR_STOP;
	if (c_cflag & PARENB) {
		cval |= UART_LCR_PARITY;
		if (up->bugs & UART_BUG_PARITY)
			up->fifo_bug = true;
	}
	if (!(c_cflag & PARODD))
		cval |= UART_LCR_EPAR;
#ifdef CMSPAR
	if (c_cflag & CMSPAR)
		cval |= UART_LCR_SPAR;
#endif

	return cval;
}

void serial8250_do_set_divisor(struct uart_port *port, unsigned int baud,
			       unsigned int quot, unsigned int quot_frac)
{
	struct uart_8250_port *up = up_to_u8250p(port);

	/* Workaround to enable 115200 baud on OMAP1510 internal ports */
	if (is_omap1510_8250(up)) {
		if (baud == 115200) {
			quot = 1;
			serial_port_out(port, UART_OMAP_OSC_12M_SEL, 1);
		} else
			serial_port_out(port, UART_OMAP_OSC_12M_SEL, 0);
	}

	/*
	 * For NatSemi, switch to bank 2 not bank 1, to avoid resetting EXCR2,
	 * otherwise just set DLAB
	 */
	if (up->capabilities & UART_NATSEMI)
		serial_port_out(port, UART_LCR, 0xe0);
	else
		serial_port_out(port, UART_LCR, up->lcr | UART_LCR_DLAB);

	serial_dl_write(up, quot);
}
EXPORT_SYMBOL_GPL(serial8250_do_set_divisor);

static void serial8250_set_divisor(struct uart_port *port, unsigned int baud,
				   unsigned int quot, unsigned int quot_frac)
{
	if (port->set_divisor)
		port->set_divisor(port, baud, quot, quot_frac);
	else
		serial8250_do_set_divisor(port, baud, quot, quot_frac);
}

static unsigned int serial8250_get_baud_rate(struct uart_port *port,
					     struct ktermios *termios,
					     struct ktermios *old)
{
	unsigned int tolerance = port->uartclk / 100;
	unsigned int min;
	unsigned int max;

	/*
	 * Handle magic divisors for baud rates above baud_base on SMSC
	 * Super I/O chips.  Enable custom rates of clk/4 and clk/8, but
	 * disable divisor values beyond 32767, which are unavailable.
	 */
	if (port->flags & UPF_MAGIC_MULTIPLIER) {
		min = port->uartclk / 16 / UART_DIV_MAX >> 1;
		max = (port->uartclk + tolerance) / 4;
	} else {
		min = port->uartclk / 16 / UART_DIV_MAX;
		max = (port->uartclk + tolerance) / 16;
	}

	/*
	 * Ask the core to calculate the divisor for us.
	 * Allow 1% tolerance at the upper limit so uart clks marginally
	 * slower than nominal still match standard baud rates without
	 * causing transmission errors.
	 */
	return uart_get_baud_rate(port, termios, old, min, max);
}

/*
 * Note in order to avoid the tty port mutex deadlock don't use the next method
 * within the uart port callbacks. Primarily it's supposed to be utilized to
 * handle a sudden reference clock rate change.
 */
void serial8250_update_uartclk(struct uart_port *port, unsigned int uartclk)
{
	struct uart_8250_port *up = up_to_u8250p(port);
	struct tty_port *tport = &port->state->port;
	unsigned int baud, quot, frac = 0;
	struct ktermios *termios;
	struct tty_struct *tty;
	unsigned long flags;

	tty = tty_port_tty_get(tport);
	if (!tty) {
		mutex_lock(&tport->mutex);
		port->uartclk = uartclk;
		mutex_unlock(&tport->mutex);
		return;
	}

	down_write(&tty->termios_rwsem);
	mutex_lock(&tport->mutex);

	if (port->uartclk == uartclk)
		goto out_lock;

	port->uartclk = uartclk;

	if (!tty_port_initialized(tport))
		goto out_lock;

	termios = &tty->termios;

	baud = serial8250_get_baud_rate(port, termios, NULL);
	quot = serial8250_get_divisor(port, baud, &frac);

	serial8250_rpm_get(up);
	spin_lock_irqsave(&port->lock, flags);

	uart_update_timeout(port, termios->c_cflag, baud);

	serial8250_set_divisor(port, baud, quot, frac);
	serial_port_out(port, UART_LCR, up->lcr);

	spin_unlock_irqrestore(&port->lock, flags);
	serial8250_rpm_put(up);

out_lock:
	mutex_unlock(&tport->mutex);
	up_write(&tty->termios_rwsem);
	tty_kref_put(tty);
}
EXPORT_SYMBOL_GPL(serial8250_update_uartclk);

void
serial8250_do_set_termios(struct uart_port *port, struct ktermios *termios,
			  struct ktermios *old)
{
	struct uart_8250_port *up = up_to_u8250p(port);
	unsigned char cval;
	unsigned long flags;
	unsigned int baud, quot, frac = 0;

	// if (up->capabilities & UART_CAP_MINI) { /// 应该skip，没有UART_CAP_MINI
	// 	termios->c_cflag &= ~(CSTOPB | PARENB | PARODD | CMSPAR);
	// 	if ((termios->c_cflag & CSIZE) == CS5 ||
	// 	    (termios->c_cflag & CSIZE) == CS6)
	// 		termios->c_cflag = (termios->c_cflag & ~CSIZE) | CS7;
	// }
	cval = serial8250_compute_lcr(up, termios->c_cflag);

	baud = serial8250_get_baud_rate(port, termios, old);
	quot = serial8250_get_divisor(port, baud, &frac);

	/*
	 * Ok, we're now changing the port state.  Do it with
	 * interrupts disabled.
	 */
	serial8250_rpm_get(up);
	spin_lock_irqsave(&port->lock, flags);

	up->lcr = cval;					/* Save computed LCR */

	if (up->capabilities & UART_CAP_FIFO && port->fifosize > 1) {
		/* NOTE: If fifo_bug is not set, a user can set RX_trigger. */
		if ((baud < 2400 && !up->dma) || up->fifo_bug) {
			up->fcr &= ~UART_FCR_TRIGGER_MASK;
			up->fcr |= UART_FCR_TRIGGER_1;
		}
	}

	/*
	 * MCR-based auto flow control.  When AFE is enabled, RTS will be
	 * deasserted when the receive FIFO contains more characters than
	 * the trigger, or the MCR RTS bit is cleared.
	 */
	if (up->capabilities & UART_CAP_AFE) {
		up->mcr &= ~UART_MCR_AFE;
		if (termios->c_cflag & CRTSCTS)
			up->mcr |= UART_MCR_AFE;
	}

	/*
	 * Update the per-port timeout.
	 */
	uart_update_timeout(port, termios->c_cflag, baud);

	port->read_status_mask = UART_LSR_OE | UART_LSR_THRE | UART_LSR_DR;
	if (termios->c_iflag & INPCK)
		port->read_status_mask |= UART_LSR_FE | UART_LSR_PE;
	if (termios->c_iflag & (IGNBRK | BRKINT | PARMRK))
		port->read_status_mask |= UART_LSR_BI;

	/*
	 * Characteres to ignore
	 */
	port->ignore_status_mask = 0;
	if (termios->c_iflag & IGNPAR)
		port->ignore_status_mask |= UART_LSR_PE | UART_LSR_FE;
	if (termios->c_iflag & IGNBRK) {
		port->ignore_status_mask |= UART_LSR_BI;
		/*
		 * If we're ignoring parity and break indicators,
		 * ignore overruns too (for real raw support).
		 */
		if (termios->c_iflag & IGNPAR)
			port->ignore_status_mask |= UART_LSR_OE;
	}

	/*
	 * ignore all characters if CREAD is not set
	 */
	if ((termios->c_cflag & CREAD) == 0)
		port->ignore_status_mask |= UART_LSR_DR;

	/*
	 * CTS flow control flag and modem status interrupts
	 */
	up->ier &= ~UART_IER_MSI;
	if (!(up->bugs & UART_BUG_NOMSR) &&
			UART_ENABLE_MS(&up->port, termios->c_cflag))
		up->ier |= UART_IER_MSI;
	if (up->capabilities & UART_CAP_UUE)
		up->ier |= UART_IER_UUE;
	if (up->capabilities & UART_CAP_RTOIE)
		up->ier |= UART_IER_RTOIE;

	serial_port_out(port, UART_IER, up->ier);

	// if (up->capabilities & UART_CAP_EFR) {
	// 	unsigned char efr = 0;
	// 	/*
	// 	 * TI16C752/Startech hardware flow control.  FIXME:
	// 	 * - TI16C752 requires control thresholds to be set.
	// 	 * - UART_MCR_RTS is ineffective if auto-RTS mode is enabled.
	// 	 */
	// 	if (termios->c_cflag & CRTSCTS)
	// 		efr |= UART_EFR_CTS;

	// 	serial_port_out(port, UART_LCR, UART_LCR_CONF_MODE_B);
	// 	if (port->flags & UPF_EXAR_EFR)
	// 		serial_port_out(port, UART_XR_EFR, efr);
	// 	else
	// 		serial_port_out(port, UART_EFR, efr);
	// }

	serial8250_set_divisor(port, baud, quot, frac);

	/*
	 * LCR DLAB must be set to enable 64-byte FIFO mode. If the FCR
	 * is written without DLAB set, this mode will be disabled.
	 */
	// if (port->type == PORT_16750)
	// 	serial_port_out(port, UART_FCR, up->fcr);

	serial_port_out(port, UART_LCR, up->lcr);	/* reset DLAB */
	if (port->type != PORT_16750) {
		/* emulated UARTs (Lucent Venus 167x) need two steps */
		if (up->fcr & UART_FCR_ENABLE_FIFO)
			serial_port_out(port, UART_FCR, UART_FCR_ENABLE_FIFO);
		serial_port_out(port, UART_FCR, up->fcr);	/* set fcr */
	}
	serial8250_set_mctrl(port, port->mctrl);
	spin_unlock_irqrestore(&port->lock, flags);
	serial8250_rpm_put(up);

	/* Don't rewrite B0 */
	if (tty_termios_baud_rate(termios))
		tty_termios_encode_baud_rate(termios, baud, baud);

	serial_port_in(port, UART_RX);
}
EXPORT_SYMBOL(serial8250_do_set_termios);

static void
serial8250_set_termios(struct uart_port *port, struct ktermios *termios,
		       struct ktermios *old)
{
	if (port->set_termios) /// 先检查8250_dw.c 下层driver是否实现了uart_port->set_termios
		port->set_termios(port, termios, old);
	else
		serial8250_do_set_termios(port, termios, old);
}

void serial8250_do_set_ldisc(struct uart_port *port, struct ktermios *termios)
{
	if (termios->c_line == N_PPS) {
		port->flags |= UPF_HARDPPS_CD;
		spin_lock_irq(&port->lock);
		serial8250_enable_ms(port);
		spin_unlock_irq(&port->lock);
	} else {
		port->flags &= ~UPF_HARDPPS_CD;
		if (!UART_ENABLE_MS(port, termios->c_cflag)) {
			spin_lock_irq(&port->lock);
			serial8250_disable_ms(port);
			spin_unlock_irq(&port->lock);
		}
	}
}
EXPORT_SYMBOL_GPL(serial8250_do_set_ldisc);

static void
serial8250_set_ldisc(struct uart_port *port, struct ktermios *termios)
{
	if (port->set_ldisc)
		port->set_ldisc(port, termios);
	else
		serial8250_do_set_ldisc(port, termios);
}

void serial8250_do_pm(struct uart_port *port, unsigned int state,
		      unsigned int oldstate)
{
	struct uart_8250_port *p = up_to_u8250p(port);

	serial8250_set_sleep(p, state != 0);
}
EXPORT_SYMBOL(serial8250_do_pm);

static void
serial8250_pm(struct uart_port *port, unsigned int state,
	      unsigned int oldstate)
{
	if (port->pm)
		port->pm(port, state, oldstate);
	else
		serial8250_do_pm(port, state, oldstate);
}

static unsigned int serial8250_port_size(struct uart_8250_port *pt)
{
	if (pt->port.mapsize)
		return pt->port.mapsize;
	// if (pt->port.iotype == UPIO_AU) {
	// 	if (pt->port.type == PORT_RT2880)
	// 		return 0x100;
	// 	return 0x1000;
	// }
	// if (is_omap1_8250(pt))
	// 	return 0x16 << pt->port.regshift;

	return 8 << pt->port.regshift; // 32
}

/*
 * Resource handling.
 */
static int serial8250_request_std_resource(struct uart_8250_port *up)
{
	unsigned int size = serial8250_port_size(up);
	struct uart_port *port = &up->port;
	int ret = 0;

	switch (port->iotype) {
	case UPIO_AU:
	case UPIO_TSI:
	case UPIO_MEM32:
	case UPIO_MEM32BE:
	case UPIO_MEM16:
	case UPIO_MEM:
		if (!port->mapbase) {
			ret = -EINVAL;
			break;
		}

		if (!request_mem_region(port->mapbase, size, "serial")) { /// ??? 这边好像会走?，8250_dw.c中没有
			ret = -EBUSY;
			break;
		}

		// if (port->flags & UPF_IOREMAP) { /// 没有这个flag
		// 	port->membase = ioremap(port->mapbase, size);
		// 	if (!port->membase) {
		// 		release_mem_region(port->mapbase, size);
		// 		ret = -ENOMEM;
		// 	}
		// }
		// break;

	// case UPIO_HUB6:
	// case UPIO_PORT:
	// 	if (!request_region(port->iobase, size, "serial"))
	// 		ret = -EBUSY;
	// 	break;
	}
	return ret;
}

static void serial8250_release_std_resource(struct uart_8250_port *up)
{
	unsigned int size = serial8250_port_size(up);
	struct uart_port *port = &up->port;

	switch (port->iotype) {
	case UPIO_AU:
	case UPIO_TSI:
	case UPIO_MEM32:
	case UPIO_MEM32BE:
	case UPIO_MEM16:
	case UPIO_MEM:
		if (!port->mapbase)
			break;

		if (port->flags & UPF_IOREMAP) {
			iounmap(port->membase);
			port->membase = NULL;
		}

		release_mem_region(port->mapbase, size);
		break;

	case UPIO_HUB6:
	case UPIO_PORT:
		release_region(port->iobase, size);
		break;
	}
}

static void serial8250_release_port(struct uart_port *port)
{
	struct uart_8250_port *up = up_to_u8250p(port);

	serial8250_release_std_resource(up);
}

static int serial8250_request_port(struct uart_port *port)
{
	struct uart_8250_port *up = up_to_u8250p(port);

	return serial8250_request_std_resource(up);
}

static int fcr_get_rxtrig_bytes(struct uart_8250_port *up)
{
	const struct serial8250_config *conf_type = &uart_config[up->port.type];
	unsigned char bytes;

	bytes = conf_type->rxtrig_bytes[UART_FCR_R_TRIG_BITS(up->fcr)];

	return bytes ? bytes : -EOPNOTSUPP;
}

static int bytes_to_fcr_rxtrig(struct uart_8250_port *up, unsigned char bytes)
{
	const struct serial8250_config *conf_type = &uart_config[up->port.type];
	int i;

	if (!conf_type->rxtrig_bytes[UART_FCR_R_TRIG_BITS(UART_FCR_R_TRIG_00)])
		return -EOPNOTSUPP;

	for (i = 1; i < UART_FCR_R_TRIG_MAX_STATE; i++) {
		if (bytes < conf_type->rxtrig_bytes[i])
			/* Use the nearest lower value */
			return (--i) << UART_FCR_R_TRIG_SHIFT;
	}

	return UART_FCR_R_TRIG_11;
}

static int do_get_rxtrig(struct tty_port *port)
{
	struct uart_state *state = container_of(port, struct uart_state, port);
	struct uart_port *uport = state->uart_port;
	struct uart_8250_port *up = up_to_u8250p(uport);

	if (!(up->capabilities & UART_CAP_FIFO) || uport->fifosize <= 1)
		return -EINVAL;

	return fcr_get_rxtrig_bytes(up);
}

static int do_serial8250_get_rxtrig(struct tty_port *port)
{
	int rxtrig_bytes;

	mutex_lock(&port->mutex);
	rxtrig_bytes = do_get_rxtrig(port);
	mutex_unlock(&port->mutex);

	return rxtrig_bytes;
}

static ssize_t rx_trig_bytes_show(struct device *dev,
	struct device_attribute *attr, char *buf)
{
	struct tty_port *port = dev_get_drvdata(dev);
	int rxtrig_bytes;

	rxtrig_bytes = do_serial8250_get_rxtrig(port);
	if (rxtrig_bytes < 0)
		return rxtrig_bytes;

	return snprintf(buf, PAGE_SIZE, "%d\n", rxtrig_bytes);
}

static int do_set_rxtrig(struct tty_port *port, unsigned char bytes)
{
	struct uart_state *state = container_of(port, struct uart_state, port);
	struct uart_port *uport = state->uart_port;
	struct uart_8250_port *up = up_to_u8250p(uport);
	int rxtrig;

	if (!(up->capabilities & UART_CAP_FIFO) || uport->fifosize <= 1 ||
	    up->fifo_bug)
		return -EINVAL;

	rxtrig = bytes_to_fcr_rxtrig(up, bytes);
	if (rxtrig < 0)
		return rxtrig;

	serial8250_clear_fifos(up);
	up->fcr &= ~UART_FCR_TRIGGER_MASK;
	up->fcr |= (unsigned char)rxtrig;
	serial_out(up, UART_FCR, up->fcr);
	return 0;
}

static int do_serial8250_set_rxtrig(struct tty_port *port, unsigned char bytes)
{
	int ret;

	mutex_lock(&port->mutex);
	ret = do_set_rxtrig(port, bytes);
	mutex_unlock(&port->mutex);

	return ret;
}

static ssize_t rx_trig_bytes_store(struct device *dev,
	struct device_attribute *attr, const char *buf, size_t count)
{
	struct tty_port *port = dev_get_drvdata(dev);
	unsigned char bytes;
	int ret;

	if (!count)
		return -EINVAL;

	ret = kstrtou8(buf, 10, &bytes);
	if (ret < 0)
		return ret;

	ret = do_serial8250_set_rxtrig(port, bytes);
	if (ret < 0)
		return ret;

	return count;
}

static DEVICE_ATTR_RW(rx_trig_bytes); /// 相当于DEVICE_ATTR(rx_trig_bytes, 0644, rx_trig_bytes_show, rx_trig_bytes_store);

static struct attribute *serial8250_dev_attrs[] = {
	&dev_attr_rx_trig_bytes.attr,
	NULL
};

static struct attribute_group serial8250_dev_attr_group = {
	.attrs = serial8250_dev_attrs,
};

static void register_dev_spec_attr_grp(struct uart_8250_port *up)
{
	const struct serial8250_config *conf_type = &uart_config[up->port.type];

	if (conf_type->rxtrig_bytes[0])
		up->port.attr_group = &serial8250_dev_attr_group;
}

static void serial8250_config_port(struct uart_port *port, int flags)
{
	struct uart_8250_port *up = up_to_u8250p(port);
	int ret;

	/*
	 * Find the region that we can probe for.  This in turn
	 * tells us whether we can probe for the type of port.
	 */
	ret = serial8250_request_std_resource(up);
	if (ret < 0)
		return;

	if (port->iotype != up->cur_iotype) /// skip serial8250_register_8250_port->serial8250_set_defaults->set_io_from_up设置过了
		set_io_from_upio(port);

	if (flags & UART_CONFIG_TYPE) /// yes
		autoconfig(up);

	// /* if access method is AU, it is a 16550 with a quirk */
	// if (port->type == PORT_16550A && port->iotype == UPIO_AU) /// skip
	// 	up->bugs |= UART_BUG_NOMSR;

	// /* HW bugs may trigger IRQ while IIR == NO_INT */
	// if (port->type == PORT_TEGRA) /// skip
	// 	up->bugs |= UART_BUG_NOMSR;

	// if (port->type != PORT_UNKNOWN && flags & UART_CONFIG_IRQ) /// skip
	// 	autoconfig_irq(up);

	// if (port->type == PORT_UNKNOWN) /// skip
	// 	serial8250_release_std_resource(up);

	register_dev_spec_attr_grp(up); /// 创建sysfs接口
	up->fcr = uart_config[up->port.type].fcr;
}

static int
serial8250_verify_port(struct uart_port *port, struct serial_struct *ser)
{
	if (ser->irq >= nr_irqs || ser->irq < 0 ||
	    ser->baud_base < 9600 || ser->type < PORT_UNKNOWN ||
	    ser->type >= ARRAY_SIZE(uart_config) || ser->type == PORT_CIRRUS ||
	    ser->type == PORT_STARTECH)
		return -EINVAL;
	return 0;
}

static const char *serial8250_type(struct uart_port *port)
{
	int type = port->type;

	if (type >= ARRAY_SIZE(uart_config))
		type = 0;
	return uart_config[type].name;
}

static const struct uart_ops serial8250_pops = {
	.tx_empty	= serial8250_tx_empty, /// 判断tx transmitter 和 fifo是否为空
	.set_mctrl	= serial8250_set_mctrl, /// 设置MCR寄存器(0x10)
	.get_mctrl	= serial8250_get_mctrl, /// 读取MCR寄存器
	.stop_tx	= serial8250_stop_tx, /// 关闭UART_IER_THRI中断
	.start_tx	= serial8250_start_tx, /// 打开UART_IER_THRI中断
	.throttle	= serial8250_throttle, /// NULL
	.unthrottle	= serial8250_unthrottle, /// NULL
	.stop_rx	= serial8250_stop_rx, /// 关闭(UART_IER_RLSI | UART_IER_RDI)中断
	.enable_ms	= serial8250_enable_ms, /// enable modem status interrupt
	.break_ctl	= serial8250_break_ctl, /// break control bit
	// serial_core.c uart_open ---> tty_driver->ops->open ---> tty_port_open ---> port->ops->activate
	///  ---> uart_startup ---> uart_port_startup ---> uart_port->ops->startup
	.startup	= serial8250_startup, /// open /dev/ttyS1 会调用到.
	.shutdown	= serial8250_shutdown,
	.set_termios	= serial8250_set_termios, /// 用户层uart设置函数最后会进入这里
	.set_ldisc	= serial8250_set_ldisc, /// 和line discipline
	.pm		= serial8250_pm, /// do nothing, flag没有can sleep
	.type		= serial8250_type, /// 返回16550A
	.release_port	= serial8250_release_port, /// release resource
	.request_port	= serial8250_request_port, /// request resource
	.config_port	= serial8250_config_port, /// 在uart_add_one_port中会调用到
	.verify_port	= serial8250_verify_port, /// verify
// #ifdef CONFIG_CONSOLE_POLL
// 	.poll_get_char = serial8250_get_poll_char,
// 	.poll_put_char = serial8250_put_poll_char,
// #endif
};

void serial8250_init_port(struct uart_8250_port *up)
{
	struct uart_port *port = &up->port;

	spin_lock_init(&port->lock);
	port->ops = &serial8250_pops;
	port->has_sysrq = IS_ENABLED(CONFIG_SERIAL_8250_CONSOLE);

	up->cur_iotype = 0xFF;
}
EXPORT_SYMBOL_GPL(serial8250_init_port);

void serial8250_set_defaults(struct uart_8250_port *up)
{
	struct uart_port *port = &up->port;

	// if (up->port.flags & UPF_FIXED_TYPE) { /// skip 8250_dw 和 core都没指定这个属性
	// 	unsigned int type = up->port.type;

	// 	if (!up->port.fifosize)
	// 		up->port.fifosize = uart_config[type].fifo_size;
	// 	if (!up->tx_loadsz)
	// 		up->tx_loadsz = uart_config[type].tx_loadsz;
	// 	if (!up->capabilities)
	// 		up->capabilities = uart_config[type].flags;
	// }

	set_io_from_upio(port);

	/* default dma handlers */
	// if (up->dma) { /// 应该都没指定
	// 	if (!up->dma->tx_dma)
	// 		up->dma->tx_dma = serial8250_tx_dma;
	// 	if (!up->dma->rx_dma)
	// 		up->dma->rx_dma = serial8250_rx_dma;
	// }
}
EXPORT_SYMBOL_GPL(serial8250_set_defaults);

#ifdef CONFIG_SERIAL_8250_CONSOLE

static void serial8250_console_putchar(struct uart_port *port, int ch)
{
	struct uart_8250_port *up = up_to_u8250p(port);

	wait_for_xmitr(up, UART_LSR_THRE);
	serial_port_out(port, UART_TX, ch);
}

/*
 *	Restore serial console when h/w power-off detected
 */
static void serial8250_console_restore(struct uart_8250_port *up)
{
	struct uart_port *port = &up->port;
	struct ktermios termios;
	unsigned int baud, quot, frac = 0;

	termios.c_cflag = port->cons->cflag;
	termios.c_ispeed = port->cons->ispeed;
	termios.c_ospeed = port->cons->ospeed;
	if (port->state->port.tty && termios.c_cflag == 0) {
		termios.c_cflag = port->state->port.tty->termios.c_cflag;
		termios.c_ispeed = port->state->port.tty->termios.c_ispeed;
		termios.c_ospeed = port->state->port.tty->termios.c_ospeed;
	}

	baud = serial8250_get_baud_rate(port, &termios, NULL);
	quot = serial8250_get_divisor(port, baud, &frac);

	serial8250_set_divisor(port, baud, quot, frac);
	serial_port_out(port, UART_LCR, up->lcr);
	serial8250_out_MCR(up, up->mcr | UART_MCR_DTR | UART_MCR_RTS);
}

/*
 *	Print a string to the serial port trying not to disturb
 *	any possible real use of the port...
 *
 *	The console_lock must be held when we get here.
 *
 *	Doing runtime PM is really a bad idea for the kernel console.
 *	Thus, we assume the function is called when device is powered up.
 */
void serial8250_console_write(struct uart_8250_port *up, const char *s,
			      unsigned int count)
{
	struct uart_8250_em485 *em485 = up->em485;
	struct uart_port *port = &up->port;
	unsigned long flags;
	unsigned int ier;
	int locked = 1;

	touch_nmi_watchdog();

	if (oops_in_progress)
		locked = spin_trylock_irqsave(&port->lock, flags);
	else
		spin_lock_irqsave(&port->lock, flags);

	/*
	 *	First save the IER then disable the interrupts
	 */
	ier = serial_port_in(port, UART_IER);

	// if (up->capabilities & UART_CAP_UUE)
	// 	serial_port_out(port, UART_IER, UART_IER_UUE);
	// else
		serial_port_out(port, UART_IER, 0);

	/* check scratch reg to see if port powered off during system sleep */
	if (up->canary && (up->canary != serial_port_in(port, UART_SCR))) {
		serial8250_console_restore(up);
		up->canary = 0;
	}

	// if (em485) {
	// 	if (em485->tx_stopped)
	// 		up->rs485_start_tx(up);
	// 	mdelay(port->rs485.delay_rts_before_send);
	// }

	uart_console_write(port, s, count, serial8250_console_putchar);

	/*
	 *	Finally, wait for transmitter to become empty
	 *	and restore the IER
	 */
	wait_for_xmitr(up, BOTH_EMPTY);

	if (em485) {
		mdelay(port->rs485.delay_rts_after_send);
		if (em485->tx_stopped)
			up->rs485_stop_tx(up);
	}

	serial_port_out(port, UART_IER, ier);

	/*
	 *	The receive handling will happen properly because the
	 *	receive ready bit will still be set; it is not cleared
	 *	on read.  However, modem control will not, we must
	 *	call it if we have saved something in the saved flags
	 *	while processing with interrupts off.
	 */
	if (up->msr_saved_flags)
		serial8250_modem_status(up);

	if (locked)
		spin_unlock_irqrestore(&port->lock, flags);
}

static unsigned int probe_baud(struct uart_port *port)
{
	unsigned char lcr, dll, dlm;
	unsigned int quot;

	lcr = serial_port_in(port, UART_LCR);
	serial_port_out(port, UART_LCR, lcr | UART_LCR_DLAB);
	dll = serial_port_in(port, UART_DLL);
	dlm = serial_port_in(port, UART_DLM);
	serial_port_out(port, UART_LCR, lcr);

	quot = (dlm << 8) | dll;
	return (port->uartclk / 16) / quot;
}

int serial8250_console_setup(struct uart_port *port, char *options, bool probe)
{
	int baud = 9600;
	int bits = 8;
	int parity = 'n';
	int flow = 'n';
	int ret;

	if (!port->iobase && !port->membase)
		return -ENODEV;

	if (options)
		uart_parse_options(options, &baud, &parity, &bits, &flow);
	else if (probe)
		baud = probe_baud(port);

	ret = uart_set_options(port, port->cons, baud, parity, bits, flow);
	if (ret)
		return ret;

	if (port->dev)
		pm_runtime_get_sync(port->dev);

	return 0;
}

int serial8250_console_exit(struct uart_port *port)
{
	if (port->dev)
		pm_runtime_put_sync(port->dev);

	return 0;
}

#endif /* CONFIG_SERIAL_8250_CONSOLE */

MODULE_LICENSE("GPL");
