# mimalloc replacement on Horizon

The owner selected mimalloc and explicitly authorized implementation after
device measurements proved native fragmentation with the newlib allocator.
The starting Java configuration stays `-Xms1948m -Xmx1948m`; NXVK/Zink and
standard Sodium buffers remain in use. This is an isolated build variant.

## Source and integration

- Upstream: Microsoft mimalloc2.2.4, commit
  `fbd8b99c2b828428947d70fdc046bb55609be93e`, MIT license.
- Source: `.upstream/mimalloc`; pin/license hash:
  `runtime/memory/mimalloc/provenance.json`.
- Reviewed source changes: `runtime/memory/mimalloc/horizon.patch`.
- Port: `runtime/memory/mimalloc/prim_horizon.c`, `page_pool.c`, and
  `newlib_adapter.c`.

The native backend allocates real4KiB pages from the loader/libnx process heap.
Two bounded bitmaps track ownership and accessibility, supporting up to8GiB of
actual process heap. Physical memory is eagerly backed by Horizon; changing
permissions does not claim to release physical backing. A freed range returns
to this page pool for reuse. Partial frees are supported. Allocation searches
honor alignment, bounds and exhaustion without allocating bookkeeping memory.

Commit/decommit/protection use actual kernel permissions, split by old
permission runs, with rollback on failure. Host tests use real mapped memory
and mprotect rather than simulated successful operations. Huge pages are
unsupported and fail honestly. Overcommit and virtual-only reservation are
reported unavailable. Random initialization uses libnx's kernel-seeded RNG.

Horizon uses libnx mutexes and its actual thread-local storage. The compiler's
generic AArch64 hardware-thread-pointer shortcut is disabled in mimalloc;
the port uses its C TLS default-heap address with libnx's soft thread pointer.
A libnx TLS destructor abandons each terminating thread's heap correctly, so
live objects can subsequently be freed from another thread. No process-wide
allocator destructor races live JVM/driver threads during application exit.

Inspection confirmed that `newlibSetup()` initializes the main C TLS before
heap setup and allocation, so a second bootstrap allocator is unnecessary.

Both the public and reentrant newlib allocation ABI are supplied, including
malloc/free/calloc/realloc/reallocf, alignment, usable-size and statistics
entrypoints. SDK statistics wrappers share an object with `_mstats_r`; providing
the complete public interface prevents mixed old/new allocator objects.
The entire heap has one owner; legacy `_sbrk_r` rejects requests rather than
handing out overlapping storage. Newlib-specific mallopt knobs are unsupported
and report failure instead of pretending to apply them.

The first variant uses4MiB mimalloc segments and disables automatic large arena
reservation, to avoid the upstream desktop defaults reserving large portions
of a constrained eagerly backed heap. These are configuration choices, not
measured optimal settings. They are included in the build receipt.

`mallinfo` compatibility reports conservative backing usage: `uordblks` includes
whole pages/ranges held by mimalloc, including reusable slots inside them.
`MINESWITCH_MIMALLOC` adds exact backend capacity/backing/accessibility, free
backend pages and largest contiguous **backend** extent. Those free extents do
not include reusable space inside active mimalloc segments, and are not the
same metric as the former newlib free-chunk inventory.

## Validation

Builds use `runtime/limit-build.py`, at most six jobs/CPUs and nice15.
`build.py` verifies source commit, license, recorded patch and target ABI
symbols before publishing the archive.

Host debug and release each pass160135checks with ASan/UBSan. Coverage includes
real bounded3GiB backing, mixed permissions,2GiB allocation, alignment through
16MiB, failed-realloc preservation, calloc overflow, POSIX errno/output-pointer
semantics, cross-thread frees after thread exit and concurrent churn. The
synthetic memory-pressure scenario simultaneously reserves1948MiB Java-sized
storage,192MiB metadata-sized storage,96MiB code-sized storage,160000small
objects,512MiB graphics-sized blocks and24mesh-sized2.5MiB blocks. It is an
allocator test, not a Minecraft performance benchmark or GPU rendering test.

Actual Switch probe passed **50checks,0failures**:

- libc ABI calls route through mimalloc;
-2GiB nxvm reservation and genuine kernel permissions;
- uncommit/recommit zeroing and release;
- native libnx threads, cross-thread free and concurrent allocation;
- full1948MiB nxvm commit;
- real96MiB CodeMemory arena, execution of generated RX code returning42;
- the large/small allocation workload above and cleanup.

Evidence: `logs/mineswitch-mimalloc-probe-20261006T194744Z.log`.
Probe SHA256: `4ea347381dc684051a62ef48ac0636385d5c0035126e8c36d6c7beb341194d5f`.
The probe uses the same port/adapter as the game. Its process heap is larger
because it does not contain Minecraft/JVM/driver code; this is not proof of
gameplay stability at1948MiB.

Minecraft candidate: `runtime/launcher/MineSwitch-NXVK-Mimalloc.nro`, SHA256
`d603b045a5f3f6470f25b62a92591d1babfe855f9a46244baca28f6b2fe11766`.
Link audit found no old newlib allocator members, global bin state, or old
failure-snapshot observer. Candidate includes generic allocation inventory and
NVK backing counters; the previous cache-pressure realloc experiment is off.
Game run: `logs/MineSwitch-NXVK-Mimalloc-20261006T194908Z.log` **failed** during
graphics loading: repeated16MiB graphics allocation failures, followed by JVM
`Chunk::new` failure for32744bytes. The page pool still had104914944bytes free,
but its largest extent was3932160bytes, below the4MiB normal-segment requirement.
The standalone probe did not establish game stability.

## Compact segment revision

Expanded the host workload with32simultaneously live thread heaps retained
through the GPU/mesh-sized allocation phase. The old4MiB configuration fails
this regression at graphics allocation (`logs/mimalloc-retained-heaps-4m-negative.log`).
This adds a workload dimension missing from the first probe, whose worker
threads had already exited before that phase.

The revised configuration uses **1MiB segments and16KiB slices/pages**. Both
compile-time settings are supported tuning points in the pinned source; their
ratio remains64slices, preserving the commit-mask constraints. Application
requests and alignments remain unchanged. Debug and release each pass160199
host checks under ASan/UBSan, including the expanded simultaneous-thread test.
Log: `logs/mimalloc-compact-build.log`.

The native probe also retains32libnx threads through the large-allocation
workload. Its compact candidate SHA256 is
`40a44608a188d5cef2f1192eca8d494efdb992d4c68a988922fcf3813580f225`.
Device validation of this revision is pending. The first50-pass probe applies
only to the previous4MiB configuration; it must not be attributed to this one.

`MINESWITCH_MIMALLOC` now also includes the reporting thread's most recent failed
backend size/alignment. The aggregate failure count and per-thread last request
are deliberately distinguished. All pool/free measurements exclude reusable
slots inside still-owned mimalloc segments.

## Subsequent measurements and current candidate

The compact native probe subsequently passed179checks/0failures, including
32retained threads (`logs/mineswitch-mimalloc-probe-20261006T200103Z.log`).
Its game build still exhausted suitable backing during graphics loading and
ended in a null-source memcpy in Mesa `tc_buffer_map`, after GPU allocation
failures. This is not a successful game validation.

Adapted two remaining desktop assumptions for the eagerly backed Horizon pool:
OS requests round to4KiB rather than coarse megabyte multiples, and unused
leading alignment prefixes are actually returned to the common pool rather
than only decommitted. The memid's ownership is adjusted to prevent double-free;
the actual segment metadata and all caller-requested bytes remain allocated.
New host tests bound the backing cost of16MiB and16MiB-aligned192MiB requests.
The old configuration fails the former (`logs/mimalloc-rounding-negative.log`).
The updated debug/release suite passes160204checks, with release backing reduced
by~95MiB on the same retained-thread workload. Native probe179/0 passed again,
with peak3052691456bytes (`logs/mineswitch-mimalloc-probe-20261006T201742Z.log`).

The resulting Pages game build reached server connection but failed to allocate
a16MiB GPU buffer with~119MiB aggregate pool free and~10.5MiB largest extent.
Changing backing placement to low small allocations/high best-fit large
allocations also passed host/native probes but failed game entry. These are
recorded failures, not recommended releases:

- `logs/MineSwitch-NXVK-Mimalloc-Pages-20261006T201831Z.log`
- `logs/MineSwitch-NXVK-Mimalloc-Split-20261006T203647Z.log`

Added a bounded8192-record trace of primitive allocation/free operations,
including partial frees. It freezes at the first backing allocation failure
and is emitted outside the allocator lock on a failed public allocation.
Completeness is explicit; incomplete traces must not be used as full replays.
It records sizes/addresses only, not buffer contents. The first complete device
trace contains2547records. A numerical replay reproduced every native offset
and the recorded failure. The original C page pool on real host mmap/mprotect
backing also reproduced those exact offsets and failure.

The next candidate applies best-fit to **all** sizes: small metadata allocations
prefer existing small gaps, preserving larger extents; ties/placement prefer
low addresses for small requests and high addresses for large requests. The
same recorded first failure now succeeds in both the model and actual C
implementation. This proves that first placement failure is avoided on that
fixed sequence, not that subsequent gameplay is stable. Evidence:

- `logs/mimalloc-placement-replay.json`
- `logs/mimalloc-c-replay-reference.json`
- `logs/mimalloc-c-replay-bestfit.json`
- `logs/mimalloc-bestfit-build.log` (160217checks per debug/release)

BestFit game candidate: `runtime/launcher/MineSwitch-NXVK-Mimalloc-BestFit.nro`,
SHA256 `ee3315668aa9b5df58431c1f259081004f0ce46ef6dbc6c54ccb3f0e3d0da351`.
Run: `logs/MineSwitch-NXVK-Mimalloc-BestFit-20261006T205727Z.log`.
This game also failed at world entry:16MiB GPU allocation needed16809984bytes
of backing;129355776bytes remained free but the largest extent was8388608bytes.
No newlib cache-reclamation retry or
Sodium-buffer reduction is enabled in these mimalloc candidates.

## Fragmented backing for cached GPU buffers

Placement changes alone did not prevent the observed graphics failure. The
next isolated variant, `GPU_ALIAS=1`, gives eligible GPU buffers a contiguous
CPU virtual address backed by multiple real page-pool extents. It does not add
physical capacity, copy buffers, reduce their size or claim overcommit support.
The normal mimalloc ABI remains the allocator for other requests.

`runtime/graphics/nxvk/gpu_alias.c` allocates64KiB..2MiB physical pieces from the
smallest suitable holes using `nxmi_pool_alloc_some`. It reserves contiguous
stack-region VA and maps each piece there with ordinary Horizon `svcMapMemory`.
A1GiB aggregate VA budget includes allocations in progress. Allocation metadata
and the registry are mutex protected; mapping and page-pool operations happen
outside that registry lock. Failure rolls back every completed mapping and
owned page. Free closes the NVIDIA mapping first, flushes/unmaps aliases, then
returns physical pages and the VA reservation. Failed unmapping aborts instead
of recycling backing that is still mapped.

Only cached, unshared, uncompressed allocations of at least1MiB are eligible.
Ordinary aligned allocations remain the fallback. The NVK reuse cache tracks
alias eligibility so a later shared/compressed request cannot obtain an alias
through a cache hit. The public libc free path never receives alias pointers.
`MINESWITCH_GPU_ALIAS` records actual backing/active allocations, cumulative
pieces and provider failures without counting mapped VA as extra physical RAM.

Actual Switch probe: **123PASS/0FAIL**, log
`logs/mineswitch-gpu-alias-probe-20261006T211359Z.log`. Sixteen nonadjacent64KiB
pieces mapped into one1MiB view; CPU data roundtrip and live guards passed.
Cached `nvMapCreate` and GPU address-space mapping/unmapping also passed.
Changing the alias to uncached memory was rejected with`0x0000d401`; therefore
coherent/uncached GPU allocations MUST keep ordinary heap backing. This probe
establishes kernel/driver registration, not actual GPU rendering or readback.

Host test `runtime/graphics/nxvk/tests/alias_host.c` uses a real shared memfd and
`mmap` aliases on a finite64MiB pool. A16MiB ordinary allocation fails with32MiB
free in1MiB holes, while the alias allocation succeeds. Data is checked through
the restored physical-source view, guards survive, injected mapping failure
and genuine physical exhaustion roll back fully. Four concurrent workers also
allocate/write/free simultaneously for eight rounds. ASan/UBSan passes with no
leaked reservations, mappings or physical pages. The updated mimalloc debug
and release suites each pass160221checks. Evidence:
`logs/mimalloc-alias-build.log`.

Game artifact: `runtime/launcher/MineSwitch-NXVK-Mimalloc-Alias.nro`, SHA256
`e8a83443b6de3fa12cbda2842458686f2ffcd73ce1e55e496185408e08a10f61`.
Source and allocator receipts: `logs/nxvk-mimalloc-alias-receipt.json`.
Game run: `logs/MineSwitch-NXVK-Mimalloc-Alias-20261006T213332Z.log`.
This game failed at world entry in Sodium `ChunkMeshBufferBuilder.reallocate`:
`realloc(NULL,2621440)` was denied with26.33MiB pool free and2MiB largest extent.
GPU alias counters were89successful allocations/0failures, with369MiB mapped
backing in the final sample. No GPU allocation error preceded the crash. This
fixes the prior large GPU allocation failure in this run, but does not establish
world/game stability. NVK retained approximately61MiB of idle stores shortly
before the CPU allocation failure.

The next variant combines `GPU_ALIAS=1 RECLAIM_NXVK=1`: the existing bounded
public-realloc retry can now release idle cached aliases through the paired
free path. It still uses nonblocking registry/cache acquisition, skips recursive
reclamation and retries at most once after backing was actually released. It
does not reduce application requests, free active buffers or periodically purge
the useful cache. The previous prohibition on mimalloc+reclamation is lifted
only for this alias integration, after the independent game failure above.

`bash runtime/graphics/nxvk/tests/test-gpu-alias.sh` runs the real shared-backing
tests, a physical-exhaustion/reclaim/aligned-retry test, nonblocking cache/lifetime
checks and realloc ownership/tracking checks under ASan/UBSan. All pass:
`logs/gpu-alias-integrated-host.log`. The fake host cache callback frees actual
page-pool-backed aliases; actual NVIDIA cache behavior still requires the device.

Combined candidate: `runtime/launcher/MineSwitch-NXVK-Mimalloc-AliasPressure.nro`,
SHA256 `f313cbd6fb9d5fee9a8363ace0718bd02754148d015e83c0e814d7b27dd2c6e6`.
Receipt: `logs/nxvk-mimalloc-alias-pressure-receipt.json`.
Run: `logs/MineSwitch-NXVK-Mimalloc-AliasPressure-20261006T213740Z.log`.
This combined variant also failed: the idle-cache retry released16MiB but the
2.5MiB CPU allocation still failed. A3,129,344-byte free extent could not satisfy
the2,637,824-byte mimalloc backing request with1MiB segment alignment. Aggregate
pool free at that point was19,173,376bytes. This is a failed game validation.

## Direct page backing for large public allocations

`large_pages.c` now handles public requests of at least256KiB directly through
the same physical page pool. It preserves the application's requested alignment
and rounds sizes only to4KiB, avoiding mimalloc's extra1MiB segment alignment.
Small allocations still use mimalloc. The8192-entry,192KiB bounded ownership
registry lives outside payload pages; large JVM reservations may therefore
change protection across their entire payload without protecting allocator
metadata. All routes share one heap owner; no fixed physical partition exists.
If the registry is full, allocation falls back to ordinary mimalloc.

Free and usable-size distinguish direct allocations by exact pointer ownership.
Realloc preserves the original data/allocation on failed growth, handles
transitions between small/large routes, and returns complete trailing pages on
large shrink. Zero-size realloc frees and returnsNULL. Allocation-free registry
lookups fast-reject unaligned pointers; allocation/free do not hold the registry
mutex while calling page-pool operations. New `MINESWITCH_LARGE_PAGES` counters
are a subset of pool ownership, not additional memory.

Host regression creates a finite64MiB pool, fills it, then frees an intentionally
misaligned3MiB hole. The original1MiB-aligned request fails; the same2.5MiB
payload succeeds through the direct route. Guard bytes survive, the fully
protected payload can still be freed, and all pages are returned. The full
debug/release ASan/UBSan suites each pass160229checks, including mixed small/large
concurrent churn, cross-thread frees, shrinking, failed growth/reallocf and
small/large transitions (`logs/mimalloc-large-pages-build.log`).

Native Switch probe again passes **179/0**, including full1948MiB heap backing,
real protection changes,96MiB executable JIT arena and32retained threads:
`logs/mineswitch-large-pages-probe-20261006T214355Z.log`.
Game candidate: `runtime/launcher/MineSwitch-NXVK-Mimalloc-LargePages.nro`, SHA256
`3ac04ad575109f9984e32fe315af5bf8ed63cdba1881efd1c853fc9b50d3ad73`.
Receipt: `logs/nxvk-mimalloc-large-pages-receipt.json`. The actual game rendered
the normal world (`logs/switch-large-pages-current.png`) but still ended after
about105seconds of JVM time. Two2.5MiB realloc retries succeeded after reclaiming
66,125,824 and20,971,520bytes. Later only2,101,248bytes remained in the page pool,
below the2,621,440-byte request even with4KiB alignment; NVK idle cache was empty.
The network log ends mid diagnostic trace; no fresh Java crash report was found
on SD (latest remained the previous AliasPressure00:39:01 report). Game stability
at1948MiB is not established.

The next bounded comparison varies only mimalloc segment/slice sizes; direct
large pages and GPU behavior stay the same. `build.py --segment-shift=19` uses
512KiB segments/8KiB slices, and`--segment-shift=18` uses256KiB/4KiB. Both preserve
64slices and the native minimum4KiB page. Outputs are isolated in`build-s19` and
`build-s18`; default`build` retains1MiB/16KiB. Both host debug/release suites pass
160229checks and the targeted fragmentation regression. Release workload backing:

| Segment/slice | Backing bytes | Savings vs1MiB |
| --- | ---: | ---: |
|1MiB/16KiB|3026583552|0|
|512KiB/8KiB|3011903488|14MiB|
|256KiB/4KiB|3007971328|17.75MiB|

Logs:`logs/mimalloc-small-segments-512k-host.log` and
`logs/mimalloc-small-segments-256k-host.log`. These are allocator workload savings,
not measured Minecraft savings. Native256KiB candidate subsequently passed179/0
(`logs/mineswitch-small-segments-probe-20261006T215236Z.log`), with peak3,028,979,712
bytes versus3,047,591,936 on the1MiB direct-large-page probe. Game variant
`MineSwitch-NXVK-Mimalloc-SmallSegments.nro` uses the isolated`build-s18` archive,
SHA256`62e93fed9bf8f87e5ac2460f1e3d52f5e1874e78981af3bd2f884e26ecb83965`.
Receipt:`logs/nxvk-mimalloc-small-segments-receipt.json`; run:
`logs/MineSwitch-NXVK-Mimalloc-SmallSegments-20261006T215345Z.log`.
The SmallSegments game also rendered the world (roughly64–80FPS sample windows)
but failed after about107seconds while loading more terrain. At the last failed
2.5MiB CPU allocation only2,179,072bytes remained, largest extent1,499,136bytes;
NVK backing had grown to573,599,744bytes and idle cache was empty. It improved
memory use but did not establish stability.

## JVM reserve candidate outside mod allocations

The next isolated`COMPACT_VM=1` build keeps1948MiB Java heap and the same mod
requests. It reduces two eagerly backed JVM reserves:

- initial non-class metaspace backing192→160MiB for large Java heaps, retaining
  normal16MiB node expansion and the existing maximum (no new metaspace cap);
- code cache96→64MiB. Observed longer1500MiB gameplay used approximately43–46MiB
  of JIT code, while recent1948MiB runs used around39–43MiB.

The candidate also appends`-XX:SweeperThreshold=40` after selected-launch options.
In this JDK21 source this is percent allocated **since last unloading**, scaled
by remaining free fraction once usage exceeds the threshold. The default is15,
not a free-space threshold. Code flushing stays enabled and the aggressive
low-space trigger remains10%. This choice needs actual GC/frame measurements;
reducing reserved cache alone could increase threshold-triggered serial fullGCs.

JDK build now excludes`runtime/memory/mimalloc` from its recursive runtime-memory
source search, just as it excludes the newlib overlay. Allocators remain linked
once through the launcher. Target archive disassembly confirms the160MiB initial
node; archive membership has no accidental allocator/probe objects.

Artifact:`runtime/launcher/MineSwitch-NXVK-Mimalloc-CompactVM.nro`, SHA256
`dc90e0452293272a40459d1d75261cb735fbad2548dc094f237f1f07c152342d`.
Receipt:`logs/nxvk-mimalloc-compact-vm-receipt.json`; run:
`logs/MineSwitch-NXVK-Mimalloc-CompactVM-20261006T220034Z.log`.
Actual device logs confirmed the roughly64MiB reserve reduction and real world
flight for over3minutes. A later failure at JVM~270seconds was native
`Chunk::new` malloc917456bytes (`hs_err_pid195.log`), with approximately2.36MiB
pool free/852KiB largest extent while NVK still cached55.86MiB. The recovery
hook covered only realloc, so this compiler allocation could not reclaim it.
The run is therefore FAILED; the early no-failure observation was temporary.
`logs/nxvk-mimalloc-compact-vm-first-minute.json` records49.72FPS over60.27seconds
of owner flight with terrain loading, including one987ms fullGC. Selected later
5second windows exceeded100FPS; these are not a controlled baseline comparison.

The `AllPressure` candidate extends that same try-lock/reentrancy-controlled,
at-most-once retry to outer malloc/calloc/memalign/posix_memalign ENOMEM as well.
Invalid/overflowing requests do not trigger reclamation. It preserves requested
sizes/alignment, calloc zeroing, POSIX errno/output-pointer behavior and realloc
ownership on failure. Real shared-alias reclaim tests, concurrent cache lifetime
tests and injected-failure ABI tests all pass under ASan/UBSan:
`logs/gpu-alias-all-pressure-host.log`.

Artifact:`runtime/launcher/MineSwitch-NXVK-Mimalloc-AllPressure.nro`, SHA256
`54506315108b3cbaec5ceb44a1e3d2647bd211f7ff1c35cb2ed0da76be6a4533`.
Receipt:`logs/nxvk-mimalloc-all-pressure-receipt.json`; run:
`logs/MineSwitch-NXVK-Mimalloc-AllPressure-20261006T220917Z.log`.
All JVM/heap/mod settings matched the CompactVM candidate. The1948MiB run also
failed around JVM153seconds: a2.5MiB retry succeeded after releasing63,901,696
bytes, but subsequent requests failed with about59MiB free and largest extent
2,617,344bytes, one4KiB page below the requested2,621,440bytes. It is not stable.

The owner then explicitly requested **1900MiB**. The same unchanged AllPressure
binary was uploaded with`--heap-mib=1900 --precommit-heap` (48MiB more native
headroom). Run:`logs/MineSwitch-NXVK-Mimalloc-AllPressure-20261006T221553Z.log`;
receipt:`logs/nxvk-mimalloc-1900-receipt.json`. Ordinary CPU malloc alias fallback
was discussed but has NOT been implemented; this test isolates the heap change.
The1900MiB run passed the owner's short flight test; the owner reported
“всё отлично”. The actual Java backing was1900MiB, initial heap was fully
committed, and the setting was persisted. The server session lasted about4m55s
before the owner disconnected, with no observed crash. An explicit230.237second
world-flight interval(t100..325) averaged87.19FPS, with5second sample windows
18.05–118.27FPS. There were zero page-pool allocation failures, zero GPU alias
failures and zero driver counter errors. Minimum headroom derived from recorded
pool peak was36.54MiB. This is a successful short test, not exhaustive long-term
stress validation. GC pauses remain: four fullGCs in that measured interval,
maximum1248.51ms; maximum frame interval1263.92ms.

Evidence:`logs/nxvk-mimalloc-1900-world-flight.json`,
`logs/nxvk-mimalloc-1900-memory.json`, and
`logs/switch-1900-world-continued.png`. Keep this exact AllPressure artifact and
1900MiB setting as the owner-confirmed candidate;1948MiB remains unverified and
has repeated recorded failures. CPU malloc alias fallback remains unimplemented.

## Rebuild

```sh
python3 runtime/memory/mimalloc/build.py
python3 runtime/limit-build.py -- make -C runtime/memory/mimalloc -f probe.mk -j6
bash runtime/build-nxvk-launcher.sh ALLOCATOR_BACKEND=mimalloc PROFILE_ALLOC=1 BUILD=/home/justxls/mineswitch/runtime/launcher/build-nxvk-mimalloc OUTPUT=/home/justxls/mineswitch/runtime/launcher/MineSwitch-NXVK-Mimalloc
```

The pinned upstream source must already exist with the recorded patch applied;
the build does not silently download or replace source. Installed SDK files and
the prior verified1500MiB NXVK artifacts remain intact. Build receipts include
source/archive hashes; upstream embeds build date/time, so unchanged rebuilds
may have different archive/binary hashes.
