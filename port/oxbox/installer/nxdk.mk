# The Halo 1 Unlimited installer (port/oxbox/installer/README.md), with nxdk:
#     make -C port/oxbox/installer -f nxdk.mk NXDK_DIR=/path/to/nxdk     (with $NXDK_DIR/bin on the PATH)
# makes bin/default.xbe. (nxdk's own objects go beside its sources; this folder's beside these.) It trusts the public
# keys in keys.txt (KEYS=other.txt for another list, to test with keys of your own).
XBE_TITLE = Halo\ 1\ Unlimited\ installer
MONOCYPHER = $(CURDIR)/../../third_party/monocypher
SRCS = $(CURDIR)/main.c $(CURDIR)/package.c $(CURDIR)/pictures.c $(CURDIR)/screen.c $(CURDIR)/sha256.c \
	$(MONOCYPHER)/monocypher.c $(MONOCYPHER)/monocypher-ed25519.c
NXDK_SDL = y
KEYS ?= $(CURDIR)/keys.txt
# (single precision in SSE, in the order written: the pictures come out as the PC tool makes them)
CFLAGS += -O2 -mfpmath=sse -ffp-contract=off -Wall -I$(MONOCYPHER)

include $(NXDK_DIR)/Makefile

# the keys it trusts, as C (rewritten only when they change)
$(CURDIR)/keys.h: FORCE
	python3 $(CURDIR)/../tools/mkkeys.py $(KEYS) $@
$(CURDIR)/main.obj $(CURDIR)/oxpatch.obj: $(CURDIR)/keys.h
.PHONY: FORCE
FORCE:
