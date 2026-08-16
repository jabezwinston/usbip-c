# The role libraries libusbip-device / libusbip-host, static and shared.
#
# Each role names its own sources, objects and export list; the archive and the
# shared library are then built by one recipe each.

TOP := ..
include $(TOP)/config.mk
include $(TOP)/src/libs.mk

DEVICE_SRC := src/usbip_device.c src/usbip.c src/pcap.c
# the host library carries the device core too; those symbols stay internal
HOST_SRC   := src/usbip_host.c $(DEVICE_SRC)

DEVICE_OBJ := $(call lib_objects,$(DEVICE_SRC))
HOST_OBJ   := $(call lib_objects,$(HOST_SRC))

DEVICE_EXPORTS := $(OBJ_DIR)/exports/device.$(EXPORT_FMT)
HOST_EXPORTS   := $(OBJ_DIR)/exports/host.$(EXPORT_FMT)

# empty off Windows
DEVICE_RES := $(call winres_obj,libusbip-device)
HOST_RES   := $(call winres_obj,libusbip-host)

DEPS := $(patsubst %.o,%.d,$(sort $(DEVICE_OBJ) $(HOST_OBJ)))

.PHONY: all libs static-libs shared-libs device-lib host-lib check clean
all: static-libs shared-libs
libs: all
static-libs: $(DEVICE_A) $(HOST_A)
shared-libs: $(DEVICE_SO) $(HOST_SO)

# one role's shared library on its own
device-lib: $(DEVICE_SO)
host-lib:   $(HOST_SO)

# ---------------------------------------------------------------------------
# What each role is made of
# ---------------------------------------------------------------------------
$(DEVICE_A):  $(DEVICE_OBJ)
$(DEVICE_SO): $(DEVICE_OBJ) $(DEVICE_EXPORTS) $(DEVICE_RES)
$(DEVICE_SO): LIB_NAME    := libusbip-device
$(DEVICE_SO): EXPORT_LIST := $(DEVICE_EXPORTS)

$(HOST_A):  $(HOST_OBJ)
$(HOST_SO): $(HOST_OBJ) $(HOST_EXPORTS) $(HOST_RES)
$(HOST_SO): LIB_NAME    := libusbip-host
$(HOST_SO): EXPORT_LIST := $(HOST_EXPORTS)

ifdef WINRES
$(DEVICE_RES) $(HOST_RES): RC_FILETYPE := VFT_DLL
$(DEVICE_RES): RC_FILE_NAME   := $(call shlib,libusbip-device)
$(DEVICE_RES): RC_DESCRIPTION := USB/IP virtual USB device library
$(HOST_RES):   RC_FILE_NAME   := $(call shlib,libusbip-host)
$(HOST_RES):   RC_DESCRIPTION := USB/IP host (client) library
endif

# ---------------------------------------------------------------------------
# How any of them is built
# ---------------------------------------------------------------------------
# library objects see the public headers and the private src/ tree, nothing else
$(LIBOBJ_DIR)/%.o: $(TOP)/%.c
	@$(call mkdir_p,$(@D))
	@echo [ CC lib ] $(call tree_path,$<)
	@$(CC) $(CFLAGS) $(PICFLAG) -I$(TOP)/include -I$(TOP)/src -MMD -MP -c -o $@ $<

# src/exports/<role>.syms is the source of truth; gen_exports.py renders the dialect
GEN_EXPORTS := $(TOP)/scripts/gen_exports.py
$(OBJ_DIR)/exports/%.$(EXPORT_FMT): $(TOP)/src/exports/%.syms $(GEN_EXPORTS)
	@$(call mkdir_p,$(@D))
	@$(PYTHON) $(GEN_EXPORTS) $< --format $(EXPORT_FMT) --library libusbip-$* -o $@

# recreate each time: `ar r` would keep members dropped from the list
$(DEVICE_A) $(HOST_A):
	@echo [ AR ] $(call tree_path,$@)
	@$(call rm_f,$@)
	@$(AR) rcs $@ $^

# winres rides in via $(filter %.o,$^); the export list goes to the linker instead
$(DEVICE_SO) $(HOST_SO):
	@echo [ LD ] $(call tree_path,$@)
	@$(CC) $(CFLAGS) $(SHLIB_MODE) -o $@ $(filter %.o,$^) \
	    $(call shlib_ldflags,$(LIB_NAME),$(EXPORT_LIST))
	@$(call shlib_link_cmd,$(LIB_NAME))

# ---------------------------------------------------------------------------
# make check: every public header must compile on its own
# ---------------------------------------------------------------------------
# hdr_both.c is the one TU that includes both roles
CHECK_SRC := include/usbip.h include/usbip-device.h include/usbip-host.h \
             include/classes/usb_class.h include/classes/hid.h \
             include/classes/mtp.h include/classes/bluetooth.h \
             include/classes/uac.h scripts/hdr_both.c

# one target per file, so -j checks them in parallel
CHECK_TGTS := $(addprefix check@,$(CHECK_SRC))
.PHONY: $(CHECK_TGTS)
$(CHECK_TGTS):
	@$(CC) $(CFLAGS) -fsyntax-only -I$(TOP)/include -x c $(TOP)/$(@:check@%=%)

check: $(CHECK_TGTS)
	@echo OK: headers compile

clean:
	$(clean_build_dir)

-include $(DEPS)
