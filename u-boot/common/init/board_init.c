// SPDX-License-Identifier: GPL-2.0+
/*
 * Code shared between SPL and U-Boot proper
 *
 * Copyright (c) 2015 Google, Inc
 * Written by Simon Glass <sjg@chromium.org>
 */

#include <common.h>

DECLARE_GLOBAL_DATA_PTR;

/* Unfortunately x86 or ARM can't compile this code as gd cannot be assigned */
#if !defined(CONFIG_X86) && !defined(CONFIG_ARM)
__weak void arch_setup_gd(struct global_data *gd_ptr)
{
	gd = gd_ptr;
}
#endif /* !CONFIG_X86 && !CONFIG_ARM */

/**
 * This function is called from board_init_f_init_reserve to set up
 * gd->start_addr_sp for stack protection if not already set otherwise
 */
__weak void board_init_f_init_stack_protection_addr(ulong base)
{
#if CONFIG_IS_ENABLED(SYS_REPORT_STACK_F_USAGE)
	/* set up stack pointer for stack usage if not set yet */
	if (!gd->start_addr_sp)
		gd->start_addr_sp = base;
#endif
}

/**
 * This function is called after the position of the initial stack is
 * determined in gd->start_addr_sp. Boards can override it to set up
 * stack-checking markers.
 */
__weak void board_init_f_init_stack_protection(void)
{
#if CONFIG_IS_ENABLED(SYS_REPORT_STACK_F_USAGE)
	ulong stack_bottom = gd->start_addr_sp -
		CONFIG_VAL(SIZE_LIMIT_PROVIDE_STACK);

	/* substact some safety margin (0x20) since stack is in use here */
	memset((void *)stack_bottom, CONFIG_VAL(SYS_STACK_F_CHECK_BYTE),
	       CONFIG_VAL(SIZE_LIMIT_PROVIDE_STACK) - 0x20);
#endif
}


ulong board_init_f_alloc_reserve(ulong top)
{
	/* Reserve early malloc arena */
#if CONFIG_VAL(SYS_MALLOC_F_LEN) /// 没定义，不分配early malloc内存池
	top -= CONFIG_VAL(SYS_MALLOC_F_LEN);
#endif
	/* LAST : reserve GD (rounded up to a multiple of 16 bytes) */
	top = rounddown(top-sizeof(struct global_data), 16); /// top=0x80100000 减去global_data的大小预留空间。

	return top;
}

void board_init_f_init_reserve(ulong base)
{
	struct global_data *gd_ptr;

	/*
	 * clear GD entirely and set it up.
	 * Use gd_ptr, as gd may not be properly set yet.
	 */

	gd_ptr = (struct global_data *)base;
	/* zero the area */
	memset(gd_ptr, '\0', sizeof(*gd));
	/* set GD unless architecture did it already */
#if !defined(CONFIG_ARM)
	arch_setup_gd(gd_ptr);
#endif

	if (CONFIG_IS_ENABLED(SYS_REPORT_STACK_F_USAGE)) /// skip
		board_init_f_init_stack_protection_addr(base);

	/* next alloc will be higher by one GD plus 16-byte alignment */
	base += roundup(sizeof(struct global_data), 16); /// base又变回0x80100000

	/*
	 * record early malloc arena start.
	 * Use gd as it is now properly set for all architectures.
	 */

#if CONFIG_VAL(SYS_MALLOC_F_LEN) /// skip
	/* go down one 'early malloc arena' */
	gd->malloc_base = base;
#endif

	if (CONFIG_IS_ENABLED(SYS_REPORT_STACK_F_USAGE)) /// skip
		board_init_f_init_stack_protection();
}

/*
 * Board-specific Platform code can reimplement show_boot_progress () if needed
 */
__weak void show_boot_progress(int val) {}
