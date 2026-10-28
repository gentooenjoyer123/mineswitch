# Native AArch64 HotSpot build

The native backend uses the AArch64 template interpreter and real C1/C2
compilers. Its build output is separate from the working Zero configuration.
The default scripts still select Zero.

```sh
MINESWITCH_JVM_VARIANT=server \
MINESWITCH_JDK_CONF=horizon-aarch64-server-release \
bash runtime/configure-horizon.sh

MINESWITCH_JVM_VARIANT=server \
MINESWITCH_JDK_CONF=horizon-aarch64-server-release \
bash runtime/build-horizon.sh hotspot
```

The scripts apply the shared six-worker limit, six-CPU affinity and nice level
15. Configure also records six build jobs/cores and passes the host JVM limit
through `--with-boot-jdk-jvmargs`, because OpenJDK rejects the environment-variable
banner during Boot JDK detection. Existing output directories cannot be
reconfigured to a different JVM variant.

The selected features are `compiler1 compiler2 jni-check management serialgc
static-build`. The target remains **Horizon/newlib**, with AArch64 CPU sources;
neither the Linux implementation nor Zero is substituted. Frame pointers and
the devkitA64 soft TLS model are retained. Native assembly receives
`NXVM_HOTSPOT_AARCH64` separately from C/C++ flags, so the archive contains the
full native exception entry and only the shared SafeFetch leaf functions.

The library is written to:

```
.upstream/openjdk21/build/horizon-aarch64-server-release/support/modules_libs/java.base/server/libjvm.a
```

ADLC generation, native compilation, isolated NRO linking and actual Switch
execution have succeeded. WhiteBox JIT tests passed 45 checks at both C1 and
C2; additional native runtime contracts passed 31 at each level. VM creation
and destruction returned zero. The full native Minecraft image then passed
24 LWJGL checks and 90 presented frames. Exact logs and scope are recorded in
[`../docs/JIT_BRINGUP.md`](../docs/JIT_BRINGUP.md).

The verified contract includes executed nmethods, writable metadata with RX
code addresses, instruction publication, GC, stack overflow/reguarding and
exceptions. These focused results do not establish a rendered Minecraft world
or a game-performance measurement.

Image/launcher integration is separate. Use the dedicated generated directory
and launcher output when preparing the native probe; do not replace the proven
Zero image as a side effect of compiling this library. Coordinate a final build
after shared code-cache/interpreter headers are frozen, then perform the real
launcher link and target probes. A static archive can contain unresolved
references and is not a substitute for either check.

The three-module diagnostic image uses `runtime/generated-jit` and
`mineswitch-jit.nro`. The 21-module Minecraft image uses
`runtime/generated-native-minecraft` and `mineswitch-native-minecraft.nro`, with
the actual LWJGL/native library registry. Default Zero output paths remain
separate. See `build-jit-image.sh` and `build-jit-launcher.sh` for the probe path.
