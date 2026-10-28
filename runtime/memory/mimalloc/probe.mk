ROOT := $(abspath ../../..)
SDK := $(ROOT)/.toolchain/opt/devkitpro
export DEVKITPRO := $(SDK)
export DEVKITA64 := $(SDK)/devkitA64
CC := $(SDK)/devkitA64/bin/aarch64-none-elf-gcc
TOOLS := $(SDK)/tools/bin
BUILD := $(CURDIR)/build-probe
OUTPUT := $(CURDIR)/mineswitch-mimalloc-probe
PROBE_SOURCE ?= probe.c
ARCH := -march=armv8-a+crc+crypto -mtune=cortex-a57 -mtp=soft -fPIE -ftls-model=local-exec
CFLAGS := $(ARCH) -D__SWITCH__ -D_GNU_SOURCE -std=gnu11 -O2 -g -fno-builtin -Wall -Wextra -Werror -ffunction-sections -fdata-sections -MMD -MP -I$(SDK)/libnx/include -I$(ROOT)/.upstream/mimalloc/include -I$(CURDIR)
LIB := $(CURDIR)/build/libmineswitch_mimalloc.a
OBJECTS := $(BUILD)/probe.o $(BUILD)/nx_memory.o $(BUILD)/nx_jit.o
all: $(OUTPUT).nro
$(BUILD):
	mkdir -p $@
$(BUILD)/probe.o: $(PROBE_SOURCE) probe.mk | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@
$(BUILD)/nx_memory.o: ../nx_memory.c | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@
$(BUILD)/nx_jit.o: ../nx_jit.c | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@
$(OUTPUT).elf: $(OBJECTS) $(LIB)
	$(CC) $(ARCH) -specs=$(SDK)/libnx/switch.specs -Wl,--gc-sections -Wl,-Map,$(OUTPUT).map -Wl,-u,_malloc_r,-u,_free_r,-u,_calloc_r,-u,_realloc_r,-u,_memalign_r,-u,_malloc_usable_size_r,-u,_mallinfo_r,-u,_sbrk_r $(OBJECTS) $(LIB) -L$(SDK)/libnx/lib -lnx -o $@
$(OUTPUT).nacp: probe.mk
	$(TOOLS)/nacptool --create 'MineSwitch mimalloc test' MineSwitch 0.1.0 $@
$(OUTPUT).nro: $(OUTPUT).elf $(OUTPUT).nacp
	$(TOOLS)/elf2nro $< $@ --nacp=$(OUTPUT).nacp --icon=$(SDK)/libnx/default_icon.jpg
-include $(OBJECTS:.o=.d)
