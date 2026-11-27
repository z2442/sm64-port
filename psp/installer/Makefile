# Standalone build of the on-device installer EBOOT. It does not touch the main Makefile, so the normal build keeps working as always.
#
#   make -C psp/installer VERSION=us        -> build/us_psp/installer/EBOOT.PBP     (VERSION: us, jp, eu or sh)
#   make -C psp/installer host-test         -> builds the PC test driver (build/host_test)
#
# The result is only the installer + loader. tools/psp/make_release.py --pack puts it together with SM64.template.prx and manifest.bin.
VERSION ?= us
TOP := $(abspath $(dir $(lastword $(MAKEFILE_LIST)))/../..)
OUT := $(TOP)/build/$(VERSION)_psp/installer

PSPSDK_PREFIX := $(shell psp-config -p)
PSP_PREFIX := $(shell psp-config -P)

ifneq ($(filter jp sh,$(VERSION)),)
  TITLE     := "スーパーマリオ64"
  EBOOT_DIR := $(TOP)/psp/JP_EBOOT
else
  TITLE     := "Super Mario 64™"
  EBOOT_DIR := $(TOP)/psp/EBOOT
endif

# The port's version in PARAM.SFO (XMB: Triangle > Information), the same as the game's EBOOT: it is read from PORT_VERSION in the main
# Makefile (`make release ... PORT_VERSION=x.y.z` passes its own down), and turned into APP_VER / DISC_VERSION the same way.
PORT_VERSION ?= $(shell sed -n 's/^PORT_VERSION ?= *//p' $(TOP)/Makefile)
PORT_VERSION_XYZ := $(shell echo '$(PORT_VERSION)' | sed 's/[-+].*//')
SFO_VERSION = $(shell echo '$(PORT_VERSION_XYZ)' | awk -F. '{m=$$2>9?9:$$2;p=$$3>9?9:$$3;printf "$(1)",$$1,m,p}')
SFOFLAGS := -s APP_VER=$(call SFO_VERSION,%02d.%d%d) -s DISC_VERSION=$(call SFO_VERSION,%d.%d%d)

SRC := $(TOP)/psp/installer
CFLAGS := -O2 -G0 -Wall -DSMI_PSP_IO -D_PSP_FW_VERSION=500 -I$(SRC) -I$(PSPSDK_PREFIX)/include -I$(PSP_PREFIX)/include

all: $(OUT)/EBOOT.PBP

$(OUT)/%.o: $(SRC)/%.c $(SRC)/smi_core.h $(SRC)/installer_text.h $(SRC)/text_ja.inc
	@mkdir -p $(OUT)
	psp-gcc $(CFLAGS) -c $< -o $@

$(OUT)/installer.elf: $(OUT)/installer.o $(OUT)/smi_core.o
	psp-gcc -o $@ $^ -L$(PSPSDK_PREFIX)/lib -L$(PSP_PREFIX)/lib -specs=$(PSPSDK_PREFIX)/lib/prxspecs -Wl,-q,-T$(PSPSDK_PREFIX)/lib/linkfile.prx $(PSPSDK_PREFIX)/lib/prxexports.o -lpspdebug -lpspdisplay -lpspge -lpsprtc -lpsputility -lpsppower -lpspctrl -lc -lpspuser
	psp-fixup-imports $@

$(OUT)/installer.prx: $(OUT)/installer.elf
	psp-prxgen $< $@

# (also remade when the main Makefile changes: that is where the version is)
$(OUT)/EBOOT.PBP: $(OUT)/installer.prx $(TOP)/Makefile
	mksfoex -d MEMSIZE=1 $(SFOFLAGS) $(TITLE) $(OUT)/PARAM.SFO
	python3 $(TOP)/tools/psp/sort_sfo.py $(OUT)/PARAM.SFO # keys in alphabetical order, as in Sony's (see the script)
	pack-pbp $@ $(OUT)/PARAM.SFO $(EBOOT_DIR)/ICON0.png NULL NULL $(EBOOT_DIR)/PIC0.png $(EBOOT_DIR)/SND0.at3 $< NULL

host-test: $(TOP)/build/host_test

$(TOP)/build/host_test: $(SRC)/host_test.c $(SRC)/smi_core.c $(SRC)/smi_core.h
	@mkdir -p $(TOP)/build
	$(CC) -O2 -Wall -Wextra -I$(SRC) -o $@ $(SRC)/host_test.c $(SRC)/smi_core.c

clean:
	rm -rf $(OUT)

.PHONY: all clean host-test
