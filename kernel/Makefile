# SPDX-License-Identifier: GPL-2.0-only OR MIT
# Out-of-tree build of geforce_sriov; the object list is in Kbuild.
# KVER selects an installed kernel, KDIR any prepared kernel build tree.
KVER ?= $(shell uname -r)
KDIR ?= /lib/modules/$(KVER)/build

# A clang-built kernel needs its modules built with LLVM too.
LLVM_ARG := $(if $(shell grep -qsx 'CONFIG_CC_IS_CLANG=y' "$(KDIR)/.config" && echo y),LLVM=1)

.PHONY: all modules clean
all: modules
modules clean:
	$(MAKE) -C "$(KDIR)" M="$(CURDIR)" $(LLVM_ARG) $@
