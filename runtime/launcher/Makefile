ROOT := $(abspath ../..)
DEVKITPRO ?= $(ROOT)/.toolchain/opt/devkitpro
DEVKITA64 ?= $(DEVKITPRO)/devkitA64
REGISTRY ?= $(ROOT)/runtime/generated/native_registry.c
ROMFS ?= $(ROOT)/runtime/generated/romfs
BUILD ?= $(CURDIR)/build
OUTPUT ?= $(CURDIR)/mineswitch-jvm
APP_MODE ?= probe
GRAPHICS_BACKEND ?= nouveau
ALLOCATOR_BACKEND ?= newlib
APP_VERSION := 0.1.0
APP_ICON := $(DEVKITPRO)/libnx/default_icon.jpg
include $(dir $(REGISTRY))/native-archives.mk
MINESWITCH_JVM_VARIANT ?= zero
JDK_BUILD ?= $(if $(MINESWITCH_JDK_BUILD),$(MINESWITCH_JDK_BUILD),$(ROOT)/.upstream/openjdk21/build/horizon-aarch64-zero-release)
CC := $(DEVKITA64)/bin/aarch64-none-elf-gcc
CXX := $(DEVKITA64)/bin/aarch64-none-elf-g++
TOOLS := $(DEVKITPRO)/tools/bin
PORTLIBS := $(DEVKITPRO)/portlibs/switch
ARCH := -march=armv8-a+crc+crypto -mtune=cortex-a57 -mtp=soft -fPIE -ftls-model=local-exec
INCLUDES := -I$(DEVKITPRO)/libnx/include -I$(PORTLIBS)/include \
 -I$(JDK_BUILD)/support/modules_include/java.base \
 -I$(JDK_BUILD)/support/modules_include/java.base/horizon \
 -I$(ROOT)/.upstream/openjdk21/src/hotspot/os/horizon
CFLAGS := $(ARCH) $(INCLUDES) -D__SWITCH__ -D_GNU_SOURCE -std=gnu11 \
 -O2 -g -Wall -Wextra -Werror -ffunction-sections -fdata-sections -MMD -MP
ifeq ($(APP_MODE),minecraft)
CFLAGS += -DMINESWITCH_DEFAULT_GAME=1
APP_TITLE := MineSwitch 1.21.1
else ifeq ($(APP_MODE),probe)
CFLAGS += -DMINESWITCH_DEFAULT_GAME=0
APP_TITLE := MineSwitch Java 21
else ifeq ($(APP_MODE),launcher)
CFLAGS += -DMINESWITCH_DEFAULT_GAME=0 -DMINESWITCH_FRONTEND=1 -I$(ROOT)/launcher/include
APP_TITLE := MineSwitch
APP_VERSION := 0.2.1
APP_ICON := $(ROOT)/launcher/assets/icon.jpg
else
$(error Unsupported launcher mode: $(APP_MODE))
endif
ifeq ($(MINESWITCH_JVM_VARIANT),zero)
CFLAGS += -DMINESWITCH_ZERO_VM=1
else ifeq ($(MINESWITCH_JVM_VARIANT),server)
CFLAGS += -DMINESWITCH_ZERO_VM=0
else
$(error Unsupported JVM variant: $(MINESWITCH_JVM_VARIANT))
endif
LDFLAGS := $(ARCH) -specs=$(DEVKITPRO)/libnx/switch.specs \
 -Wl,-Map,$(OUTPUT).map -Wl,--gc-sections -Wl,--no-undefined -Wl,--wrap=exit -Wl,--wrap=_exit
ifeq ($(MINESWITCH_JVM_VARIANT),server)
# Demand the native HotSpot strong entry from libjvm before SDK's weak default.
LDFLAGS += -Wl,-u,__libnx_exception_entry
endif
JDKLIBS := $(MINESWITCH_JDK_ARCHIVES) $(MINESWITCH_EXTRA_ARCHIVES)
ifeq ($(ALLOCATOR_BACKEND),mimalloc)
ALLOCATOR := $(ROOT)/runtime/memory/mimalloc/build/libmineswitch_mimalloc.a
CFLAGS += -DMINESWITCH_ALLOCATOR_MIMALLOC=1
LDFLAGS += -Wl,-u,_free_r,-u,_calloc_r,-u,_malloc_usable_size_r,-u,_mallinfo_r,-u,_mallopt_r,-u,_malloc_trim_r,-u,_malloc_stats_r,-u,_mstats_r,-u,_sbrk_r
else ifeq ($(ALLOCATOR_BACKEND),newlib)
ALLOCATOR := $(ROOT)/runtime/memory/newlib/build/libmineswitch_newlib.a
else
$(error Unsupported allocator backend: $(ALLOCATOR_BACKEND))
endif
# Pull the audited allocator overlay before the SDK's automatic libc scan.
LDFLAGS += -Wl,-u,_malloc_r,-u,_realloc_r,-u,_memalign_r
SDKLIBS := $(ROOT)/.research/vm-probe-libffi/stage/lib/libffi.a \
 $(PORTLIBS)/lib/libmbedcrypto.a -L$(PORTLIBS)/lib -lSDL2 \
 -L$(DEVKITPRO)/libnx/lib -lnx
OBJECTS := $(BUILD)/main.o $(BUILD)/install.o $(BUILD)/config.o $(BUILD)/clock_diagnostic.o $(BUILD)/log_tee.o $(BUILD)/native_registry.o
GRAPHICS_DEPS :=
ifeq ($(GRAPHICS_BACKEND),nouveau)
OBJECTS += $(BUILD)/nouveau_topology.o
LDFLAGS += -Wl,--wrap=nouveau_getparam
SDKLIBS += -lglapi -ldrm_nouveau
else ifeq ($(GRAPHICS_BACKEND),nxvk)
include $(ROOT)/runtime/graphics/nxvk/build/archives.mk
CFLAGS += -DMINESWITCH_GRAPHICS_NXVK=1
JDKLIBS := $(MINESWITCH_JDK_ARCHIVES) $(filter-out %/libEGL.a,$(MINESWITCH_EXTRA_ARCHIVES))
OBJECTS += $(BUILD)/nxvk_compat.o
GRAPHICS_DEPS := $(NXVK_WHOLE_ARCHIVES) $(NXVK_SUPPORT_ARCHIVES)
SDKLIBS += -Wl,--whole-archive $(NXVK_WHOLE_ARCHIVES) -Wl,--no-whole-archive \
 $(NXVK_SUPPORT_ARCHIVES) -lexpat -lz -lpcre2-posix -lpcre2-8
LDFLAGS += -Wl,-u,vk_icdGetInstanceProcAddr
else
$(error Unsupported graphics backend: $(GRAPHICS_BACKEND))
endif
ifeq ($(PROFILE_ALLOC),1)
CFLAGS += -DMINESWITCH_ALLOC_TRACE=1
OBJECTS += $(BUILD)/alloc_trace.o
LDFLAGS += -Wl,--wrap=malloc,--wrap=calloc,--wrap=realloc,--wrap=memalign,--wrap=posix_memalign,--wrap=free
ifeq ($(GRAPHICS_BACKEND),nouveau)
CFLAGS += -DMINESWITCH_ALLOC_TRACE_NOUVEAU=1
LDFLAGS += -Wl,--wrap=nouveau_bo_new
else ifeq ($(GRAPHICS_BACKEND),nxvk)
OBJECTS += $(BUILD)/nxvk_memory_trace.o
LDFLAGS += -Wl,--wrap=jitCreate,--wrap=jitClose
endif
endif
ifeq ($(RECLAIM_NXVK),1)
ifneq ($(ALLOCATOR_BACKEND),newlib)
ifneq ($(GPU_ALIAS),1)
$(error mimalloc cache reclamation requires the validated GPU alias integration)
endif
endif
ifneq ($(PROFILE_ALLOC)-$(GRAPHICS_BACKEND),1-nxvk)
$(error RECLAIM_NXVK currently requires the NXVK allocation diagnostic build)
endif
CFLAGS += -DMINESWITCH_NXVK_RECLAIM=1
OBJECTS += $(BUILD)/nxvk_cache_pressure.o
endif
ifeq ($(GPU_ALIAS),1)
ifneq ($(ALLOCATOR_BACKEND)-$(GRAPHICS_BACKEND),mimalloc-nxvk)
$(error GPU_ALIAS requires the mimalloc NXVK backend)
endif
CFLAGS += -DMINESWITCH_GPU_ALIAS=1
OBJECTS += $(BUILD)/nxvk_gpu_alias.o
endif
ifeq ($(COMPACT_VM),1)
CFLAGS += -DMINESWITCH_COMPACT_VM=1
endif
ifeq ($(APP_MODE),launcher)
# Game builds: OpenAL device close must not hang the render thread.
OBJECTS += $(BUILD)/audio_shutdown.o
LDFLAGS += -Wl,--wrap=alcCloseDevice
FRONTEND_SOURCES := drawing frontend http storage import catalog prepare repair loading heap mods nbt content versions images packwiz i18n
OBJECTS += $(addprefix $(BUILD)/frontend-,$(addsuffix .o,$(FRONTEND_SOURCES)))
# Use the same FreeType implementation as java.desktop in this process.
FRONTEND_CXXFLAGS := $(ARCH) -D__SWITCH__ -D_GNU_SOURCE -DCURL_STATICLIB -std=gnu++17 \
 -O2 -g -Wall -Wextra -Werror -ffunction-sections -fdata-sections -MMD -MP \
 -I$(ROOT)/launcher/include -isystem $(DEVKITPRO)/libnx/include \
 -isystem $(PORTLIBS)/include -isystem $(PORTLIBS)/include/SDL2 \
 -isystem $(ROOT)/.upstream/openjdk21/src/java.desktop/share/native/libfreetype/include
SDKLIBS += -lcurl -ljansson -lminizip -lwebp -lz
endif
# Guarantee that the real CodeMemory allocator satisfies the ffi symbols before
# the generic libffi archive can extract its non-executable malloc fallback.
ifneq ($(filter ffi_closure_alloc,$(MINESWITCH_FORCE_SYMBOLS)),)
OBJECTS += $(ROOT)/runtime/lwjgl/build/lwjgl/horizon_ffi_closure.o
endif

.PHONY: all
all: $(OUTPUT).nro
$(BUILD):
	mkdir -p $@
$(BUILD)/%.o: %.c Makefile $(dir $(REGISTRY))/native-archives.mk | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@
$(BUILD)/native_registry.o: $(REGISTRY) | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@
$(BUILD)/nouveau_topology.o: $(ROOT)/runtime/graphics/nouveau_topology.c $(ROOT)/runtime/graphics/nouveau_topology.h Makefile | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@
$(BUILD)/nxvk_compat.o: $(ROOT)/runtime/graphics/nxvk/compat.c Makefile | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@
$(BUILD)/nxvk_memory_trace.o: $(ROOT)/runtime/graphics/nxvk/memory_trace.c $(ROOT)/runtime/graphics/nxvk/memory_trace.h Makefile | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@
$(BUILD)/nxvk_cache_pressure.o: $(ROOT)/runtime/graphics/nxvk/cache_pressure.c $(ROOT)/runtime/graphics/nxvk/cache_pressure.h Makefile | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@
$(BUILD)/nxvk_gpu_alias.o: $(ROOT)/runtime/graphics/nxvk/gpu_alias.c $(ROOT)/runtime/graphics/nxvk/gpu_alias.h $(ROOT)/runtime/memory/mimalloc/page_pool.h Makefile | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@
$(BUILD)/frontend-%.o: $(ROOT)/launcher/src/%.cpp Makefile | $(BUILD)
	$(CXX) $(FRONTEND_CXXFLAGS) -c $< -o $@
$(OUTPUT).elf: $(OBJECTS) $(JDKLIBS) $(ALLOCATOR) $(GRAPHICS_DEPS)
	$(CXX) $(LDFLAGS) $(OBJECTS) $(ALLOCATOR) -Wl,--start-group $(JDKLIBS) $(SDKLIBS) -Wl,--end-group -o $@
$(OUTPUT).nacp: Makefile
	$(TOOLS)/nacptool --create "$(APP_TITLE)" "MineSwitch" "$(APP_VERSION)" $@
$(OUTPUT).nro: $(OUTPUT).elf $(OUTPUT).nacp $(APP_ICON) $(shell rg --files $(ROMFS))
	$(TOOLS)/elf2nro $(OUTPUT).elf $@ --nacp=$(OUTPUT).nacp --icon=$(APP_ICON) --romfsdir=$(ROMFS)
-include $(OBJECTS:.o=.d)
