
## 2026-10-06: fragmentation measured; cache reclamation candidate

- Actual locked failure snapshot: requested2621440bytes, padded2621456,
  totalfree72712400, largest usable2621416,0fitting chunks,157605free chunks;
  validity/traversal checks both clean. Fragmentation is now directly confirmed.
- Added read-only allocator observer, largest-fit statistics and size histogram;
 1855ASan/UBSan host checks passed plus actual Switch failure capture.
- Prepared opt-in NVK idle-cache reclamation on failed public realloc, followed
  by one unchanged retry. No live GPU or Sodium resource-size changes. Both
  registry/cache locking are nonblocking during reclamation; lifetime and
  ownership host checks passed. Device rendering validation pending.
- Evidence and limitations: docs/NXVK_MEMORY.md,
  logs/fragmentation-confirmed-1948.json.

## 2026-10-06: owner-requested1948MiB test failed

- Same NXVK profiler binary, Xms1948m/Xmx1948m verified after correcting an
  accidental2048MiB UI selection by relaunch. Normal-world login succeeded.
- Fatal: LWJGL native realloc(NULL,2621440) ENOMEM twice in Sodium chunk
  meshing, then Minecraft crash report. Java heap was not full.
- Adjacent telemetry:171.2MiB aggregate native available,1.60MiB unclaimed
  allocator capacity,8KiB top,4.0MiB NVK cache. Largest free block unknown;
  fragmentation is suspected, not yet directly measured.
- Report already captured in NXLink and extracted as
  logs/crash-1948-20261006-client.txt; structured logs/heap-1948-failure.json.
  No mod reductions or automatic heap rollback were applied.

## 2026-10-06: NXVK normal-world benchmark and memory profiling

- NXVK/Zink native gameplay confirmed at Xms/Xmx1500m in the separate normal
  world on192.168.3.163:25577; old flat server preserved.
- Ordinary-world first minute58.44FPS included a1114ms CodeCache-triggered
  FullGC. After the owner-observed spontaneous cap release, separate minute
 110.71FPS, max interval73.06ms, no>100ms intervals. User-selected CPU~2192MHz,
  GPU768MHz, RAM2400MHz; agent changed no clocks or cap.
- Prepared separate NXVK memory-profiler NRO with physical backing/cache,
  JIT source-storage, VM reservation and large malloc-owner metrics. Host
  bookkeeping sanitizers and cross build passed; device profiling also passed.
  Thirty post-warmup samples show GPU backing547–585MiB, reuse-cache subset
 37–62MiB, aggregate availability530–590MiB, zero counter errors/dropped records.
- Found NXVK Horizon available-memory query returns total process allowance;
  correcting that and measuring Zink cache retention are next. No saving claimed
  and no cache/mod allocation policy changed in this profiling build.
- Current Sodium buffers are standard. The reductions described in older
  entries below were removed at the owner's request and are historical only.
- Details and reproducible evidence: docs/NXVK_MEMORY.md.

## 2026-10-06: native memory at world entry (historical experiment)

- Latest tested game before this adapter: Xms2048m/Xmx2048m, 96 MiB JIT
  cache and one early 192 MiB metaspace node, then 16 MiB growth.
  Actual `heap_committed=heap_max=2075918336`; Java startup103.384s.
- `logs/MineSwitch-Launcher-20261006T155948Z.log` passed the prior metaspace
  reserve failure but reports GL_OUT_OF_MEMORY at glBufferStorage, followed
  by an invalid16MiB map in Sodium MappedStagingBuffer. World login aborted;
  Controlify's missing-player guide NPE then produced the final crash report.
- Public exact Sodium0.6.13+mc1.21.1 jar was obtained from Modrinth; published
  SHA512 verified. Jar SHA256 e04599514d88e41765f710eeba59e3814909833396bc11b977f48fc2e7e50353.
- Read-only javap research remains outside the project at
  /home/justxls/mineswitch-recon/sodium-buffers.javap. Default constructor
  MappedStagingBuffer(CommandList) delegates with16777216 to the explicit
  capacity constructor; enqueueCopy routes copies larger than remaining
  space through the original FallbackStagingBuffer.enqueueCopy.
- New companion adapter changes just the default constructor constant to2MiB,
  preserves the explicit-capacity constructor, copy bounds, fences and fallback.
  Gated on Horizon/MC1.21.1/Sodium0.6.13 plus SHA256 of the exact target class:
  ec30adb4851565f17cea893404e578c83b225aba6f00b373ab75089d38d6513c.
  Other target bytes/platforms skip the adapter. Original mod jar stays untouched.
- Companion103 general checks and107 checks with the actual pinned jar pass;
  device verification remains pending. No third-party publishing performed.

-16:10Z game run logged the real2MiB staging constructor twice and no staging
 GL_OUT_OF_MEMORY. World entry next failed MemoryUtil.nmemReallocChecked from
 ChunkMeshBufferBuilder.start/reallocate. Java heap still had~1309MiB free;
 this is native builder storage, not Java heap exhaustion.
-Read-only exact-bytecode follow-up: ChunkBuildBuffers constructs a builder for
 each ModelQuadFacing for each default render pass with131072 initial vertices.
 ChunkMeshBufferBuilder.start reallocates to that initial capacity. Its original
 ensureCapacity/grow uses max(2*capacity,capacity+requested) and reallocates as
 geometry grows. New pinned constructor-only mixin selects4096 initial vertices;
 no geometry count, vertex stride, growth rule or render distance is changed.
 Both constructor class and unmodified growth class are fingerprint-gated.
 See /home/justxls/mineswitch-recon/sodium-mesh.javap for private inspection.

## Driver allocation investigation and owner constraint

Owner rejected reducing Sodium buffers; both memory-budget mixins were removed
from source/build/ROMFS. Only the verified file-save companion remains. The
budget experiment is preserved outside the project under
/home/justxls/mineswitch-recon/companion-budget-experiment. It did render a
world for a short interval but then failed C2 Chunk::new malloc934416 bytes;
full report: logs/hs165-report-transfer.log. It is not a stability success.

Native PROFILE_ALLOC diagnostic only (separate build-memory-profile/NRO):
public malloc/calloc/realloc/memalign/posix_memalign/free inventory, >=256KiB;
8 largest live blocks per5s sample; caller addresses resolved against matching
archived ELF with scripts/analyze-native-memory.py. Host ASan/UBSan verifies
real allocation ownership, failed realloc preservation and800 concurrent reuse
cycles. It cannot see allocations before trace start or all SDK private
allocator calls, and is not a startup benchmark. No allocation sizes changed.

Device profile logs163329Z and164638Z establish:
- Heap VA region8GiB, alias64GiB, stack2GiB, system resource0. Ordinary
  MapMemory stack-region reservations cannot fit a2GiB Java heap plus stacks.
- SDK nouveau_getparam(GRAPH_UNITS=13) disassembly returns constant0x1004.
- Public libnx hardware data: chipset0x120,1GPC,2TPC/GPC,gpc_mask1,TPCmask3.
- Real nvc0_screen_create allocates67633152 bytes=64.5MiB for GPU TLS.
  Mesa20.1 source computes allocation from the reported SM count. Actual2SM
  with identical per-thread parameters requires8519680 bytes=8.125MiB.
- Many16MiB BOs are nvc0_miptree_create textures; they must not be casually
  shrunk. A separate16MiB world-stage BO comes from nouveau_mm_allocate.

New runtime/graphics/nouveau_topology.c corrects only the public GRAPH_UNITS
query for0x120 from validated active masks; all other queries delegate. It does
not change SDK files, clocking, resolution, geometry or permission checks.
Pure host tests pass; device candidate165938Z awaits game start.

Research found SwitchGLES (GLES over deko3d; insufficient desktop GL coverage
for direct Minecraft/Sodium replacement) and NXVK (modern Mesa NVK+Zink with
claimed OpenGL4.5 core/EGL on Horizon). NXVK source is cloned at the pinned
commit in ACTIVE_WORK.json; no NXVK code has been built or run on device yet.
