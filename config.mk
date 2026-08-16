# Settings and rules shared by every Makefile in this tree. Each one sets TOP to
# its path back to c/, then includes this file.

TOP ?= .

.DEFAULT_GOAL := all

# ---------------------------------------------------------------------------
# Host portability
# ---------------------------------------------------------------------------
# cmd builtins under Windows mingw32-make, POSIX otherwise
ifneq (,$(findstring mingw32-make,$(notdir $(MAKE))))
    WINDOWS_HOST := 1
    SHELL   := cmd
    mkdir_p  = if not exist "$(subst /,\,$(1))" mkdir "$(subst /,\,$(1))"
    rm_rf    = if exist "$(subst /,\,$(1))" rmdir /s /q "$(subst /,\,$(1))"
    rm_f     = if exist "$(subst /,\,$(1))" del /q "$(subst /,\,$(1))"
else
    mkdir_p  = mkdir -p $(1)
    rm_rf    = rm -rf $(1)
    rm_f     = rm -f $(1)
endif

# a path as spelled in the tree, without a sub-Makefile's leading ../
tree_path = $(patsubst $(TOP)/%,%,$(1))

# ---------------------------------------------------------------------------
# Compiler flags
# ---------------------------------------------------------------------------
# optimization level, and DEBUG=1 for debug symbols
OPT   ?= -O2
DEBUG ?= 0
ifeq ($(DEBUG),1)
    OPT += -g3
endif

# public headers and the private src/ tree; a Makefile adds its own directories
INCLUDEPATHS += include src
CFLAGS  += -std=gnu11 -Wall -Wextra $(OPT) -pthread
LDFLAGS += -pthread

# ---------------------------------------------------------------------------
# Build target and toolchain
# ---------------------------------------------------------------------------
ifeq ($(OS),Windows_NT)
    # i686 and x86_64 mingw produce identically named objects, so the stamp below
    # tells them apart too: windows-i686 / windows-x86_64
    BUILD_TARGET = windows$(if $(CROSS_COMPILE),-$(firstword $(subst -, ,$(CROSS_COMPILE))))
    EXT      = .exe
    CFLAGS  += -D_WIN32_WINNT=0x0601
    # -static folds in libwinpthread/libgcc, so no runtime DLLs to ship
    LDFLAGS += -static
    LIBS    += ws2_32
    # native gcc on a Windows host, mingw cross-prefix from Linux
    ifndef WINDOWS_HOST
        CROSS_COMPILE ?= i686-w64-mingw32-
    endif
else
    # An explicit prefix cross-builds, the same way the mingw one does; the arch it
    # names joins the stamp below, so switching back and forth cleans build/ itself.
    # CROSS_COMPILE=s390x-linux-gnu- is what the big-endian gate uses.
    CROSS_COMPILE ?=
    EXT      =
    UNAME_S := $(shell uname -s)
    ifeq ($(UNAME_S),Darwin)
        # macOS builds and links (Apple clang behind the `gcc` name); the test
        # scripts, which drive the Linux vhci-hcd client, still need Linux.
        BUILD_TARGET = macos
    else
        BUILD_TARGET = linux$(if $(CROSS_COMPILE),-$(firstword $(subst -, ,$(CROSS_COMPILE))))
    endif
endif

CC = $(CROSS_COMPILE)gcc
AR = $(CROSS_COMPILE)ar
RC = $(CROSS_COMPILE)windres

# Handing a descriptor struct to add_descriptor() as opaque bytes is the whole
# authoring idiom, and on a big-endian build GCC warns at every one of those calls
# that the const void* drops the struct's little-endian storage order (USB_PACKED,
# see include/usbip.h). Dropping it is the intent - the callee only copies bytes.
#
# -Wno-scalar-storage-order is GCC-only, and clang answers an unknown -Wno-... with
# a warning of its own on every file, so ask the compiler first. -Werror turns that
# answer into the non-empty output this tests for. A Windows host has no shell to
# probe with, and builds with mingw gcc, which has the flag.
SSO_FLAG := -Wno-scalar-storage-order
ifdef WINDOWS_HOST
    CFLAGS += $(SSO_FLAG)
else
    ifeq ($(shell $(CC) -Werror $(SSO_FLAG) -x c -c /dev/null -o /dev/null 2>&1),)
        CFLAGS += $(SSO_FLAG)
    endif
endif

# runs scripts/gen_exports.py
ifdef WINDOWS_HOST
    PYTHON ?= python
else
    PYTHON ?= python3
endif

# ---------------------------------------------------------------------------
# Build directory
# ---------------------------------------------------------------------------
# Each Makefile builds into build/ beside itself: src/build, examples/build,
# wrapper/build, and build/ here for the packaging output.
#
# One build/ per directory serves every target. Object files and libraries are
# named the same whatever the target, so a switch wipes the directory first.
# Override BUILD_DIR to keep two.
BUILD_DIR ?= build

ifeq ($(TOP),.)
    OBJ_DIR := $(BUILD_DIR)
else
    OBJ_DIR := $(TOP)/$(notdir $(CURDIR))/$(BUILD_DIR)
endif

$(shell $(call mkdir_p,$(OBJ_DIR)))

# Which target a build/ holds changes only when cross-building, so a Windows
# host - which builds for Windows and nothing else - keeps no stamp. Elsewhere
# the target is the stamp's name rather than its contents: make has been able to
# write a file since 4.0, but cannot read one before 4.2. A suffixless `.target`
# is the older spelling, and counts as stale so one build converts a tree.
ifndef WINDOWS_HOST

TARGET_STAMP := $(OBJ_DIR)/.target-$(BUILD_TARGET)
STALE_STAMPS := $(filter-out $(TARGET_STAMP),\
                    $(wildcard $(OBJ_DIR)/.target $(OBJ_DIR)/.target-*))

ifneq ($(STALE_STAMPS),)
    LAST_TARGET := $(patsubst .target-%,%,$(notdir $(firstword $(STALE_STAMPS))))
    $(info Build target changed ($(LAST_TARGET) -> $(BUILD_TARGET)); \
           cleaning $(call tree_path,$(OBJ_DIR)))
    $(shell $(call rm_rf,$(OBJ_DIR)))
    $(shell $(call mkdir_p,$(OBJ_DIR)))
endif

# idempotent re-stamp, recursive sub-makes included
$(file > $(TARGET_STAMP))

endif  # WINDOWS_HOST

# Release version, single-sourced from the public header. grep and findstr are
# both real executables, so make runs them without a shell - which is what a
# Windows host has not got, along with sed. The marker glues the value to its
# name to make it one word; splitting on the closing quote then also drops the
# CR that findstr leaves behind.
ifdef WINDOWS_HOST
    version_lines := $(shell findstr USBIP_VERSION $(subst /,\,$(TOP)/include/usbip.h))
else
    version_lines := $(shell grep USBIP_VERSION $(TOP)/include/usbip.h)
endif
version_word  := $(firstword $(filter VERSION=%,$(subst USBIP_VERSION ",VERSION=,$(version_lines))))
USBIP_VERSION := $(firstword $(subst ", ,$(patsubst VERSION=%,%,$(version_word))))

# ---------------------------------------------------------------------------
# Shared libraries: the only place that knows the platforms apart
# ---------------------------------------------------------------------------
#   shlib           the file that gets linked   libfoo.so.0 / libfoo.0.dylib / foo.dll
#   shlib_link      the file beside it          libfoo.so   / libfoo.dylib   / foo.dll.a
#   shlib_link_cmd  the command that makes it
#   shlib_ldflags   $(1) = library base name, $(2) = its export-list file
ifeq ($(OS),Windows_NT)
    shlib      = $(1).dll
    shlib_link = $(1).dll.a
    SHLIB_MODE = -shared
    EXPORT_FMT = def
    # PE is already relocatable
    PICFLAG    =
    # without a .def mingw exports every symbol; --out-implib makes shlib_link
    shlib_ldflags  = $(2) -Wl,--out-implib,$(OBJ_DIR)/$(call shlib_link,$(1)) \
                     -static $(addprefix -l,$(LIBS))
    shlib_link_cmd = echo [ IMP ] $(call tree_path,$(OBJ_DIR)/$(call shlib_link,$(1)))
else ifeq ($(UNAME_S),Darwin)
    shlib      = $(1).0.dylib
    shlib_link = $(1).dylib
    SHLIB_MODE = -dynamiclib
    EXPORT_FMT = sym
    PICFLAG    = -fPIC
    # -exported_symbols_list is Apple ld's version script. No --no-undefined twin:
    # erroring on an unresolved symbol is already the default for a dylib, and
    # spelling it out (-undefined error) is deprecated in ld-prime, which says so
    # once per link.
    shlib_ldflags  = -install_name @rpath/$(call shlib,$(1)) \
                     -Wl,-exported_symbols_list,$(2)
    shlib_link_cmd = ln -sf $(call shlib,$(1)) $(OBJ_DIR)/$(call shlib_link,$(1))
else
    shlib      = $(1).so.0
    shlib_link = $(1).so
    SHLIB_MODE = -shared
    EXPORT_FMT = map
    PICFLAG    = -fPIC
    # the version script exports the .syms list and hides the rest
    shlib_ldflags  = -Wl,-soname,$(call shlib,$(1)) \
                     -Wl,--version-script=$(2) -Wl,--no-undefined
    shlib_link_cmd = ln -sf $(call shlib,$(1)) $(OBJ_DIR)/$(call shlib_link,$(1))
endif

# ---------------------------------------------------------------------------
# Windows VERSIONINFO. Static archives get none: the linker would drop it.
# ---------------------------------------------------------------------------
# Only the cross-build carries a resource: the released artifacts are the
# cross-built ones, and skipping the step leaves a build on a Windows host as a
# plain compile and link. WINRES says which build this is; the Makefiles that
# name the per-artifact strings test it too.
winres_obj =
ifeq ($(OS),Windows_NT)
    ifndef WINDOWS_HOST
        WINRES     := 1
        winres_obj  = $(OBJ_DIR)/res/$(1).o
    endif
endif

ifdef WINRES

# Every .dll and .exe links one $(call winres_obj,<name>). What differs per
# artifact is set on that object as a target variable, and reaches windres in a
# generated .rc that includes the shared scripts/usbip.rc - never as a -D on the
# command line, where windres backslash-escapes spaces for a POSIX shell to undo
# and cmd is left holding the backslashes.
RC_FILETYPE    = VFT_APP
RC_FILE_NAME   = $*$(EXT)
RC_DESCRIPTION = usbip $*

V_PARTS := $(subst ., ,$(USBIP_VERSION))

ifeq ($(DEBUG),1)
    RC_DEBUG_FLAG := VS_FF_DEBUG
else
    RC_DEBUG_FLAG := 0
endif
# 0.x is pre-release; drops out by itself at 1.0.0
ifeq ($(word 1,$(V_PARTS)),0)
    RC_PRERELEASE_FLAG := VS_FF_PRERELEASE
else
    RC_PRERELEASE_FLAG := 0
endif

define rc_source
/* Generated by config.mk. Edit scripts/usbip.rc, not this file. */
#define RC_VER_MAJOR $(word 1,$(V_PARTS))
#define RC_VER_MINOR $(word 2,$(V_PARTS))
#define RC_VER_MICRO $(word 3,$(V_PARTS))
#define RC_VER_NANO  0
#define RC_VERSION_STR "$(USBIP_VERSION)"
#define RC_DEBUG_FLAG $(RC_DEBUG_FLAG)
#define RC_PRERELEASE_FLAG $(RC_PRERELEASE_FLAG)
#define RC_FILETYPE $(RC_FILETYPE)
#define RC_FILE_NAME "$(RC_FILE_NAME)"
#define RC_INTERNAL_NAME "$*"
#define RC_DESCRIPTION "$(RC_DESCRIPTION)"
#include "usbip.rc"
endef

# the directory is a prerequisite, not a recipe line: make expands a whole
# recipe before running any of it, so $(file >) would write before a mkdir
$(OBJ_DIR)/res:
	@$(call mkdir_p,$@)

$(OBJ_DIR)/res/%.o: $(TOP)/scripts/usbip.rc | $(OBJ_DIR)/res
	@echo [ RC ] $(call tree_path,$@)
	$(file > $(@D)/$*.rc,$(rc_source))
	@$(RC) --include-dir $(TOP)/scripts $(@D)/$*.rc -o $@

endif  # WINRES

# ---------------------------------------------------------------------------
# Objects and the C compile rule
# ---------------------------------------------------------------------------
# tree-relative .c sources -> their object files
objects = $(addprefix $(OBJ_DIR)/,$(1:.c=.o))

# one object set for both the archive and the shared library; $(PICFLAG) is why
LIBOBJ_DIR  = $(OBJ_DIR)/libobj
lib_objects = $(addprefix $(LIBOBJ_DIR)/,$(1:.c=.o))

define compile_c
	@$(call mkdir_p,$(@D))
	@echo [ CC ] $(call tree_path,$<)
	@$(CC) $(CFLAGS) $(addprefix -I$(TOP)/,$(INCLUDEPATHS)) -MMD -MP -c -o $@ $<
endef

$(OBJ_DIR)/%.o: $(TOP)/%.c
	$(compile_c)

# every Makefile's clean target: its own build/ and nothing else
define clean_build_dir
	@echo Cleaning $(call tree_path,$(OBJ_DIR))
	@$(call rm_rf,$(OBJ_DIR))
endef
