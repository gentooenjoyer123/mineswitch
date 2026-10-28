# Native HotSpot CodeMemory contract

`runtime/memory/nx_jit.c` uses libnx's public `JitType_CodeMemory` route.
It does not use process-permission services or change the loader configuration.
The native AArch64 VM and the proven Zero configuration have separate code-space
paths; Zero continues to use ordinary non-executable data reservations.

## Two addresses, one allocation

Every native code arena has simultaneous RW and RX aliases of the same physical
backing. Instructions are emitted and patched through RW; published machine PCs
and entry points are RX. Neither alias is RWX.

- `CodeHeap` allocation headers, `CodeBlob`/`nmethod` objects, relocation tables
  and mutable VM data stay at RW addresses.
- `CodeBlob` code/content bounds, nmethod code/constant/stub bounds and global
  `CodeCache` branch-distance bounds use RX addresses.
- A PC lookup translates RX to RW before examining the heap segment map, then
  returns the actual writable metadata object.
- Assemblers retain their logical RX PC when calculating instruction-relative
  offsets. Only write destinations are translated to RW. Temporary buffers
  outside registered arenas retain their ordinary addresses.
- Inline oop/metadata relocation cells are writable through RW even when their
  logical position in the instruction/constant stream is RX.

Translations are idempotent and preserve offsets. Interior addresses take
precedence over another arena's one-past-end bound. Translation only reads an
atomically published fixed registry; it does not allocate or take a mutex in
an exception or publication path. Creation and destruction are serialized.
Requested alignment applies to RW; RX is page aligned and preserves the
alignment required for code entries.

Instruction publication cleans the RW data-cache range with `armDCacheFlush`
and invalidates the corresponding RX instruction-cache range with
`armICacheInvalidate`. Cache maintenance on temporary assembler buffers uses
their actual address. These are real cache operations, not a success stub.

## Reservation, commitment and lifetime

CodeMemory pins its full physical allocation. Native `ReservedCodeSpace`
therefore calls HotSpot's existing `os::reserve_memory_special` contract, even
though the backing uses ordinary 4 KiB pages rather than large pages.
The existing OS wrapper records the complete range as reserved **and committed**
in Native Memory Tracking. `VirtualSpace` special-space growth adjusts the
logical code-heap capacity; it does not claim to commit physical pages lazily.
`actual_committed_size()` continues to report the full pinned range.

`nxjit_release_memory` only accepts an exact full RW base and size. An RX base,
offset range or partial range is rejected. `os::release_memory_special` records
NMT release only after the actual full arena close succeeds. All execution and
lookups must be quiescent before release. If SDK close fails after possibly
removing mappings, the process aborts instead of continuing with stale metadata.

CodeCache's segmented configuration creates logical views of one arena. Those
views do not independently own or release the backing; `VirtualSpace::release`
only clears its bookkeeping. Releasing an individual segment through
`ReservedSpace::release` is unsupported and will be rejected by the native
backend. The current small JVM probe uses one unsegmented code cache. Ordinary
per-method free/reuse remains a CodeHeap allocator operation inside the live
arena and does not close CodeMemory.

Partial physical release, executable uncommit, fixed executable placement and
changing these aliases to a different permission mode are unsupported. Native
code-cache budget must account for the entire reserved size. The bridge can
allocate extra backing to satisfy larger RW alignment; production code-cache
alignment currently equals the ordinary 4 KiB page size.

## Device evidence and remaining gates

`logs/mineswitch-jit-memory-probe-20261006T041229Z.log` reports 27 passes and one
failure. All CodeMemory checks passed: distinct aliases with actual RW/RX
kernel permissions, requested alignment, translation, generated return value,
actual RX PC, a PC-relative branch, mutable RW metadata read by RX code, and an
instruction update observed by another CPU core after warming the old code.
Whole-arena close and removal from the registry also passed.

The failure was the obsolete stack permission experiment. Its replacement
using the SDK worker's owned unmap/remap pair passed 28 checks separately in
`logs/mineswitch-stack-guard-probe-20261006T043128Z.log`; see
`HORIZON_STACK_GUARDS.md`. The original JIT-memory NRO is kept as the tested
artifact (SHA256
`9c8ec84c030247209c586309c86f57f037e757842d76dbcd8af20a113593f721`).

That probe precedes the exact-range release wrapper, translation boundary
priority adjustment and the complete HotSpot metadata/PC integration. It proves
the tested SDK memory and execution mechanics. Successful native JVM startup,
executed C1/C2 nmethods, GC relocation, exception handling, deoptimization and
recompilation still require the coordinated native JVM device probes.
