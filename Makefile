# -*- Mode: makefile-gmake -*-

.PHONY: clean all debug release coverage pkgconfig install install-dev

#
# Required packages
#

PKGS = nfcd-plugin libncicore libglibutil gobject-2.0 glib-2.0

#
# Default target
#

all: debug release pkgconfig

#
# Library name
#

NAME = nciplugin
LIB_NAME = lib$(NAME)
LIB_DEV_SYMLINK = $(LIB_NAME).so
LIB_SYMLINK1 = $(LIB_DEV_SYMLINK).$(VERSION_MAJOR)
LIB_SYMLINK2 = $(LIB_SYMLINK1).$(VERSION_MINOR)
LIB_SONAME = $(LIB_SYMLINK1)
LIB = $(LIB_SONAME).$(VERSION_MINOR).$(VERSION_RELEASE)
STATIC_LIB = $(LIB_NAME).a

#
# Library version
#

VERSION_MAJOR = 1
VERSION_MINOR = 0
VERSION_RELEASE = 11

# Version for pkg-config
PCVERSION = $(VERSION_MAJOR).$(VERSION_MINOR).$(VERSION_RELEASE)

#
# Sources
#

SRC = \
  nci_adapter.c \
  nci_initiator.c \
  nci_target.c

#
# Directories
#

SRC_DIR = src
INCLUDE_DIR = include
BUILD_DIR = build
DEBUG_BUILD_DIR = $(BUILD_DIR)/debug
RELEASE_BUILD_DIR = $(BUILD_DIR)/release
COVERAGE_BUILD_DIR = $(BUILD_DIR)/coverage

#
# Tools and flags
#

CC = $(CROSS_COMPILE)gcc
LD = $(CC)
WARNINGS = -Wall -Wstrict-aliasing -Wunused-result
INCLUDES = -I$(INCLUDE_DIR)
BASE_FLAGS = -fPIC
FULL_CFLAGS = $(BASE_FLAGS) $(CFLAGS) $(DEFINES) $(WARNINGS) $(INCLUDES) \
  -MMD -MP $(shell pkg-config --cflags $(PKGS))
FULL_LDFLAGS = $(BASE_FLAGS) $(LDFLAGS) -shared -Wl,-soname -Wl,$(LIB_SONAME)
LIBS = $(shell pkg-config --libs $(PKGS))
DEBUG_FLAGS = -g
RELEASE_FLAGS =
COVERAGE_FLAGS = -g

KEEP_SYMBOLS ?= 0
ifneq ($(KEEP_SYMBOLS),0)
RELEASE_FLAGS += -g
endif

DEBUG_LDFLAGS = $(FULL_LDFLAGS) $(DEBUG_FLAGS)
RELEASE_LDFLAGS = $(FULL_LDFLAGS) $(RELEASE_FLAGS)
DEBUG_CFLAGS = $(FULL_CFLAGS) $(DEBUG_FLAGS) -DDEBUG
RELEASE_CFLAGS = $(FULL_CFLAGS) $(RELEASE_FLAGS) -O2
COVERAGE_CFLAGS = $(FULL_CFLAGS) $(COVERAGE_FLAGS) --coverage

#
# Files
#

PKGCONFIG = $(BUILD_DIR)/$(LIB_NAME).pc
DEBUG_OBJS = $(SRC:%.c=$(DEBUG_BUILD_DIR)/%.o)
RELEASE_OBJS = $(SRC:%.c=$(RELEASE_BUILD_DIR)/%.o)
COVERAGE_OBJS = $(SRC:%.c=$(COVERAGE_BUILD_DIR)/%.o)

DEBUG_LIB = $(DEBUG_BUILD_DIR)/$(LIB)
RELEASE_LIB = $(RELEASE_BUILD_DIR)/$(LIB)
DEBUG_LINK = $(DEBUG_BUILD_DIR)/$(LIB_SYMLINK1)
RELEASE_LINK = $(RELEASE_BUILD_DIR)/$(LIB_SYMLINK1)
DEBUG_STATIC_LIB = $(DEBUG_BUILD_DIR)/$(STATIC_LIB)
RELEASE_STATIC_LIB = $(RELEASE_BUILD_DIR)/$(STATIC_LIB)
COVERAGE_STATIC_LIB = $(COVERAGE_BUILD_DIR)/$(STATIC_LIB)

#
# Dependencies
#

DEPS = $(DEBUG_OBJS:%.o=%.d) $(RELEASE_OBJS:%.o=%.d)
ifneq ($(MAKECMDGOALS),clean)
ifneq ($(strip $(DEPS)),)
-include $(DEPS)
endif
endif

$(PKGCONFIG): | $(BUILD_DIR)
$(DEBUG_OBJS): | $(DEBUG_BUILD_DIR)
$(RELEASE_OBJS): | $(RELEASE_BUILD_DIR)
$(COVERAGE_OBJS): | $(COVERAGE_BUILD_DIR)

#
# Rules
#

DEBUG_LIB = $(DEBUG_BUILD_DIR)/$(LIB)
RELEASE_LIB = $(RELEASE_BUILD_DIR)/$(LIB)
DEBUG_LINK = $(DEBUG_BUILD_DIR)/$(LIB_SONAME)
RELEASE_LINK = $(RELEASE_BUILD_DIR)/$(LIB_SONAME)

debug: $(DEBUG_STATIC_LIB) $(DEBUG_LIB) $(DEBUG_LINK)

release: $(RELEASE_STATIC_LIB) $(RELEASE_LIB) $(RELEASE_LINK)

coverage: $(COVERAGE_STATIC_LIB)

pkgconfig: $(PKGCONFIG)

clean:
	rm -f *~ $(SRC_DIR)/*~ $(INCLUDE_DIR)/*~ rpm/*~
	rm -fr $(BUILD_DIR) RPMS installroot
	rm -fr debian/tmp debian/lib$(NAME) debian/lib$(NAME)-dev
	rm -f documentation.list debian/files debian/*.substvars
	rm -f debian/*.debhelper.log debian/*.debhelper debian/*~
	rm -f debian/*.install

$(BUILD_DIR):
	mkdir -p $@

$(DEBUG_BUILD_DIR):
	mkdir -p $@

$(RELEASE_BUILD_DIR):
	mkdir -p $@

$(COVERAGE_BUILD_DIR):
	mkdir -p $@

$(DEBUG_BUILD_DIR)/%.o : $(SRC_DIR)/%.c
	$(CC) -c $(DEBUG_CFLAGS) -MT"$@" -MF"$(@:%.o=%.d)" $< -o $@

$(RELEASE_BUILD_DIR)/%.o : $(SRC_DIR)/%.c
	$(CC) -c $(RELEASE_CFLAGS) -MT"$@" -MF"$(@:%.o=%.d)" $< -o $@

$(COVERAGE_BUILD_DIR)/%.o : $(SRC_DIR)/%.c
	$(CC) -c $(COVERAGE_CFLAGS) -MT"$@" -MF"$(@:%.o=%.d)" $< -o $@

$(DEBUG_LIB): $(DEBUG_OBJS)
	$(LD) $(DEBUG_LDFLAGS) $^ -o $@ $(LIBS)

$(RELEASE_LIB): $(RELEASE_OBJS)
	$(LD) $(RELEASE_LDFLAGS) $^ -o $@ $(LIBS)
ifeq ($(KEEP_SYMBOLS),0)
	strip $@
endif

$(DEBUG_STATIC_LIB): $(DEBUG_OBJS)
	$(AR) rc $@ $?

$(RELEASE_STATIC_LIB): $(RELEASE_OBJS)
	$(AR) rc $@ $?

$(COVERAGE_STATIC_LIB): $(COVERAGE_OBJS)
	$(AR) rc $@ $?

$(DEBUG_BUILD_DIR)/$(LIB_SYMLINK1): $(DEBUG_BUILD_DIR)/$(LIB_SYMLINK2)
	ln -sf $(LIB_SYMLINK2) $@

$(RELEASE_BUILD_DIR)/$(LIB_SYMLINK1): $(RELEASE_BUILD_DIR)/$(LIB_SYMLINK2)
	ln -sf $(LIB_SYMLINK2) $@

$(DEBUG_BUILD_DIR)/$(LIB_SYMLINK2): $(DEBUG_LIB)
	ln -sf $(LIB) $@

$(RELEASE_BUILD_DIR)/$(LIB_SYMLINK2): $(RELEASE_LIB)
	ln -sf $(LIB) $@

#
# LIBDIR usually gets substituted with arch specific dir.
# It's relative in deb build and can be whatever in rpm build.
#

LIBDIR ?= usr/lib
ABS_LIBDIR := $(shell echo /$(LIBDIR) | sed -r 's|/+|/|g')

$(PKGCONFIG): $(LIB_NAME).pc.in Makefile
	sed -e 's|@version@|$(PCVERSION)|g' -e 's|@libdir@|$(ABS_LIBDIR)|g' $< > $@

debian/%.install: debian/%.install.in
	sed 's|@LIBDIR@|$(LIBDIR)|g' $< > $@

#
# Install
#

INSTALL = install
INSTALL_DIRS = $(INSTALL) -d
INSTALL_FILES = $(INSTALL) -m 644

INSTALL_LIB_DIR = $(DESTDIR)$(ABS_LIBDIR)
INSTALL_INCLUDE_DIR = $(DESTDIR)/usr/include/$(NAME)
INSTALL_PKGCONFIG_DIR = $(DESTDIR)$(ABS_LIBDIR)/pkgconfig

install: $(INSTALL_LIB_DIR)
	$(INSTALL) -m 755 $(RELEASE_LIB) $(INSTALL_LIB_DIR)
	ln -sf $(LIB) $(INSTALL_LIB_DIR)/$(LIB_SYMLINK2)
	ln -sf $(LIB_SYMLINK2) $(INSTALL_LIB_DIR)/$(LIB_SYMLINK1)

install-dev: install $(INSTALL_INCLUDE_DIR) $(INSTALL_PKGCONFIG_DIR)
	$(INSTALL_FILES) $(INCLUDE_DIR)/*.h $(INSTALL_INCLUDE_DIR)
	$(INSTALL_FILES) $(PKGCONFIG) $(INSTALL_PKGCONFIG_DIR)
	ln -sf $(LIB_SYMLINK1) $(INSTALL_LIB_DIR)/$(LIB_DEV_SYMLINK)

$(INSTALL_LIB_DIR):
	$(INSTALL_DIRS) $@

$(INSTALL_INCLUDE_DIR):
	$(INSTALL_DIRS) $@

$(INSTALL_PKGCONFIG_DIR):
	$(INSTALL_DIRS) $@
