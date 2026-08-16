# usbip C tree: drives src/, examples/ and wrapper/.
# make | make OS=Windows_NT | make check | make clean | sudo make install

TOP := .
include $(TOP)/config.mk
include $(TOP)/src/libs.mk

.PHONY: all src examples wrapper libs libusb libusbk check clean

all: src examples wrapper

# .PHONY is what stops make calling these up to date because the dirs exist
src:
	@$(MAKE) -C src

# order-only: examples link the role libraries, but -j still works inside
examples: | src
	@$(MAKE) -C examples

# the wrappers compile the core themselves, so they need nothing from src
wrapper:
	@$(MAKE) -C wrapper

# names the tests and documentation already use
libs: src
libusb libusbk:
	@$(MAKE) -C wrapper $@

check:
	@$(MAKE) -C src check
	@$(MAKE) -C wrapper check

# each directory cleans its own build/; this one also has the packaging output
clean:
	@$(MAKE) -C src clean
	@$(MAKE) -C examples clean
	@$(MAKE) -C wrapper clean
	$(clean_build_dir)

# ---------------------------------------------------------------------------
# Install. POSIX-shell only, and onto the build host: the release archives are
# assembled by .github/workflows/release.yml, not here.
# ---------------------------------------------------------------------------
ifndef WINDOWS_HOST

PUBLIC_HEADERS := include/usbip.h include/usbip-device.h include/usbip-host.h

.PHONY: install
PREFIX ?= /usr/local

# pkg-config files, one per role, from the shared template
$(OBJ_DIR)/usbip-%.pc: scripts/usbip.pc.in include/usbip.h
	@$(call mkdir_p,$(@D))
	@sed -e 's|@PREFIX@|$(PREFIX)|g' -e 's|@ROLE@|$*|g' \
	     -e 's|@VERSION@|$(USBIP_VERSION)|g' $< > $@

ifeq ($(OS),Windows_NT)
install:
	@echo "make install installs onto the build host, which is not the Windows target" >&2; exit 1
else
LIB_DIR := $(DESTDIR)$(PREFIX)/lib
install: libs $(OBJ_DIR)/usbip-device.pc $(OBJ_DIR)/usbip-host.pc
	install -d $(DESTDIR)$(PREFIX)/include $(LIB_DIR)/pkgconfig
	install -m644 $(PUBLIC_HEADERS) $(DESTDIR)$(PREFIX)/include/
	install -m644 $(DEVICE_A) $(HOST_A) $(LIB_DIR)/
	install -m755 $(DEVICE_SO) $(HOST_SO) $(LIB_DIR)/
	ln -sf $(call shlib,libusbip-device) $(LIB_DIR)/$(call shlib_link,libusbip-device)
	ln -sf $(call shlib,libusbip-host)   $(LIB_DIR)/$(call shlib_link,libusbip-host)
	install -m644 $(OBJ_DIR)/usbip-device.pc $(OBJ_DIR)/usbip-host.pc $(LIB_DIR)/pkgconfig/
	@echo Installed to $(DESTDIR)$(PREFIX)
endif

endif  # WINDOWS_HOST
