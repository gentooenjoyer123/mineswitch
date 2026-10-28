# LWJGL 3.3.3 on Horizon

Upstream source is pinned to `f0ec1437a6149dae00eaa27c47268e5e4efb129f`
(tag `3.3.3`) in `.upstream/lwjgl`. Download metadata and archive SHA256 are in
`.research/lwjgl333/FULL_SOURCE.json`. Source changes remain visible in that
tree's Git diff against its import commit.

`build_native.py` cross-compiles upstream core JNI, generated OpenGL bindings
and STB into actual AArch64 archives under `build/`. `build_java.py` overlays
Horizon-specific classes onto the original verified Minecraft dependency jars.
The compile-only JSR305 jar has a pinned SHA256. Runtime APIs and generated JNI
signatures remain those of LWJGL 3.3.3.

The platform changes are:

* A distinct `Platform.HORIZON`, plus `HorizonLibrary` resolving the real NRO
  registry through the JVM native loader. Static libraries have process lifetime
  and no independent filesystem path.
* OpenGL function lookup through actual EGL/Mesa `eglGetProcAddress`; GLFW is
  the real Switch fork rebuilt separately under `runtime/glfw/`.
* Genuine libffi closure storage using documented libnx CodeMemory RW/RX
  aliases. Upstream AArch64 `ffi_prep_closure_loc` and assembly dispatch remain
  in use; publication flushes code and closure metadata after preparation.
  Other JIT modes are explicitly rejected because LWJGL reads closure metadata
  through its data pointer while the code is executable.
* Newlib's real `memalign` supplies validated aligned allocations; there is no
  `posix_memalign` symbol in this SDK. The LWJGL `jvm` cache is private to avoid
  colliding with AWT's separate variable in a static executable.
* The real C allocator is selected with `org.lwjgl.system.allocator=system`.
  Jemalloc is not required. OpenAL uses its actual SDK SDL2 backend; sound has
  not been established by the graphics probe.

`registry-libraries.json` lists concrete archives and required exports. The
launcher places `horizon_ffi_closure.o` directly before archives so libffi's
malloc-only closure implementation cannot be selected. EGL's SDK dependencies
and SDL2 are linked as needed. No second FreeType implementation should be
added beside the JDK's actual `libfreetype.a`.

The named `mineswitch.probe.LwjglProbe` uses these jars through the real JVM.
It checks the platform, real GLFW error callback, executable mixed pointer/double
callback, native window/context, GL 3.2 capability, STB PNG decoding, GLSL 150
program linking, triangle drawing, known GPU pixels and 90 buffer swaps. Those
are integration checks; a successful build alone is not execution evidence.

The launcher must relinquish its console framebuffer on its owner thread before
GLFW creates the EGL surface. It retains the queued NXLink stream and explicitly
sleeps in the main loop after console rendering stops. User controller adaptation
is deferred, as requested.

## Verified device integration

The real OLED/Horizon 21.1.0 execution in
`logs/mineswitch-jvm-20261006T021608Z.log` passed all 16 Java LWJGL checks and
90 presented frames. The executable libffi callbacks ran, including a native
pointer-plus-two-double call back into Java. The driver reported OpenGL 4.3
Core / GLSL 4.30, Mesa 20.1.0-rc3, renderer NV120. Actual GPU pixels were
`13,20,31` for the cleared background and `51,178,102` inside the shader triangle.
STB decoded the independent PNG fixture. Java main and DestroyJavaVM both
returned success before process exit.

The tested NRO SHA256 is
`a75a20682bd574d3374717f4ada787e621477cd60001b9fcb415648a7066e437`.
The preceding execution stopped at the required GLFW binding gate, before
callback execution. The Switch GLFW fork now provides real allocator callbacks
and its own platform-query identity for the three newer APIs required by
LWJGL's binding table; it continues to report its actual 3.3.4 base version.
The optional Vulkan wrapper class is not initialized by the probe; its real
stable GLFW capability function reports no Vulkan backend.

These results establish this Java/JNI/GLFW/EGL/OpenGL/STB integration path.
They do not establish Minecraft startup, audio playback or complete LWJGL API
coverage.

## Callback resource limit and pooled-closure verification

The first actual Minecraft client run reached window/input initialization and
then exhausted a kernel resource while allocating another CodeMemory object:
`0x0000ce01` is kernel `ResourceExhausted` (description 103), not the distinct
kernel out-of-memory code. The failure occurred in `glfwSetScrollCallback`; Java
still reported 140 MiB free within its committed heap. The original one-object-
per-closure policy was therefore unsuitable for the game's callback count.

Closures now share real 64 KiB CodeMemory slabs, normally with 64-byte slots.
Every allocation has separate RW/RX addresses, and publication flushes the slab
without revoking peers' permissions. A slab is released only when its final live
allocation is freed. Failed partial unmaps are quarantined from future reuse.
The installed libnx binary was audited: its CodeMemory branch always calls both
cache-maintenance functions, including repeated publication; disassembly is in
`logs/lwjgl-jit-publish-disassembly.txt`.

The device execution `logs/mineswitch-jvm-20261006T025317Z.log` passed **24 checks
and 90 frames**, including 128 simultaneous mixed integer/pointer and
double/pointer callbacks, real native invocations, sparse free while peers stay
callable, slot reuse with changed captured values, and return to baseline.
Native diagnostics reported `live=129 pools=1` including the existing GLFW error
callback, then `pools=0 live=0` after complete cleanup. Java main and
DestroyJavaVM returned zero. Tested NRO SHA256:
`8ac909149ec7bf46988e005d07ab893f09cec484aef13a0c37b64c2696391f0b`.

## Native AArch64 HotSpot verification

`logs/mineswitch-native-minecraft-20261006T053948Z.log` repeats the complete
24-check / 90-frame probe on the actual `OpenJDK 64-Bit Server VM`. It verifies
the real shared-slab callbacks (including 128 live entries and sparse reuse),
STB decoding, GLSL 150, window/context creation and GPU readback. OpenGL reports
4.3 Core / GLSL 4.30, Mesa 20.1.0-rc3, renderer NV120. Slab cleanup reaches
`pools=0 live=0`; Java main, VM destruction and the launcher return success.
The owner confirmed normal AM/application exit to HOME.

This validates LWJGL on the native JIT-enabled runtime. Full Minecraft world
rendering, game FPS and complete graphics/audio API coverage remain separate
acceptance criteria; the authoritative JVM gates are in `docs/JIT_BRINGUP.md`.
