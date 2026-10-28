# Horizon memory backend for HotSpot Zero

`nx_memory.c` implements real page access permissions through documented libnx
`svcSetMemoryPermission`. It is compiled as C to keep libnx's `Thread`/`Mutex`
names out of the HotSpot C++ namespace. `os_horizon_memory.cpp` adapts these calls
to HotSpot's memory interfaces.

Reservations obtain aligned newlib allocations with **eager physical backing**.
The kernel initially denies access. Commit enables read/write and zeroes newly
committed pages; uncommit denies access and causes a later commit to zero them.
Uncommit **does not return physical memory**. Full release restores read/write
before returning storage to newlib. Partial release creates inaccessible holes;
the original allocation is retained until the last live pages are released.
Statistics report backing separately from reserved and committed bytes.

Protection changes split at old-permission boundaries, as required by Horizon.
The backend records exact per-page state and rolls protection back on a syscall
failure. Failure to roll back terminates the process rather than using a false
memory map. Unaligned ranges, holes, unowned memory, executable and fixed-address
allocations are rejected. No attempt is made to override loader/OS permissions.

This backend shares the existing libnx/newlib process heap with native libraries.
It must not be configured as additional memory beyond the measured process
budget. Start with a 64 MiB Java maximum heap. It is not suitable for reserving
large sparsely committed arenas without accounting for their entire backing.

Named immutable VM data files use an eager snapshot and may be read-only or
read/write. The backend opens its own descriptor and reads from it, leaving the
caller's file offset unchanged. The installed newlib declares `pread` but does
not provide its implementation; an `lseek`/`read` substitution on a shared caller
descriptor would race. File-descriptor-only maps, files renamed/replaced during
mapping, fixed mappings, executable maps, replacement maps, shared heap maps and
large pages are unsupported. Java NIO mappings need their own later implementation;
this is not a generic POSIX `mmap` substitute.

Horizon Zero's `ReservedCodeSpace` is data-only. The upstream Zero assembler
header states that it stores entry points in CodeBuffers without generating
assembly; its stubs are precompiled C++ functions. The shared code-space change
is guarded with `HORIZON && ZERO`; executable requests elsewhere still fail.
Native AArch64 HotSpot uses the separate `nx_jit.c` CodeMemory backend. Its
dual-address layout, eager commitment, exact whole-arena release and device
evidence are documented in `../../docs/HORIZON_JIT_MEMORY.md`.

Zero uses explicit stack-overflow checks. Native AArch64 uses the separate
owned-stack unmap/remap and exception-dispatch lifecycle documented in
`../../docs/HORIZON_STACK_GUARDS.md`. Loader stacks without a recorded backing
pair remain unsupported for native JVM attachment.

Build the bounded device probe from the repository root:

```bash
export DEVKITPRO="$PWD/.toolchain/opt/devkitpro"
export DEVKITA64="$DEVKITPRO/devkitA64"
export PATH="$DEVKITPRO/tools/bin:$DEVKITA64/bin:$PATH"
make -C runtime/memory/probe
bash scripts/run-nro.sh 192.168.3.177 runtime/memory/probe/mineswitch-memory-probe.nro
```

It reserves 32 KiB, verifies actual kernel permissions with `svcQueryMemory`,
checks mixed protection transitions, data preservation, zero-on-recommit,
trimming and release, and returns automatically. It never deliberately touches
inaccessible memory or executes generated code. A pass is evidence for these
memory contracts, not evidence that HotSpot or Minecraft starts.

Verified on the user's OLED in application mode on 6 October 2026:
**50 checks passed, 0 failed**, including loader return. Full log:
`logs/mineswitch-memory-probe-20261005T235312Z.log`. The process reports
`InfoType_SystemResourceSizeTotal = 0`, which rules out the documented
`svcMapPhysicalMemory` API for this process; the eager backing strategy remains
necessary without changing the host process configuration.
