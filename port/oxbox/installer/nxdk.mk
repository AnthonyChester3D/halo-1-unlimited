# The Halo 1 Unlimited installer (port/oxbox/installer/README.md), with nxdk:
#     make -C port/oxbox/installer -f nxdk.mk NXDK_DIR=/path/to/nxdk     (with $NXDK_DIR/bin on the PATH)
# makes bin/default.xbe. (nxdk's own objects go beside its sources; this folder's beside these.)
XBE_TITLE = Halo\ 1\ Unlimited\ installer
SRCS = $(CURDIR)/main.c $(CURDIR)/package.c $(CURDIR)/pictures.c $(CURDIR)/sha256.c
NXDK_SDL = y
# (single precision in SSE, in the order written: the pictures come out as the PC tool makes them)
CFLAGS += -O2 -mfpmath=sse -ffp-contract=off -Wall

include $(NXDK_DIR)/Makefile
