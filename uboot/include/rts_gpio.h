#ifndef __GPIO_HW_H
#define __GPIO_HW_H

//#include <asm/io.h>
#include <asm/gpio.h>

#define RTS_GPIO_BASEADDR 0x18800000
#define RTS_GPIOS_NUM 89

struct rts_gpio_priv {
	u32 base_addr;
	u8 audio_adda_gpio_value;
	u8 usb0_gpio_value;
	u8 usb1_gpio_value;
};

struct pinregs {
	u32 gpio_oe;
	u32 gpio_value;
	u32 pullctrl;
	u32 gpio_int_en;
	u32 gpio_int;
	u32 drv_sel;
	u32 sr_ctrl;
	u32 pad_cfg;
};

struct sharepin_cfg_addr {
	int pinl;
	int pinh;
	int pint;
	int pinaddr;
};

enum {
	GPIO_TYPE_GENERIC,
	GPIO_TYPE_UART0,
	GPIO_TYPE_UART1,
	GPIO_TYPE_UART2,
	GPIO_TYPE_PWM,
	GPIO_TYPE_I2C,
	GPIO_TYPE_SDIO0,
	GPIO_TYPE_SDIO1,
	GPIO_TYPE_SSOR,
	GPIO_TYPE_DMIC,
	GPIO_TYPE_ADDA,
	GPIO_TYPE_I2S,
	GPIO_TYPE_SARADC,
	GPIO_TYPE_USBH,
	GPIO_TYPE_USBD,
	GPIO_TYPE_USB3,
	GPIO_TYPE_SSORI2C,
	GPIO_TYPE_SPI,
};

#define GPIO_OE					0x0000
#define UART0_GPIO_OE			0x0020
#define UART1_GPIO_OE			0x0040
#define UART2_GPIO_OE			0x0060
#define PWM_GPIO_OE			0x0080
#define XB2_I2C_GPIO_OE		0x00a0
#define SD0_GPIO_OE			0x00c0
#define SD1_GPIO_OE			0x00e0
#define VIDEO_GPIO_OE			0x0100
#define DMIC_GPIO_OE			0x0130
#define AUDIO_ADDA_GPIO_OE		0x0150
#define I2S_GPIO_OE				0x0170
#define SARADC_GPIO_OE			0x0190
#define USB0_GPIO_OE			0x01f0
#define USB1_GPIO_OE			0x0210
#define USB2_GPIO_OE			0x0230
#define SPI_GPIO_OE			0x0250
#define SSOR_I2C_GPIO_OE		0x0270
#endif
