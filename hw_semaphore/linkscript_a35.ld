OUTPUT_FORMAT(elf64-littleaarch64)
OUTPUT_ARCH(aarch64)

ENTRY(_Reset)

MEMORY
{
    DDRRAM (rx) : ORIGIN = 0x88000000, LENGTH = 16M
}

__HEAP_SIZE = 0x40000;

SECTIONS
{
    .text : {
        KEEP(*(.text.boot)) /* _Reset must be first: TF-A's baremetal loader jumps to the image start */
        *(.vector_table)
        *(.text)
        *(.text*)
					/* Todo: check if we need the next 3 lines */
		*(.glue_7)         /* glue arm to thumb code */
		*(.glue_7t)        /* glue thumb to arm code */
		*(.eh_frame)

		KEEP (*(.init)) /* libc ctors */
		KEEP (*(.fini)) /* libc dtors */

        . = ALIGN(8);
     } > DDRRAM

	/* .rodata sections (constants, strings, etc.) */
	.rodata :
	{
		. = ALIGN(8);
		*(.rodata)         
		*(.rodata*) 
		. = ALIGN(8);
	} > DDRRAM

	/* used for unwinding (probably not used, but is ignored if your app doens't use exceptions */
	.ARM.extab	 : { *(.ARM.extab* .gnu.linkonce.armextab.*) } >DDRRAM
	.ARM : { __exidx_start = .; *(.ARM.exidx*) __exidx_end = .; } >DDRRAM

	.preinit_array :
	{
		. = ALIGN(8);
		PROVIDE_HIDDEN (__preinit_array_start = .);
		KEEP (*(.preinit_array*))
		PROVIDE_HIDDEN (__preinit_array_end = .);
		. = ALIGN(8);
	} > DDRRAM

	.init_array :
	{
		. = ALIGN(8);
		PROVIDE_HIDDEN (__init_array_start = .);
		KEEP (*(SORT(.init_array.*)))
		KEEP (*(.init_array*))
		PROVIDE_HIDDEN (__init_array_end = .);
		. = ALIGN(8);
	} > DDRRAM

	.fini_array :
	{
		. = ALIGN(8);
		PROVIDE_HIDDEN (__fini_array_start = .);
		KEEP (*(SORT(.fini_array.*)))
		KEEP (*(.fini_array*))
		PROVIDE_HIDDEN (__fini_array_end = .);
		. = ALIGN(8);
	} > DDRRAM

    .data : 
    {
        . = ALIGN(8);
	 	*(.got)
	 	*(.got.plt)
        *(.data)
        *(.data*)
        . = ALIGN(8);
        _data_end = .;
    } > DDRRAM

    .bss : {
        _bss_start = .;
        *(.bss)
        *(.bss*)   /* required for some malloc calls */
		*(COMMON)  /* required for libc, such as __lock___atexit_recursive_mutex */
        . = ALIGN(8);
        _bss_end = .;
    } > DDRRAM

	.heap (NOLOAD):
	{
		. = ALIGN(16);
		_sheap = .;
		. += __HEAP_SIZE;
		_eheap = .;
	} > DDRRAM

    /* AArch64 requires SP to be 16-byte aligned for any stack access, so every
     * stack base below must be too (each size is a multiple of 16, so aligning
     * the first one keeps the rest aligned). */
    _fiq_stack_start = ALIGN(ADDR(.heap) + SIZEOF(.heap), 16);
    _fiq_stack_end = _fiq_stack_start + 0x1000; /* 64 KB */

    _irq_stack_start = _fiq_stack_end;
    _irq_stack_end = _irq_stack_start + 0x1000; /* 64 KB */

    _stack_start = _irq_stack_end;
    _stack_end = _stack_start + 0x1000; /* 64 KB */

    /* A35 core 1 runs the same image and needs its own stack (aux_core_startup.s) */
    _cpu1_stack_start = _stack_end;
    _cpu1_stack_end = _cpu1_stack_start + 0x1000; /* 64 KB */

    _irq_stack_size = _irq_stack_end - _irq_stack_start;
    _fiq_stack_size = _fiq_stack_end - _fiq_stack_start;

	/*
	.dma_buffer (NOLOAD) :
	{
		. = ALIGN(8);
		*(.dma_buffer)
		*(.dma_buffer*)
		. = ALIGN(8);
	} > SRAM3
	*/
	

  /* Remove information from the compiler libraries */
  /DISCARD/ :
  {
    libc.a ( * )
    libm.a ( * )
    libgcc.a ( * )
  }

	.ARM.attributes 0 : { *(.ARM.attributes) }
}
