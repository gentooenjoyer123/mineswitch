ROOT := $(abspath ../../..)
DEVKITPRO ?= $(ROOT)/.toolchain/opt/devkitpro
DEVKITA64 ?= $(DEVKITPRO)/devkitA64
export DEVKITPRO DEVKITA64
JDK := $(ROOT)/.upstream/openjdk21
CC := $(DEVKITA64)/bin/aarch64-none-elf-gcc
CXX := $(DEVKITA64)/bin/aarch64-none-elf-g++
TOOLS := $(DEVKITPRO)/tools/bin
ARCH := -march=armv8-a -mtune=cortex-a57 -mtp=soft -fPIE -ftls-model=local-exec
INCLUDES := -isystem $(DEVKITPRO)/libnx/include -I$(ROOT)/runtime/memory \
  -I$(JDK)/src/java.base/share/native/include -I$(JDK)/src/java.base/unix/native/include \
  -I$(JDK)/src/java.base/share/native/libjimage
FLAGS := $(ARCH) $(INCLUDES) -DHORIZON -D__SWITCH__ -D_LP64=1 -O2 -g \
  -ffunction-sections -fdata-sections -Wall -Wextra -Werror -Wno-unused-parameter
CXXFLAGS := $(FLAGS) -std=gnu++14 -fno-exceptions -fno-rtti
CFLAGS := $(FLAGS) -std=gnu11
TARGET := mineswitch-jimage-io-probe
OBJECTS := build/main.o build/osSupport_horizon.o build/nx_memory.o

.PHONY: all clean
all: $(TARGET).nro

build:
	mkdir -p $@

build/main.o: main.cpp $(ROOT)/runtime/memory/nx_memory.h | build
	$(CXX) $(CXXFLAGS) -c $< -o $@

build/osSupport_horizon.o: $(JDK)/src/java.base/horizon/native/libjimage/osSupport_horizon.cpp | build
	$(CXX) $(CXXFLAGS) -c $< -o $@

build/nx_memory.o: $(ROOT)/runtime/memory/nx_memory.c $(ROOT)/runtime/memory/nx_memory.h | build
	$(CC) $(CFLAGS) -c $< -o $@

$(TARGET).elf: $(OBJECTS)
	$(CXX) $(ARCH) -specs=$(DEVKITPRO)/libnx/switch.specs -Wl,-Map,$(TARGET).map \
	  $(OBJECTS) -L$(DEVKITPRO)/libnx/lib -lnx -o $@

$(TARGET).nacp: Makefile
	$(TOOLS)/nacptool --create 'MineSwitch JImage IO' 'MineSwitch' '0.1.0' $@

$(TARGET).nro: $(TARGET).elf $(TARGET).nacp $(ROOT)/runtime/memory/probe/romfs/memory-fixture.bin
	$(TOOLS)/elf2nro $< $@ --nacp=$(TARGET).nacp \
	  --icon=$(DEVKITPRO)/libnx/default_icon.jpg --romfsdir=$(ROOT)/runtime/memory/probe/romfs

clean:
	rm -rf build $(TARGET).elf $(TARGET).map $(TARGET).nacp $(TARGET).nro
