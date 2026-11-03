# SPDX-License-Identifier: MIT
#
# Identity hook. Called from the root Makefile ($(MAKE) -C hook, $(MAKE) -C hook check).
#   all    libgeforce_sriov.so (the shipped LD_PRELOAD library)
#   check  builds the behavioural fixtures into build/ and runs them (no GPU, driver or root)
#   clean  removes both

CC ?= cc
CFLAGS ?= -O2
WARN := -std=c11 -D_GNU_SOURCE -Wall -Wextra -Werror
CPPFLAGS += -I../include -Itests
LIB_CFLAGS := -fPIC -fvisibility=hidden
LIB_LDFLAGS := -shared -Wl,-z,defs -Wl,-z,now -Wl,-z,relro
LIB_LDLIBS := -ldl -pthread

HEADERS := ../include/gs_hook.h ../include/gs_devices.h
FIXTURE := tests/fixture.h
BUILD := build

all: libgeforce_sriov.so

libgeforce_sriov.so: gs_hook.c $(HEADERS)
	$(CC) $(WARN) $(CPPFLAGS) $(CFLAGS) $(LIB_CFLAGS) $(LDFLAGS) $(LIB_LDFLAGS) \
		-o $@ gs_hook.c $(LIB_LDLIBS)

# The test build differs only by -DGS_HOOK_TEST_ENV (test path overrides, stderr logging).
$(BUILD)/libgeforce_sriov_test.so: gs_hook.c $(HEADERS) | $(BUILD)
	$(CC) $(WARN) $(CPPFLAGS) $(CFLAGS) -DGS_HOOK_TEST_ENV $(LIB_CFLAGS) $(LDFLAGS) \
		$(LIB_LDFLAGS) -o $@ gs_hook.c $(LIB_LDLIBS)

$(BUILD)/fake_driver.so: tests/fake_driver.c $(FIXTURE) $(HEADERS) | $(BUILD)
	$(CC) $(WARN) $(CPPFLAGS) $(CFLAGS) $(LIB_CFLAGS) $(LDFLAGS) $(LIB_LDFLAGS) \
		-o $@ tests/fake_driver.c -ldl

# Must be named nvidia-vgpud: the hook acts only in a process of that name.
$(BUILD)/nvidia-vgpud: tests/vgpud.c $(FIXTURE) $(HEADERS) | $(BUILD)
	$(CC) $(WARN) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ tests/vgpud.c -ldl -pthread

$(BUILD)/nvidia-vgpu-mgr: $(BUILD)/nvidia-vgpud
	cp $< $@

$(BUILD)/run_hook_tests: tests/run_hook_tests.c $(FIXTURE) $(HEADERS) | $(BUILD)
	$(CC) $(WARN) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ tests/run_hook_tests.c

$(BUILD):
	mkdir -p $@

check: libgeforce_sriov.so $(BUILD)/libgeforce_sriov_test.so $(BUILD)/fake_driver.so \
		$(BUILD)/nvidia-vgpud $(BUILD)/nvidia-vgpu-mgr $(BUILD)/run_hook_tests
	$(BUILD)/run_hook_tests "$(CURDIR)/$(BUILD)/libgeforce_sriov_test.so" \
		"$(CURDIR)/$(BUILD)/fake_driver.so" "$(CURDIR)/$(BUILD)/nvidia-vgpud" \
		"$(CURDIR)/$(BUILD)/nvidia-vgpu-mgr"

clean:
	rm -rf $(BUILD) libgeforce_sriov.so

.PHONY: all check clean
