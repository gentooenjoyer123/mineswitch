# NXVK normal-world baseline and memory work — 6 October 2026

The actual Switch OLED renders Minecraft 1.21.1/Fabulously Optimized using
NXVK + Zink at `-Xms1500m -Xmx1500m`. The owner confirmed movement and good
rendering in a normally generated world. Standard Sodium buffers are retained;
the former staging/mesh reductions are removed. No agent clock changes were made.

## Baseline

Live binary SHA256: `9e7636eed2cb8355357624628a1311b92ebcf783e408a2baeb436e5bea36d22b`.
Log: `logs/MineSwitch-NXVK-20261006T180820Z.log`.

The separate normal-world server is `192.168.3.163:25577`, seed `1211006`,
world `world_mineswitch_normal`, directory `minecraft/test-server/normal-run`.
It uses the previously accepted EULA, offline whitelist, two-player limit,
normal terrain/structures/entities, view distance 6 and simulation distance 4.
The previous flat fixture at port 25576 is preserved.

| Scene / heartbeat interval | Observed time | Mean FPS | Max swap interval | >50 / >100 ms | GC pauses |
| --- | ---: | ---: | ---: | ---: | --- |
| Flat reference, 751–806 s | 60.117 s | 59.75 | 46.547 ms | 0 / 0 | 2 young; max 10.598 ms |
| Normal world, 1196–1251 s | 60.100 s | 58.44 | 1132.877 ms | 2 / 1 | 4 young + 1 full; max 1114.205 ms |
| Normal world after cap spontaneously lifted, 1287–1342 s | 60.092 s | 110.71 | 73.060 ms | 32 / 0 | 4 young; max 21.620 ms |

The full pause was explicitly `CodeCache GC Threshold`, not Java heap
exhaustion. At JVM time 1224.862 s it reduced heap usage 544→402 MiB. The
matching frame spike was 1132.877 ms. The ordinary world-entry warmup also
contained two CodeCache-triggered full pauses of 1713 and 1661 ms.

These are intervals between completed swaps, including waits/GC. They do not
measure GPU execution time or prove that the display presents 120 distinct
frames per second. No per-frame percentile data was collected. The capture
remains 1920×1080. Client view distance was not changed by the agent during
these measurements; the previously observed value was four chunks.

The owner reported that the 60 FPS cap lifted spontaneously; no cap patch was
applied. `logs/switch-nxvk-normal-120.png` shows actual terrain, ~116 FPS and
instantaneous clocks CPU 2191.8 MHz, GPU 768 MHz, RAM 2400 MHz, GPU load 59%.
An earlier normal-world capture showed CPU 2193.5 MHz and GPU load 21% at
60 FPS. Earlier flat captures used ~1310 MHz CPU. These windows therefore are
observations, not a controlled clock-for-clock speedup comparison.

Machine-readable measurements:

- `logs/nxvk-flat-baseline-1500-detail.json`
- `logs/nxvk-normal-baseline-1500-minute.json`
- `logs/nxvk-normal-uncapped-1500.json`

`scripts/summarize-game-run.py` accepts explicit heartbeat boundaries and a
scene description. It recomputed the saved flat baseline exactly. Other
samples are assigned to their preceding heartbeat, so boundary times are
approximate. The longer mixed-cap file is exploratory and should not replace
the separated windows above.

## Current budget

The process allowance is 3,343,908,864 bytes (~3189 MiB). In the uncapped window:

| Measurement | Value | Interpretation |
| --- | ---: | --- |
| Java physical heap reservation | 1500 MiB | Xms and Xmx are equal; eager backing |
| Java reported heap capacity | 1450 MiB | Serial GC excludes one survivor space from this metric |
| Java heap occupancy | 467–807 MiB | Includes objects eligible for later GC; not all live data |
| Metaspace used / committed | ~156 / 156.75 MiB | Physical reservation granularity is additional |
| JIT CodeCache used / committed | ~43–45 / 48.81 MiB | Physical reserve remains 96 MiB |
| Aggregate native available | 493–513 MiB | Includes free malloc storage and unclaimed space; not largest block |

Do not add these overlapping metrics. GPU buffers also use shared system RAM
and are part of process/native allocation accounting. Kernel mapped memory
alone is not malloc occupancy. `nxvm` currently allocates physical backing at
reservation time; merely lowering Xms does not reclaim it, and uncommit retains
the backing. Full 2 GiB work is deferred by the owner.

## Diagnostic build

`MineSwitch-NXVK-Memory.nro` SHA256:
`7a35be6f0d276ff550b1e85b14e9c9060fac4aeba259bac944ed86894bfed2ab`.
It is a separate artifact; the successful gameplay binary remains intact.

The diagnostic build does not change allocations, cache policy, resolution,
mods, heap size, or JVM collection policy. It adds:

- A weak observer at actual NVK/Horizon backing allocation/free, plus cache
  put/take/eviction/miss events. Cached memory is a subset of backing, not an
  extra allocation; GPU aliases are not counted again. Driver internal buffers
  are included if they use this allocator. Other SDK allocations are outside it.
- Public `jitCreate`/`jitClose` tracking of source storage, counting RW/RX aliases
  once. Successful allocations before the launcher starts are included.
- `nxvm_get_memory_stats`, distinguishing reserved, committed and physical backing.
- Generic public-allocation tracing from launcher trace start for blocks at
  least 256 KiB, eight largest live blocks and twelve largest caller totals.
  This overlaps graphics/JVM backing and misses pre-start/private allocations.
- Newlib capacity, arena, occupancy and free-space metrics.

Counters run without allocating or printing on the graphics path. Reports run
on the existing sampler, so profiling overhead must not be used as an FPS
improvement claim. A nonzero `counter_errors` or dropped generic records
invalidates completeness claims.

Host ASan/UBSan checks passed: 800 concurrent real allocator reuse cycles,
failed realloc preservation, aligned allocations and caller aggregation;
40,000 graphics-counter lifecycles, cache reuse/eviction and JIT failed-create/
failed-close preservation. These test bookkeeping, not Horizon/NVK execution.
The cross driver and diagnostic NRO compiled and linked successfully. Device
profiling also passed: normal-world login at local 21:36:20, real terrain
rendering and movement, no counter errors or dropped generic records in the
saved profile. Log: `logs/MineSwitch-NXVK-Memory-20261006T183438Z.log`.
Screenshot: `logs/switch-nxvk-memory-stable.png` (actual gameplay, ~118 FPS).

Thirty post-warmup samples spanning heartbeat170–315 s are summarized in
`logs/nxvk-memory-budget-1500.json`; decoded owners and original snapshots are
in `logs/nxvk-memory-profile.json`. Representative physical accounting at315 s:

| Category | MiB |
| --- | ---: |
| Java heap backing | 1500.00 |
| Other VM backing, including metaspace and GC tables | 166.62 |
| JIT source storage, including closure arena | 96.06 |
| NXVK backing, including reuse cache | 585.07 |
| Other malloc allocations and allocator overhead, residual | 253.78 |
| Mapped process storage outside the malloc heap | 40.87 |
| Aggregate available | 546.57 |

The categories total within32KiB of the3189MiB process allowance; snapshot
queries are sequential while game allocations continue. The residual is not
evidence of a leak. The VM subtotal includes actual eager reservation backing,
not just committed Java management metrics. Public malloc traces overlap these
categories and must not be added again.

Across this interval NXVK backing ranged546.7–585.1MiB, its cache subset
36.9–62.2MiB, and aggregate availability529.5–589.6MiB. Cache traffic was289hits
and20misses during the interval, so turning it off blindly would discard useful
reuse. Limiting it may release tens of MiB; that alone cannot establish a safe
548MiB heap increase from1500 to2048MiB.

Build with the existing staged NXVK toolchain:

```sh
python3 runtime/limit-build.py -- ninja -C runtime/graphics/nxvk/build/cross-zink -j6 src/nouveau/vulkan/libnvk.a
bash runtime/build-nxvk-launcher.sh PROFILE_ALLOC=1 BUILD=/home/justxls/mineswitch/runtime/launcher/build-nxvk-memory OUTPUT=/home/justxls/mineswitch/runtime/launcher/MineSwitch-NXVK-Memory
```

## Explicit 1948 MiB test — failed world stability

The owner requested1948MiB after reviewing the1500MiB budget. The same profiler
binary was relaunched with `--heap-mib=1948 --precommit-heap`. An accidental UI
selection of2048MiB was corrected by a second relaunch. The tested log
`logs/MineSwitch-NXVK-Memory-20261006T184726Z.log` confirms persisted1948MiB,
`-Xms1948m`, and an actual2,042,626,048-byte physical Java heap allocation.

The normal server accepted the player at21:49:07. Minecraft produced its crash
report at21:49:33. The fatal cause was two native
`realloc(NULL,2621440)` failures (2.5MiB, `ENOMEM`) from LWJGL
`MemoryUtil.nmemReallocChecked`, called by Sodium's
`ChunkMeshBufferBuilder.reallocate/start` during chunk meshing. Java heap
exhaustion is not indicated: the nearby sampler showed~471MiB occupied out of
1883MiB reported Serial GC capacity (1948MiB physical reservation).

The heartbeat125s snapshot preceding failure showed171.2MiB aggregate native
available,165.8MiB free within malloc, but only1.60MiB still unclaimed by its
arena and an8KiB top chunk. The largest free block was not captured. Thus
fragmentation is the leading explanation for refusing a2.5MiB contiguous
allocation despite substantial aggregate free space; an allocator/accounting
defect cannot be excluded by these measurements alone.

NXVK's backing reuse cache had already fallen to4.0MiB. An earlier3.69MiB GPU
backing allocation failed around90s; the driver has a cache-trim/retry path,
and gameplay continued until the separate native mesh failure. Simply reducing
the configured64MiB cache limit cannot be claimed to recover60MiB at the actual
failure point. The crash report lists client render distance6/12chunks; the
agent did not change that setting.

Full report extracted locally: `logs/crash-1948-20261006-client.txt`.
Structured evidence: `logs/heap-1948-failure.json` and
`logs/nxvk-memory-profile-1948-failed.json`. No additional diagnostic upload is
required to obtain this report. The1948MiB setting remains persisted; it is a
failed stability test, not a recommended working configuration.1500MiB remains
the verified fallback. Standard Sodium buffers remain intact.

## Locked allocator measurement confirms fragmentation

`MineSwitch-NXVK-Fragmentation.nro`, SHA256
`2797d9ae2162f1d4c4aa7cb40d3324c03a4ae1016c4c66049a5e4ab227c570f6`,
ran with the same1948MiB heap. The allocator now captures its free lists under
its actual malloc lock at the instant heap growth fails, storing the report
in thread-local storage for later output. The implementation preserves the
audited allocator layout and does not change allocation policy.

At the fatal native2.5MiB request:

| Locked measurement | Bytes |
| --- | ---: |
| Requested payload | 2,621,440 |
| Allocator padded requirement | 2,621,456 |
| Total free chunks including headers | 72,712,400 |
| Largest free chunk including header | 2,621,424 |
| Largest usable payload | 2,621,416 |
| Top chunk | 8,176 |
| Unclaimed malloc-heap capacity | 1,482,752 |

There were157605free chunks and **zero fitting chunks**, with `invalid=0` and
`truncated=0`. The largest payload was24bytes short; changing the requested
size would violate the caller's contract and is not a fix. This proves native
contiguous-space shortage, rather than relying on aggregate availability.
The earlier3.69MiB graphics failure is not required to trigger this failure.

Evidence: `logs/MineSwitch-NXVK-Fragmentation-20261006T190412Z.log`, decoded
`logs/nxvk-fragmentation-1948.json`, and
`logs/fragmentation-confirmed-1948.json`. Full report was extracted locally as
`logs/crash-fragmentation-1948-client.txt`. The diagnostic source passes1855
ASan/UBSan host checks, including real fragmented allocation failure and
400independent failure captures on four threads.

## Bounded cache reclamation candidate

The last pre-failure sample in this run still had61.3MiB in NVK's idle backing
cache. NVK already trims this cache when one of its own GPU backing allocations
fails; a native LWJGL mesh-buffer failure does not enter that path.

An explicitly gated `PROFILE_ALLOC=1 RECLAIM_NXVK=1` candidate connects the
paths. After an outer public `realloc` returns `ENOMEM`, it tries to release
idle NVK cached backing and retries the **unchanged** allocation once if any
backing was released. A failed retry preserves the caller's old buffer and
returns failure. It does not resize Sodium buffers, lower quality, change the
heap, or release referenced/mapped live GPU resources. This first candidate
covers public realloc failures, not every possible native allocation failure.

The callback runs after libc has released its malloc lock. Device registration
and per-device cache access use try-locks during reclamation; busy caches are
skipped. Destruction unregisters before taking device locks and waits for any
ongoing callback, protecting the context lifetime. Cache records are detached
under the cache lock, then actual backing is freed after that lock is released.
Reentrancy is rejected without waiting. The profiling wrapper tracks these
separate frees normally, so reclaimed allocations do not remain in its inventory.

Host ASan/UBSan checks passed for successful retry/data preservation, failed
retry/ownership retention, empty cache, recursion, no second retry, and tracer
accounting; registry tests cover busy/reentrant skip and destruction
synchronization. These tests do not establish device rendering stability.
Candidate build/device results are recorded in `runtime/ACTIVE_WORK.json`.

## Next decisions depend on the profile

1. Correct the upstream Horizon available-memory report. In
   `.upstream/nxvk/src/util/os_misc.c`, `os_get_available_system_memory` currently
   returns the total process allowance. This ignores Java/native occupancy.
   The existing launcher has actual newlib occupancy/unclaimed-space accounting;
   the driver can use that aggregate, without claiming contiguous availability.
   This defect was identified by source inspection; no correction or resulting
   memory saving has been tested yet.
2. Measure Zink's separate BO cache/slab retention inside NVK backing and break
   down the native residual further. Zink initializes its cache limit to one
   eighth of exposed GPU heap size, with500ms expiry; the limit is not actual
   occupancy. NVK's separate64MiB reuse cache is now measured above. Prefer a
   bounded-pressure policy over disabling demonstrated reuse.
3. Investigate frequent JIT-triggered full GC independently of Java heap size.
   Current HotSpot `SweeperThreshold=15` requests collections after relatively
   little code growth even when roughly half of the 96 MiB cache is free.
   A threshold change needs a measured test of pauses and eventual cache
   pressure; no collection/flushing mechanism has been disabled.
4. Validate the bounded idle-cache reclamation candidate at1948MiB. Capture
   released bytes, actual retry outcome and subsequent world stability. A
   successful allocation retry alone is not evidence of stable gameplay.
