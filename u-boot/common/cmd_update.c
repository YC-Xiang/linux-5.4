// SPDX-License-Identifier: GPL-2.0
#include <common.h>
#include <malloc.h>
#include <asm/io.h>
#include <net.h>
#include <spi_flash.h>
#include <configs/rlxboard.h>
#include <part.h>
#include <fat.h>
#include <fs.h>
#include <linux/libfdt.h>
#include <linux/ctype.h>
#include <dfu.h>

#include <cpu_func.h>
#include <mapmem.h>
#include <dma.h>
#include <mmc.h>

#ifndef _MEM_TEST3_
static char *tmpfile;
#define GETMEM(addr)	(*(volatile u32 *)(addr))
struct fdt_header *kernel_fdt;
u32 dtb_len = CONFIG_FDT_FLASH_SIZE;

void set_kernel_fdt_addr(void)
{
	void *buf;
	ulong fdt_addr = env_get_hex("dtb_addr", CONFIG_FDT_ADDR);
#if defined(CONFIG_FIT) || defined(CONFIG_CRYPTO_BOOT)
	ulong length = env_get_hex("dtb_len", 0x100);
#endif

	buf = map_sysmem(fdt_addr, 0);
#ifdef CONFIG_FIT
	printf("## Checking for 'FDT'/'FDT Image' at %08lx\n",
		      buf);
	ulong load, len;
	if (fit_check_format(buf)) {
		void *fit = buf;
		int cfg_noffset, noffset;
		const char *prop_name = "fdt";
		const char *fit_base_uname_config;
		int ret;

		cfg_noffset = fit_conf_get_node(fit, NULL);
		if (cfg_noffset < 0) {
			puts("Could not find configuration node\n");
			return;
		}
		fit_base_uname_config = fdt_get_name(fit, cfg_noffset, NULL);
		printf("   Using '%s' configuration\n", fit_base_uname_config);
		if (IMAGE_ENABLE_VERIFY) {
			puts("   Verifying Hash Integrity ... ");
			if (fit_config_verify(fit, cfg_noffset)) {
				puts("Bad Data Hash\n");
				return;
			}
			puts("OK\n");
		}
		noffset = fit_conf_get_prop_node(fit, cfg_noffset,
						 prop_name);
		ret = fit_image_get_data_and_size(fit, noffset, &load, &len);
		printf("   Data Start:   ");
		if (ret) {
			printf("unavailable\n");
		} else {
			void *vdata = (void *)load;

			printf("0x%08lx\n", (ulong)map_to_sysmem(vdata));
			env_set_hex("dtb_addr", (ulong)map_to_sysmem(vdata));
		}
		puts("   Verifying Hash Integrity ... ");
		if (!fit_image_verify(fit, noffset)) {
			puts("Bad Data Hash\n");
			return;
		}
		puts("OK\n");
		fdt_addr = load;
	} else {
		debug("*  fdt: raw FDT blob\n");
		printf("## Flattened Device Tree blob at %08lx\n",
			(long)fdt_addr);
		return;
	}
	buf = map_sysmem(fdt_addr, 0);
	length = len;
#endif
#ifdef CONFIG_CRYPTO_BOOT
	int ret;

	ret = rlx_aes_ecb_decrypt((void *)buf, (void *)buf, length);
	mdelay(1000);
	if (ret)
		printf("## Decrypto dtb err\n");
#endif
	kernel_fdt = buf;
}

/**
 * fdt_valid() - Check if an FDT is valid. If not, change it to NULL
 *
 * @blobp: Pointer to FDT pointer
 * @return 1 if OK, 0 if bad (in which case *blobp is set to NULL)
 */
static int fdt_valid(struct fdt_header **blobp)
{
	const void *blob = *blobp;
	int err;

	if (blob == NULL) {
		printf("The address of the fdt is invalid (NULL).\n");
		return 0;
	}

	err = fdt_check_header(blob);
	if (err == 0)
		return 1;	/* valid */

	if (err < 0) {
		printf("libfdt fdt_check_header(): %s", fdt_strerror(err));
		/*
		 * Be more informative on bad version.
		 */
		if (err == -FDT_ERR_BADVERSION) {
			if (fdt_version(blob) <
			    FDT_FIRST_SUPPORTED_VERSION) {
				printf(" - too old, fdt %d < %d",
					fdt_version(blob),
					FDT_FIRST_SUPPORTED_VERSION);
			}
			if (fdt_last_comp_version(blob) >
			    FDT_LAST_SUPPORTED_VERSION) {
				printf(" - too new, fdt %d > %d",
					fdt_version(blob),
					FDT_LAST_SUPPORTED_VERSION);
			}
		}
		printf("\n");
		*blobp = NULL;
		return 0;
	}
	return 1;
}

void dma_copy_dtb_to_ddr(const char *label)
{
	ulong image_start;
	ulong load = env_get_hex("dtb_addr", CONFIG_FDT_ADDR); /// 0x8775af00 gd->new_fdt
	// ulong load_end = load + dtb_len;

	if (dtb_len % 8)
		dtb_len = (dtb_len / 8) * 8 + 8;

	image_start = env_get_hex(label, CONFIG_FDT_FLASH_ADDR); // label="dtb_offset"0xa0000需要update dtb之后才能正确获取. 否则得到CONFIG_FDT_FLASH_ADDR=0x80000不对。
#ifdef CONFIG_FIT
	image_start += 0x04000000;
#endif
	dma_copy(image_start, load, dtb_len);
	flush_cache(load, ALIGN(dtb_len, ARCH_DMA_MINALIGN));
}

#ifdef CONFIG_RTS_EMMC_BOOT
void mmc_read_dtb_to_ddr(void)
{
	struct mmc *mmc;
	int dev = mmc_get_env_dev();
	uint blk_start, blk_cnt, n;
	ulong load = env_get_hex("dtb_addr", CONFIG_FDT_ADDR);
	ulong offset;

	offset = env_get_hex("dtb_offset", CONFIG_FDT_FLASH_ADDR);

	mmc = find_mmc_device(dev);
	blk_select_hwpart_devnum(IF_TYPE_MMC, 0, 0);
	mmc_set_part_conf(mmc, 0, 0, 0);
	blk_start = ALIGN(offset, mmc->read_bl_len) / mmc->read_bl_len;
	blk_cnt = ALIGN(dtb_len, mmc->read_bl_len) / mmc->read_bl_len;
	printf("start is %x, cnt is %x\n", blk_start, blk_cnt);
	n = blk_dread(mmc_get_blk_desc(mmc), blk_start,
		       blk_cnt, (void *)load);
	if (n == blk_cnt)
		printf("mmc read OK\n");
	else
		printf("mmc read failed\n");
	blk_select_hwpart_devnum(IF_TYPE_MMC, dev, 1);
	mmc_set_part_conf(mmc, 0, 1, 0);
}
#endif

void nand_read_dtb_to_ddr(const char *label)
{
	int rval = 0;
	ulong dtb_start;
	ulong load = env_get_hex("dtb_addr", CONFIG_FDT_ADDR);
	dtb_start = env_get_hex("dtb_offset", CONFIG_FDT_FLASH_ADDR);
#ifdef CONFIG_FAST_BOOT
	struct fdt_header *fdt_address;

	copy_fdt_to_ram(dtb_start, load, 0x800);
	fdt_address = map_sysmem(load, 0);
	dtb_len = fdt_totalsize(fdt_address);
#endif
	if (dtb_len % 8)
		dtb_len = (dtb_len / 8) * 8 + 8;
#ifdef CONFIG_DUAL_KERNEL_LOAD_CHECK
	if (strcmp(label, "dtb_b_offset") == 0)
		dtb_start = env_get_hex(label, CONFIG_FDT_B_FLASH_ADDR);
	else
#endif
		dtb_start = env_get_hex(label, CONFIG_FDT_FLASH_ADDR);

	rval = copy_fdt_to_ram(dtb_start, load, dtb_len);

	if (rval)
		printf("read error!\n");
}

static u32 get_dtb_data_of_offset(const void *data, int len)
{
	if (len == 0)
		return -1;

	if ((len % 4) == 0) {
		const __be32 *p = data;

		return fdt32_to_cpu(p[0]);
	}
	return -1;
}

static u32 get_dtb_data_of_size(const void *data, int len)
{
	if (len == 0)
		return -1;

	if ((len % 4) == 0) {
		const __be32 *p = data;

		return fdt32_to_cpu(p[1]);
	}
	return -1;
}

int fdt_node_check_label(const void *fdt, int nodeoffset,
			      const char *label)
{
	const void *prop;
	int len;

	prop = fdt_getprop(fdt, nodeoffset, "label", &len);
	if (!prop)
		return len;

	return !fdt_stringlist_contains(prop, len, label);
}

int fdt_node_offset_by_label(const void *fdt, int startoffset,
				  const char *label)
{
	int offset, err;

	/* FIXME: The algorithm here is pretty horrible: we scan each
	 * property of a node in fdt_node_check_compatible(), then if
	 * that didn't find what we want, we scan over them again
	 * making our way to the next node.  Still it's the easiest to
	 * implement approach; performance can come later.
	 */
	for (offset = fdt_next_node(fdt, startoffset, NULL);
	     offset >= 0;
	     offset = fdt_next_node(fdt, offset, NULL)) {
		err = fdt_node_check_label(fdt, offset, label);
		if ((err < 0) && (err != -FDT_ERR_NOTFOUND))
			return err;
		else if (err == 0)
			return offset;
	}

	return offset; /* error from fdt_next_node() */
}

/*
 * The kernel_fdt points to kernel device tree.
 */

int _do_get_offset(const char *label)
{
	int nodeoffset;	/* node offset from libfdt */
	int nodeoffset_s;
#ifdef CONFIG_RTS_EMMC_BOOT
	char *pathp = "/soc/mmc_sdhc/partitions";
#else
	char *pathp = "/soc/spic/partitions";
#endif
	char *prop = "reg";
	int offset;
	const void *nodep;
	int len;

	if (!kernel_fdt || !fdt_valid(&kernel_fdt)) { /// 第一次调用_do_get_offset会拷贝dtb，board_r.c中initr_get_fdt_offset
#ifdef CONFIG_RTS_SPI_NAND_FLASH
		nand_read_dtb_to_ddr("dtb_offset"); /// nand flash 不能dma copy
#elif CONFIG_RTS_EMMC_BOOT
		mmc_read_dtb_to_ddr();
#else
		dma_copy_dtb_to_ddr("dtb_offset"); /// nor flash dma copy dtb(0xa0000) to gd->new_fdt
#endif
		set_kernel_fdt_addr(); /// 设置kernel_fdt全局指针指向dtb_addr即gd->new_fdt地址
	}
	if (!fdt_valid(&kernel_fdt))
		return 0;

	nodeoffset = fdt_path_offset(kernel_fdt, pathp);
	if (nodeoffset < 0) {
		printf("libfdt can't find kernel label returned\n");
		return 0;
	}
	nodeoffset_s = fdt_node_offset_by_label(kernel_fdt, nodeoffset, label);
	if (!(nodeoffset_s > 0))
		return 0;
	nodep = fdt_getprop(kernel_fdt, nodeoffset_s, prop, &len);
	if (nodep && len > 0) {
		offset = get_dtb_data_of_offset(nodep, len);
		if (offset < 0)
			return 0;
		return offset;
	}
	printf("libfdt fdt_getprop():\n");
	return 0;
}
#ifdef CONFIG_CHECK_DUALKERNEL_LOAD
int _do_get_offset_b(const char *label)
{
	int nodeoffset;	/* node offset from libfdt */
	int nodeoffset_s;
	char *pathp = "/soc/spic/partitions";
	char *prop = "reg";
	int offset;
	const void *nodep;
	int len;

#ifdef CONFIG_RTS_SPI_NAND_FLASH
		nand_read_dtb_to_ddr("dtb_b_offset");
#else
		dma_copy_dtb_to_ddr("dtb_b_offset");
#endif
		set_kernel_fdt_addr();
	if (!fdt_valid(&kernel_fdt))
		return 0;

	nodeoffset = fdt_path_offset(kernel_fdt, pathp);
	if (nodeoffset < 0) {
		printf("libfdt can't find kernel label returned\n");
		return 0;
	}
	nodeoffset_s = fdt_node_offset_by_label(kernel_fdt, nodeoffset, label);
	if (!(nodeoffset_s > 0))
		return 0;
	nodep = fdt_getprop(kernel_fdt, nodeoffset_s, prop, &len);
	if (nodep && len > 0) {
		offset = get_dtb_data_of_offset(nodep, len);
		if (offset < 0)
			return 0;
		return offset;
	}
	printf("libfdt fdt_getprop():\n");
	return 0;
}
#endif
/*
 * The kernel_fdt points to kernel device tree.
 */

int _do_get_size(const char *label)
{
	int nodeoffset;	/* node offset from libfdt */
	int nodeoffset_s;
#ifdef CONFIG_RTS_EMMC_BOOT
	char *pathp = "/soc/mmc_sdhc/partitions";
#else
	char *pathp = "/soc/spic/partitions";
#endif
	char *prop = "reg";
	int size;
	const void *nodep;
	int len;

	if (!kernel_fdt || !fdt_valid(&kernel_fdt)) {
#ifdef CONFIG_RTS_SPI_NAND_FLASH
		nand_read_dtb_to_ddr("dtb_offset");
#elif CONFIG_RTS_EMMC_BOOT
		mmc_read_dtb_to_ddr();
#else
		dma_copy_dtb_to_ddr("dtb_offset");
#endif
		set_kernel_fdt_addr();
	}
	if (!fdt_valid(&kernel_fdt))
		return 0;

	nodeoffset = fdt_path_offset(kernel_fdt, pathp);
	if (nodeoffset < 0) {
		printf("libfdt can't find kernel label returned\n");
		return 0;
	}
	nodeoffset_s = fdt_node_offset_by_label(kernel_fdt, nodeoffset, label);
	if (!(nodeoffset_s > 0))
		return 0;
	nodep = fdt_getprop(kernel_fdt, nodeoffset_s, prop, &len);
	if (nodep && len > 0) {
		size = get_dtb_data_of_size(nodep, len);
		if (size < 0)
			return 0;
		return size;
	}
	printf("libfdt fdt_getprop():\n");
	return 0;
}

#ifndef CONFIG_FAST_BOOT
static u32 _do_get_file_(char *filename)
{
	u32 len;

	tmpfile = filename;

	if  (net_loop(TFTPSRV) < 0)
		return 0;

	len = env_get_hex("filesize", 0);

	env_set("filesize", NULL);
	return len;
}
#endif

static int _do_write_file_(u32 offset, u32 len, u32 loadaddr,
				u32 partition_size, u32 magic_num)
{
	int ret = 0;
#ifdef CONFIG_RTS_EMMC_BOOT
	struct mmc *mmc;
	int dev = mmc_get_env_dev();
	uint blk_start, blk_cnt, n;
#else
	char *buf;

	buf = (char *)loadaddr;
#endif
	if (len == 0)
		return CMD_RET_FAILURE;

	if (partition_size == 0)
		partition_size = len;

#if defined CONFIG_RTS_QSPI
	ret = spi_flash_update_external(offset, len, buf);
#endif

#ifdef CONFIG_RTS_SPI_NAND_FLASH
	ret = update_image_for_nand(offset, len, (unsigned char *)buf,
			partition_size);
#endif

#ifdef CONFIG_RTS_EMMC_BOOT
	mmc = find_mmc_device(dev);
	if ((strcmp(tmpfile, "/u-boot.bin") == 0) || magic_num == 0x626f6f74) {
		blk_select_hwpart_devnum(IF_TYPE_MMC, dev, 1);
		mmc_set_part_conf(mmc, 0, 1, 1);
	} else {
		blk_select_hwpart_devnum(IF_TYPE_MMC, 0, 0);
		mmc_set_part_conf(mmc, 0, 0, 0);
	}

	if (mmc_getwp(mmc) == 1) {
		printf("Error: card is write protected!\n");
		return -1;
	}
	blk_start = ALIGN(offset, mmc->read_bl_len) / mmc->read_bl_len;
	blk_cnt = ALIGN(len, mmc->read_bl_len) / mmc->read_bl_len;
	printf("start is %x, cnt is %x\n", blk_start, blk_cnt);
	blk_derase(mmc_get_blk_desc(mmc), blk_start, blk_cnt);
	n = blk_dwrite(mmc_get_blk_desc(mmc), blk_start, blk_cnt,
		(void *)loadaddr);

	if (n == blk_cnt)
		ret = 0;
	else
		ret = -1;

	blk_select_hwpart_devnum(IF_TYPE_MMC, dev, 1);
	mmc_set_part_conf(mmc, 0, 1, 1);
#endif

	env_set("fileaddr", NULL);
	return ret;
}

#ifndef CONFIG_FAST_BOOT
static int do_update_uboot(const char *dfu)
{
	int ret = 0;
	u32 len = 0;
	u32 offset = 0;
	u32 loadaddr = CONFIG_SYS_LOAD_ADDR;

#ifdef CONFIG_CMD_DFU
	if (strcmp(dfu, "dfu") == 0) {
		char *argv[4] = {"dfu", "0", "ram", "0"};
		char **p = argv;

		ret = do_dfu(NULL, 0, 4, p);
		if (ret)
			return CMD_RET_FAILURE;
		len = dfu_all_len;
		dfu_all_len = 0;
	} else
#endif
		len = _do_get_file_("/u-boot.bin");

	ret = _do_write_file_(offset, len, loadaddr, 0, 0);

	debug("get %s from %x , write %x byte to offset %x\n",
		tmpfile, loadaddr, len, offset);
	return ret;
}

static int do_update_kernel(const char *dfu)
{
	int ret = 0;
	u32 offset;
	u32 size;
	u32 len = 0;
	u32 loadaddr = CONFIG_SYS_LOAD_ADDR;
	char *label = "kernel";

	offset = _do_get_offset(label);
	size = _do_get_size(label);
	if (!offset || !size)
		return 1;
	printf("update kernel to offset %x, partition size: %x\n",
		offset, size);

#ifdef CONFIG_CMD_DFU
	if (strcmp(dfu, "dfu") == 0) {
		char *argv[4] = {"dfu", "0", "ram", "0"};
		char **p = argv;

		ret = do_dfu(NULL, 0, 4, p);
		if (ret)
			return CMD_RET_FAILURE;
		len = dfu_all_len;
		dfu_all_len = 0;
	} else
#endif
		len = _do_get_file_("/vmlinux.img");

#ifdef CONFIG_CRYPTO_UPDATE
	len = patchkernelimg(loadaddr, len);
#endif
	ret = _do_write_file_(offset, len, loadaddr, size, 0);

	env_set_hex("kernel_offset", offset);

	debug("get %s from %x , write %x byte to offset %x\n",
		tmpfile, loadaddr, len, offset);
	return ret;
}

#ifdef SPI_ROOTFS_OFFSET
static int do_update_rootfs(void)
{
	int ret = 0;
	u32 offset;
	u32 size;
	u32 len = 0;
	u32 loadaddr = CONFIG_SYS_LOAD_ADDR;
	char *label = "rootfs";

	offset = _do_get_offset(label);
	size = _do_get_size(label);
	if (!offset || !size)
		return 1;
	printf("update rootfs to offset %x, partition size: %x\n",
		offset, size);
	len = _do_get_file_("/rootfs.bin");
	ret = _do_write_file_(offset, len, loadaddr, size, 0);

	debug("get %s from %x , write %x byte to offset %x\n",
		tmpfile, loadaddr, len, offset);
	return ret;
}
#endif

static int do_update_dtb(const char *dfu)
{
	int ret = 0;
	u32 offset, dtb_offset;
	u32 loadaddr = CONFIG_SYS_LOAD_ADDR;
	u32 dtb_addr = env_get_hex("dtb_addr", CONFIG_FDT_ADDR);
	void *load_buf, *image_buf;
	char *label = "dtb";

#ifdef CONFIG_CMD_DFU
	if (strcmp(dfu, "dfu") == 0) {
		char *argv[4] = {"dfu", "0", "ram", "0"};
		char **p = argv;

		ret = do_dfu(NULL, 0, 4, p);
		if (ret)
			return CMD_RET_FAILURE;
		dtb_len = dfu_all_len;
		dfu_all_len = 0;
	} else
#endif
		dtb_len = _do_get_file_("/vmlinux.dtb");

	load_buf = map_sysmem(dtb_addr, dtb_len);
	image_buf = map_sysmem(loadaddr, dtb_len);
	memmove_wd(load_buf, image_buf, dtb_len, CHUNKSZ);

	set_kernel_fdt_addr();
	dtb_offset = env_get_hex("dtb_offset", CONFIG_FDT_FLASH_ADDR);
	offset = _do_get_offset(label);
	if (offset && (offset != dtb_offset)) {
		dtb_offset = offset;
		env_set_hex("dtb_offset", offset);
	}
	env_set_hex("dtb_len", dtb_len);
	env_save();
	ret = _do_write_file_(dtb_offset, dtb_len, loadaddr,
			CONFIG_FDT_FLASH_SIZE, 0);
	debug("get %s from %x , write %x byte to offset %x\n",
		tmpfile, loadaddr, dtb_len, dtb_offset);
	return ret;
}
#endif

#ifdef CONFIG_DUAL_KERNEL_LOAD_CHECK
static int do_update_dtb_b(const char *dfu)
{
	int ret = 0;
	u32 offset, dtb_b_offset;
	u32 loadaddr = CONFIG_SYS_LOAD_ADDR;
	u32 dtb_addr = env_get_hex("dtb_addr", CONFIG_FDT_ADDR);
	void *load_buf, *image_buf;
	char *label = "dtb_b";

#ifdef CONFIG_CMD_DFU
	if (strcmp(dfu, "dfu") == 0) {
		char *argv[4] = {"dfu", "0", "ram", "0"};
		char **p = argv;

		ret = do_dfu(NULL, 0, 4, p);
		if (ret)
			return CMD_RET_FAILURE;
		dtb_len = dfu_all_len;
		dfu_all_len = 0;
	} else
#endif
		dtb_len = _do_get_file_("/vmlinux.dtb");

	load_buf = map_sysmem(dtb_addr, dtb_len);
	image_buf = map_sysmem(loadaddr, dtb_len);
	memmove_wd(load_buf, image_buf, dtb_len, CHUNKSZ);

	set_kernel_fdt_addr();
	dtb_b_offset = env_get_hex("dtb_b_offset", CONFIG_FDT_B_FLASH_ADDR);
	offset = _do_get_offset(label);
	if (offset && (offset != dtb_b_offset)) {
		dtb_b_offset = offset;
		env_set_hex("dtb_b_offset", offset);
	}
	env_set_hex("dtb_b_len", dtb_len);
	env_save();
	ret = _do_write_file_(dtb_b_offset, dtb_len, loadaddr,
			CONFIG_FDT_FLASH_SIZE, 0);
	debug("get %s from %x , write %x byte to offset %x\n",
		tmpfile, loadaddr, dtb_len, dtb_b_offset);
	return ret;
}
#endif

static int _do_write_all_(u32 all_len)
{
	int ret = 0;

	u32 magic_num;
	u32 ram_offset = 0x100;
	u32 flash_offset = 0;
	u32 filelen;
	u32 dtb_offset;
	u32 partition_size = 0;
#ifdef CONFIG_CRYPTO_UPDATE
	u32 klen;
	u32 addr;
	u32 all_len_temp;

	all_len_temp = all_len;
#endif

	all_len -= 0x100;
	while (all_len != 0) {
		magic_num = htonl(GETMEM(CONFIG_SYS_LOAD_ADDR + ram_offset));
		debug("%x:%x\n", (CONFIG_SYS_LOAD_ADDR + ram_offset), magic_num);
		partition_size = htonl(GETMEM(CONFIG_SYS_LOAD_ADDR +
				ram_offset + 0x0C));
		flash_offset = htonl(GETMEM(CONFIG_SYS_LOAD_ADDR + ram_offset + 0x14));
		filelen = htonl(GETMEM(CONFIG_SYS_LOAD_ADDR + ram_offset + 0x1C));

		switch (magic_num) {
		/*dtb*/
		case 0x64746200:
			printf("offset is %x\n", flash_offset);
			dtb_offset = env_get_hex("dtb_offset",
						CONFIG_FDT_FLASH_ADDR);
			if (dtb_offset != flash_offset) {
				env_set_hex("dtb_offset", flash_offset);
				env_save();
			}
			memcpy((void *)env_get_hex("dtb_addr", CONFIG_FDT_ADDR),
			(void *)(CONFIG_SYS_LOAD_ADDR + ram_offset + 0x20),
			filelen);
			env_set_hex("dtb_len", filelen);
			env_save();
			break;

		/*uboot*/
		case 0x626f6f74:

		/*MCU fw*/
		case 0x6669726d:

		/*hw config*/
		case 0x68617264:

		/*sw config*/
		case 0x6a667332:

		/*kernel*/
		case 0x6c696e78:

		/*rootfs*/
		case 0x726f6f74:

		/*ldc table*/
		case 0x6c646300:

		/*gpt head*/
		case 0x67707468:

		/*gpt tail*/
		case 0x67707474:
			printf("offset is %x\n", flash_offset);
			break;

		/*file end*/
		case 0x46454f46:
			printf("file end\n");
			return 0;

		default:
			printf("user defined magic number %x at %x\n",
			magic_num, (CONFIG_SYS_LOAD_ADDR + ram_offset));
			break;
		}
#ifdef CONFIG_CRYPTO_UPDATE
		if (flash_offset == SPI_KERNEL_OFFSET) {
			addr = CONFIG_SYS_LOAD_ADDR + all_len_temp;
			addr = (addr + 0x3) & ~0x3;
			memcpy((void *)addr,
			(void *)(CONFIG_SYS_LOAD_ADDR + ram_offset + 0x20), filelen);
			klen = patchkernelimg(addr, filelen);
			ret = _do_write_file_(flash_offset, klen, addr,
						partition_size, magic_num);
		} else
#endif
			ret = _do_write_file_(flash_offset, filelen,
				(CONFIG_SYS_LOAD_ADDR + ram_offset + 0x20),
					partition_size, magic_num);

		if (ret != 0)
			return ret;
		printf("get %x file from %x, wtite %x bytes to flash offset %x\n",
			magic_num, ram_offset, filelen, flash_offset);
		all_len -= (filelen + 0x24);
		ram_offset += (filelen + 0x24);
	}
	debug("update all image from ethernet and write to flash\n");
	return ret;
}

#ifndef CONFIG_FAST_BOOT
static int do_update_all(const char *dfu)
{
	int ret = 0;
	u32 all_len = 0;

#ifdef CONFIG_CMD_DFU
	if (strcmp(dfu, "dfu") == 0) {
		char *argv[4] = {"dfu", "0", "ram", "0"};
		char **p = argv;

		ret = do_dfu(NULL, 0, 4, p);
		if (ret)
			return CMD_RET_FAILURE;
		all_len = dfu_all_len;
		dfu_all_len = 0;
	} else
#endif
		all_len = _do_get_file_("/linux.bin");

	if (all_len == 0)
		return CMD_RET_FAILURE;

	ret = _do_write_all_(all_len);

	return ret;
}

static int do_update(cmd_tbl_t *cmdtp, int flag, int argc,
			char * const argv[])
{

	const char *cmd;
	char *dfu_argv = NULL;
	int ret = CMD_RET_FAILURE;

	/* need at least two arguments */
	if (argc < 2)
		goto usage;

	cmd = argv[1];

	if (argc == 3)
		dfu_argv = argv[2];

	if (strcmp(cmd, "uboot") == 0) {
		ret = do_update_uboot(dfu_argv);
		goto done;
	}

	if (strcmp(cmd, "kernel") == 0) {
		ret = do_update_kernel(dfu_argv);
		goto done;
	}

#ifdef SPI_ROOTFS_OFFSET
	if (strcmp(cmd, "rootfs") == 0) {
		ret = do_update_rootfs();
		goto done;
	}
#endif

	if (strcmp(cmd, "all") == 0) {
		ret = do_update_all(dfu_argv);
		goto done;
	}

	if (strcmp(cmd, "dtb") == 0) {
		ret = do_update_dtb(dfu_argv);
		goto done;
	}
#ifdef CONFIG_DUAL_KERNEL_LOAD_CHECK
	if (strcmp(cmd, "dtb_b") == 0) {
		ret = do_update_dtb_b(dfu_argv);
		goto done;
	}
#endif

done:
	if (ret == CMD_RET_SUCCESS)
		printf("update done\n\n");
	else
		printf("update failed\n\n");

	return ret;

usage:
	return CMD_RET_USAGE;
}
#endif

int do_write_for_rescure(void)
{
	int ret = 0;

	u32 magic_num;
	u32 ram_offset = 0x0;
	u32 flash_offset = 0;
	u32 filelen;
	u32 dtb_offset;
	u32 partition_size = 0;

	while (1) {
		magic_num = htonl(REG32(RESCUE_LINUX_LOAD_ADDR + ram_offset));
		debug("%x:%x\n", (RESCUE_LINUX_LOAD_ADDR + ram_offset),
			magic_num);
		partition_size = htonl(REG32(RESCUE_LINUX_LOAD_ADDR +
				ram_offset + 0x0C));
		flash_offset = htonl(REG32(RESCUE_LINUX_LOAD_ADDR +
			ram_offset + 0x14));
		filelen = htonl(REG32(RESCUE_LINUX_LOAD_ADDR +
			ram_offset + 0x1C));

		switch (magic_num) {
		/*dtb*/
		case 0x64746200:
			printf("offset is %x\n", flash_offset);
			dtb_offset = env_get_hex("dtb_offset",
						CONFIG_FDT_FLASH_ADDR);
			if (dtb_offset != flash_offset) {
				env_set_hex("dtb_offset", flash_offset);
				env_save();
			}
			memcpy((void *)env_get_hex("dtb_addr", CONFIG_FDT_ADDR),
			(void *)(RESCUE_LINUX_LOAD_ADDR + ram_offset + 0x20),
			filelen);
			break;

		/*uboot*/
		case 0x626f6f74:

		/*MCU fw*/
		case 0x6669726d:

		/*hw config*/
		case 0x68617264:

		/*sw config*/
		case 0x6a667332:

		/*kernel*/
		case 0x6c696e78:

		/*rootfs*/
		case 0x726f6f74:

		/*ldc table*/
		case 0x6c646300:
			printf("offset is %x\n", flash_offset);
			break;

		/*file end*/
		case 0x46454f46:
			printf("file end\n");
			return 0;

		default:
			printf("user defined magic number %x at %x\n",
			magic_num, (RESCUE_LINUX_LOAD_ADDR + ram_offset));
			break;
		}

		ret = _do_write_file_(flash_offset, filelen,
			(RESCUE_LINUX_LOAD_ADDR + ram_offset + 0x20),
				partition_size, magic_num);
		if (ret != 0)
			return ret;
		printf("get %x file from %x, wtite %x bytes to flash %x\n",
			magic_num, ram_offset, filelen, flash_offset);
		ram_offset += (filelen + 0x24);
	}

	return ret;
}

int load_bin_from_sd(void)
{
	unsigned long bytes;
	unsigned long pos;
	int len_read;
	unsigned long time;
	int ret;
	int *temp = &len_read;
	const char *filename  = "linux.bin";
	unsigned long loadaddr = CONFIG_SYS_LOAD_ADDR;

	if (fs_set_blk_dev("mmc", "0", FS_TYPE_FAT))
		if (fs_set_blk_dev("mmc", "1", FS_TYPE_FAT))
			return 0;

	bytes = 0;
	pos = 0;
	time = get_timer(0);
	ret = fs_read(filename, loadaddr, pos, bytes, (loff_t *)temp);
	printf("in load_bin_from_sd\n");
	time = get_timer(time);
	if (ret < 0)
		return 0;

	printf("%d bytes read in %lu ms", len_read, time);
	if (time > 0) {
		puts(" (");
		print_size(len_read / time * 1000, "/s");
		puts(")");
	}
	puts("\n");

	_do_write_all_(len_read);

	return 0;
}

int load_key_from_sd(void)
{
	unsigned long addr;
	const char *filename;
	unsigned long bytes;
	unsigned long pos;
	int len_read;
	unsigned long time;
	int ret;
	int *temp = &len_read;

	if (fs_set_blk_dev("mmc", "0", FS_TYPE_FAT))
		if (fs_set_blk_dev("mmc", "1", FS_TYPE_FAT))
			return 0;

	addr = CONFIG_SYS_LOAD_ADDR;
	printf("load key from sd addr %lx\n", addr);

	filename = "ipcam_crypto.bin";

	bytes = 0;
	pos = 0;
	time = get_timer(0);
	ret = fs_read(filename, addr, pos, bytes, (loff_t *)temp);
	printf("in load_key_from_sd\n");
	time = get_timer(time);
	if (ret < 0)
		return 0;

	printf("%d bytes read in %lu ms", len_read, time);
	if (time > 0) {
		puts(" (");
		print_size(len_read / time * 1000, "/s");
		puts(")");
	}
	puts("\n");

	return 0;
}

int load_iv_from_sd(void)
{
	unsigned long addr;
	const char *filename;
	unsigned long bytes;
	unsigned long pos;
	int len_read;
	unsigned long time;
	int ret;
	int *temp = &len_read;

	if (fs_set_blk_dev("mmc", "0", FS_TYPE_FAT))
		if (fs_set_blk_dev("mmc", "1", FS_TYPE_FAT))
			return 0;

	addr = CONFIG_SYS_LOAD_ADDR;
	printf("load iv from sd addr %lx\n", addr);

	filename = "ipcam_crypto_iv.bin";

	bytes = 0;
	pos = 0;
	time = get_timer(0);
	ret = fs_read(filename, addr, pos, bytes, (loff_t *)temp);
	printf("in load_iv_from_sd\n");
	time = get_timer(time);
	if (ret < 0)
		return 0;

	printf("%d bytes read in %lu ms", len_read, time);
	if (time > 0) {
		puts(" (");
		print_size(len_read / time * 1000, "/s");
		puts(")");
	}
	puts("\n");

	return 0;
}

#ifdef CONFIG_CMD_SF_TEST
#define UPDATE_TEST_HELP "\nsf test offset len		" \
		"- run a very basic destructive test"
#else
#define UPDATE_TEST_HELP
#endif

#ifndef CONFIG_FAST_BOOT
U_BOOT_CMD(
	update,	3,	0,	do_update,
	"update xxx",
	"uboot	- get uboot.bin by tftp and write to flash\n"
	"update kernel	- get kernel image by tftp and write to flash\n"
	"update dtb	- get dtb by tftp and write to flash\n"
#ifdef CONFIG_DUAL_KERNEL_LOAD_CHECK
	"update dtb_b	- get dtb by tftp and write to flash dtb_b offset\n"
#endif
	"update rootfs	- get rootfs by tftp and write to flash\n"
	"update all	- get all image by tftp , parse it and  write to flash\n"
	UPDATE_TEST_HELP
);
#endif
#endif
