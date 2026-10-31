/* OpenCE's game code and our Xbox layer, after the retail XBE's last section (.cap16 ends at 0x3B9000):
   the retail sections stay where they are, the libraries' code and data among them */
INCLUDE ovl.ld   /* OpenCE .bss over the retail game's unused code and globals (port/oxbox/tools/gen_overlay.py) */
SECTIONS {
  . = 0x3B9000;
  __oxbox_image_start = .;
  .text : { *(.text.oxbox_entry) *(EXCLUDE_FILE(*oxbox_perf.o *oxbox_ftp.o *oxbox_update.o *mbedtls/library/*.o) .text .text.*) }
  .rodata : { *(EXCLUDE_FILE(*oxbox_perf.o *oxbox_ftp.o *oxbox_update.o *mbedtls/library/*.o) .rodata .rodata.*) }
  .data : { *(EXCLUDE_FILE(*oxbox_perf.o *oxbox_ftp.o *oxbox_update.o *mbedtls/library/*.o) .data .data.*) }
  .init_array : { __oxbox_init_start = .; KEEP(*(.init_array*)) __oxbox_init_end = .; }
  __oxbox_image_end = .;
  .bss (NOLOAD) : { __oxbox_bss_start = .; *(EXCLUDE_FILE(*oxbox_perf.o *oxbox_ftp.o *oxbox_update.o *mbedtls/library/*.o) .bss .bss.* COMMON)
                    __oxbox_bss_end = .; }
  /* the developers' perf log (port/oxbox/oxbox_perf.c), code, data and buffers, in its own XBE section (OXPERF, tools/mkxbe.py)
     after the image: not loaded at boot, only when opence.ini says perf_log = 1 (oxbox_sdk.c oxbox_perf_load) */
  . = ALIGN(0x1000);
  __oxperf_start = .;
  .oxperf : { *oxbox_perf.o(.text .text.* .rodata .rodata.* .data .data.*) }
  __oxperf_load_end = .;
  .oxperf.bss (NOLOAD) : { *oxbox_perf.o(.bss .bss.* COMMON) }
  __oxperf_end = .;
  /* the developers' FTP server (port/oxbox/net/oxbox_ftp.c), likewise: OXFTP, loaded only with ftp = 1 (oxbox_sdk.c) */
  . = ALIGN(0x1000);
  __oxftp_start = .;
  .oxftp : { *oxbox_ftp.o(.text .text.* .rodata .rodata.* .data .data.*) }
  __oxftp_load_end = .;
  .oxftp.bss (NOLOAD) : { *oxbox_ftp.o(.bss .bss.* COMMON) }
  __oxftp_end = .;
  /* the update check (port/oxbox/net/oxbox_update.c) and Mbed TLS under it, likewise: OXUPD, loaded for the check and
     unloaded after it (oxbox_sdk.c oxbox_update_frame) */
  . = ALIGN(0x1000);
  __oxupd_start = .;
  .oxupd : { *oxbox_update.o(.text .text.* .rodata .rodata.* .data .data.*)
             *mbedtls/library/*.o(.text .text.* .rodata .rodata.* .data .data.*) }
  __oxupd_load_end = .;
  .oxupd.bss (NOLOAD) : { *oxbox_update.o(.bss .bss.* COMMON) *mbedtls/library/*.o(.bss .bss.* COMMON) }
  __oxupd_end = .;
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
