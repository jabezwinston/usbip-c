# The example programs. Each one is a SRC_<name> entry in the registry below;
# one link rule builds them all against the packaged library of their role.

TOP := ..
include $(TOP)/config.mk
include $(TOP)/src/libs.mk

DEV     := examples/device
CLASSES := src/classes/device

# the class layer, shared by every class-based example
USB_CLASS := src/classes/usb_class.c

# shared example helpers: grouped event logging + command-line parsing
SUPPORT := $(DEV)/_support/logging.c $(DEV)/_support/cmdline.c

# ---------------------------------------------------------------------------
# Registry: what each example is built from
# ---------------------------------------------------------------------------

# a virtual CDC-ACM serial port
SRC_cdc_acm_device := $(USB_CLASS) $(SUPPORT) $(CLASSES)/cdc_acm.c $(DEV)/cdc_acm_device.c


# a virtual USB mass-storage device
SRC_msc_device := $(USB_CLASS) $(SUPPORT) $(CLASSES)/msc.c $(DEV)/msc_device.c


# a UVC webcam (iso); the class is YUYV-only, MJPEG comes from jpeg.c
SRC_uvc_device := $(USB_CLASS) $(SUPPORT) $(CLASSES)/uvc.c \
                  $(DEV)/video/jpeg.c $(DEV)/video/uvc_device.c

# a virtual DFU device: file-backed targets, WinUSB
SRC_dfu_device := $(USB_CLASS) $(SUPPORT) $(CLASSES)/dfu.c $(DEV)/dfu_device.c

# a virtual MTP device that exports a directory
SRC_mtp_device := $(USB_CLASS) $(SUPPORT) $(CLASSES)/mtp.c $(DEV)/mtp_device.c

# a generic HID device: mouse / keyboard / raw profiles
SRC_hid_device := $(USB_CLASS) $(CLASSES)/hid.c $(DEV)/hid_device.c

# a composite CDC-ACM serial port plus a HID boot keyboard
SRC_cdc_hid_device := $(USB_CLASS) $(CLASSES)/cdc_acm.c $(CLASSES)/hid.c \
                      $(DEV)/cdc_hid_device.c

# two separate devices from one process on one listener
SRC_multi_device := $(USB_CLASS) $(SUPPORT) $(CLASSES)/hid.c $(CLASSES)/cdc_acm.c \
                    $(DEV)/multi_device.c


# a UAC1 sound card: stereo speaker + mono mic, 48 kHz / 16-bit
SRC_audio_device := $(USB_CLASS) $(SUPPORT) $(CLASSES)/uac.c \
                    $(DEV)/audio/main.c $(DEV)/audio/wav.c

# vendor class 0xFF bulk loopback, self-contained, no class code
SRC_vendor_device := $(DEV)/vendor_device.c

# a WebUSB device co-advertising WinUSB, self-contained, no class code
SRC_webusb_device := $(DEV)/webusb_device.c


# host side: import and drive a device (libusb-shaped client)
SRC_cdc_host := examples/host/cdc_host.c

# host side: drive a UVC camera over the isochronous host API
SRC_uvc_host := examples/host/uvc_host.c

# host side: enumerate and probe any USB/IP server
SRC_host_probe := examples/host/host_probe.c

# Built like any other example, so the guide pages cannot drift from the API.
# boot_keyboard: a HID keyboard on the device core alone. host_drive: usbip_host_*.
SRC_boot_keyboard := doc/examples/boot_keyboard.c
SRC_host_drive    := doc/examples/host_drive.c

# ---------------------------------------------------------------------------
# Which of them this tree can build
# ---------------------------------------------------------------------------
ALL_EXAMPLES := cdc_acm_device dual_cdc_device msc_device msc_cdc_device cdc_dfu_device \
                uvc_device dfu_device mtp_device hid_device cdc_hid_device multi_device \
                bluetooth_device ble_hid_dongle bt_classic_dongle audio_device \
                vendor_device webusb_device fido_device cdc_host uvc_host host_probe \
                boot_keyboard host_drive

# Build what is present: the published repos ship a subset, so an example drops out
# when its registry entry or any source it names did not survive the export.
missing_sources = $(strip $(foreach s,$(SRC_$(1)),$(if $(wildcard $(TOP)/$(s)),,$(s))))
buildable       = $(if $(SRC_$(1)),$(if $(call missing_sources,$(1)),,$(1)))

EXAMPLES := $(foreach e,$(ALL_EXAMPLES),$(call buildable,$(e)))
BINS     := $(addprefix $(OBJ_DIR)/,$(addsuffix $(EXT),$(EXAMPLES)))

# every object across all examples, deduplicated, for the .d files
ALL_OBJ := $(call objects,$(sort $(foreach e,$(EXAMPLES),$(SRC_$(e))))) \
           $(foreach e,$(EXAMPLES),$(EXTRA_OBJ_$(e)))
DEPS    := $(patsubst %.o,%.d,$(ALL_OBJ))

# ---------------------------------------------------------------------------
# Linking
# ---------------------------------------------------------------------------
.PHONY: all clean $(EXAMPLES)
all: $(BINS)

clean:
	$(clean_build_dir)

# `make -C examples hid_device` builds exactly that one
$(EXAMPLES): %: $(OBJ_DIR)/%$(EXT)

# these four take the host role; every other example links libusbip-device.a
HOST_EXAMPLES := cdc_host uvc_host host_probe host_drive
role_lib = $(if $(filter $(1),$(HOST_EXAMPLES)),$(HOST_A),$(DEVICE_A))

# fallback so a standalone `make -C examples` works in a fresh tree
$(DEVICE_A) $(HOST_A):
	@$(MAKE) -C $(TOP)/src $@

# what each binary is made of; the role library comes last so it resolves the objects
$(foreach e,$(EXAMPLES),$(eval $(OBJ_DIR)/$(e)$(EXT): $(call objects,$(SRC_$(e))) \
    $(EXTRA_OBJ_$(e)) $(call winres_obj,$(e)) $(call role_lib,$(e))))

$(OBJ_DIR)/uvc_device$(EXT): LDLIBS := -lm

$(BINS):
	@echo [ LD ] $(call tree_path,$@)
	@$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS) $(addprefix -l,$(LIBS))

# ---------------------------------------------------------------------------
# Per-directory compile settings
# ---------------------------------------------------------------------------
# examples see the public headers and their own directories, never src/
$(OBJ_DIR)/examples/%.o: INCLUDEPATHS = include $(DEV)


ifdef WINRES
# examples describe themselves in their VERSIONINFO block
$(foreach e,$(EXAMPLES),$(call winres_obj,$(e))): RC_DESCRIPTION = usbip example: $*
endif

-include $(DEPS)
