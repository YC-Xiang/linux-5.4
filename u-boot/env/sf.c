// SPDX-License-Identifier: GPL-2.0+
/*
 * (C) Copyright 2000-2010
 * Wolfgang Denk, DENX Software Engineering, wd@denx.de.
 *
 * (C) Copyright 2001 Sysgo Real-Time Solutions, GmbH <www.elinos.com>
 * Andreas Heppel <aheppel@sysgo.de>
 *
 * (C) Copyright 2008 Atmel Corporation
 */
#include <common.h>
#include <dm.h>
#include <env.h>
#include <env_internal.h>
#include <malloc.h>
#include <spi.h>
#include <spi_flash.h>
#include <search.h>
#include <errno.h>
#include <dm/device-internal.h>
#include <u-boot/crc.h>

#ifndef CONFIG_SPL_BUILD
#define CMD_SAVEENV
#define INITENV
#endif

#ifdef CONFIG_ENV_OFFSET_REDUND
#ifdef CMD_SAVEENV
static ulong env_offset		= CONFIG_ENV_OFFSET;
static ulong env_new_offset	= CONFIG_ENV_OFFSET_REDUND;
#endif

#endif /* CONFIG_ENV_OFFSET_REDUND */

DECLARE_GLOBAL_DATA_PTR;
static void self_copy(void);

static int setup_flash_device(void)
{
#ifdef CONFIG_DM_SPI_FLASH
	struct udevice *new;
	int	ret;

	/* speed and mode will be read from DT */
	ret = spi_flash_probe_bus_cs(CONFIG_ENV_SPI_BUS, CONFIG_ENV_SPI_CS,
				     CONFIG_ENV_SPI_MAX_HZ, CONFIG_ENV_SPI_MODE,
				     &new);
	if (ret) {
		env_set_default("spi_flash_probe_bus_cs() failed", 0);
		return ret;
	}

	flash = dev_get_uclass_priv(new);
#else

	if (!flash) {
		flash = spi_flash_probe(CONFIG_ENV_SPI_BUS,
			CONFIG_ENV_SPI_CS,
			CONFIG_ENV_SPI_MAX_HZ, CONFIG_ENV_SPI_MODE);
		if (!flash) {
			env_set_default("spi_flash_probe() failed", 0);
			return -EIO;
		}
	}
#endif
	return 0;
}

#if defined(CONFIG_ENV_OFFSET_REDUND)
#ifdef CMD_SAVEENV
static int env_sf_save(void)
{
	env_t	env_new;
	char	*saved_buffer = NULL, flag = ENV_REDUND_OBSOLETE;
	u32	saved_size, saved_offset, sector;
	int	ret;

	ret = setup_flash_device();
	if (ret)
		return ret;

	ret = env_export(&env_new);
	if (ret)
		return -EIO;
	env_new.flags	= ENV_REDUND_ACTIVE;

	if (gd->env_valid == ENV_VALID) {
		env_new_offset = CONFIG_ENV_OFFSET_REDUND;
		env_offset = CONFIG_ENV_OFFSET;
	} else {
		env_new_offset = CONFIG_ENV_OFFSET;
		env_offset = CONFIG_ENV_OFFSET_REDUND;
	}

	/* Is the sector larger than the env (i.e. embedded) */
	if (CONFIG_ENV_SECT_SIZE > CONFIG_ENV_SIZE) {
		saved_size = CONFIG_ENV_SECT_SIZE - CONFIG_ENV_SIZE;
		saved_offset = env_new_offset + CONFIG_ENV_SIZE;
		saved_buffer = memalign(ARCH_DMA_MINALIGN, saved_size);
		if (!saved_buffer) {
			ret = -ENOMEM;
			goto done;
		}
		ret = spi_flash_read(flash, saved_offset,
					saved_size, saved_buffer);
		if (ret)
			goto done;
	}

	sector = DIV_ROUND_UP(CONFIG_ENV_SIZE, CONFIG_ENV_SECT_SIZE);

	puts("Erasing SPI flash...");
	flash->flash_unlock(flash, 0, 0);
	ret = spi_flash_erase(flash, env_new_offset,
				sector * CONFIG_ENV_SECT_SIZE);
	if (ret)
		goto done;

	puts("Writing to SPI flash...");

	ret = spi_flash_write(flash, env_new_offset,
		CONFIG_ENV_SIZE, &env_new);
	if (ret)
		goto done;

	if (CONFIG_ENV_SECT_SIZE > CONFIG_ENV_SIZE) {
		ret = spi_flash_write(flash, saved_offset,
					saved_size, saved_buffer);
		if (ret)
			goto done;
	}

	ret = spi_flash_write(flash, env_offset + offsetof(env_t, flags),
				sizeof(env_new.flags), &flag);
	if (ret)
		goto done;

	puts("done\n");

	gd->env_valid = gd->env_valid == ENV_REDUND ? ENV_VALID : ENV_REDUND;

	printf("Valid environment: %d\n", (int)gd->env_valid);

 done:
	flash->flash_lock(flash, 0, 0);
	if (saved_buffer)
		free(saved_buffer);

	return ret;
}
#endif /* CMD_SAVEENV */

static int env_sf_load(void)
{
	int ret;
	int read1_fail, read2_fail;
	env_t *tmp_env1, *tmp_env2;
	u8 board_reset_mode = get_rst_mode();

	if (board_reset_mode == RES_MODE) {
		//Open Nor flash control
		ret = REG32(SYS_DMY_0);
		REG32(SYS_DMY_0) = ret | NOR_FLASH_CTRL;
	}

	tmp_env1 = (env_t *)memalign(ARCH_DMA_MINALIGN,
			CONFIG_ENV_SIZE);
	tmp_env2 = (env_t *)memalign(ARCH_DMA_MINALIGN,
			CONFIG_ENV_SIZE);
	if (!tmp_env1 || !tmp_env2) {
		env_set_default("malloc() failed", 0);
		ret = -EIO;
		goto out;
	}

	ret = setup_flash_device();
	if (ret)
		goto out;

	read1_fail = spi_flash_read(flash, CONFIG_ENV_OFFSET,
				    CONFIG_ENV_SIZE, tmp_env1);
	read2_fail = spi_flash_read(flash, CONFIG_ENV_OFFSET_REDUND,
				    CONFIG_ENV_SIZE, tmp_env2);

	ret = env_import_redund((char *)tmp_env1, read1_fail, (char *)tmp_env2,
				read2_fail);
	if (ret)
		goto err_read;
	if (board_reset_mode == RES_MODE)
		self_copy();
	goto out;
err_read:
	spi_flash_free(flash);
	flash = NULL;
out:
	free(tmp_env1);
	free(tmp_env2);

	return ret;
}
#else
#ifdef CMD_SAVEENV
static int env_sf_save(void)
{
	u32	saved_size, saved_offset, sector;
	char	*saved_buffer = NULL;
	int	ret = 1;
	env_t	env_new;

	ret = setup_flash_device();
	if (ret)
		return ret;

	/* Is the sector larger than the env (i.e. embedded) */
	if (CONFIG_ENV_SECT_SIZE > CONFIG_ENV_SIZE) {
		saved_size = CONFIG_ENV_SECT_SIZE - CONFIG_ENV_SIZE;
		saved_offset = CONFIG_ENV_OFFSET + CONFIG_ENV_SIZE;
		saved_buffer = malloc(saved_size);
		if (!saved_buffer)
			goto done;

		ret = spi_flash_read(flash, saved_offset,
			saved_size, saved_buffer);
		if (ret)
			goto done;
	}

	ret = env_export(&env_new);
	if (ret)
		goto done;

	sector = DIV_ROUND_UP(CONFIG_ENV_SIZE, CONFIG_ENV_SECT_SIZE);

	puts("Erasing SPI flash...");
	flash->flash_unlock(flash, 0, 0);
	ret = spi_flash_erase(flash, CONFIG_ENV_OFFSET,
		sector * CONFIG_ENV_SECT_SIZE);
	if (ret)
		goto done;

	puts("Writing to SPI flash...");
	ret = spi_flash_write(flash, CONFIG_ENV_OFFSET,
		CONFIG_ENV_SIZE, &env_new);
	if (ret)
		goto done;

	if (CONFIG_ENV_SECT_SIZE > CONFIG_ENV_SIZE) {
		ret = spi_flash_write(flash, saved_offset,
			saved_size, saved_buffer);
		if (ret)
			goto done;
	}
	flash->flash_lock(flash, 0, 0);
	ret = 0;
	puts("done\n");

 done:
	flash->flash_lock(flash, 0, 0);
	if (saved_buffer)
		free(saved_buffer);

	return ret;
}
#endif /* CMD_SAVEENV */

static int env_sf_load(void)
{
	int ret;
	char *buf = NULL;

	u8 board_reset_mode = get_rst_mode();

	if (board_reset_mode == RES_MODE) {
		//Open Nor flash control
		ret = REG32(SYS_DMY_0);
		REG32(SYS_DMY_0) = ret | NOR_FLASH_CTRL;
	}
	buf = (char *)memalign(ARCH_DMA_MINALIGN, CONFIG_ENV_SIZE);
	if (!buf) {
		env_set_default("malloc() failed", 0);
		return -EIO;
	}

	ret = setup_flash_device(); /// 初始化nor flash
	if (ret)
		goto out;
	ret = spi_flash_read(flash,
		CONFIG_ENV_OFFSET, CONFIG_ENV_SIZE, buf); /// 从flash中读环境变量到buf
	if (ret) {
		env_set_default("spi_flash_read() failed", 0);
		goto err_read;
	}

	ret = env_import(buf, 1); /// 将环境变量存入一个hash table
	if (!ret)
		gd->env_valid = ENV_VALID;

	if (board_reset_mode == RES_MODE)
		self_copy();
	goto out;

err_read:
	spi_flash_free(flash);
	flash = NULL;
out:
	if (buf)
		free(buf);

	return ret;
}
#endif

void self_copy(void)
{
	u8 ret = 0;
	u32 magic_num;
	u32 magic_num2;

	magic_num = htonl(REG32(RESCUE_UBOOT1_2_LOAD_ADDR));
	if (magic_num != 0x626f6f74) {
		printf("user write uboot to flash\n");

		flash->flash_unlock(flash, 0, 0);
		ret = spi_flash_erase(flash, RESCUE_FLASH_OFFSET,
							RESCUE_BIN_LENGTH);
		printf("SF: %zu bytes @ %#x Erased: %s\n",
			(size_t)RESCUE_BIN_LENGTH,
			(u32)RESCUE_FLASH_OFFSET, ret ? "ERROR" : "OK");
		if (ret)
			return;

		ret = spi_flash_write(flash, RESCUE_FLASH_OFFSET,
			RESCUE_UBOOT1_2_LOAD_SIZE,
			(void *)RESCUE_UBOOT1_2_LOAD_ADDR);
		if (ret) {
			printf("SF: %zu bytes @ %#x %s: %s\n",
				(size_t)RESCUE_BIN_LENGTH,
				(u32)RESCUE_FLASH_OFFSET,
				"Written", "ERROR");
			return;
		}
		magic_num2 = REG32(RESCUE_UBOOT3_LOAD_ADDR);
		if (magic_num != 0x10064AA || magic_num2 == 0) {
			ret = spi_flash_write(flash, RESCUE_UBOOT3_FLASH_OFFSET,
				RESCUE_UBOOT3_LOAD_SIZE,
				(void *)RESCUE_UBOOT3_LOAD_ADDR);
		} else {
			magic_num2 = REG32(0x19000020);
			ret = spi_flash_write(flash, RESCUE_UBOOT3_FLASH_OFFSET,
				RESCUE_UBOOT3_LOAD_SIZE,
				(void *)(RESCUE_UBOOT3_LOAD_ADDR - magic_num2));
		}
		printf("SF: %zu bytes @ %#x %s: %s\n",
			(size_t)RESCUE_BIN_LENGTH,
			(u32)RESCUE_FLASH_OFFSET,
			"Written", ret ? "ERROR" : "OK");
		flash->flash_lock(flash, 0, 0);
	} else {
		printf("user write linux to flash\n");

		memcpy((void *)(RESCUE_LINUX_LOAD_ADDR),
			(void *)RESCUE_UBOOT1_2_LOAD_ADDR,
			RESCUE_UBOOT1_2_LOAD_SIZE);
		flush_cache(RESCUE_LINUX_LOAD_ADDR & ~(ARCH_DMA_MINALIGN - 1),
			RESCUE_UBOOT1_2_LOAD_SIZE);
		ret = do_write_for_rescure();
		printf("self_copy %s\n", ret ? "ERROR" : "OK");
	}
}

#if CONFIG_ENV_ADDR != 0x0
__weak void *env_sf_get_env_addr(void)
{
	return (void *)CONFIG_ENV_ADDR;
}
#endif

#if defined(INITENV) && (CONFIG_ENV_ADDR != 0x0)
static int env_sf_init(void)
{
	gd->env_addr = (ulong)&default_environment[0];
	gd->env_valid = 1;

	return 0;
}
#endif

U_BOOT_ENV_LOCATION(sf) = {
	.location	= ENVL_SPI_FLASH,
	ENV_NAME("SPI Flash")
	.load		= env_sf_load,
#ifdef CMD_SAVEENV
	.save		= env_save_ptr(env_sf_save),
#endif
#if defined(INITENV) && (CONFIG_ENV_ADDR != 0x0)
	.init		= env_sf_init,
#endif
};
