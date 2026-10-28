ROOT := $(abspath ../..)
DEVKITPRO ?= $(ROOT)/.toolchain/opt/devkitpro
DEVKITA64 ?= $(DEVKITPRO)/devkitA64
export DEVKITPRO DEVKITA64
CC := $(DEVKITA64)/bin/aarch64-none-elf-gcc
TOOLS := $(DEVKITPRO)/tools/bin
PORTLIBS := $(DEVKITPRO)/portlibs/switch
BUILD := $(CURDIR)/build
OUTPUT := $(CURDIR)/MineSwitch-Installer
ROMFS := $(CURDIR)/generated/romfs
ARCH := -march=armv8-a+crc+crypto -mtune=cortex-a57 -mtp=soft -fPIE -ftls-model=local-exec
CFLAGS := $(ARCH) -D__SWITCH__ -D_GNU_SOURCE -std=gnu11 -O2 -g \
 -Wall -Wextra -Werror -ffunction-sections -fdata-sections -MMD -MP \
 -I$(DEVKITPRO)/libnx/include -I$(PORTLIBS)/include -I$(ROOT)/runtime/launcher
LDFLAGS := $(ARCH) -specs=$(DEVKITPRO)/libnx/switch.specs -Wl,--gc-sections -Wl,-Map,$(OUTPUT).map
OBJECTS := $(BUILD)/main.o $(BUILD)/install.o

.PHONY: all check-compile
all: $(OUTPUT).nro
check-compile: $(OUTPUT).elf
$(BUILD):
	mkdir -p $@
$(BUILD)/main.o: main.c Makefile | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@
$(BUILD)/install.o: $(ROOT)/runtime/launcher/install.c Makefile | $(BUILD)
	$(CC) $(CFLAGS) -DRUNTIME_SOURCE='"romfs:/package/"' \
	 -DRUNTIME_TARGET='"sdmc:/switch/mineswitch/"' -c $< -o $@
$(OUTPUT).elf: $(OBJECTS)
	$(CC) $(LDFLAGS) $(OBJECTS) -L$(PORTLIBS)/lib -lmbedcrypto \
	 -L$(DEVKITPRO)/libnx/lib -lnx -o $@
$(OUTPUT).nacp: Makefile
	$(TOOLS)/nacptool --create "Install MineSwitch" "MineSwitch" "0.1.0" $@
$(OUTPUT).nro: $(OUTPUT).elf $(OUTPUT).nacp $(ROMFS)/package/files.manifest $(ROMFS)/package/MineSwitch.nro
	$(TOOLS)/elf2nro $(OUTPUT).elf $@ --nacp=$(OUTPUT).nacp \
	 --icon=$(DEVKITPRO)/libnx/default_icon.jpg --romfsdir=$(ROMFS)
-include $(OBJECTS:.o=.d)
