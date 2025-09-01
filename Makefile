CC       ?= clang
BIN      ?= a.out
SRC      ?= $(firstword $(wildcard *.c))
CFLAGS   ?= -O2 -g -Wall -Wextra -std=gnu11
LDFLAGS  ?=

# Kernel build dir (for module builds)
UNAME_R  := $(shell uname -r)
# Prefer DKMS-provided KVER when present, otherwise fallback to running kernel
ifeq ($(origin KVER), undefined)
KDIR     ?= /lib/modules/$(UNAME_R)/build
else
KDIR     ?= /lib/modules/$(KVER)/build
endif
PWD      := $(shell pwd)

# If the running kernel was built with clang/lld, mirror that toolchain
LLVM_OPT := $(shell grep -q "^#define CONFIG_CC_IS_CLANG 1" $(KDIR)/include/generated/autoconf.h 2>/dev/null && echo LLVM=1)

# Allow building one or more modules: MODULE=foo or MODULES="foo bar"
# When building with `make modules MODULE=xyz`, the kernel will treat this
# Makefile as the Kbuild file because of M=$(PWD).
ifneq ($(strip $(MODULES)$(MODULE)),)
MODULES ?= $(MODULE)
export obj-m := $(patsubst %,%.o,$(MODULES))
endif

DEFAULT_MODULES := dchu_core dchu_hwmon dchu_leds

compile: $(BIN)

$(BIN): $(SRC)
	$(CC) $(CFLAGS) -o $@ $(SRC) $(LDFLAGS)

# ===== kernel module =====
# e.g. `make modules MODULE=foo` (expects foo.c in this dir)
modules:
	@test -n "$(obj-m)" || { echo "Set MODULE=<name> or MODULES=\"a b\""; exit 2; }
	$(MAKE) -C $(KDIR) M=$(PWD) $(LLVM_OPT) modules

modules_install:
	@test -n "$(obj-m)" || { echo "Set MODULE=<name> or MODULES=\"a b\""; exit 2; }
	$(MAKE) -C $(KDIR) M=$(PWD) $(LLVM_OPT) modules_install

all: modules_all

modules_all:
	$(MAKE) modules MODULES="$(DEFAULT_MODULES)"

load: modules
	@test -n "$(MODULE)" || { echo "need MODULE=<name>, e.g. make load MODULE=dchu_hwmon"; exit 1; }
	sudo modprobe -r $(MODULE) >/dev/null 2>&1 || true
	sudo insmod ./$(MODULE).ko $(PARAMS)
	dmesg | tail -n 30

unload:
	@test -n "$(MODULE)" || { echo "need MODULE=<name>, e.g. make unload MODULE=dchu_hwmon"; exit 1; }
	sudo modprobe -r $(MODULE) || sudo rmmod $(MODULE) || true

reload: unload load

load_all: modules_all
	sudo insmod ./dchu_core.ko
	sudo insmod ./dchu_hwmon.ko $(PARAMS)
	sudo insmod ./dchu_leds.ko

unload_all:
	sudo rmmod dchu_hwmon dchu_leds dchu_core 2>/dev/null || true

clean:
	$(RM) $(BIN) *.o *.ko *.mod *.mod.c *.symvers Module.symvers modules.order .*.cmd

.PHONY: compile modules modules_install modules_all all clean load unload reload load_all unload_all help
