OUTPUT_FORMAT("elf32-littlearm", "elf32-littlearm", "elf32-littlearm")
OUTPUT_ARCH(arm)
ENTRY(_start)
SECTIONS
{

 . = 0x00000000;// 定义了地址为_start的地址,所以我们分析代码就是从这个函数开始分析的.
 . = ALIGN(4);
 .text : // 定义text段
 {
  *(.__image_copy_start)
  *(.vectors) //所有目标文件的vectors段,也就是中断向量表连接到这里来
  arch/arm/cpu/armv7/start.o (.text*) //start.o文件的.text段链接到这里来
 }
 .__efi_runtime_start : {
  *(.__efi_runtime_start)
 }
 .efi_runtime : {
  *(.text.efi_runtime*)
  *(.rodata.efi_runtime*)
  *(.data.efi_runtime*)
 }
 .__efi_runtime_stop : {
  *(.__efi_runtime_stop)
 }
 .text_rest :
 {
  *(.text*) //所有目标文件的.text段链接到这里来
 }
 . = ALIGN(4);
 .rodata : { *(SORT_BY_ALIGNMENT(SORT_BY_NAME(.rodata*))) } //定义只读数据段
 . = ALIGN(4);
 .data : { //定义数据段
  *(.data*) //所有目标文件的.data段链接到这里来
 }
 . = ALIGN(4);
 . = .;
 . = ALIGN(4);
 __u_boot_list : {
  KEEP(*(SORT(__u_boot_list*)));
 }
 . = ALIGN(4);
 .efi_runtime_rel_start :
 {
  *(.__efi_runtime_rel_start)
 }
 .efi_runtime_rel : {
  *(.rel*.efi_runtime)
  *(.rel*.efi_runtime.*)
 }
 .efi_runtime_rel_stop :
 {
  *(.__efi_runtime_rel_stop)
 }
 . = ALIGN(4);
 .image_copy_end :
 {
  *(.__image_copy_end) //从__image_copy_start 到__image_copy_end的区间，包含了代码段和数据段。
 }
 .rel_dyn_start :
 {
  *(.__rel_dyn_start)
 }
 .rel.dyn : {
  *(.rel*)
 }
 .rel_dyn_end :
 {
  *(.__rel_dyn_end)
 }
 .end :
 {
  *(.__end)
 }
 _image_binary_end = .; //定义_image_binary_end 符号的地址为当前地址
 . = ALIGN(4096);
 .mmutable : {
  *(.mmutable)
 }
 .bss_start __rel_dyn_start (OVERLAY) : {
  KEEP(*(.__bss_start));
  __bss_base = .;
 }
 .bss __bss_base (OVERLAY) : {
  *(.bss*)
   . = ALIGN(4);
   __bss_limit = .;
 }
 .bss_end __bss_limit (OVERLAY) : {
  KEEP(*(.__bss_end));
 }
 .dynsym _image_binary_end : { *(.dynsym) }
 .dynbss : { *(.dynbss) }
 .dynstr : { *(.dynstr*) }
 .dynamic : { *(.dynamic*) }
 .plt : { *(.plt*) }
 .interp : { *(.interp*) }
 .gnu.hash : { *(.gnu.hash) }
 .gnu : { *(.gnu*) }
 .ARM.exidx : { *(.ARM.exidx*) }
 .gnu.linkonce.armexidx : { *(.gnu.linkonce.armexidx.*) }
}
