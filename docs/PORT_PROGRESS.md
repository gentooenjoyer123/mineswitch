# Native Minecraft port: active work

The active goal remains a working Minecraft Java 1.21.1 client in Horizon OS on
the owner's Switch OLED. Controller adaptation is explicitly deferred. A JVM
probe, successful compilation, or a rendering demo does not complete this goal.

The owner subsequently selected a local profile for the MVP and support for
offline-mode multiplayer servers. Microsoft account login is deferred.

Build resource constraint: **at most six compilation jobs**, as explicitly
requested by the owner after the PC became sluggish. Run builds through
`runtime/limit-build.py`; it serializes project builds, confines them to six
logical CPUs, sets nice 15 and limits host Java's processor count to six.
Never alter unrelated user Java/Minecraft/Gradle processes.

## Native device validation snapshot

Real AArch64 HotSpot JIT now runs on the Switch. The validated device gates are:

| Gate | Result | Log |
| --- | --- | --- |
| C1 / level 1 WhiteBox JIT | 45 passed | `logs/mineswitch-jit-20261006T051055Z.log` |
| C2 / level 4 WhiteBox JIT | 45 passed | `logs/mineswitch-jit-20261006T051252Z.log` |
| C2 native contracts | 31 passed | `logs/mineswitch-jit-20261006T053717Z.log` |
| C1 native contracts | 31 passed | `logs/mineswitch-jit-20261006T053838Z.log` |
| Native LWJGL/GPU | 24 passed, 90 frames | `logs/mineswitch-native-minecraft-20261006T053948Z.log` |

All five logs show VM creation/destruction and launcher completion with result
zero. Exact requested compilation levels, installed machine code, executed
results, GC-surviving references, exceptions, stack overflow/reguard and live
compiled-loop stack walking are checked. The owner confirmed graceful HOME
exit through the current AM/application termination path. See `JIT_BRINGUP.md`
for scope and supporting platform evidence.

The full native Minecraft client is the next end-to-end acceptance step.
These probe logs do not establish a rendered game world or measured Minecraft
FPS. Controller adaptation and Microsoft login remain deferred as selected.

## Current source and build

- Upstream: OpenJDK 21u commit `bb6b279495ceff8442dbd0225756b92d466d5309`.
- Working source: `.upstream/openjdk21`, with a pristine local Git import baseline.
- Exported changes: `runtime/patches/openjdk21-horizon.patch`; refresh with
  `python3 runtime/export-openjdk-patch.py` after source changes.
- Reproduction starts with `runtime/bootstrap-openjdk.py`, the local devkitPro
  bootstrap and `vm-probe/bootstrap-libffi.py`.
- Genuine target: Horizon, AArch64/newlib, Serial GC and static native libraries.
  Both the retained Zero configuration and native AArch64 interpreter/C1/C2 use
  this platform identity.
- Configurations: `horizon-aarch64-zero-release` and
  `horizon-aarch64-server-release`; their VM archives are under their own
  `support/modules_libs/java.base/{zero,server}/libjvm.a` directories.
- Java/native libraries and generated data for the full 21-module Minecraft
  profile are built for the server configuration. The separate image is
  `runtime/generated-native-minecraft`: 112 base runtime files / 33,963,036
  bytes before launch overlays, with 5,835 registered functions in 27 libraries.
- The three-module native diagnostic image remains in `runtime/generated-jit`;
  the original Zero image is retained in `runtime/generated`.
- `runtime/build-image.sh` creates linked Java 21 images with
  `horizon-aarch64` module metadata. A host-only enum patch enables stock jlink
  to recognize the new target while the host remains Linux.
- `runtime/generate-native-registry.py` creates tables of actual linked
  exports; these replace dynamic library discovery on Horizon.

## Additional device evidence, 6 October 2026

| Component | Evidence |
| --- | --- |
| Page-backed VM memory | `logs/mineswitch-memory-probe-20261005T235312Z.log`: 50 passed, 0 failed |
| Guarded memory reads | `logs/mineswitch-safefetch-probe-20261005T235754Z.log`: 23 passed, 0 failed, including 8000 expected read faults on CPU cores 0 and 1 |
| libjimage OS file adapter | `logs/mineswitch-jimage-io-probe-20261006T001327Z.log`: 30 passed, 0 failed |
| Shared regular-file backend | `logs/mineswitch-file-io-probe-20261006T003733Z.log`: 47 passed, 0 failed, including owned SD writes and cleanup |

Process system-resource budget is zero. VM reservations therefore use eager
physical backing inside the existing process heap. Commit/uncommit change real
kernel access permissions; uncommit does not return backing to the kernel.
Code cache storage is non-executable only for the Zero variant. Native AArch64
uses real CodeMemory RW/RX aliases and an eagerly backed code arena. The native
device gates above verify execution/publication, GC and runtime contracts.

The file test first found three canonical-path failures: `stat`/`realpath`
reopened a file while its writable handle was held. Metadata-only path
translation and a real directory-entry walk fixed the failures. The SDK leaves
inode identifiers zero, so the port does not treat zero/zero as file identity.

## First actual JVM execution — verified

`runtime/launcher` links a real `JNI_CreateJavaVM`, starts it on a 4 MiB native
stack, and runs `runtime/tests/HelloHorizon.java` as a version-65 class. Its
checks include exceptions, real GC pressure, Java threads, Java→JNI→Java calls,
CSRNG and JRT/NIO file access.

Actual device log: `logs/mineswitch-jvm-20261006T004427Z.log`. NRO size:
18733257 bytes; SHA256:
`7968b0fdcb5525493933dde31861eee455143e046d1871ab31811c7b51b4fabb`.

- Runtime installed and hash-verified: 30 files.
- `JNI_CreateJavaVM` returned 0 and a real `JNIEnv`; interface version `0x150000`.
- Runtime identified itself as Java `21.0.13-internal`, `OpenJDK 64-Bit Zero VM`,
  OS `Horizon`.
- All 21 Java checks passed, including JNI calls and callbacks, Unicode strings,
  exceptions, actual repeated Serial GC cycles, a managed Java thread, CSRNG,
  class-file version 65 loaded from JRT, and owned file read/write/cleanup.
- `DestroyJavaVM` returned 0; worker exited; final marker was
  `MINESWITCH_JVM_END result=0 exit=process`.

This establishes a working initial JVM/Java-base port for the tested behaviors.
It does not establish full Java SE compatibility or Minecraft execution.

Runtime data is packed into NRO RomFS, hash-verified and installed into the owned
SD directory `/switch/mineswitch/runtime`. The launcher uses a 64 MiB maximum
Java heap initially and `-Djdk.image.map.all=false` to match index-only native
JImage mappings. This is not the planned heap size for Minecraft.

After a JVM attempt, the launcher exits the entire process. Returning into the
homebrew loader with surviving JVM/native threads could execute unloaded code.
HOME after this test is expected; the owner must reopen Sphaira for another run.
The Java-test and DestroyJavaVM results above are independently present in the
historical Zero log. Subsequent native tests use the normal AM exit path; the
owner confirmed graceful HOME rather than a system error for that path.

## Known remaining requirements

- Complete native Minecraft startup, ordinary server configuration and a
  rendered world, then measure performance under the actual game workload.
- Verify the relevant persistence and longer-running game behavior; focused
  file/lock/selector/JVM probes do not provide full Java SE or game coverage.
- Package the selected client profile for standalone Sphaira launch while
  retaining the already installed owned game data.
- Repair any additional runtime contracts exposed by the full client. The
  native interpreter, C1/C2, required module image and LWJGL device gates are
  already established by the cited evidence.

## Minecraft preparation after the first JVM run

- Real dependency analysis and services require 21 Java modules; the expanded
  image is built and records its closure in `runtime/generated/module-image.json`.
- Official game inputs are installed on SD: 3941 files / 912723284 bytes,
  SHA256 verified by the installer. Log:
  `logs/mineswitch-game-data-20261006T011605Z.log`.
- Native-backed whole-file lock bridge: 29 checks passed in
  `logs/mineswitch-fs-lock-probe-20261006T010439Z.log`.
- Loopback Pipe/poll/shutdown: 74 checks passed, including four simultaneous
  pollers sharing three BSD sessions, in
  `logs/mineswitch-pipe-probe-20261006T013104Z.log`.
- Real GLFW Switch sources are rebuilt with EGL/OpenGL and an honest
  unavailable Vulkan-loader path: `runtime/glfw/build/libglfw3.a`.
- LWJGL core/OpenGL/STB native archives and Horizon Java bindings are built;
  subsequent Zero and native device runs verified callbacks and GPU output,
  as recorded below and in the current validation table.
- `minecraft/prepare-launch.py` stages the local-profile, namespaced bootstrap.
  Its default 1024 MiB heap is an initial client setting, not a measured capacity
  or performance guarantee.

All four major work areas can proceed independently: build/image tooling,
HotSpot/libjava/launcher, filesystem/libnio, and root integration/device tests.
Only the root agent uploads to the Switch; `scripts/run-nro.sh` serializes uploads.
The console address is `192.168.3.177`, the development PC `192.168.3.163`.
Do not send empty TCP readiness connections. A frozen capture image alone is not
evidence of a frozen console; the owner identified a capture-card issue.

## Historical first client startup and resolved callback blocker

`logs/mineswitch-jvm-20261006T021608Z.log` establishes 16 passing Java LWJGL
checks and 90 frames: real libffi callbacks, STB PNG decoding, GLSL 150, GPU
readback and OpenGL 4.3 on nouveau.

The next run, `logs/mineswitch-jvm-20261006T022345Z.log`, executed the original
signed Minecraft 1.21.1 client with the local MineSwitch profile. It completed
226 Datafixer optimizations and reached render-thread/GLFW initialization. At
approximately 625 seconds it exited with a callback allocation failure:
CodeMemory creation returned kernel ResourceExhausted (`0xCE01`) while creating
the scroll callback. The Java heap still had approximately 140 MiB free; the
allocator's one-CodeMemory-per-callback strategy was subsequently replaced with
shared slabs. This run did not reach a rendered world or join the test server.

The isolated official server at `192.168.3.163:25576` has its EULA explicitly
accepted by the owner, a saved flat fixture world, offline mode and a whitelist
for MineSwitch. It is kept in its own process and directory. No user server is
modified. Minecraft startup profiling and the SHA-384 signature-verification
cost investigation are described in `MINECRAFT_STARTUP_ANALYSIS.md`.

The shared-slab fix passed on the device in
`logs/mineswitch-jvm-20261006T025317Z.log`: 24 checks and 90 GPU frames. The GLFW
error callback and 128 stress callbacks occupied one 64 KiB CodeMemory object.
Mixed integer/pointer and floating-point/pointer callback invocation, sparse
frees, surviving callbacks and recycled slots all passed. Final cleanup released
the slab and returned live/pool counts to zero; Java main and JVM shutdown
returned success. The second real-client attempt below included this fix, and
the native server-VM LWJGL run later repeated all 24 checks and 90 frames.

## Second actual client run and JIT priority

`logs/mineswitch-jvm-20261006T025810Z.log` passed the former callback failure,
rendered the actual Mojang loading screen, initialized OpenAL, created the
texture atlases and opened the multiplayer connection. The owner confirmed the
screen, and a capture of the OBS preview independently showed the client UI.
The isolated server identified the real MineSwitch offline profile, then timed
out while the client rebuilt tag-dependent block-state caches. The client
displayed the ordinary timeout screen and kept rendering. The owner later
closed it and reopened Sphaira; this was not a JVM crash.

Vanilla configuration completion gives the client 15 seconds on this server;
the repeated cache work took roughly two minutes in Zero. The audited protocol
sequence is in `MINECRAFT_CONFIGURATION_TIMEOUT.md`. Increasing this fixture's
timeout would not establish ordinary server compatibility.

The owner explicitly prioritized real AArch64 HotSpot JIT next. Build and
device-validation gates are recorded in `JIT_BRINGUP.md`. The overall goal
remains incomplete: a rendered Minecraft world and usable performance are not
yet verified. The owned fixture server was gracefully stopped and saved while
JIT work proceeds; other user servers were not touched.
