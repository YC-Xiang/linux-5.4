/*
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2019 Western Digital Corporation or its affiliates.
 *
 * Authors:
 *   Anup Patel <anup.patel@wdc.com>
 */

#include <sbi/riscv_asm.h>
#include <sbi/riscv_encoding.h>
#include <sbi/sbi_platform.h>
#include <sbi/sbi_scratch.h>
#include <sbi/sbi_trap.h>

#define BOOT_STATUS_RELOCATE_DONE	1
#define BOOT_STATUS_BOOT_HART_DONE	2

.macro	MOV_3R __d0, __s0, __d1, __s1, __d2, __s2
	add	\__d0, \__s0, zero
	add	\__d1, \__s1, zero
	add	\__d2, \__s2, zero
.endm

.macro	MOV_5R __d0, __s0, __d1, __s1, __d2, __s2, __d3, __s3, __d4, __s4
	add	\__d0, \__s0, zero
	add	\__d1, \__s1, zero
	add	\__d2, \__s2, zero
	add	\__d3, \__s3, zero
	add	\__d4, \__s4, zero
.endm

/*
 * If __start_reg <= __check_reg and __check_reg < __end_reg then
 *   jump to __pass
 */
.macro BRANGE __start_reg, __end_reg, __check_reg, __jump_lable
	blt	\__check_reg, \__start_reg, 999f
	bge	\__check_reg, \__end_reg, 999f
	j	\__jump_lable
999:
.endm

	.section .entry, "ax", %progbits
	.align 3
	.globl _start
	.globl _start_warm
_start:
	/* Find preferred boot HART id */
	MOV_3R	s0, a0, s1, a1, s2, a2 /// 保存a0, a1, a2的值到s0 s1 s2
	call	fw_boot_hart /// 跳转到fw_payload.S, 执行li a0, -1后返回
	add	a6, a0, zero /// a6=a0=-1
	MOV_3R	a0, s0, a1, s1, a2, s2 ///恢复a0, a1, a2的值
	li	a7, -1
	beq	a6, a7, _try_lottery ///if a6==a7==-1 jump to _try_lottery
	/* Jump to relocation wait loop if we are not boot hart */
	bne	a0, a6, _wait_relocate_copy_done
_try_lottery:
	/* Jump to relocation wait loop if we don't get relocation lottery */
	la	a6, _relocate_lottery /// 将_relocate_lottery的地址加载到a6
	li	a7, 1
	amoadd.w a6, a7, (a6)
	bnez	a6, _wait_relocate_copy_done

	/* Save load address */
	la	t0, _load_start
	la	t1, _start
	REG_S	t1, 0(t0) /// sw t1, 0(t0) _load_start = _start

	/* Relocate if load address != link address */
_relocate:
	la	t0, _link_start
	REG_L	t0, 0(t0)
	la	t1, _link_end
	REG_L	t1, 0(t1)
	la	t2, _load_start
	REG_L	t2, 0(t2)
	sub	t3, t1, t0
	add	t3, t3, t2
	beq	t0, t2, _relocate_done
	la	t4, _relocate_done
	sub	t4, t4, t2
	add	t4, t4, t0
	blt	t2, t0, _relocate_copy_to_upper
_relocate_copy_to_lower:
	ble	t1, t2, _relocate_copy_to_lower_loop
	la	t3, _relocate_lottery
	BRANGE	t2, t1, t3, _start_hang
	la	t3, _boot_status
	BRANGE	t2, t1, t3, _start_hang
	la	t3, _relocate
	la	t5, _relocate_done
	BRANGE	t2, t1, t3, _start_hang
	BRANGE	t2, t1, t5, _start_hang
	BRANGE  t3, t5, t2, _start_hang
_relocate_copy_to_lower_loop:
	REG_L	t3, 0(t2)
	REG_S	t3, 0(t0)
	add	t0, t0, __SIZEOF_POINTER__
	add	t2, t2, __SIZEOF_POINTER__
	blt	t0, t1, _relocate_copy_to_lower_loop
	jr	t4
_relocate_copy_to_upper:
	ble	t3, t0, _relocate_copy_to_upper_loop
	la	t2, _relocate_lottery
	BRANGE	t0, t3, t2, _start_hang
	la	t2, _boot_status
	BRANGE	t0, t3, t2, _start_hang
	la	t2, _relocate
	la	t5, _relocate_done
	BRANGE	t0, t3, t2, _start_hang
	BRANGE	t0, t3, t5, _start_hang
	BRANGE	t2, t5, t0, _start_hang
_relocate_copy_to_upper_loop:
	add	t3, t3, -__SIZEOF_POINTER__
	add	t1, t1, -__SIZEOF_POINTER__
	REG_L	t2, 0(t3)
	REG_S	t2, 0(t1)
	blt	t0, t1, _relocate_copy_to_upper_loop
	jr	t4
_wait_relocate_copy_done:
	la	t0, _start
	la	t1, _link_start
	REG_L	t1, 0(t1)
	beq	t0, t1, _wait_for_boot_hart
	la	t2, _boot_status
	la	t3, _wait_for_boot_hart
	sub	t3, t3, t0
	add	t3, t3, t1
1:
	/* waitting for relocate copy done (_boot_status == 1) */
	li	t4, BOOT_STATUS_RELOCATE_DONE
	REG_L	t5, 0(t2)
	/* Reduce the bus traffic so that boot hart may proceed faster */
	nop
	nop
	nop
	bgt     t4, t5, 1b
	jr	t3
_relocate_done:

	/*
	 * Mark relocate copy done
	 * Use _boot_status copy relative to the load address
	 */
	la	t0, _boot_status
	la	t1, _link_start
	REG_L	t1, 0(t1)
	la	t2, _load_start
	REG_L	t2, 0(t2)
	sub	t0, t0, t1
	add	t0, t0, t2
	li	t1, BOOT_STATUS_RELOCATE_DONE
	REG_S	t1, 0(t0)
	fence	rw, rw

	/* At this point we are running from link address */

	/* Reset all registers for boot HART */
	li	ra, 0
	call	_reset_regs

	/* Zero-out BSS */
	la	s4, _bss_start
	la	s5, _bss_end
_bss_zero:
	REG_S	zero, (s4)
	add	s4, s4, __SIZEOF_POINTER__
	blt	s4, s5, _bss_zero /// s4 < s5 继续清零

	/* Setup temporary trap handler */
	la	s4, _start_hang
	csrw	CSR_MTVEC, s4 /// 设置此时中断和异常地址是_start_hang, 此时发生中断和异常会hang住

	/* Setup temporary stack */
	la	s4, _fw_end
	li	s5, (SBI_SCRATCH_SIZE * 2) /// 0x1000 * 2
	add	sp, s4, s5 /// sp = _fw_end + 0x2000

	/* Allow main firmware to save info */
	MOV_5R	s0, a0, s1, a1, s2, a2, s3, a3, s4, a4
	call	fw_save_info ///fw_payload.S fw_jump.S直接ret了, fw_dynamic.S需要保存寄存器
	MOV_5R	a0, s0, a1, s1, a2, s2, a3, s3, a4, s4

#ifdef FW_FDT_PATH
	/* Override previous arg1 */
	la	a1, fw_fdt_bin /// 传入设备树地址
#endif

	/*
	 * Initialize platform
	 * Note: The a0 to a4 registers passed to the
	 * firmware are parameters to this function.
	 */
	MOV_5R	s0, a0, s1, a1, s2, a2, s3, a3, s4, a4
	call	fw_platform_init /// platform/generic/platform.c
	add	t0, a0, zero
	MOV_5R	a0, s0, a1, s1, a2, s2, a3, s3, a4, s4
	add	a1, t0, zero

	/* Preload HART details
	 * s7 -> HART Count
	 * s8 -> HART Stack Size
	 */
	la	a4, platform /// 将platform/realtek/platform.c中的const struct sbi_platform platform传进来
#if __riscv_xlen == 64
	lwu	s7, SBI_PLATFORM_HART_COUNT_OFFSET(a4)
	lwu	s8, SBI_PLATFORM_HART_STACK_SIZE_OFFSET(a4)
#else
	lw	s7, SBI_PLATFORM_HART_COUNT_OFFSET(a4)  /// SBI_PLATFORM_HART_COUNT_OFFSET=0x50 结构体偏移0x50得到.hart_count
	lw	s8, SBI_PLATFORM_HART_STACK_SIZE_OFFSET(a4)  /// SBI_PLATFORM_HART_COUNT_OFFSET=0x54 结构体偏移0x54得到.hart_stack_size
#endif

	/* Setup scratch space for all the HARTs*/
	la	tp, _fw_end  /// _fw_end c0015000
	mul	a5, s7, s8 /// hard_count * hard_stack_size
	add	tp, tp, a5 /// _fw_end + hard_count * hard_stack_size
	/* Keep a copy of tp */
	add	t3, tp, zero
	/* Counter */
	li	t2, 1
	/* hartid 0 is mandated by ISA */
	li	t1, 0
_scratch_init:
	add	tp, t3, zero
	mul	a5, s8, t1
	sub	tp, tp, a5
	li	a5, SBI_SCRATCH_SIZE
	sub	tp, tp, a5 /// tp = _fw_end + hard_count * hard_stack_size - SBI_SCRATCH_SIZE.

	/* Initialize scratch space */
	/* Store fw_start and fw_size in scratch space */
	la	a4, _fw_start
	la	a5, _fw_end
	mul	t0, s7, s8
	add	a5, a5, t0
	sub	a5, a5, a4
	REG_S	a4, SBI_SCRATCH_FW_START_OFFSET(tp) ///_fw_start传入sbi_scratch->fw_start
	REG_S	a5, SBI_SCRATCH_FW_SIZE_OFFSET(tp) /// size传入sbi_scratch->fw_size
	/* Store next arg1 in scratch space */
	MOV_3R	s0, a0, s1, a1, s2, a2
	call	fw_next_arg1
	REG_S	a0, SBI_SCRATCH_NEXT_ARG1_OFFSET(tp) /// 把设备树地址传入sbi_scratch->next_arg1(next booting stage的arg,即a1 register)
	MOV_3R	a0, s0, a1, s1, a2, s2
	/* Store next address in scratch space */
	MOV_3R	s0, a0, s1, a1, s2, a2
	call	fw_next_addr
	REG_S	a0, SBI_SCRATCH_NEXT_ADDR_OFFSET(tp) /// payload_bin地址传入sbi_scratch->next_addr(next booting stage) payload_bin=FW_PAYLOAD_PATH 在命令行传入
	MOV_3R	a0, s0, a1, s1, a2, s2
	/* Store next mode in scratch space */
	MOV_3R	s0, a0, s1, a1, s2, a2
	call	fw_next_mode
	REG_S	a0, SBI_SCRATCH_NEXT_MODE_OFFSET(tp) /// 把Smode传入sbi_scratch->next_mode
	MOV_3R	a0, s0, a1, s1, a2, s2
	/* Store warm_boot address in scratch space */
	la	a4, _start_warm
	REG_S	a4, SBI_SCRATCH_WARMBOOT_ADDR_OFFSET(tp) /// _start_warm传入sbi_scratch->warmboot_addr
	/* Store platform address in scratch space */
	la	a4, platform
	REG_S	a4, SBI_SCRATCH_PLATFORM_ADDR_OFFSET(tp) /// platform传入sbi_scratch->platform_addr
	/* Store hartid-to-scratch function address in scratch space */
	la	a4, _hartid_to_scratch
	REG_S	a4, SBI_SCRATCH_HARTID_TO_SCRATCH_OFFSET(tp) /// _hartid_to_scratch传入sbi_scratch->hartid_to_scratch
	/* Store trap-exit function address in scratch space */
	la	a4, _trap_exit
	REG_S	a4, SBI_SCRATCH_TRAP_EXIT_OFFSET(tp) /// _trap_exit传入sbi_scratch->trap_exit
	/* Clear tmp0 in scratch space */
	REG_S	zero, SBI_SCRATCH_TMP0_OFFSET(tp) /// 0传入sbi_scratch->tmp0
	/* Store firmware options in scratch space */
	MOV_3R	s0, a0, s1, a1, s2, a2
#ifdef FW_OPTIONS
	li	a0, FW_OPTIONS
#else
	call	fw_options
#endif
	REG_S	a0, SBI_SCRATCH_OPTIONS_OFFSET(tp)
	MOV_3R	a0, s0, a1, s1, a2, s2
	/* Move to next scratch space */
	add	t1, t1, t2 /// t1 = 0 + 1 = 1
	blt	t1, s7, _scratch_init /// 如果hart count > 1, 接着初始化另一个sbi_scratch

	/*
	 * Relocate Flatened Device Tree (FDT)
	 * source FDT address = previous arg1
	 * destination FDT address = next arg1
	 *
	 * Note: We will preserve a0 and a1 passed by
	 * previous booting stage.
	 */
	beqz	a1, _fdt_reloc_done /// a1是设备树地址fw_fdt_bin c000f000。187定义了
	/* Mask values in a3 and a4 */
	li	a3, ~(__SIZEOF_POINTER__ - 1)
	li	a4, 0xff
	/* t1 = destination FDT start address */
	MOV_3R	s0, a0, s1, a1, s2, a2
	call	fw_next_arg1 /// a0传入FW_PAYLOAD_FDT_ADDR 0xC1f00000 在platform/realtek/sheipa/config.mk中定义，为fdt relocate地址。
	add	t1, a0, zero
	MOV_3R	a0, s0, a1, s1, a2, s2
	beqz	t1, _fdt_reloc_done
	beq	t1, a1, _fdt_reloc_done
	and	t1, t1, a3
	/* t0 = source FDT start address */
	add	t0, a1, zero
	and	t0, t0, a3
	/* t2 = source FDT size in big-endian */
#if __riscv_xlen == 64
	lwu	t2, 4(t0)
#else
	lw	t2, 4(t0)
#endif
	/* t3 = bit[15:8] of FDT size */
	add	t3, t2, zero
	srli	t3, t3, 16
	and	t3, t3, a4
	slli	t3, t3, 8
	/* t4 = bit[23:16] of FDT size */
	add	t4, t2, zero
	srli	t4, t4, 8
	and	t4, t4, a4
	slli	t4, t4, 16
	/* t5 = bit[31:24] of FDT size */
	add	t5, t2, zero
	and	t5, t5, a4
	slli	t5, t5, 24
	/* t2 = bit[7:0] of FDT size */
	srli	t2, t2, 24
	and	t2, t2, a4
	/* t2 = FDT size in little-endian */
	or	t2, t2, t3
	or	t2, t2, t4
	or	t2, t2, t5
	/* t2 = destination FDT end address */
	add	t2, t1, t2
	/* FDT copy loop */
	ble	t2, t1, _fdt_reloc_done
_fdt_reloc_again:
	REG_L	t3, 0(t0)
	REG_S	t3, 0(t1)
	add	t0, t0, __SIZEOF_POINTER__
	add	t1, t1, __SIZEOF_POINTER__
	blt	t1, t2, _fdt_reloc_again
_fdt_reloc_done:

	/* mark boot hart done */
	li	t0, BOOT_STATUS_BOOT_HART_DONE
	la	t1, _boot_status
	REG_S	t0, 0(t1)
	fence	rw, rw
	j	_start_warm /// 初始化第二个cpu核心

	/* waiting for boot hart to be done (_boot_status == 2) */
_wait_for_boot_hart:
	li	t0, BOOT_STATUS_BOOT_HART_DONE
	la	t1, _boot_status
	REG_L	t1, 0(t1)
	/* Reduce the bus traffic so that boot hart may proceed faster */
	nop
	nop
	nop
	bne	t0, t1, _wait_for_boot_hart

_start_warm:
	/* Reset all registers for non-boot HARTs */
	li	ra, 0
	call	_reset_regs

	/* Disable and clear all interrupts */
	csrw	CSR_MIE, zero
	csrw	CSR_MIP, zero

	/* Find HART count and HART stack size */
	la	a4, platform
#if __riscv_xlen == 64
	lwu	s7, SBI_PLATFORM_HART_COUNT_OFFSET(a4)
	lwu	s8, SBI_PLATFORM_HART_STACK_SIZE_OFFSET(a4)
#else
	lw	s7, SBI_PLATFORM_HART_COUNT_OFFSET(a4)
	lw	s8, SBI_PLATFORM_HART_STACK_SIZE_OFFSET(a4)
#endif
	REG_L	s9, SBI_PLATFORM_HART_INDEX2ID_OFFSET(a4) /// SBI_PLATFORM_HART_INDEX2ID_OFFSET=0x58+sizeof(pointer)*2=0x60结构体偏移0x54得到.hart_index2id

	/* Find HART id */
	csrr	s6, CSR_MHARTID /// s6=CSR_MHARTID的值,CSR_MHARTID寄存器为0，表示为单核架构，寄存器为1，表示双核架构

	/* Find HART index */
	beqz	s9, 3f /// 如果hardid=0跳过下面这段，直接到3:	bge	s6, s7, _start_hang
	li	a4, 0
1:
#if __riscv_xlen == 64
	lwu	a5, (s9)
#else
	lw	a5, (s9)
#endif
	beq	a5, s6, 2f
	add	s9, s9, 4
	add	a4, a4, 1
	blt	a4, s7, 1b
	li	a4, -1
2:	add	s6, a4, zero
3:	bge	s6, s7, _start_hang

	/* Find the scratch space based on HART index */
	la	tp, _fw_end /// _fw_end c0015000
	mul	a5, s7, s8 /// hard_count * hard_stack_size
	add	tp, tp, a5 /// _fw_end + hard_count * hard_stack_size
	mul	a5, s8, s6 /// CSR_MHARTID * hart_stack_size
	sub	tp, tp, a5 /// _fw_end + (hard_count - CSR_MHARTID) * hard_stack_size
	li	a5, SBI_SCRATCH_SIZE /// 0x1000
	sub	tp, tp, a5 /// tp = _fw_end + (hard_count - CSR_MHARTID) * hard_stack_size - 0x1000

	/* update the mscratch */
	csrw	CSR_MSCRATCH, tp /// 将tp写入csr状态寄存器CSR_MSCRATCH(0x340), 提供临时变量空间

	/* Setup stack */
	add	sp, tp, zero /// 设置堆栈

	/* Setup trap handler */
	la	a4, _trap_handler
#if __riscv_xlen == 32
	csrr	a5, CSR_MISA
	srli	a5, a5, ('H' - 'A') /// 取CSR_MISA的bit7 hypervisor extesion
	andi	a5, a5, 0x1
	beq	a5, zero, _skip_trap_handler_rv32_hyp
	la	a4, _trap_handler_rv32_hyp
_skip_trap_handler_rv32_hyp:
#endif
	csrw	CSR_MTVEC, a4 /// 存储中断或异常入口地址_trap_handler

#if __riscv_xlen == 32
	/* Override trap exit for H-extension */
	csrr	a5, CSR_MISA
	srli	a5, a5, ('H' - 'A')
	andi	a5, a5, 0x1
	beq	a5, zero, _skip_trap_exit_rv32_hyp
	la	a4, _trap_exit_rv32_hyp
	csrr	a5, CSR_MSCRATCH
	REG_S	a4, SBI_SCRATCH_TRAP_EXIT_OFFSET(a5)
_skip_trap_exit_rv32_hyp:
#endif

	/* Initialize SBI runtime */
	csrr	a0, CSR_MSCRATCH /// 读控制状态寄存器csr(CSR_MSCRATCH)的值(地址为0x340)写入a0
	call	sbi_init /// lib/sbi/sbi_init.c

	/* We don't expect to reach here hence just hang */
	j	_start_hang

	.align 3
_relocate_lottery:
	RISCV_PTR	0
_boot_status:
	RISCV_PTR	0
_load_start:
	RISCV_PTR	_fw_start
_link_start:
	RISCV_PTR	_fw_start
_link_end:
	RISCV_PTR	_fw_reloc_end

	.section .entry, "ax", %progbits
	.align 3
	.globl _hartid_to_scratch
_hartid_to_scratch:
	/*
	 * a0 -> HART ID (passed by caller)
	 * a1 -> HART Index (passed by caller)
	 * t0 -> HART Stack Size
	 * t1 -> HART Stack End
	 * t2 -> Temporary
	 */
	la	t2, platform
#if __riscv_xlen == 64
	lwu	t0, SBI_PLATFORM_HART_STACK_SIZE_OFFSET(t2)
	lwu	t2, SBI_PLATFORM_HART_COUNT_OFFSET(t2)
#else
	lw	t0, SBI_PLATFORM_HART_STACK_SIZE_OFFSET(t2)
	lw	t2, SBI_PLATFORM_HART_COUNT_OFFSET(t2)
#endif
	sub	t2, t2, a1
	mul	t2, t2, t0
	la	t1, _fw_end
	add	t1, t1, t2
	li	t2, SBI_SCRATCH_SIZE
	sub	a0, t1, t2
	ret

	.section .entry, "ax", %progbits
	.align 3
	.globl _start_hang
_start_hang:
	wfi
	j	_start_hang

	.section .entry, "ax", %progbits
	.align 3
	.globl fw_platform_init
	.weak fw_platform_init
fw_platform_init:
	add	a0, a1, zero
	ret

.macro	TRAP_SAVE_AND_SETUP_SP_T0
	/* Swap TP and MSCRATCH */
	csrrw	tp, CSR_MSCRATCH, tp /// 交换tp和CSR_MSCRATCH

	/* Save T0 in scratch space */
	REG_S	t0, SBI_SCRATCH_TMP0_OFFSET(tp)

	/*
	 * Set T0 to appropriate exception stack
	 *
	 * Came_From_M_Mode = ((MSTATUS.MPP < PRV_M) ? 1 : 0) - 1;
	 * Exception_Stack = TP ^ (Came_From_M_Mode & (SP ^ TP))
	 *
	 * Came_From_M_Mode = 0    ==>    Exception_Stack = TP
	 * Came_From_M_Mode = -1   ==>    Exception_Stack = SP
	 */
	csrr	t0, CSR_MSTATUS
	srl	t0, t0, MSTATUS_MPP_SHIFT /// 获取CSR_MSTATUS的bit11 MSTATUS_MPP，中断发生前的特权级别
	and	t0, t0, PRV_M /// 如果M模式发生的中断t0=3，S模式发生的中断t0=1
	slti	t0, t0, PRV_M /// 如果t0(MSTATUS_MPP) < PRV_M 则t0=1
	add	t0, t0, -1 /// t0=0 t0=-1
	xor	sp, sp, tp ///
	and	t0, t0, sp
	xor	sp, sp, tp
	xor	t0, tp, t0

	/* Save original SP on exception stack */
	REG_S	sp, (SBI_TRAP_REGS_OFFSET(sp) - SBI_TRAP_REGS_SIZE)(t0)

	/* Set SP to exception stack and make room for trap registers */
	add	sp, t0, -(SBI_TRAP_REGS_SIZE)

	/* Restore T0 from scratch space */
	REG_L	t0, SBI_SCRATCH_TMP0_OFFSET(tp)

	/* Save T0 on stack */
	REG_S	t0, SBI_TRAP_REGS_OFFSET(t0)(sp)

	/* Swap TP and MSCRATCH */
	csrrw	tp, CSR_MSCRATCH, tp
.endm

.macro	TRAP_SAVE_MEPC_MSTATUS have_mstatush
	/* Save MEPC and MSTATUS CSRs */
	csrr	t0, CSR_MEPC /// t0=CSR_MEPC
	REG_S	t0, SBI_TRAP_REGS_OFFSET(mepc)(sp) /// 存MEPC的值
	csrr	t0, CSR_MSTATUS
	REG_S	t0, SBI_TRAP_REGS_OFFSET(mstatus)(sp) /// 存CSR_MSTATUS的值
	.if \have_mstatush /// hypervisor 跳过
	csrr	t0, CSR_MSTATUSH
	REG_S	t0, SBI_TRAP_REGS_OFFSET(mstatusH)(sp)
	.else
	REG_S	zero, SBI_TRAP_REGS_OFFSET(mstatusH)(sp)
	.endif
.endm

.macro	TRAP_SAVE_GENERAL_REGS_EXCEPT_SP_T0
	/* Save all general regisers except SP and T0 */
	REG_S	zero, SBI_TRAP_REGS_OFFSET(zero)(sp)
	REG_S	ra, SBI_TRAP_REGS_OFFSET(ra)(sp)
	REG_S	gp, SBI_TRAP_REGS_OFFSET(gp)(sp)
	REG_S	tp, SBI_TRAP_REGS_OFFSET(tp)(sp)
	REG_S	t1, SBI_TRAP_REGS_OFFSET(t1)(sp)
	REG_S	t2, SBI_TRAP_REGS_OFFSET(t2)(sp)
	REG_S	s0, SBI_TRAP_REGS_OFFSET(s0)(sp)
	REG_S	s1, SBI_TRAP_REGS_OFFSET(s1)(sp)
	REG_S	a0, SBI_TRAP_REGS_OFFSET(a0)(sp)
	REG_S	a1, SBI_TRAP_REGS_OFFSET(a1)(sp)
	REG_S	a2, SBI_TRAP_REGS_OFFSET(a2)(sp)
	REG_S	a3, SBI_TRAP_REGS_OFFSET(a3)(sp)
	REG_S	a4, SBI_TRAP_REGS_OFFSET(a4)(sp)
	REG_S	a5, SBI_TRAP_REGS_OFFSET(a5)(sp)
	REG_S	a6, SBI_TRAP_REGS_OFFSET(a6)(sp)
	REG_S	a7, SBI_TRAP_REGS_OFFSET(a7)(sp)
	REG_S	s2, SBI_TRAP_REGS_OFFSET(s2)(sp)
	REG_S	s3, SBI_TRAP_REGS_OFFSET(s3)(sp)
	REG_S	s4, SBI_TRAP_REGS_OFFSET(s4)(sp)
	REG_S	s5, SBI_TRAP_REGS_OFFSET(s5)(sp)
	REG_S	s6, SBI_TRAP_REGS_OFFSET(s6)(sp)
	REG_S	s7, SBI_TRAP_REGS_OFFSET(s7)(sp)
	REG_S	s8, SBI_TRAP_REGS_OFFSET(s8)(sp)
	REG_S	s9, SBI_TRAP_REGS_OFFSET(s9)(sp)
	REG_S	s10, SBI_TRAP_REGS_OFFSET(s10)(sp)
	REG_S	s11, SBI_TRAP_REGS_OFFSET(s11)(sp)
	REG_S	t3, SBI_TRAP_REGS_OFFSET(t3)(sp)
	REG_S	t4, SBI_TRAP_REGS_OFFSET(t4)(sp)
	REG_S	t5, SBI_TRAP_REGS_OFFSET(t5)(sp)
	REG_S	t6, SBI_TRAP_REGS_OFFSET(t6)(sp)
.endm

.macro	TRAP_CALL_C_ROUTINE
	/* Call C routine */
	add	a0, sp, zero
	call	sbi_trap_handler
.endm

.macro	TRAP_RESTORE_GENERAL_REGS_EXCEPT_SP_T0
	/* Restore all general regisers except SP and T0 */
	REG_L	ra, SBI_TRAP_REGS_OFFSET(ra)(sp)
	REG_L	gp, SBI_TRAP_REGS_OFFSET(gp)(sp)
	REG_L	tp, SBI_TRAP_REGS_OFFSET(tp)(sp)
	REG_L	t1, SBI_TRAP_REGS_OFFSET(t1)(sp)
	REG_L	t2, SBI_TRAP_REGS_OFFSET(t2)(sp)
	REG_L	s0, SBI_TRAP_REGS_OFFSET(s0)(sp)
	REG_L	s1, SBI_TRAP_REGS_OFFSET(s1)(sp)
	REG_L	a0, SBI_TRAP_REGS_OFFSET(a0)(sp)
	REG_L	a1, SBI_TRAP_REGS_OFFSET(a1)(sp)
	REG_L	a2, SBI_TRAP_REGS_OFFSET(a2)(sp)
	REG_L	a3, SBI_TRAP_REGS_OFFSET(a3)(sp)
	REG_L	a4, SBI_TRAP_REGS_OFFSET(a4)(sp)
	REG_L	a5, SBI_TRAP_REGS_OFFSET(a5)(sp)
	REG_L	a6, SBI_TRAP_REGS_OFFSET(a6)(sp)
	REG_L	a7, SBI_TRAP_REGS_OFFSET(a7)(sp)
	REG_L	s2, SBI_TRAP_REGS_OFFSET(s2)(sp)
	REG_L	s3, SBI_TRAP_REGS_OFFSET(s3)(sp)
	REG_L	s4, SBI_TRAP_REGS_OFFSET(s4)(sp)
	REG_L	s5, SBI_TRAP_REGS_OFFSET(s5)(sp)
	REG_L	s6, SBI_TRAP_REGS_OFFSET(s6)(sp)
	REG_L	s7, SBI_TRAP_REGS_OFFSET(s7)(sp)
	REG_L	s8, SBI_TRAP_REGS_OFFSET(s8)(sp)
	REG_L	s9, SBI_TRAP_REGS_OFFSET(s9)(sp)
	REG_L	s10, SBI_TRAP_REGS_OFFSET(s10)(sp)
	REG_L	s11, SBI_TRAP_REGS_OFFSET(s11)(sp)
	REG_L	t3, SBI_TRAP_REGS_OFFSET(t3)(sp)
	REG_L	t4, SBI_TRAP_REGS_OFFSET(t4)(sp)
	REG_L	t5, SBI_TRAP_REGS_OFFSET(t5)(sp)
	REG_L	t6, SBI_TRAP_REGS_OFFSET(t6)(sp)
.endm

.macro	TRAP_RESTORE_MEPC_MSTATUS have_mstatush
	/* Restore MEPC and MSTATUS CSRs */
	REG_L	t0, SBI_TRAP_REGS_OFFSET(mepc)(sp)
	csrw	CSR_MEPC, t0
	REG_L	t0, SBI_TRAP_REGS_OFFSET(mstatus)(sp)
	csrw	CSR_MSTATUS, t0
	.if \have_mstatush
	REG_L	t0, SBI_TRAP_REGS_OFFSET(mstatusH)(sp)
	csrw	CSR_MSTATUSH, t0
	.endif
.endm

.macro TRAP_RESTORE_SP_T0
	/* Restore T0 */
	REG_L	t0, SBI_TRAP_REGS_OFFSET(t0)(sp)

	/* Restore SP */
	REG_L	sp, SBI_TRAP_REGS_OFFSET(sp)(sp)
.endm

	.section .entry, "ax", %progbits
	.align 3
	.globl _trap_handler
_trap_handler:
	TRAP_SAVE_AND_SETUP_SP_T0

	TRAP_SAVE_MEPC_MSTATUS 0

	TRAP_SAVE_GENERAL_REGS_EXCEPT_SP_T0

	TRAP_CALL_C_ROUTINE

	TRAP_RESTORE_GENERAL_REGS_EXCEPT_SP_T0

	TRAP_RESTORE_MEPC_MSTATUS 0

	TRAP_RESTORE_SP_T0

	mret

	.section .entry, "ax", %progbits
	.align 3
	.globl _trap_exit
_trap_exit:
	add	sp, a0, zero

	TRAP_RESTORE_GENERAL_REGS_EXCEPT_SP_T0

	TRAP_RESTORE_MEPC_MSTATUS 0

	TRAP_RESTORE_SP_T0

	mret

#if __riscv_xlen == 32
	.section .entry, "ax", %progbits
	.align 3
	.globl _trap_handler_rv32_hyp
_trap_handler_rv32_hyp:
	TRAP_SAVE_AND_SETUP_SP_T0

	TRAP_SAVE_MEPC_MSTATUS 1

	TRAP_SAVE_GENERAL_REGS_EXCEPT_SP_T0

	TRAP_CALL_C_ROUTINE

	TRAP_RESTORE_GENERAL_REGS_EXCEPT_SP_T0

	TRAP_RESTORE_MEPC_MSTATUS 1

	TRAP_RESTORE_SP_T0

	mret

	.section .entry, "ax", %progbits
	.align 3
	.globl _trap_exit_rv32_hyp
_trap_exit_rv32_hyp:
	add	sp, a0, zero

	TRAP_RESTORE_GENERAL_REGS_EXCEPT_SP_T0

	TRAP_RESTORE_MEPC_MSTATUS 1

	TRAP_RESTORE_SP_T0

	mret
#endif

	.section .entry, "ax", %progbits
	.align 3
	.globl _reset_regs
_reset_regs:

	/* flush the instruction cache */
	fence.i
	/* Reset all registers except ra, a0, a1 and a2 */
	li sp, 0
	li gp, 0
	li tp, 0
	li t0, 0
	li t1, 0
	li t2, 0
	li s0, 0
	li s1, 0
	li a3, 0
	li a4, 0
	li a5, 0
	li a6, 0
	li a7, 0
	li s2, 0
	li s3, 0
	li s4, 0
	li s5, 0
	li s6, 0
	li s7, 0
	li s8, 0
	li s9, 0
	li s10, 0
	li s11, 0
	li t3, 0
	li t4, 0
	li t5, 0
	li t6, 0
	csrw CSR_MSCRATCH, 0

	ret

#ifdef FW_FDT_PATH
	.section .rodata
	.align 4
	.globl fw_fdt_bin
fw_fdt_bin:
	.incbin FW_FDT_PATH /// 把二进制设备树文件加到.rodata段, 通过objdump出来fw_fdt_bin地址为c000f000
#ifdef FW_FDT_PADDING
	.fill FW_FDT_PADDING, 1, 0
#endif
#endif
