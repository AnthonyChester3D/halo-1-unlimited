/* OpenCE's game code and our Xbox layer, after the retail XBE's last section (.cap16 ends at 0x3B9000):
   the retail sections stay where they are, the libraries' code and data among them */
INCLUDE ovl.ld   /* OpenCE .bss over the retail game's unused code and globals (port/oxbox/tools/gen_overlay.py) */
SECTIONS {
  . = 0x3B9000;
  __oxbox_image_start = .;
  .text : { *(.text.oxbox_entry) *(.text .text.*) }
  .rodata : { *(.rodata .rodata.*) }
  .data : { *(.data .data.*) }
  .init_array : { __oxbox_init_start = .; KEEP(*(.init_array*)) __oxbox_init_end = .; }
  __oxbox_image_end = .;
  .bss (NOLOAD) : { __oxbox_bss_start = .; *(.bss .bss.*) *(COMMON) __oxbox_bss_end = .; }
  /DISCARD/ : { *(.comment) *(.note*) *(.eh_frame*) *(.gnu*) }
}
/* the overlay's .bss parts oxbox_entry zeroes (ovl.ld defines those it has) */
PROVIDE(__ovlz0_start = 0); PROVIDE(__ovlz0_end = 0);
PROVIDE(__ovlz1_start = 0); PROVIDE(__ovlz1_end = 0);
PROVIDE(__ovlz2_start = 0); PROVIDE(__ovlz2_end = 0);
PROVIDE(__ovlz3_start = 0); PROVIDE(__ovlz3_end = 0);
PROVIDE(__ovlz4_start = 0); PROVIDE(__ovlz4_end = 0);
PROVIDE(__ovlz5_start = 0); PROVIDE(__ovlz5_end = 0);
PROVIDE(__ovlz6_start = 0); PROVIDE(__ovlz6_end = 0);
PROVIDE(__ovlz7_start = 0); PROVIDE(__ovlz7_end = 0);
PROVIDE(__ovlz8_start = 0); PROVIDE(__ovlz8_end = 0);
PROVIDE(__ovlz9_start = 0); PROVIDE(__ovlz9_end = 0);
PROVIDE(__ovlz10_start = 0); PROVIDE(__ovlz10_end = 0);
PROVIDE(__ovlz11_start = 0); PROVIDE(__ovlz11_end = 0);
PROVIDE(__ovlz12_start = 0); PROVIDE(__ovlz12_end = 0);
PROVIDE(__ovlz13_start = 0); PROVIDE(__ovlz13_end = 0);
PROVIDE(__ovlz14_start = 0); PROVIDE(__ovlz14_end = 0);
PROVIDE(__ovlz15_start = 0); PROVIDE(__ovlz15_end = 0);
INCLUDE retail.ld
