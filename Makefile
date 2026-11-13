KVER ?= $(shell uname -r)
KDIR ?= /lib/modules/$(KVER)/build
BUILD := $(CURDIR)/build
CFLAGS_TEST ?= -O2 -Wall -Wextra

# MO= (kernel 6.13+) keeps kbuild's objects and .cmd files out of src/.
KBUILD_ARGS := -C $(KDIR) M=$(CURDIR)/src MO=$(BUILD)

all: module tests

module: | $(BUILD)
	$(MAKE) $(KBUILD_ARGS) modules

tests: $(BUILD)/uinput_kbd $(BUILD)/uhid_kbd

$(BUILD)/%: tests/%.c | $(BUILD)
	$(CC) $(CFLAGS_TEST) -o $@ $<

$(BUILD):
	mkdir -p $@

check: all
	sudo tests/check.sh

clean:
	rm -rf $(BUILD)

.PHONY: all module tests check clean
