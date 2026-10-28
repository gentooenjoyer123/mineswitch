ROOT := $(abspath ../../../..)
DEVKITPRO ?= $(ROOT)/.toolchain/opt/devkitpro
DEVKITA64 ?= $(DEVKITPRO)/devkitA64
export DEVKITPRO DEVKITA64
JDK := $(ROOT)/.upstream/openjdk21
NATIVE_CPU := $(JDK)/src/hotspot/os_cpu/horizon_aarch64
CC := $(DEVKITA64)/bin/aarch64-none-elf-gcc
CXX := $(DEVKITA64)/bin/aarch64-none-elf-g++
TOOLS := $(DEVKITPRO)/tools/bin
ARCH := -march=armv8-a -mtune=cortex-a57 -mtp=soft -fPIE -ftls-model=local-exec
INCLUDES := -isystem $(DEVKITPRO)/libnx/include -I$(ROOT)/runtime/memory \
  -I$(ROOT)/runtime/safefetch -I$(NATIVE_CPU) \
  -I$(JDK)/src/java.base/share/native/include -I$(JDK)/src/java.base/unix/native/include \
  -I$(JDK)/src/java.base/share/native/libjimage
FLAGS := $(ARCH) $(INCLUDES) -DHORIZON -D__SWITCH__ -D_LP64=1 -O2 -g \
  -DNXVM_HOTSPOT_AARCH64 \
  -ffunction-sections -fdata-sections -Wall -Wextra -Werror -Wno-unused-parameter
CXXFLAGS := $(FLAGS) -std=gnu++14 -fno-exceptions -fno-rtti
CFLAGS := $(FLAGS) -std=gnu11
TARGET := mineswitch-stack-guard-probe
OBJECTS := build/main.o build/nx_memory.o build/guard_store.o build/nx_safefetch.o \
  build/exception_entry.o build/exception_support.o

.PHONY: all clean
all: $(TARGET).nro

build:
	mkdir -p $@

build/main.o: main.c $(ROOT)/runtime/memory/nx_memory.h | build
	$(CC) $(CFLAGS) -c $< -o $@

build/nx_memory.o: $(ROOT)/runtime/memory/nx_memory.c $(ROOT)/runtime/memory/nx_memory.h | build
	$(CC) $(CFLAGS) -c $< -o $@

build/guard_store.o: guard_store.S | build
	$(CC) $(FLAGS) -c $< -o $@

build/nx_safefetch.o: $(ROOT)/runtime/safefetch/nx_safefetch.S | build
	$(CC) $(FLAGS) -c $< -o $@

build/exception_entry.o: $(NATIVE_CPU)/exception_horizon_aarch64.S | build
	$(CC) $(FLAGS) -c $< -o $@

build/exception_support.o: $(NATIVE_CPU)/horizon_aarch64_exception_support.c $(NATIVE_CPU)/horizon_aarch64_context.h | build
	$(CC) $(CFLAGS) -c $< -o $@

$(TARGET).elf: $(OBJECTS)
	$(CXX) $(ARCH) -specs=$(DEVKITPRO)/libnx/switch.specs -Wl,-Map,$(TARGET).map -Wl,-u,__libnx_exception_entry \
	  $(OBJECTS) -L$(DEVKITPRO)/libnx/lib -lnx -o $@

$(TARGET).nacp: Makefile
	$(TOOLS)/nacptool --create 'MineSwitch Stack Guard' 'MineSwitch' '0.1.0' $@

$(TARGET).nro: $(TARGET).elf $(TARGET).nacp
	$(TOOLS)/elf2nro $< $@ --nacp=$(TARGET).nacp \
	  --icon=$(DEVKITPRO)/libnx/default_icon.jpg

clean:
	rm -rf build $(TARGET).elf $(TARGET).map $(TARGET).nacp $(TARGET).nro
