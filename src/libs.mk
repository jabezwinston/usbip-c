# The role libraries src/ produces, named once for the Makefiles that use them:
# src/ builds them, examples/ links them, the top level packages and installs them.
# Include after config.mk, which provides BUILD_DIR and the shlib helpers.

SRC_BUILD := $(TOP)/src/$(BUILD_DIR)

DEVICE_A  := $(SRC_BUILD)/libusbip-device.a
DEVICE_SO := $(SRC_BUILD)/$(call shlib,libusbip-device)
HOST_A    := $(SRC_BUILD)/libusbip-host.a
HOST_SO   := $(SRC_BUILD)/$(call shlib,libusbip-host)
