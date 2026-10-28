# Project-local newlib allocation-width repair

The deployed devkitA64 PIC allocator rejects padded `malloc` and `realloc`
requests larger than `INT_MAX`. `nxvm_reserve` uses `memalign`, which delegates
to that allocator, so a 2 GiB Java heap is rejected before backing allocation
or page permissions are attempted. The deployed launcher matching
`f54dc93374a18c3661c9c71f30ce80c2ca422b0ba3ffbed7cb22cdca64db908b`
contains this check in `_malloc_r` at ELF address `0x10b01c4`.

The overlay uses the original allocator and its existing shared data. It
changes the padded request limit to `PTRDIFF_MAX - MINSIZE`, matching the
target's signed 64-bit `sbrk` increment while retaining headroom for signed
chunk-size comparisons, and retains the request-rounding overflow check.
Heap-growth padding is checked before adding the top pad, chunk overhead and
page rounding, so a large positive request cannot become a negative increment.
The rejected-size `realloc` path also releases its already-held allocator lock;
the unmodified source and installed binary omitted that unlock.

The patch does not change chunk layout, alignment, free-list globals, locks,
backing ownership or `sbrk`. Failed allocations remain failures. No installed
SDK files are changed. The Java heap remains physically backed by the same
finite process heap, and available-byte totals do not promise a sufficiently
large contiguous free block.

## Source and ABI verification

[provenance.json](provenance.json) pins official newlib `4.6.0.20260123`, its
published devkitA64 `r29.2` packaging/patch, raw source hashes, compiler version,
and the installed PIC `libc.a` hash. The three allocator source files are
byte-identical in the official upstream archive and the pinned devkitPro
mirror; the vendor patch does not change these files. Original license notices
are preserved in `vendor/_mallocr.c`.

Before building the repair, `build.py` compiles all three **unmodified** wrappers
with the installed compiler and headers. It requires exact equality with the
SDK members for all allocated ELF sections, global symbols/types/sizes, and
symbolic relocations. This includes byte-for-byte matches of `_malloc_r`
(2112 bytes), `_realloc_r` (1384 bytes), and the allocator's shared data.
The patched build must retain all original shared-data bytes, sizes, alignments
and relocations, plus exactly the same defined symbol names. A changed SDK or
source fails this check and requires another audit.

## Build and link

```sh
python3 runtime/memory/newlib/build.py
```

The build enters `runtime/limit-build.py`, applies the reviewable
[allocator-width.patch](allocator-width.patch) to a private build copy, runs
the host regressions and the unmodified negative control, and publishes:

- `build/libmineswitch_newlib.a`
- `build/mallocr.o`, `build/reallocr.o`, and `build/malignr.o`
- `build/verification.json`, including hashes and ABI comparison results

There are no build-time downloads. `--toolchain DIR` selects another installed
toolchain that must match the pinned audit; `--output FILE` copies the verified
archive to an explicit output path.

Link all three overlay members before the SDK libraries, either as explicit objects
or using a bounded whole-archive section:

```make
-Wl,--whole-archive /absolute/path/libmineswitch_newlib.a -Wl,--no-whole-archive
```

The archive supplies `_malloc_r`, `_realloc_r`, `_memalign_r`, `__malloc_av_`,
`__malloc_current_mallinfo`, `__malloc_sbrk_base`, `__malloc_top_pad`,
`__malloc_trim_threshold`, `__malloc_max_sbrked_mem` and
`__malloc_max_total_mem`. SDK `free`, `calloc`, usable-size, trim and
statistics continue using that same allocator state and layout. The final link
map must select the overlay `mallocr.o`/`reallocr.o`/`malignr.o` and omit SDK
`libc_a-mallocr.o`/`libc_a-reallocr.o`/`libc_a-malignr.o`, ensuring one owner for that state.

The **1619 ASan/UBSan host checks** exercise the actual allocator source using a bounded real anonymous
memory mapping, a test `sbrk` boundary, and tracked recursive locks. They cover
2 GiB allocation and alignment, first/last data, realloc retention, invalid
size/padding rejection and balanced locks. They do not establish Horizon page
permission behavior. The coordinator's device probe must validate at least
2 GiB reservation, first/last-page commit and data, uncommit/recommit zeroing,
whole release and genuine failure results before Minecraft validation.

The fragmented-metaspace regression uses separate live allocations to isolate
40 MiB free chunks. With at least 160 MiB genuinely free, a 64 MiB request with
16 MiB alignment fails while a 16 MiB root node succeeds with real writable
first/last pages. This proves the allocator consequence of fragmentation; it
does not establish the exact layout of a particular console crash.

Device probe `logs/mineswitch-large-heap-probe-20261006T145921Z.log` passed
39 checks, including actual 2 GiB/2 GiB+64 KiB reservations, kernel permissions,
commit/uncommit/recommit and complete release. Game run
`logs/MineSwitch-Launcher-20261006T151305Z.log` then created the full 2 GiB JVM;
it later failed reserving a 64 MiB metaspace node at 103.798 seconds. That later
fragmentation/native-memory issue is separate from the fixed INT_MAX guard.

## Exact aligned reuse after padding failure

The 15:34 full game run still failed with 16 MiB metaspace nodes. At failure
the allocator reported 135,848,048 bytes in free chunks and 19,894,272 bytes
unclaimed at the end of its process heap. Newlib's usual memalign path first
requests `size + alignment + overhead`, so a 16 MiB/16 MiB-aligned request can
fail before considering a genuinely suitable existing smaller free chunk.

The audited `malignr.o` overlay preserves the existing fast path. Only after
that padded malloc fails, it inspects the ordinary bins and last remainder
under the same recursive allocator lock. A candidate must actually contain the
requested aligned interval plus valid leading-chunk space. It is unlinked and
marked owned, then goes through Newlib's original leading/trailing split and
free routines. It does not fabricate capacity, change page permissions, move
live allocations or promise success where no suitable interval exists.
Oversized alignment arithmetic is checked before addition.

Genuine bounded host cases cover both aligned-start/tail-split and misaligned
free-prefix splitting, every neighbor's data, full returned writable contents,
release/coalescing, and invalid alignment. Unmodified source fails both
negative controls at the intended padded-allocation failure.

Native probe `logs/mineswitch-large-heap-probe-20261006T154301Z.log` passed
48 checks. Its additional case held 3150 separate 1 MiB allocations while
reserving a freed 16 MiB aligned interval through the actual nxvm permission
backend. Kernel permissions, endpoint writes, neighbors, release and no leaked
reservation all passed. Full Minecraft validation is a separate next step.

## Locked free-block diagnostics

`free_snapshot.c` includes the pinned allocator declarations with no allocator
implementation selected. It reads the existing bins/top under the same malloc
lock, never allocating, logging while locked, moving chunks or changing policy.
The build verifies that its separate archive member exports only the four
diagnostic functions, and that the repaired allocator still has the original
shared-data ABI and defined symbols.

A weak observer in `_malloc_r` runs only after genuine heap-growth failure and
before unlock. It saves a per-thread snapshot: original/padded request, actual
free-chunk total, largest usable request, fitting-chunk count, top size and nine
size buckets. Public allocator wrappers reset stale samples before each outer
call and print the snapshot after return. A recovered internal memalign failure
is not reported as a failed public allocation. If the failure path did not call
the observer (e.g. invalid size), the report explicitly says
`after_failure_locked`, rather than claiming an exact failure-time snapshot.

Free-list addresses, sizes and reciprocal links are checked before following
them. Traversal is bounded at262144chunks; `invalid`/`truncated` reports must not
be treated as a complete inventory. Chunk-size histograms include headers;
`largest_request` accounts for payload overhead and the top's required minimum
remainder. Unclaimed sbrk space is reported separately by the launcher.

The new suite passes1855ASan/UBSan host checks, including a real2.5MiB failure
with more than160MiB free across smaller holes, agreement with independent
mallinfo, allocation of the reported largest request, real coalescing, data
preservation, and400failures across four independent reporting threads. No
ordinary allocator behavior is changed by this diagnostic observer. Target TLS
uses libnx's soft thread pointer and local-exec model. The device candidate is
`MineSwitch-NXVK-Fragmentation.nro`; device diagnosis is recorded separately.
