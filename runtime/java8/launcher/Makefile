# Java 8 Horizon probe NRO (JVM + libjava/zip/verify/nio/net). GRAPHICS=1 builds
# the LWJGL variant (see runtime/java8/tests/prepare-lwjgl-8.py).
# Mirrors runtime/java17/launcher/Makefile; shared helper sources are compiled
# from runtime/launcher read-only, with the Java 8 SD runtime directory.
ROOT := $(abspath ../../..)
DEVKITPRO ?= $(ROOT)/.toolchain/opt/devkitpro
DEVKITA64 ?= $(DEVKITPRO)/devkitA64
export DEVKITPRO DEVKITA64
GENERATED ?= $(ROOT)/runtime/java8/generated-probe
REGISTRY ?= $(GENERATED)/native_registry.c
ROMFS ?= $(GENERATED)/romfs
BUILD ?= $(CURDIR)/build
OUTPUT ?= $(CURDIR)/mineswitch-java8-probe
SHARED := $(ROOT)/runtime/launcher
JDK8 := $(ROOT)/.upstream/openjdk8
APP_VERSION := 0.1.0
APP_TITLE := MineSwitch Java 8
APP_ICON := $(DEVKITPRO)/libnx/default_icon.jpg
include $(GENERATED)/native-archives.mk
CC := $(DEVKITA64)/bin/aarch64-none-elf-gcc
CXX := $(DEVKITA64)/bin/aarch64-none-elf-g++
TOOLS := $(DEVKITPRO)/tools/bin
PORTLIBS := $(DEVKITPRO)/portlibs/switch
ARCH := -march=armv8-a+crc+crypto -mtune=cortex-a57 -mtp=soft -fPIE -ftls-model=local-exec
INCLUDES := -I$(SHARED) -I$(DEVKITPRO)/libnx/include -I$(PORTLIBS)/include \
 -I$(JDK8)/jdk/src/share/javavm/export -I$(JDK8)/jdk/src/solaris/javavm/export \
 -I$(JDK8)/hotspot/src/os/horizon/vm -I$(ROOT)/runtime/io
CFLAGS := $(ARCH) $(INCLUDES) -D__SWITCH__ -D_GNU_SOURCE -std=gnu11 \
 -O2 -g -Wall -Wextra -Werror -ffunction-sections -fdata-sections -MMD -MP \
 -DMINESWITCH_DEFAULT_GAME=0 -DMINESWITCH_ZERO_VM=0 \
 -DRUNTIME_TARGET='"sdmc:/switch/mineswitch/runtime-8/"' \
 -DMINESWITCH_RUNTIME_HOME='"/switch/mineswitch/runtime-8"'
LDFLAGS := $(ARCH) -specs=$(DEVKITPRO)/libnx/switch.specs \
 -Wl,-Map,$(OUTPUT).map -Wl,--gc-sections -Wl,--no-undefined -Wl,--wrap=exit -Wl,--wrap=_exit \
 -Wl,-u,__libnx_exception_entry
JDKLIBS := $(MINESWITCH_JDK_ARCHIVES) $(MINESWITCH_EXTRA_ARCHIVES) $(MINESWITCH_SUPPORT_ARCHIVES)
ALLOCATOR := $(ROOT)/runtime/memory/newlib/build/libmineswitch_newlib.a
LDFLAGS += -Wl,-u,_malloc_r,-u,_realloc_r,-u,_memalign_r
SDKLIBS := $(PORTLIBS)/lib/libmbedcrypto.a -L$(DEVKITPRO)/libnx/lib -lnx
OBJECTS := $(BUILD)/main.o $(BUILD)/install.o $(BUILD)/config.o $(BUILD)/clock_diagnostic.o \
 $(BUILD)/log_tee.o $(BUILD)/native_registry.o
# GRAPHICS=1 links the LWJGL/GLFW/OpenGL(EGL)/STB/OpenAL natives of the Java 21
# port (JVM-independent JNI code, Java classes built with --release 8) through
# the same nouveau path as runtime/launcher, exactly like the Java 17 port.
ifeq ($(GRAPHICS),1)
OBJECTS += $(BUILD)/nouveau_topology.o
LDFLAGS += -Wl,--wrap=nouveau_getparam
SDKLIBS := $(ROOT)/.research/vm-probe-libffi/stage/lib/libffi.a $(SDKLIBS) \
 -L$(PORTLIBS)/lib -lSDL2 -lglapi -ldrm_nouveau
APP_TITLE := MineSwitch Java 8 LWJGL
endif
ifneq ($(filter ffi_closure_alloc,$(MINESWITCH_FORCE_SYMBOLS)),)
OBJECTS += $(ROOT)/runtime/lwjgl/build/lwjgl/horizon_ffi_closure.o
endif

.PHONY: all
all: $(OUTPUT).nro
$(BUILD):
	mkdir -p $@
$(BUILD)/main.o: main.c Makefile | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@
$(BUILD)/%.o: $(SHARED)/%.c Makefile | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@
$(BUILD)/native_registry.o: $(REGISTRY) | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@
$(BUILD)/nouveau_topology.o: $(ROOT)/runtime/graphics/nouveau_topology.c $(ROOT)/runtime/graphics/nouveau_topology.h Makefile | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@
$(OUTPUT).elf: $(OBJECTS) $(JDKLIBS) $(ALLOCATOR)
	$(CXX) $(LDFLAGS) $(OBJECTS) $(ALLOCATOR) -Wl,--start-group $(JDKLIBS) $(SDKLIBS) -Wl,--end-group -o $@
$(OUTPUT).nacp: Makefile
	$(TOOLS)/nacptool --create "$(APP_TITLE)" "MineSwitch" "$(APP_VERSION)" $@
$(OUTPUT).nro: $(OUTPUT).elf $(OUTPUT).nacp $(APP_ICON) $(shell find $(ROMFS) -type f)
	$(TOOLS)/elf2nro $(OUTPUT).elf $@ --nacp=$(OUTPUT).nacp --icon=$(APP_ICON) --romfsdir=$(ROMFS)
-include $(OBJECTS:.o=.d)
