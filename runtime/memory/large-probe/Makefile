ROOT := $(abspath ../../..)
DEVKITPRO ?= $(ROOT)/.toolchain/opt/devkitpro
DEVKITA64 ?= $(DEVKITPRO)/devkitA64
export DEVKITPRO DEVKITA64
CC := $(DEVKITA64)/bin/aarch64-none-elf-gcc
TOOLS := $(DEVKITPRO)/tools/bin
BUILD := build
TARGET := mineswitch-large-heap-probe
ALLOCATOR ?= $(ROOT)/runtime/memory/newlib/build/libmineswitch_newlib.a
ARCH := -march=armv8-a+crc+crypto -mtune=cortex-a57 -mtp=soft -fPIE
CFLAGS := $(ARCH) -D__SWITCH__ -std=gnu11 -O2 -g -fno-builtin -Wall -Wextra -Werror -MMD -MP \
 -ffunction-sections -fdata-sections -I$(ROOT)/runtime/memory -I$(DEVKITPRO)/libnx/include
LDFLAGS := $(ARCH) -specs=$(DEVKITPRO)/libnx/switch.specs -Wl,--gc-sections \
 -Wl,-Map,$(TARGET).map -Wl,-u,_malloc_r,-u,_realloc_r,-u,_memalign_r
OBJECTS := $(BUILD)/main.o $(BUILD)/nx_memory.o
.PHONY: all
all: $(TARGET).nro
$(BUILD):
	mkdir -p $@
$(BUILD)/main.o: main.c Makefile | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@
$(BUILD)/nx_memory.o: ../nx_memory.c Makefile | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@
$(TARGET).elf: $(OBJECTS) $(ALLOCATOR)
	$(CC) $(LDFLAGS) $(OBJECTS) $(ALLOCATOR) -L$(DEVKITPRO)/libnx/lib -lnx -o $@
$(TARGET).nacp: Makefile
	$(TOOLS)/nacptool --create "MineSwitch large heap check" "MineSwitch" "0.1.0" $@
$(TARGET).nro: $(TARGET).elf $(TARGET).nacp
	$(TOOLS)/elf2nro $< $@ --nacp=$(TARGET).nacp --icon=$(DEVKITPRO)/libnx/default_icon.jpg
-include $(OBJECTS:.o=.d)
