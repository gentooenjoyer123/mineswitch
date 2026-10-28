ROOT := $(abspath ../../..)
DEVKITPRO ?= $(ROOT)/.toolchain/opt/devkitpro
export DEVKITPRO
APP_TITLE := MineSwitch Native Exceptions
APP_AUTHOR := MineSwitch
APP_VERSION := 0.1.0
include $(DEVKITPRO)/libnx/switch_rules
TARGET := mineswitch-native-exceptions-probe
BUILD := build
RUNTIME := $(ROOT)/.upstream/openjdk21/src/hotspot/os_cpu/horizon_aarch64
ARCH := -march=armv8-a -mtune=cortex-a57 -mtp=soft -ftls-model=local-exec -fPIE
CFLAGS := $(ARCH) -O2 -g -std=gnu11 -Wall -Wextra -Werror -D__SWITCH__ \
 -DNXVM_HOTSPOT_AARCH64 -I$(LIBNX)/include -I$(RUNTIME) -I../../safefetch -ffunction-sections
ASFLAGS := $(ARCH) -g -DNXVM_HOTSPOT_AARCH64
LD := $(CC)
LDFLAGS := $(ARCH) -specs=$(LIBNX)/switch.specs -Wl,-Map,$(BUILD)/probe.map -Wl,-u,__libnx_exception_entry
LIBPATHS := -L$(LIBNX)/lib
LIBS := -lnx
OFILES := $(BUILD)/main.o $(BUILD)/register_probe.o $(BUILD)/exception_entry.o \
 $(BUILD)/exception_support.o $(BUILD)/nx_safefetch.o
NROFLAGS := --nacp=$(TARGET).nacp --icon=$(APP_ICON)
.PHONY: all
all: $(TARGET).nro
$(TARGET).nro: $(TARGET).elf $(TARGET).nacp
$(TARGET).elf: $(OFILES)
$(BUILD):
	mkdir -p $@
$(BUILD)/main.o: main.c $(RUNTIME)/horizon_aarch64_context.h | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@
$(BUILD)/register_probe.o: register_probe.S | $(BUILD)
	$(CC) $(ASFLAGS) -c $< -o $@
$(BUILD)/exception_entry.o: $(RUNTIME)/exception_horizon_aarch64.S | $(BUILD)
	$(CC) $(ASFLAGS) -c $< -o $@
$(BUILD)/exception_support.o: $(RUNTIME)/horizon_aarch64_exception_support.c $(RUNTIME)/horizon_aarch64_context.h | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@
$(BUILD)/nx_safefetch.o: ../../safefetch/nx_safefetch.S | $(BUILD)
	$(CC) $(ASFLAGS) -c $< -o $@
