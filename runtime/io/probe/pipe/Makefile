ROOT := $(abspath ../../../..)
DEVKITPRO ?= $(ROOT)/.toolchain/opt/devkitpro
DEVKITA64 ?= $(DEVKITPRO)/devkitA64
export DEVKITPRO DEVKITA64
JDK := $(ROOT)/.upstream/openjdk21
CC := $(DEVKITA64)/bin/aarch64-none-elf-gcc
CXX := $(DEVKITA64)/bin/aarch64-none-elf-g++
TOOLS := $(DEVKITPRO)/tools/bin
ARCH := -march=armv8-a -mtune=cortex-a57 -mtp=soft -fPIE -ftls-model=local-exec
INCLUDES := -isystem $(DEVKITPRO)/libnx/include -I$(ROOT)/runtime/io \
  -I$(JDK)/src/java.base/share/native/include -I$(JDK)/src/java.base/unix/native/include \
  -I$(JDK)/src/java.base/horizon/native/libnio/ch
FLAGS := $(ARCH) $(INCLUDES) -DHORIZON -D__SWITCH__ -D_LP64=1 -O2 -g \
  -ffunction-sections -fdata-sections -Wall -Wextra -Werror -Wno-unused-parameter
CXXFLAGS := $(FLAGS) -std=gnu++14 -fno-exceptions -fno-rtti
CFLAGS := $(FLAGS) -std=gnu11
TARGET := mineswitch-pipe-probe
OBJECTS := build/main.o build/horizon_pipe.o build/horizon_socket_io.o

.PHONY: all clean
all: $(TARGET).nro

build:
	mkdir -p $@

build/main.o: main.c $(ROOT)/runtime/io/horizon_pipe.h | build
	$(CC) $(CFLAGS) -c $< -o $@

build/horizon_pipe.o: $(ROOT)/runtime/io/horizon_pipe.c $(ROOT)/runtime/io/horizon_pipe.h | build
	$(CC) $(CFLAGS) -c $< -o $@

build/horizon_socket_io.o: $(JDK)/src/java.base/horizon/native/libnio/ch/horizon_socket_io.c | build
	$(CC) $(CFLAGS) -c $< -o $@

$(TARGET).elf: $(OBJECTS)
	$(CXX) $(ARCH) -specs=$(DEVKITPRO)/libnx/switch.specs -Wl,-Map,$(TARGET).map \
	  $(OBJECTS) -L$(DEVKITPRO)/libnx/lib -lnx -o $@

$(TARGET).nacp: Makefile
	$(TOOLS)/nacptool --create 'MineSwitch Selector Transport' 'MineSwitch' '0.1.0' $@

$(TARGET).nro: $(TARGET).elf $(TARGET).nacp
	$(TOOLS)/elf2nro $< $@ --nacp=$(TARGET).nacp \
	  --icon=$(DEVKITPRO)/libnx/default_icon.jpg

clean:
	rm -rf build $(TARGET).elf $(TARGET).map $(TARGET).nacp $(TARGET).nro
