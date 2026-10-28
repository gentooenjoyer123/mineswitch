DEVKITPRO ?= $(abspath ../../..)/.toolchain/opt/devkitpro
export DEVKITPRO
APP_TITLE := MineSwitch SafeFetch
APP_AUTHOR := MineSwitch
APP_VERSION := 0.1.0
include $(DEVKITPRO)/libnx/switch_rules

TARGET := mineswitch-safefetch-probe
BUILD := build
ARCH := -march=armv8-a -mtune=cortex-a57 -mtp=soft -fPIE
CFLAGS := $(ARCH) -O2 -g -std=gnu11 -Wall -Wextra -Werror \
  -D__SWITCH__ -I.. -I$(LIBNX)/include -ffunction-sections
ASFLAGS := $(ARCH) -g
LD := $(CC)
LDFLAGS := $(ARCH) -specs=$(LIBNX)/switch.specs -g -Wl,-Map,$(BUILD)/probe.map
LIBPATHS := -L$(LIBNX)/lib
LIBS := -lnx
OFILES := $(BUILD)/main.o $(BUILD)/nx_safefetch.o $(BUILD)/frame_layout.o
NROFLAGS := --nacp=$(TARGET).nacp --icon=$(APP_ICON)

.PHONY: all clean
all: $(TARGET).nro
$(TARGET).nro: $(TARGET).elf $(TARGET).nacp
$(TARGET).elf: $(OFILES)
$(BUILD):
	mkdir -p $@
$(BUILD)/main.o: main.c ../nx_safefetch.h | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@
$(BUILD)/frame_layout.o: ../frame_layout.c | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@
$(BUILD)/nx_safefetch.o: ../nx_safefetch.S | $(BUILD)
	$(CC) $(ASFLAGS) -c $< -o $@
clean:
	rm -rf $(BUILD) $(TARGET).elf $(TARGET).nro $(TARGET).nacp $(TARGET).lst
