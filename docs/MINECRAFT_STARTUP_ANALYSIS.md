# Minecraft 1.21.1 startup: measured stages and remaining work

The device run in `logs/mineswitch-jvm-20261006T022345Z.log` progressed through
the original client and ended naturally at about 625 seconds. It was not
restarted for this investigation.

The second run, `logs/mineswitch-jvm-20261006T025810Z.log`, adds precise main
loading/invocation markers and a bounded sampler. It passed the former GLFW
callback allocation failure and reached vanilla resource/font loading and
OpenGL calls. That milestone does not establish a working game or complete
resource reload.

## Second-run measurements

- `Class.forName("net.minecraft.client.main.Main")` took **68.561 seconds**.
  Reflection lookup then completed and actual `main` invocation began at
  **68.563 seconds**. Six pre-invocation samples included `SHA5.lf_S`, two
  `Long.rotateRight`, `Long.reverseBytes`, `BigInteger.implMulAdd`, and
  `OutputStream.<init>`. The first four are consistent with the actual SHA-384
  signed-JAR work; the BigInteger sample is consistent with RSA verification.
- Crypto-compatible samples recur around 110–151, 241, 441–451 and 581 seconds.
  This supports accelerating compression, without claiming all startup time is
  hashing. Only the top frame was returned by the initial Zero stack walk,
  so generic `Long`/`BigInteger` samples do not identify their full callers.
- The Datafixer thread starts around 214 seconds and reports 226 optimizations
  in **27.653 seconds**, close to the first run.
- Many later samples are normal registry, collection, and voxel-shape work.
  Official mappings identify `exf` as `BitSetDiscreteVoxelShape`, `exi` as
  `CubePointRange`, `exp` as `IndirectMerger`, and `dtt` as `BooleanProperty`.
  Loaded classes keep growing; a stable class count alone is not a hang.
- Sampler management initialization took **250 ms**. Bounded snapshots usually
  take roughly **0.2 ms**; the first snapshot took 8 ms. `snapshot_us` measures
  the snapshot itself, excluding output formatting/transmission. It does not
  account for minutes of observed work.
- Through about 580 seconds, 21 logged GC pauses total **3.206 seconds**. Heap
  capacity grows normally; this evidence does not identify GC as the main delay.
- After passing window creation, the same tracked thread is renamed
  `Render thread`; samples include `GL11C.glClear` and LWJGL JNI calls while
  resource workers load the vanilla Unifont files.

Parsed samples are saved in `logs/minecraft-second-run-progress.json`.
Sampling is sparse and initially lacks caller frames; these are observations,
not exact CPU-time percentages.

## What the log establishes

- `MINESWITCH_MINECRAFT_MAIN_ENTER` precedes a combined expression performing
  `Class.forName`, method lookup **and invocation**. It cannot distinguish those
  phases. The official `Main` static initializer only sets
  `java.awt.headless=true`.
- At about 218 seconds, the VM created the Datafixer Bootstrap thread. Around
  220 seconds, the original `Main.main` logged a handled OSHI hardware-query
  failure. At about 245 seconds, DFU reported 226 optimizations in 26.846 seconds.
- Most reported GC pauses were 0.04–0.16 seconds; the longer observed full GCs
  were about 0.5 and 1 second. These pauses do not explain minutes of startup.
- The run reached the LWJGL backend and OpenGL initialization. It then failed
  allocating the GLFW scroll callback: `jitCreate` returned `0x0000ce01`, and
  LWJGL raised `OutOfMemoryError` from `Callback.create`. The crash report still
  showed about 140 MiB free inside a 418 MiB Java heap, with a 989 MiB maximum.
  That identifies a native JIT allocation/resource failure; it does not show
  Java heap exhaustion.

## Signed-JAR work

The actual official Java artifacts were inspected, without changing signature
validation or game files. Only `client.jar` among the 50 inventory inputs has
JAR signatures:

| Property | Observed value |
| --- | --- |
| Client JAR size | 26,836,906 bytes |
| Manifest | 3,185,580 bytes |
| `MOJANGCS.SF` | 3,185,778 bytes |
| Signed entries | 23,516 |
| Signed uncompressed content | 41,812,158 bytes |
| Signed class content | 28,794,084 bytes |
| Entry digest algorithm | **SHA-384** |

The inventory is saved in `logs/minecraft-signed-jars-analysis.json`.
`SignatureFileVerifier` verifies the PKCS7 block, hashes the manifest, and
associates signers with the entries. Class loading subsequently verifies entry
digests. OpenJDK Zero explicitly disables SHA intrinsics in
`cpu/zero/vm_version_zero.cpp`; SHA-384 therefore uses the 80-round Java
`sun.security.provider.SHA5` compressor. SHA-256 acceleration would not target
the observed signed-entry algorithm.

This is evidence of substantial work, **not a profile proving hashing dominates**.
Minecraft also constructs the Datafixer schema graph and bootstraps block/item
registries before its window appears. The mappings and bytecode identify these
calls before and after the first hardware-info log.

## Measurements followed by an isolated optimization candidate

The second run implemented separate loading/invocation markers and daemonized
10-second sampling. Standard thread-dump APIs use VM safepoints; this does not
require enabling JFR or attaching a debugger to another process. A separate
Zero frame-walk fix is being validated to recover the requested caller frames.

An isolated host `-Xint` benchmark can measure signed `Main` class lookup without
invoking the game, and compare SHA-384 hashing of the actual manifest. It cannot
replace a device profile, but can separate initial JAR verification from the
game's bootstrap work.

The measurements justified a narrowly scoped native SHA5 block-compression
candidate, processing multiple blocks per JNI call. The installed
SDK exposes mbedTLS SHA-512/384 compression. The Java provider identity, initial
states, padding, incremental byte counts, reset/clone behavior and all
`JarVerifier` checks must remain intact. This is native C acceleration; Cortex-A57
does not provide the newer architectural SHA-512 instruction extension.

Required validation includes known answers for SHA-384/SHA-512 and truncated
variants, empty/boundary-sized inputs, offset and chunked updates, clone/reset,
direct-buffer updates, and verification/rejection of genuine and deliberately
modified test JAR entries. No signature-verification bypass is part of the plan.

The isolated candidate and validation are described in
`docs/HORIZON_SHA5_COMPRESSION.md`. It has not been inserted into the live
second client run. No signature-verification bypass is part of the change.

## Console clock

The current libnx/newlib path obtains POSIX seconds from the UserSystemClock and
adds elapsed hardware ticks; HotSpot only converts this value to milliseconds.
The device logs and real filesystem timestamps both point to 2051 while the
host is in 2026. The exact clock source still requires a read-only raw-service
comparison; it has not been corrected or replaced with host time.

The second run demonstrates an effect: authlib's HTTPS request rejects a
certificate whose `NotAfter` is in 2029. Certificate validation remains intact.
This handled request failure did not prevent entering vanilla resource loading.

Host tooling used
`runtime/limit-build.py`: at most six build workers, the shared six-CPU affinity
set and nice level 15.
