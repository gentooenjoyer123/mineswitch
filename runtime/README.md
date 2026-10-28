# OpenJDK 21 Horizon build

This directory contains the build entry points for the native Horizon port.
The default configuration remains HotSpot Zero. The separate AArch64 server
configuration now runs real C1/C2 on Switch: 45 JIT checks and 31 native contract
checks passed at each compiler level, with successful VM creation/destruction.
Native LWJGL integration passed 24 checks and 90 frames. See
[`../docs/JIT_BRINGUP.md`](../docs/JIT_BRINGUP.md) for exact device logs and
[`JIT_BUILD.md`](JIT_BUILD.md) for the isolated native build commands.

The prepared source tree is `.upstream/openjdk21`, pinned to OpenJDK 21u commit
`bb6b279495ceff8442dbd0225756b92d466d5309`. Local port patches must be applied
before configuring. The target is **aarch64-unknown-horizon**, using devkitA64
GCC/newlib and libnx; Linux native binaries are not used on the Switch.

```sh
./runtime/configure-horizon.sh
./runtime/build-horizon.sh
```

The default build target is `hotspot`. To compile the platform's Java classes
and generated JNI headers separately:

```sh
./runtime/build-horizon.sh java.base-java
```

Generate the matching runtime data and configuration, and compile native
libraries, with:

```sh
./runtime/build-horizon.sh java.base-copy java.base-gendata java.base-libs-only
```

Configuration and build commands share a filesystem lock. A second command
waits for the first one to finish before touching the output tree. The shared
limiter enforces at most six jobs, a six-CPU affinity set and nice level 15,
including host Java's processor count.

Create the compressed target runtime image and native symbol registry with:

```sh
./runtime/build-image.sh
```

This produces `runtime/generated/romfs/jdk`, its verified-installation
`files.manifest`, `runtime/generated/jmods/java.base.jmod`, and
`runtime/generated/native_registry.c`. The registry is derived from exported
functions in the actual target archives; it includes the class loader's
non-JNI lookup names. `runtime/generated/romfs/tests` is preserved.

Stock host JDK 21 `jlink` rejects the new `horizon-aarch64` metadata name. The
script compiles a temporary host enum extension through `--patch-module` so
the tool recognizes Horizon. It uses the host's `PlatformProps`, explicitly
checks that the host has not become Horizon, and leaves the target module
metadata as `horizon-aarch64`. No host runtime library is copied into the image.

The default image profile now comes from `modules-minecraft-1.21.1.txt`, derived
from the verified official game artifacts by `analyze-minecraft-modules.py`.
The current closure contains 21 modules, including offscreen desktop/image
APIs, management, XML, DNS, elliptic-curve TLS and ZIP filesystem support.
`MINESWITCH_MODULES=java.base` creates the smaller bootstrap image;
`MINESWITCH_MODULES_FILE=/path/profile.txt` selects another explicit root list.
The builder resolves transitive dependencies from the **target** descriptors.

The Minecraft launcher's boot-module policy is separate from physical image
contents: see [`../minecraft/MODULE_POLICY.md`](../minecraft/MODULE_POLICY.md).
It excludes JFR from the game's resolved boot layer so the unmodified game
chooses its own NoOp profiler while this VM lacks recording support.

The registry discovers native archives for selected modules. Optional external
library definitions are read from `runtime/lwjgl/registry-libraries.json`, or
`MINESWITCH_NATIVE_SPECS` (path-separated); set it to `none` for a JDK-only
registry. Generated `native-archives.mk` lists the same JDK/external archives
and symbols that must be retained for linking. This prevents a launcher using
a stale hard-coded nine-library list after the module set grows.

Horizon AWT has no desktop window/input backend. Offscreen image primitives,
JPEG, color management and bundled FreeType native code compile; Java's
platform font discovery/rendering manager is still explicitly unsupported.
Java Sound exposes its format/software APIs with no OS mixer or MIDI devices;
game audio uses OpenAL. Native system GSS is not built because Horizon supplies
no such provider; the Java GSS/Kerberos classes remain. These limits are not
claims that the corresponding game paths have already been exercised.

The resulting static VM archive, once compilation succeeds, belongs at:

```
.upstream/openjdk21/build/horizon-aarch64-zero-release/support/modules_libs/java.base/zero/libjvm.a
```

It still needs a native launcher and the matching native Java libraries and
runtime image. A static archive may contain unresolved symbols; final NRO
linking and execution are separate checks, now completed for the cited Zero
and native device probes. Full Minecraft world/FPS acceptance is separate.

Prerequisites and overrides:

| Variable | Default |
| --- | --- |
| `DEVKITPRO` | `.toolchain/opt/devkitpro` in this project |
| `DEVKITA64` | `$DEVKITPRO/devkitA64` |
| `MINESWITCH_BOOT_JDK` | `/opt/openjdk-bin-21` (host JDK 21) |
| `MINESWITCH_LIBFFI` | `.research/vm-probe-libffi/stage` |
| `MINESWITCH_JDK_SOURCE` | `.upstream/openjdk21` |
| `MINESWITCH_JVM_VARIANT` | `zero`; use `server` for native AArch64 C1/C2 |
| `MINESWITCH_JDK_CONF` | `horizon-aarch64-$MINESWITCH_JVM_VARIANT-release` |
| `MINESWITCH_GENERATED_DIR` | `runtime/generated` |
| `MINESWITCH_JOBS` | `6`, enforced by the shared limiter |

`vm-probe/bootstrap-libffi.py` builds the target libffi dependency. Both the
boot JDK and build JDK are host executables. The source tree's Java compiler
tools compile target classes; no target NRO is executed during this build.
Target C/C++ uses AArch64 soft TLS and the same architecture flags as the
device probes. `build-horizon.sh` supplies `HORIZON_RUNTIME_DIR` so the memory
backend C bridge is compiled alongside HotSpot.

The native target uses suffixed built-in JNI entry points such as
`JNI_OnLoad_java` and ELF symbol lists extracted from real archives. The
standard Unix `libjli` command-line launcher is excluded: the Horizon launcher
enters the VM using JNI from its NRO. Configuration and data for a custom JMOD
are staged separately:

| JMOD input | Output path under the selected build directory |
| --- | --- |
| Classes and module resources | `jdk/modules/java.base` |
| Configuration | `support/modules_conf/java.base` |
| Runtime data (`tzdb.dat`, security store/policy data) | `support/modules_libs/java.base` |
| Module legal notices | `support/modules_legal/java.base` |
| Common legal notices | `support/modules_legal/common` |

Do not copy the static `.a` archives into the runtime image as loadable
libraries. They are linked into the NRO; the image needs its data files.

HotSpot's generic POSIX implementations are deliberately excluded for Horizon:
newlib does not supply Unix virtual memory, dynamic loading or POSIX fault
handling. Their contracts belong to the Horizon backend. The shared POSIX JNI
type headers remain usable because the target follows the AArch64 LP64 ABI.

Logs from the current build work are saved under `logs/jdk-*`. Build artifacts
remain under `.upstream`; source patches and port code must be retained in the
project separately so that they survive recreating the upstream tree.

On 6 October 2026, the `hotspot`, `java.base-java`, `java.base-copy`,
`java.base-gendata` and `java.base-libs-only` targets completed successfully.
The combined nine target archives have no undefined JDK symbols missing from
the selected devkitA64/libnx/libffi library set. This is compilation and symbol
availability evidence; final NRO linking and JVM execution remain separate
requirements.

The current native outputs are separate: `runtime/generated-jit` contains the
three-module JVM probes, and `runtime/generated-native-minecraft` the full
21-module Minecraft profile with real LWJGL native libraries. The latter's base
runtime manifest has 112 files / 33,963,036 bytes before launch overlays; its
registry identifies the actual server JVM and 27 libraries. Minecraft's JFR
boot-layer exclusion remains in force for this native image.
