/*
 * Realtek Semiconductor Corp.
 *
 * Copyright 2012  Jethro Hsu (jethro@realtek.com)
 * Copyright 2012  Tony Wu (tonywu@realtek.com)
 */

#ifndef _SHEIPA_H_
#define _SHEIPA_H_

#include <generated/autoconf.h>
#include <generated/map.h>
#include "bspchip.h"

#define CONFIG_ETHADDR		DE:AD:BE:EF:01:01	/* Ethernet address */

/* valid baudrates */
#define CONFIG_SYS_BAUDRATE_TABLE	{ 9600, 19200, 38400, 57600, 115200 }

#define CONFIG_TIMESTAMP				/* Print image info with timestamp */
#undef  CONFIG_BOOTARGS

#define CONFIG_EXTRA_ENV_SETTINGS					\
	"addmisc=setenv bootargs ${bootargs} "				\
		"console=ttyS0,${baudrate} "				\
		"panic=1\0"						\
	"bootfile=/vmlinux.elf\0"					\
	"fdt_high=0xffffffff\0"						\
	"load=tftp 80500000 ${u-boot}\0"				\
	""

#define KERNEL_ADDR			__stringify(CONFIG_SYS_KERNEL_ADDR)
#define RAMDISK_ADDR			__stringify(CONFIG_SYS_RAMDISK_ADDR)
#define FDT_ADDR			__stringify(CONFIG_SYS_FDT_ADDR)
#define FIT_ADDR			__stringify(CONFIG_SYS_FIT_ADDR)
#define IMG_FLASH_BASE			__stringify(CONFIG_SYS_IMG_FLASH_ADDR)
#define IMG_FLASH_SIZE			__stringify(CONFIG_SYS_IMG_FLASH_SIZE)
#define FDT_FLASH_BASE			__stringify(CONFIG_SYS_FDT_FLASH_ADDR)
#define FDT_FLASH_SIZE			__stringify(CONFIG_SYS_FDT_FLASH_SIZE)

#if defined(CONFIG_CMD_SF)
#if defined(CONFIG_FIT)
#define _BOOTCOMMAND(a, b, c)		"sf probe 0:0;"			\
					"sf read "a" "b" "c";"		\
					"bootm "a""

#define CONFIG_BOOTCOMMAND		"bootm ${kernel_offset} - ${dtb_addr}"
#else
#ifdef CONFIG_TARGET_EMULATOR
#define CONFIG_BOOTCOMMAND		"bootm 0x81000000 - 0x80100000"
#else
#ifdef CONFIG_FAST_BOOT
#define CONFIG_BOOTCOMMAND		"bootm 0xa0000 - 0x80100000"
#else
#define CONFIG_BOOTCOMMAND		"bootm ${kernel_offset} - ${dtb_addr}"
#endif
#endif
#endif
#else
#ifdef CONFIG_FAST_BOOT
#define CONFIG_BOOTCOMMAND		"bootm 0x100000 - 0x80100000"
#else
#define CONFIG_BOOTCOMMAND		"bootm ${kernel_offset} - ${dtb_addr}"
#endif
#endif

#ifdef RTS_KERNELLOAD_CHECK
#define CONFIG_CHECK_DUALKERNEL_LOAD		RTS_KERNELLOAD_CHECK
#define CONFIG_BOOTCOMMAND			RTS_BOOTCMD
#endif

#define CONFIG_DFU_ALT_INFO		"0 ram 0x80400000 0x400000"
#define CONFIG_DFU_BUFSIZ		"0x400000"
#define	CONFIG_ENV_OVERWRITE			1

/*
 * Miscellaneous configurable options
 */
#undef CONFIG_SYS_LONGHELP				/* undef to save memory      */
#define CONFIG_SYS_CBSIZE		256		/* Console I/O Buffer Size   */

/* Print Buffer Size */
#define CONFIG_SYS_PBSIZE 		(CONFIG_SYS_CBSIZE+sizeof(CONFIG_SYS_PROMPT)+16)
#define CONFIG_SYS_MAXARGS		16		/* max number of command args */

/* Size of malloc() pool */
#define CONFIG_SYS_MALLOC_LEN           (CONFIG_ENV_SIZE + (8 << 20))
#define CONFIG_SYS_BOOTPARAMS_LEN	(32 << 10)
#define CONFIG_SYS_HZ_CLOCK		25000000

#define CONFIG_SYS_SDRAM_BASE		0x80000000     /* Cached addr */
#define CONFIG_SYS_SDRAM_SIZE		(256 << 20)

#define CONFIG_SYS_INIT_SP_ADDR		0x80100000
#define CONFIG_SYS_INIT_SP_ADDR_SRAM	0x19010000
#define CONFIG_SYS_LOAD_ADDR		0x80400000     /* default load address */

#define CONFIG_SYS_MEMTEST_START	0x80100000
#define CONFIG_SYS_MEMTEST_END		0x80130000

/*-----------------------------------------------------------------------
 * Machine type: machine id for RLXARM is 100
 */
#define CONFIG_MACH_TYPE		100

/*-----------------------------------------------------------------------
 * FLASH and environment organization
 */
#define CONFIG_SYS_MAX_FLASH_BANKS	1		/* max number of memory banks */
#define CONFIG_SYS_MAX_FLASH_SECT	(128)		/* max number of sectors on one chip */

/* The following #defines are needed to get flash environment right */
#define CONFIG_SYS_MONITOR_BASE		CONFIG_SYS_TEXT_BASE
#define CONFIG_SYS_MONITOR_LEN		(128 << 10)

#define CONFIG_SYS_INIT_SP_OFFSET	0x2000000

#ifndef CONFIG_BOOTDELAY
#define CONFIG_BOOTDELAY	0	/* autoboot after 0 seconds	*/
#endif
/* We boot from this flash, selected with dip switch */
/* ATF_BOOT NOR_BASE 0x04000000 NAND_BASE 0x02000000 */
/* UBOOT_AS_BL33 OFFSET 0x128, UBOOT_AS_BL2 OFFSET 0X88 */
#ifdef CONFIG_FIT
#ifdef CONFIG_RTS_NAND_BOOT
#define NAND_BOOT_FIP
#else
#ifdef CONFIG_RTS_NOR_BOOT
#define NOR_BOOT_FIP
#endif
#endif
#endif

#ifdef NAND_BOOT_FIP
#ifdef UBOOT_AS_BL33
#define CONFIG_SYS_FLASH_BASE		0x02000128
#else
#define CONFIG_SYS_FLASH_BASE		0x02000088
#endif
#endif

#ifdef NOR_BOOT_FIP
#ifdef UBOOT_AS_BL33
#define CONFIG_SYS_FLASH_BASE		0x04000128
#else
#define CONFIG_SYS_FLASH_BASE		0x04000088
#endif
#endif

#ifndef NAND_BOOT_FIP
#ifndef NOR_BOOT_FIP
#define CONFIG_SYS_FLASH_BASE		0
#endif
#endif

/* timeout values are in ticks */
#define CONFIG_SYS_FLASH_ERASE_TOUT	(2 * CONFIG_SYS_HZ) /* Timeout for Flash Erase */
#define CONFIG_SYS_FLASH_WRITE_TOUT	(2 * CONFIG_SYS_HZ) /* Timeout for Flash Write */

#ifdef CONFIG_ENV_IN_RAM
#define CONFIG_ENV_IS_NOWHERE		1
#endif

/* Address and size of Primary Environment Sector	*/
#define CONFIG_NET_MULTI
#define CONFIG_MEMSIZE_IN_BYTES

#define CONFIG_SYS_LONGHELP		1

/*nand flash config*/
#define CONFIG_SYS_NAND_BASE            0x0
#define CONFIG_SYS_MAX_NAND_DEVICE      1

/*-----------------------------------------------------------------------
 * uboot Configuration
 */
#ifdef CONFIG_SYS_TEXT_BASE
#undef CONFIG_SYS_TEXT_BASE
#endif
#ifdef CONFIG_SELF_EXTRACTING
#define CONFIG_LZMA
#define CONFIG_SYS_TEXT_BASE	0x80800000
#else
#define CONFIG_SYS_TEXT_BASE	0x82800000
#endif

/* Support bootm-ing different OSes */
#define CONFIG_BOOTM_LINUX	1

#define CONFIG_GZIP		1
#define CONFIG_ZLIB		1
#define CONFIG_PARTITIONS	1

#define SPI_UBOOT_OFFSET	0
#define SPI_KERNEL_OFFSET	0xa0000
#define SPI_ROOTFS_OFFSET	0x520000

#if defined CONFIG_RTS3917_BGA240_DDR3_GENERAL || \
	defined CONFIG_RTS3917_QFN88_DDR3_1GBIT
#define DDR3_1GBIT_DEMOBOARD
#endif

#if defined CONFIG_RTS3917_BGA173_DDR2_512MBIT || \
	defined CONFIG_RTS3917_QFN88_DDR2_512MBIT
#define DDR2_MCM
#endif
#ifdef CONFIG_RTS_EMMC_BOOT
#define CONFIG_SYS_MMC_ENV_DEV 0
#define CONFIG_SUPPORT_EMMC_BOOT
#define CONFIG_SYS_MMC_ENV_PART	1
#endif

#define SYS_DMY_0 0x1886f00c
#define NOR_FLASH_CTRL	(1 << 4)

#define RST_MODE_ADDR	0x18860028
#define RST_MASK	0xF
#define RES_MODE	0x3
#define SECURE_MODE	0x2

#define RESCUE_UBOOT1_2_LOAD_ADDR	0x19000000
#define RESCUE_UBOOT1_2_LOAD_SIZE	0x4000
#define RESCUE_UBOOT3_LOAD_ADDR		0x81000000
#define RESCUE_UBOOT3_LOAD_SIZE		0x5c000
#define RESCUE_LINUX_LOAD_ADDR		(0x80ffc000 - 0x20)
#ifdef CONFIG_RTS_NAND_BOOT
#define RESCUE_UBOOT_LOAD			0x80ffc000
#define RESCUE_BIN_LENGTH			0x80000
#else
#define RESCUE_BIN_LENGTH			0x60000
#endif
#define RESCUE_FLASH_OFFSET			0x0
#define RESCUE_UBOOT3_FLASH_OFFSET	RESCUE_UBOOT1_2_LOAD_SIZE
#ifdef CONFIG_RTS_EMMC_BOOT
#define RESCUE_EMMC_DDR_MEM		0x82000000
#endif

#endif /* _SHEIPA_H_ */
