# ABI-compatible libusb-1.0 / libusbK drop-ins on the usbip_* client primitives.
# LD_PRELOAD one under any stock libusb program and it talks to a USB/IP device.

TOP := ..
include $(TOP)/config.mk

LIBUSB_DIR  := libusb-1.0
LIBUSBK_DIR := libusbK

# compiled straight into each drop-in, so no objects collide with src/Makefile
CORE_SRC := $(addprefix $(TOP)/,src/usbip_device.c src/usbip.c src/pcap.c)
CORE_INC := $(addprefix -I$(TOP)/,$(INCLUDEPATHS))

LIBUSB_SO  := $(OBJ_DIR)/$(call shlib,libusb-1.0)
LIBUSBK_SO := $(OBJ_DIR)/libusbK.dll

# empty off Windows
LIBUSB_RES  := $(call winres_obj,libusb-1.0)
LIBUSBK_RES := $(call winres_obj,libusbK)

.PHONY: all libusb libusbk check check-libusb check-libusbk clean
all: libusb
libusb: $(LIBUSB_SO)

clean:
	$(clean_build_dir)

# libusbK is a Windows-only API (HANDLE/OVERLAPPED/__stdcall)
ifeq ($(OS),Windows_NT)
all: libusbk
libusbk: $(LIBUSBK_SO)
else
libusbk:
	@echo "libusbK is a Windows-only API; build it with: make OS=Windows_NT libusbk"
endif

# ---------------------------------------------------------------------------
# libusb-1.0
# ---------------------------------------------------------------------------
# Naming and the alias step come from config.mk; only the link flags are local,
# because the exports come from a hand-written libusb-1.0/libusb.map.
ifeq ($(OS),Windows_NT)
    # the API is __stdcall, so --add-stdcall-alias also exports undecorated names.
    # -static drops the mingw runtime DLLs and re-exports winpthread's clock_gettime.
    LIBUSB_LDFLAGS = -Wl,--add-stdcall-alias \
                     -Wl,--out-implib,$(OBJ_DIR)/$(call shlib_link,libusb-1.0) \
                     -static $(addprefix -l,$(LIBS))
else ifeq ($(UNAME_S),Darwin)
    # untested; Apple ld cannot read libusb.map, so macOS exports more than Linux
    LIBUSB_LDFLAGS = -install_name @rpath/$(@F)
else
    # the version script exports only libusb_* and hides usbip_*/usbip_device_*
    LIBUSB_LDFLAGS = -Wl,-soname,$(@F) -Wl,--version-script=$(LIBUSB_DIR)/libusb.map
endif

$(LIBUSB_SO): $(CORE_SRC) $(LIBUSB_DIR)/libusb.c $(LIBUSB_DIR)/libusb.map $(LIBUSB_RES)
	@$(call mkdir_p,$(@D))
	@echo [ LD ] $(call tree_path,$@)
	@$(CC) $(CFLAGS) $(CORE_INC) -I$(LIBUSB_DIR)/include $(PICFLAG) $(SHLIB_MODE) \
	    -o $@ $(CORE_SRC) $(LIBUSB_DIR)/libusb.c $(LIBUSB_RES) $(LIBUSB_LDFLAGS)
	@$(call shlib_link_cmd,libusb-1.0)

# ---------------------------------------------------------------------------
# libusbK: Windows only
# ---------------------------------------------------------------------------
ifeq ($(OS),Windows_NT)
# the .def + --enable-stdcall-fixup export undecorated names (UsbK_Init, ...)
$(LIBUSBK_SO): $(CORE_SRC) $(LIBUSBK_DIR)/libusbk.c $(LIBUSBK_DIR)/libusbK.def $(LIBUSBK_RES)
	@$(call mkdir_p,$(@D))
	@echo [ LD ] $(call tree_path,$@)
	@$(CC) $(CFLAGS) $(CORE_INC) -I$(LIBUSBK_DIR)/include -shared \
	    -o $@ $(CORE_SRC) $(LIBUSBK_DIR)/libusbk.c $(LIBUSBK_DIR)/libusbK.def \
	    $(LIBUSBK_RES) -Wl,--enable-stdcall-fixup -static $(addprefix -l,$(LIBS))
endif

ifdef WINRES
$(LIBUSB_RES) $(LIBUSBK_RES): RC_FILETYPE := VFT_DLL
$(LIBUSB_RES):  RC_FILE_NAME   := libusb-1.0.dll
$(LIBUSB_RES):  RC_DESCRIPTION := libusb-1.0 API over USB/IP (drop-in)
$(LIBUSBK_RES): RC_FILE_NAME   := libusbK.dll
$(LIBUSBK_RES): RC_DESCRIPTION := libusbK API over USB/IP (drop-in)
endif

# ---------------------------------------------------------------------------
# make check: the drop-in headers must compile on their own
# ---------------------------------------------------------------------------
check: check-libusb
check-libusb:
	@$(CC) $(CFLAGS) -fsyntax-only -I$(LIBUSB_DIR)/include -x c $(LIBUSB_DIR)/include/libusb.h
	@echo OK: libusb.h compiles

# libusbk.h needs <windows.h>, so it is checked only under the Windows toolchain
ifeq ($(OS),Windows_NT)
check: check-libusbk
endif
check-libusbk:
	@$(CC) $(CFLAGS) -fsyntax-only -I$(LIBUSBK_DIR)/include -x c $(LIBUSBK_DIR)/include/libusbk.h
	@echo OK: libusbk.h compiles
