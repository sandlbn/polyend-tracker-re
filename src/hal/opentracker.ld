/* Teensyduino Core Library
 * http://www.pjrc.com/teensy/
 * Copyright (c) 2017 PJRC.COM, LLC.
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * 1. The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * 2. If the Software is incorporated into a build system that allows
 * selection among a list of target devices, then similar target
 * devices manufactured by PJRC.COM must be included in the list of
 * target devices and selectable in the same manner.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
 * BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
 * ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

/* OpenTracker linker script (docs/PLAN_2X.md performance workstream): the
 * Teensyduino mk66fx1m0.ld above (third_party/teensy3, left untouched) plus a
 * `.dma` section for DMA buffers (src/hal/dma_mem.h: OT_DMA_MEM).
 *
 * K66 memory map: SRAM_L 0x1FFF0000..0x1FFFFFFF (code bus, LMEM code cache),
 * SRAM_U 0x20000000..0x2002FFFF (system bus, uncached). `.dma` follows `.bss`
 * and is pushed up to SRAM_U if `.bss` ends below it, so every DMA buffer is
 * uncached and never crosses the SRAM_L/SRAM_U boundary. It sits inside
 * _sbss.._ebss, so the startup code zeroes it and the heap (_ebss) starts above.
 */

MEMORY
{
	FLASH (rx) : ORIGIN = 0x00000000, LENGTH = 1024K
	RAM  (rwx) : ORIGIN = 0x1FFF0000, LENGTH = 256K
	/* board::kSdramBssBase / kSdramBssSize (src/hal/board.h): SDRAM, NOLOAD. */
	SDRAM_BSS (rw) : ORIGIN = 0x70FD8400, LENGTH = 159K
}

ENTRY(_VectorsFlash)

SECTIONS
{
	.text : {
		. = 0;
		KEEP(*(.vectors))
		*(.startup*)
		/* TODO: does linker detect startup overflow onto flashconfig? */
		. = 0x400;
		KEEP(*(.flashconfig*))
		/* [I] shared fast pool (default; OT_HOT_POOL=0 = off): the built-in synths'
		 * hot rows, back to back (tools/hot_pool.py layout; empty otherwise). */
		INCLUDE hot_pool_text.ld
		*(.text*)
		KEEP(*(.rodata.otSdramLayoutEnd))   /* read by tools/size_report.py (CI) */
		*(.rodata*)
		/* [I] the rows' move tables (modules/hot_rows.h): after every
		 * other flash contents, so their size moves nothing they describe. */
		. = ALIGN(4);
		KEEP(*(.ot_hot_rows))
		. = ALIGN(4);
		KEEP(*(.init))
		. = ALIGN(4);
		__preinit_array_start = .;
		KEEP (*(.preinit_array))
		__preinit_array_end = .;
		__init_array_start = .;
		KEEP (*(SORT(.init_array.*)))
		KEEP (*(.init_array))
		__init_array_end = .;
	} > FLASH = 0xFF

	.ARM.exidx : {
		__exidx_start = .;
		*(.ARM.exidx* .gnu.linkonce.armexidx.*)
		__exidx_end = .;
	} > FLASH
	_etext = .;

	.usbdescriptortable (NOLOAD) : {
		/* . = ORIGIN(RAM); */
		. = ALIGN(512);
		*(.usbdescriptortable*)
	} > RAM

	.dmabuffers (NOLOAD) : {
		. = ALIGN(4);
		*(.dmabuffers*)
	} > RAM

	.usbbuffers (NOLOAD) : {
		. = ALIGN(4);
		*(.usbbuffers*)
	} > RAM

	.data : AT (_etext) {
		. = ALIGN(4);
		_sdata = .; 
		/* OT_FASTRUN_FN code (src/core/fastrun.h): runs from SRAM_L. Grouped
		 * by area for the boot log line (src/hal/board.cpp "fastrun:"). The
		 * synth engines' hot code is in the shared fast pool (.otm_fast);
		 * with make OT_HOT_POOL=0 their .fastrun code lands in "other". */
		__fastrun_start = .;
		__fastrun_smp = .;
		*/audio/voice.cpp.o(.fastrun*) */audio/voice_stereo.cpp.o(.fastrun*)
		__fastrun_fx = .;
		*/audio/effects.cpp.o(.fastrun*)
		__fastrun_mix = .;
		*/audio/engine.cpp.o(.fastrun*) */audio/synth_bank.cpp.o(.fastrun*)
		__fastrun_other = .;
		*(.fastrun*)
		__fastrun_end = .;
		*(.data*)
		. = ALIGN(4);
		_edata = .; 
	} > RAM

	/* The update bootloader can leave the image's last 4 KB flash sector
	 * unwritten, so it keeps the previous firmware's bytes; with the .data
	 * load image there, globals such as newlib's _impure_ptr start as
	 * garbage. Zeros after .data up to the next sector boundary plus one
	 * more sector: the last sector holds only padding
	 * (tools/check_image.py checks it). BYTE(0) gives the section
	 * contents; without it ld emits no bytes. */
	.flashpad LOADADDR(.data) + SIZEOF(.data) : {
		FILL(0x00)
		BYTE(0)
		. = ALIGN(4096);
		. = . + 4096;
	} > FLASH

	/* [I] OT_POOL_TEST images: the block card modules' fast parts run
	 * from (hal/module_pool.cpp), in SRAM_L. __otm_fast_bytes comes from the
	 * Makefile (--defsym; 0 = no block, the layout unchanged). */
	.otm_fast (NOLOAD) : {
		. = ALIGN(__otm_fast_bytes > 0 ? 8 : 1);
		__otm_fast_start = .;
		. = . + __otm_fast_bytes;
		__otm_fast_end = .;
	} > RAM

	.noinit (NOLOAD) : {
		*(.noinit*)
	} > RAM

	.bss : {
		. = ALIGN(4);
		_sbss = .;
		__bss_start__ = .;
		*(.bss*)
		*(COMMON)
		. = ALIGN(4);
	} > RAM

	. = MAX(., 0x20000000);
	.dma (NOLOAD) : ALIGN(32) {
		_sdma = .;
		*(.dma)
		*(.dma.*)
		. = ALIGN(4);
		_edma = .;
		_ebss = .;
		__bss_end = .;
		__bss_end__ = .;
	} > RAM

	/* Large main-loop scratch buffers in the SDRAM (src/core/sdram_bss.h,
	 * docs/OPTIMIZATION_2X.md OP-5). Not zeroed by the startup code (the
	 * SDRAM is not up yet): otAppMain() clears _ssdram_bss.._esdram_bss. */
	.sdram_bss (NOLOAD) : ALIGN(16) {
		_ssdram_bss = .;
		*(.sdram_bss)
		*(.sdram_bss.*)
		. = ALIGN(16);
		_esdram_bss = .;
	} > SDRAM_BSS
	/* The region's end, for tools/size_report.py (CI memory report). */
	_sdram_bss_limit = ORIGIN(SDRAM_BSS) + LENGTH(SDRAM_BSS);

	_estack = ORIGIN(RAM) + LENGTH(RAM);
	ASSERT(_sdma >= 0x20000000, "OpenTracker: .dma must lie in SRAM_U (uncached)")
	ASSERT(_edma <= 0x20030000, "OpenTracker: .dma overflows SRAM_U")
	/* Leave room for the main stack (field builds ran with 25-53 KB headroom). */
	ASSERT(__fastrun_end <= 0x20000000, "OpenTracker: .fastrun code must lie in SRAM_L (code bus)")
	ASSERT(__otm_fast_end <= 0x20000000, "OpenTracker: the card modules' fast block must lie in SRAM_L")
	ASSERT(_estack - _ebss >= 16384, "OpenTracker: less than 16 KB left for the stack")
	_teensy_model_identifier = 0x22;
}




