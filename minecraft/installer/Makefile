ROOT := $(abspath ../..)
DEVKITPRO ?= $(ROOT)/.toolchain/opt/devkitpro
DEVKITA64 ?= $(DEVKITPRO)/devkitA64
export DEVKITPRO DEVKITA64
CC := $(DEVKITA64)/bin/aarch64-none-elf-gcc
TOOLS := $(DEVKITPRO)/tools/bin
PORTLIBS := $(DEVKITPRO)/portlibs/switch
BUILD := $(CURDIR)/build
OUTPUT := $(CURDIR)/mineswitch-game-data
ROMFS := $(ROOT)/minecraft/generated/romfs
ARCH := -march=armv8-a+crc+crypto -mtune=cortex-a57 -mtp=soft -fPIE -ftls-model=local-exec
CFLAGS := $(ARCH) -D__SWITCH__ -D_GNU_SOURCE -std=gnu11 -O2 -g \
 -Wall -Wextra -Werror -ffunction-sections -fdata-sections -MMD -MP \
 -I$(DEVKITPRO)/libnx/include -I$(PORTLIBS)/include -I$(ROOT)/runtime/launcher
LDFLAGS := $(ARCH) -specs=$(DEVKITPRO)/libnx/switch.specs -Wl,--gc-sections -Wl,-Map,$(OUTPUT).map
OBJECTS := $(BUILD)/main.o $(BUILD)/install.o $(BUILD)/log_tee.o

.PHONY: all
all: $(OUTPUT).nro
$(BUILD):
	mkdir -p $@
$(BUILD)/main.o: main.c | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@
$(BUILD)/install.o: $(ROOT)/runtime/launcher/install.c | $(BUILD)
	$(CC) $(CFLAGS) -DINSTALL_LOG_EVERY=100 \
	 -DRUNTIME_SOURCE='"romfs:/minecraft/"' \
	 -DRUNTIME_TARGET='"sdmc:/switch/mineswitch/minecraft/"' -c $< -o $@
$(BUILD)/log_tee.o: $(ROOT)/runtime/launcher/log_tee.c | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@
$(OUTPUT).elf: $(OBJECTS)
	$(CC) $(LDFLAGS) $(OBJECTS) -L$(PORTLIBS)/lib -lmbedcrypto \
	 -L$(DEVKITPRO)/libnx/lib -lnx -o $@
$(OUTPUT).nacp: Makefile
	$(TOOLS)/nacptool --create "MineSwitch Game Data" "MineSwitch" "0.1.0" $@
$(OUTPUT).nro: $(OUTPUT).elf $(OUTPUT).nacp $(ROMFS)/minecraft/files.manifest
	$(TOOLS)/elf2nro $(OUTPUT).elf $@ --nacp=$(OUTPUT).nacp \
	 --icon=$(DEVKITPRO)/libnx/default_icon.jpg --romfsdir=$(ROMFS)
-include $(OBJECTS:.o=.d)
