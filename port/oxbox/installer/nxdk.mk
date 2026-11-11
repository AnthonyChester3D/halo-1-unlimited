# The Halo 1 Unlimited installer (port/oxbox/installer/README.md), with nxdk:
#     make -C port/oxbox/installer -f nxdk.mk NXDK_DIR=/path/to/nxdk     (with $NXDK_DIR/bin on the PATH)
# makes bin/default.xbe. (nxdk's own objects go beside its sources; this folder's beside these, and Mbed TLS's in
# mbedtls_obj.) It trusts the public keys in keys.txt (KEYS=other.txt for another list, to test with keys of your own).
XBE_TITLE = Halo\ 1\ Unlimited\ installer
MONOCYPHER = $(CURDIR)/../../third_party/monocypher
MBEDTLS = $(CURDIR)/../../third_party/mbedtls
ZLIBNG = $(CURDIR)/../../third_party/zlib-ng
SRCS = $(CURDIR)/main.c $(CURDIR)/package.c $(CURDIR)/packs.c $(CURDIR)/pictures.c $(CURDIR)/screen.c $(CURDIR)/sha256.c \
	$(CURDIR)/stockmaps.c $(CURDIR)/mapstream.c $(CURDIR)/ngz.c $(CURDIR)/consolidate.c \
	$(CURDIR)/download.c $(MONOCYPHER)/monocypher.c $(MONOCYPHER)/monocypher-ed25519.c
NXDK_SDL = y
KEYS ?= $(CURDIR)/keys.txt
# (nxdk's libraries, zlib among them, are built with these flags too when this build is the one that makes them: README.md)
# (single precision in SSE, in the order written: the pictures come out as the PC tool makes them)
CFLAGS += -O2 -mfpmath=sse -ffp-contract=off -Wall -I$(MONOCYPHER) -I$(MBEDTLS)/include -I$(CURDIR) \
	'-DMBEDTLS_CONFIG_FILE="tls_config.h"'
# (INSTALLER_VERSION=N: built as another version than main.c's, to test updating it)
ifneq ($(INSTALLER_VERSION),)
CFLAGS += -DINSTALLER_VERSION=$(INSTALLER_VERSION)
endif

include $(NXDK_DIR)/Makefile

# the keys it trusts, as C (rewritten only when they change)
$(CURDIR)/keys.h: FORCE
	python3 $(CURDIR)/../tools/mkkeys.py $(KEYS) $@
# the notices it writes beside itself (THIRD_PARTY.txt), as C
$(CURDIR)/notices.h: $(CURDIR)/THIRD_PARTY.txt $(CURDIR)/../tools/mkembed.py
	python3 $(CURDIR)/../tools/mkembed.py $< $@ notices
$(CURDIR)/main.obj: $(CURDIR)/keys.h $(CURDIR)/notices.h
$(CURDIR)/oxpatch.obj: $(CURDIR)/keys.h
.PHONY: FORCE
FORCE:

# Mbed TLS (port/third_party/mbedtls, as tls_config.h has it), a library, so only what download.c uses is linked.
# (x509_crt.c: without _WIN32, which nxdk's compiler defines, it doesn't look for Windows' sockets)
MBEDTLS_OBJS = $(patsubst $(MBEDTLS)/library/%.c,$(CURDIR)/mbedtls_obj/%.obj,$(wildcard $(MBEDTLS)/library/*.c))
$(CURDIR)/mbedtls_obj/x509_crt.obj: MBEDTLS_FLAGS = -U_WIN32
$(CURDIR)/mbedtls_obj/%.obj: $(MBEDTLS)/library/%.c $(CURDIR)/tls_config.h
	@mkdir -p $(dir $@)
	@echo "[ CC       ] $@"
	$(VE) $(CC) $(NXDK_CFLAGS) -O2 -I$(MBEDTLS)/include -I$(CURDIR) '-DMBEDTLS_CONFIG_FILE="tls_config.h"' \
		$(MBEDTLS_FLAGS) -c -o '$@' '$<'
$(CURDIR)/mbedtls_obj/libmbedtls.lib: $(MBEDTLS_OBJS)
main.exe: $(CURDIR)/mbedtls_obj/libmbedtls.lib

# zlib-ng 2.2.2 (port/third_party/zlib-ng: its plain C, native zng_ API; ngz.c is the only file that sees it), a library.
# -fhosted -fbuiltin: else nxdk's -ffreestanding makes its every small fixed-size memcpy a call. -U_MSC_VER: nxdk's compiler
# says it's Microsoft's, and zlib-ng would then use Microsoft's intrinsics
ZLIBNG_OBJS = $(patsubst $(ZLIBNG)/%.c,$(CURDIR)/zlibng_obj/%.obj,$(wildcard $(ZLIBNG)/*.c $(ZLIBNG)/arch/generic/*.c))
# (its folders before nxdk's, which has zlib's own zutil.h and the like)
ZLIBNG_FLAGS = -I$(ZLIBNG) -I$(ZLIBNG)/arch/generic -O2 -fhosted -fbuiltin -std=c11 -U_MSC_VER -DNDEBUG -DZLIBNG_NATIVE_API \
	-DDISABLE_RUNTIME_CPU_DETECTION -DHAVE_ALIGNED_ALLOC -DHAVE_ATTRIBUTE_ALIGNED -DHAVE_BUILTIN_CTZ -DHAVE_BUILTIN_CTZLL \
	-DHAVE_BUILTIN_ASSUME_ALIGNED
$(CURDIR)/zlibng_obj/%.obj: $(ZLIBNG)/%.c
	@mkdir -p $(dir $@)
	@echo "[ CC       ] $@"
	$(VE) $(CC) $(ZLIBNG_FLAGS) $(NXDK_CFLAGS) -c -o '$@' '$<'
$(CURDIR)/zlibng_obj/libzlibng.lib: $(ZLIBNG_OBJS)
main.exe: $(CURDIR)/zlibng_obj/libzlibng.lib
$(CURDIR)/ngz.obj: CFLAGS += -I$(ZLIBNG)
