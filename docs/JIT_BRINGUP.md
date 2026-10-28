# Native AArch64 HotSpot bring-up

Real AArch64 HotSpot C1 and C2 execution is verified on the owner's Switch OLED
in Horizon 21.1.0, in application mode. Both compiler levels passed the JIT and
native runtime contract probes, and the full native Minecraft image passed its
LWJGL/GPU probe. The authoritative device results below run through
`20261006T053948Z`; full Minecraft world rendering and performance are a separate
acceptance step, tracked in `PORT_PROGRESS.md`.

The owner prioritized JIT after the real Zero client reached the Minecraft UI,
loaded resources and connected to the offline fixture, but timed out during
tag-dependent block-cache rebuilding. The native configuration is
`horizon-aarch64-server-release`, with genuine C1/C2 and the AArch64 template
interpreter. Zero is retained as a separate fallback.

## Verified native JVM device gates

| Probe | Device result | Authoritative log |
| --- | --- | --- |
| WhiteBox JIT, C1 / level 1 | **45 checks passed** | `logs/mineswitch-jit-20261006T051055Z.log` |
| WhiteBox JIT, C2 / level 4 | **45 checks passed** | `logs/mineswitch-jit-20261006T051252Z.log` |
| Native contracts, C2 / level 4 | **31 checks passed** | `logs/mineswitch-jit-20261006T053717Z.log` |
| Native contracts, C1 / level 1 | **31 checks passed** | `logs/mineswitch-jit-20261006T053838Z.log` |
| LWJGL on the native server VM | **24 checks and 90 frames** | `logs/mineswitch-native-minecraft-20261006T053948Z.log` |

All five runs report successful VM creation, `JNI_DESTROY_END result=0`, and
`MINESWITCH_JVM_END result=0`. WhiteBox requests an exact compiler level and
verifies an installed nmethod, executable entry and actual machine-code bytes.
For example, the device C2 log records 368 bytes for `checksum`, 768 for
`gcSurvivors`, and 488 for the dense-switch method. The checks execute arithmetic,
branches, array/null exceptions, GC-surviving references, another Java thread,
the switch table, deoptimization and recompilation. The GC and switch targets
remain compiled after the exercised operations.

The native contract probe adds signed zero, NaN and gradual underflow, two real
StackOverflow recoveries with reguarding while the recursion method stays
compiled, polymorphic dispatch, and GC plus management stack walking of a live
compiled loop. Live FP and object values survive those safepoints. Its helper
can re-enqueue a coalesced compilation request after the compiler queue becomes
idle; exact requested-level and still-compiled assertions remain in place.

The native LWJGL run identifies `OpenJDK 64-Bit Server VM`, exercises 128 live
libffi callbacks in a shared CodeMemory slab, sparse frees/reuse, and mixed
pointer/double callbacks. It creates a real GLFW window, decodes the STB PNG,
links GLSL 150, checks GPU pixels and presents 90 frames. The driver reports
OpenGL 4.3 / GLSL 4.30, Mesa 20.1.0-rc3, renderer NV120. The slab is released
after the final callback. These results cover this integration path; they do
not measure Minecraft FPS or establish complete LWJGL/Java SE coverage.

## Supporting platform evidence

- Server configure selects `compiler1 compiler2 jni-check management serialgc
  static-build` for AArch64/Horizon. ADLC generated the actual AArch64 C2 backend.
- Native code uses real RW/RX CodeMemory aliases. Mutable metadata and stores
  use writable addresses; code entry points, branch/relocation calculations and
  execution use RX addresses. Publication cleans RW data and invalidates RX
  instructions. Symbol presence alone was not the acceptance gate.
- Native exception ABI passed **18 checks on Switch**, with complete GPR/NEON
  and flag restoration, a separate exception stack and 1000 recoveries across
  CPU cores 0 and 1. Log:
  `logs/mineswitch-native-exceptions-probe-20261006T034628Z.log`.
- The extended exception test passed 21 checks, including the exact HotSpot
  `dcps1 #0xdead` trap, in
  `logs/mineswitch-native-exceptions-probe-20261006T035930Z.log`.
- The CodeMemory probe passed all execution/publication tests: genuine RW/RX
  permissions, executable PC, relative branch, mutable data and an instruction
  update observed on another core. Its stack-permission check failed:
  `logs/mineswitch-jit-memory-probe-20261006T041229Z.log` (27 pass, 1 fail).
  A focused follow-up confirmed that main and SDK worker Stack mappings reject
  permission changes with `0xD401`:
  `logs/mineswitch-stack-guard-probe-20261006T041808Z.log`.
- Correct guard holes use the SDK worker's recorded mapping/backing pair and
  public unmap/remap operations. The device follow-up passed **28 checks** in
  `logs/mineswitch-stack-guard-probe-20261006T043128Z.log`, including fault-handler
  remapping, retrying the faulting store, cleanup and SDK thread close. Native
  JVM StackOverflow/reguard checks above subsequently passed at both compiler
  levels. See `HORIZON_STACK_GUARDS.md`.
- The same WhiteBox probe passed **45 checks at each level on the host** in
  `logs/jit-probe-host-validation.log`; those host results are supporting tests,
  separate from the device logs above.

## Separate integration outputs

```sh
MINESWITCH_JVM_VARIANT=server bash runtime/configure-horizon.sh
MINESWITCH_JVM_VARIANT=server bash runtime/build-horizon.sh hotspot
bash runtime/build-jit-image.sh
python3 runtime/tests/jit/prepare.py
bash runtime/build-jit-launcher.sh
```

The image helper includes java.base and management modules for the
probe. It uses `runtime/generated-jit`, and the launcher has its own `build-jit`
objects and `mineswitch-jit.nro`. The registry selects a real built JVM archive
and records its variant; the native launcher does not force `-Xint`. All builds
use the shared six-job, six-CPU, low-priority limiter.

The diagnostic C1 device invocation is:

```sh
bash scripts/run-nro.sh 192.168.3.177 runtime/launcher/mineswitch-jit.nro \
  --system-main --class=mineswitch.probe.JitProbe \
  --jvm-options=romfs:/launch/jit-level1.options -- 1
```

Level 4 uses `jit-level4.options` and Java argument `4`. These options enable
WhiteBox only for the diagnostic run; they are not Minecraft launch options.
The small initial code-cache settings are probe settings, not a measured
Minecraft capacity.

The full 21-module native Minecraft image is separate:
`runtime/generated-native-minecraft`. It contains 112 runtime files / 33,963,036
payload bytes before launch overlays, with 5,835 registered functions across
27 real JDK/LWJGL/GLFW/OpenGL/STB/OpenAL libraries. The corresponding launcher is
`runtime/launcher/mineswitch-native-minecraft.nro`. Its physical image includes
`jdk.jfr`, but Minecraft's resolved boot layer continues to exclude JFR according
to `minecraft/MODULE_POLICY.md`.

## Normal application exit

After JVM shutdown and log draining, the launcher requests normal application
termination through the public applet/AM session and keeps native code, service
handles and daemon storage alive until AM terminates the complete process.
It does not run `appletExit`/`atexit` cleanup or return into an unloaded NRO.
`svcExitProcess` is the explicit failure fallback if the manager request fails.

The native contract and LWJGL logs record a zero terminate result and
`HORIZON_APPLET_EXIT_REQUEST ... kind=application command=0`. The owner confirmed
graceful arrival at HOME for this path; it is not classified as a crash. An
accepted exit may terminate the process before a later acknowledgement log is
transmitted. Sphaira must be reopened for the next launch.

## Related completed and pending work

The SHA5 native compression candidate passed 2058 independent host checks with
3467 native calls, including signed-JAR verification and tamper rejection:
`logs/sha5-independent-native.log`. Its separate original-client verification
read 23,516 signed entries. Device validation is pending; see
`HORIZON_SHA5_COMPRESSION.md`.

The read-only clock diagnostic is now measured on device. The User and Local
clock helpers, their direct `ISystemClock.GetCurrentTime` IPC calls, and libc
agree on 2051 in the JIT/LWJGL logs. The Network clock is unavailable
(`0x0000cc74`). The observation therefore reaches the public console clock service.
The owner did not knowingly select that date. No clock was changed and no TLS
certificate check was bypassed; the earlier authlib expiry failure remains
explained by the observed time.
