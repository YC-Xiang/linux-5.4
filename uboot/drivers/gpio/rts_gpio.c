#include <common.h>
#include <dm.h>
#include <linux/errno.h>
#include <asm/gpio.h>
#include <asm/io.h>
#include <rts_gpio.h>
#include <dm/device-internal.h>

enum {
	RTS_GPIOS_NUM = 89,
};

#define RTS_GETFIELD(val, width, offset)	\
			((val >> offset) & ((1 << width) - 1))

#define RTS_SETFIELD(reg, field, width, offset)	((reg & \
			(~(((1 << width) - 1) <<	offset))) \
			| ((field & ((1 << width) - 1)) << offset))

static void rts_gpio_set_field(u32 reg,
			       unsigned int field, unsigned int width,
			       unsigned int offset)
{
	unsigned int val = readl(reg);

	val = RTS_SETFIELD(val, field, width, offset);
	writel(val, reg);
}

static unsigned int rts_gpio_get_field(u32 reg,
				       unsigned int width, unsigned int offset)
{
	unsigned int val = readl(reg);

	return RTS_GETFIELD(val, width, offset);
}

struct rts_gpio_priv {
	u32 base_addr;
	u64 pinsmask[2];
	u8 audio_adda_gpio_value;
	u8 usb0_gpio_value;
	u8 usb1_gpio_value;
};

static struct sharepin_cfg_addr pincfgaddr[] = {
	{.pinl = 0, .pinh = 15,
	 .pint = GPIO_TYPE_GENERIC, .pinaddr = GPIO_OE},
	{.pinl = 16, .pinh = 19,
	 .pint = GPIO_TYPE_UART0, .pinaddr = UART0_GPIO_OE},
	{.pinl = 20, .pinh = 21,
	 .pint = GPIO_TYPE_UART1, .pinaddr = UART1_GPIO_OE},
	{.pinl = 22, .pinh = 25,
	 .pint = GPIO_TYPE_UART2, .pinaddr = UART2_GPIO_OE},
	{.pinl = 26, .pinh = 29,
	 .pint = GPIO_TYPE_PWM, .pinaddr = PWM_GPIO_OE},
	{.pinl = 30, .pinh = 31,
	 .pint = GPIO_TYPE_I2C, .pinaddr = XB2_I2C_GPIO_OE},
	{.pinl = 32, .pinh = 39,
	 .pint = GPIO_TYPE_SDIO0, .pinaddr = SD0_GPIO_OE},
	{.pinl = 40, .pinh = 47,
	 .pint = GPIO_TYPE_SDIO1, .pinaddr = SD1_GPIO_OE},
	{.pinl = 48, .pinh = 60,
	 .pint = GPIO_TYPE_SSOR, .pinaddr = VIDEO_GPIO_OE},
	{.pinl = 61, .pinh = 64,
	 .pint = GPIO_TYPE_DMIC, .pinaddr = DMIC_GPIO_OE},
	{.pinl = 65, .pinh = 68,
	 .pint = GPIO_TYPE_ADDA, .pinaddr = AUDIO_ADDA_GPIO_OE},
	{.pinl = 69, .pinh = 73,
	 .pint = GPIO_TYPE_I2S, .pinaddr = I2S_GPIO_OE},
	{.pinl = 74, .pinh = 77,
	 .pint = GPIO_TYPE_SARADC, .pinaddr = SARADC_GPIO_OE},
	{.pinl = 78, .pinh = 79,
	 .pint = GPIO_TYPE_USBH, .pinaddr = USB0_GPIO_OE},
	{.pinl = 80, .pinh = 81,
	 .pint = GPIO_TYPE_USBD, .pinaddr = USB1_GPIO_OE},
	{.pinl = 82, .pinh = 84,
	 .pint = GPIO_TYPE_USB3, .pinaddr = USB2_GPIO_OE},
	{.pinl = 85, .pinh = 86,
	 .pint = GPIO_TYPE_SSORI2C, .pinaddr = SSOR_I2C_GPIO_OE},
	{.pinl = 87, .pinh = 88,
	 .pint = GPIO_TYPE_SPI, .pinaddr = SPI_GPIO_OE},
};

static struct sharepin_cfg_addr *rts_get_pinaddr(int pin)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(pincfgaddr); i++)
		if (pincfgaddr[i].pinl <= pin && pincfgaddr[i].pinh >= pin)
			return &pincfgaddr[i];

	return 0;
}

static int rts_gpio_request(struct udevice *dev, unsigned offset, const char *label)
{
	int i, j;
	struct rts_gpio_priv *priv = dev_get_priv(dev);
	u64 bs;
	printf("1111111111\n");
	if (offset > 89)
		return -EINVAL;

	i = offset > 63;
	j = i ? (offset - 64) : offset;
	bs = 1;
	bs <<= j;
	printf("22222222222\n");
	if (priv->pinsmask[i] & bs)
		return -EBUSY;

	priv->pinsmask[i] |= bs;
	printf("333333333333\n");
	printf("rts_gpio_request\n");

	return 0;
}

static int rts_gpio_free(struct udevice *dev, unsigned offset)
{
	int i, j;
	struct rts_gpio_priv *priv = dev_get_priv(dev);
	u64 bs;
	printf("rts_gpio_free\n");
	if (offset > 89)
		return -EINVAL;

	i = offset > 63;
	j = i ? (offset - 64) : offset;
	bs = 1;
	bs <<= j;

	if (priv->pinsmask[i] & bs) {
		priv->pinsmask[i] &= ~bs;
		return 0;
	} else {
		return -EINVAL;
	}
}

static int rts_gpio_get_value(struct udevice *dev, unsigned offset)
{
	struct pinregs *regs;
	struct sharepin_cfg_addr *sc;
	int bf;
	struct rts_gpio_priv *priv = dev_get_priv(dev);

	sc = rts_get_pinaddr(offset);
	regs = (struct pinregs *)sc->pinaddr;
	bf = offset - sc->pinl;

	if (sc->pinl == 65 &&
		rts_gpio_get_field(priv->base_addr + (u32)&(regs->gpio_oe), 1, bf))
		return (priv->audio_adda_gpio_value >> bf) & 0x1;
	if (sc->pinl == 78 &&
		rts_gpio_get_field(priv->base_addr + (u32)&(regs->gpio_oe), 1, bf))
		return (priv->usb0_gpio_value >> bf) & 0x1;
	if (sc->pinl == 80 &&
		rts_gpio_get_field(priv->base_addr + (u32)&(regs->gpio_oe), 1, bf))
		return (priv->usb1_gpio_value >> bf) & 0x1;

	return rts_gpio_get_field(priv->base_addr + (u32)&(regs->gpio_value),
				  1, bf);

	printf("rts_gpio_get_value\n");
	return 0;
}

static int rts_gpio_set_value(struct udevice *dev, unsigned offset,
				   int value)
{
	struct pinregs *regs;
	struct sharepin_cfg_addr *sc;
	int bf;
	struct rts_gpio_priv *priv = dev_get_priv(dev);

	sc = rts_get_pinaddr(offset);
	regs = (struct pinregs *)sc->pinaddr;
	bf = offset - sc->pinl;

	if (sc->pinl == 65) {
		if (value) {
			priv->audio_adda_gpio_value |= (1 << bf);
			writel(priv->audio_adda_gpio_value,
			       priv->base_addr + (u32)&(regs->gpio_value));
		} else {
			priv->audio_adda_gpio_value &= (~(1 << bf));
			writel(priv->audio_adda_gpio_value,
			       priv->base_addr + (u32)&(regs->gpio_value));
		}
	} else if (sc->pinl == 78) {
		if (value) {
			priv->usb0_gpio_value |= (1 << bf);
			writel(priv->usb0_gpio_value,
			       priv->base_addr + (u32)&(regs->gpio_value));
		} else {
			priv->usb0_gpio_value &= (~(1 << bf));
			writel(priv->usb0_gpio_value,
			       priv->base_addr + (u32)&(regs->gpio_value));
		}
	} else if (sc->pinl == 80) {
		if (value) {
			priv->usb1_gpio_value |= (1 << bf);
			writel(priv->usb1_gpio_value,
			       priv->base_addr + (u32)&(regs->gpio_value));
		} else {
			priv->usb1_gpio_value &= (~(1 << bf));
			writel(priv->usb1_gpio_value,
			       priv->base_addr + (u32)&(regs->gpio_value));
		}
	} else {
		if (value)
			rts_gpio_set_field(priv->base_addr + (u32)&regs->gpio_value, 1, 1, bf);
		else
			rts_gpio_set_field(priv->base_addr + (u32)&regs->gpio_value, 0, 1, bf);
	}

	printf("rts_gpio_set_value\n");
	return 0;
}

static int rts_gpio_direction_input(struct udevice *dev, unsigned offset)
{
	struct pinregs *regs;
	struct sharepin_cfg_addr *sc;
	int bf;
	struct rts_gpio_priv *priv = dev_get_priv(dev);

	sc = rts_get_pinaddr(offset);
	regs = (struct pinregs *)sc->pinaddr;
	bf = offset - sc->pinl;

	rts_gpio_set_field(priv->base_addr + (u32)&regs->gpio_oe, 0, 1, bf);

	printf("rts_gpio_direction_input\n");

	return 0;
}

static int rts_gpio_direction_output(struct udevice *dev, unsigned offset,
					  int value)
{
	struct pinregs *regs;
	struct sharepin_cfg_addr *sc;
	int bf;
	struct rts_gpio_priv *priv = dev_get_priv(dev);

	sc = rts_get_pinaddr(offset);
	regs = (struct pinregs *)sc->pinaddr;
	bf = offset - sc->pinl;

	rts_gpio_set_field(priv->base_addr + (u32)&regs->gpio_oe, 1, 1, bf);

	rts_gpio_set_value(dev, offset, value);

	printf("rts_gpio_direction_output\n");

	return 0;
}

static int rts_gpio_get_function(struct udevice *dev, unsigned offset)
{
	struct pinregs *regs;
	struct sharepin_cfg_addr *sc;
	int bf;
	struct rts_gpio_priv *priv = dev_get_priv(dev);
	int val;

	sc = rts_get_pinaddr(offset);
	regs = (struct pinregs *)sc->pinaddr;
	bf = offset - sc->pinl;

	val = rts_gpio_get_field(priv->base_addr + (u32)&(regs->gpio_oe),
				  1, bf);
	if (val)
		return GPIOF_OUTPUT;
	else
		return GPIOF_INPUT;
}

static const struct dm_gpio_ops rts_gpio_ops = {
	// .request = rts_gpio_request,
	// .rfree = rts_gpio_free,
	.direction_input	= rts_gpio_direction_input,
	.direction_output	= rts_gpio_direction_output,
	.get_value		= rts_gpio_get_value,
	.set_value		= rts_gpio_set_value,
	.get_function		= rts_gpio_get_function,
};

static int rts_gpio_probe(struct udevice *dev)
{
	struct gpio_dev_priv *uc_priv = dev_get_uclass_priv(dev);
	struct rts_gpio_priv *priv = dev_get_priv(dev);  // 私有数据
	struct rts_gpio_plat *plat = dev_get_plat(dev); // 静态dev里定义的数据

	priv->base_addr = plat->base_addr; // 0x18800000
	uc_priv->gpio_count = RTS_GPIOS_NUM;
	uc_priv->bank_name = plat->name;

	printf("rts gpio driver probe\n");

	return 0;
}

static const struct udevice_id rts_gpio_ids[] = {
	{.compatible = "realtek,rts3917-gpio"},
	{/* sentinel */}
};

U_BOOT_DRIVER(rts_gpio_drv) = {
	.name = "rts_gpio",
	.id = UCLASS_GPIO,
#if CONFIG_IS_ENABLED(OF_CONTROL)
	.of_match = rts_gpio_ids,
	.of_to_plat = rts_gpio_ofdata_to_platdata,
	.plat_auto = sizeof(struct rts_gpio_plat),
#endif
	.probe = rts_gpio_probe,
	.ops = &rts_gpio_ops,
	.priv_auto = sizeof(struct rts_gpio_priv),
};
