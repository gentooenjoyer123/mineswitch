# Native stack guards: SDK mapping contract

The native JVM cannot treat libnx stacks as ordinary permission-changeable heap
reservations. Device evidence:

- `logs/mineswitch-jit-memory-probe-20261006T041229Z.log`: the RW/RX arena,
  real generated execution, mutable metadata and cross-core repatching passed;
  `stack.guard_none` failed.
- `logs/mineswitch-stack-guard-probe-20261006T041808Z.log`: both loader main and
  a created 4 MiB worker have valid stack bounds and memory type 11. Both reject
  `svcSetMemoryPermission` with `0xD401` (`InvalidCurrentMemory`). This is not
  the helper's active-SP exclusion.

The SDK source audit uses libnx commit
`feebd026ca0f5dcc2119f46ad8e0d16ad3dd4973`, with the relevant calls and structure
layout checked against installed `thread.o` and public headers. Local evidence
is under `runtime/generated/native-aarch64-audit/libnx-thread.c`,
`thread.o.asm` and `libnx-virtmem.c`.

## Actual stack layout

`threadCreate` allocates backing for stack, TLS and newlib reentrancy state, then
calls public `svcMapMemory(stack_mirror, stack_mem, aligned_allocation_size)`.
`Thread.stack_mem` records the source backing and `Thread.stack_mirror` the
destination. The reported usable `stack_sz` excludes the 48-byte
`ThreadEntryArgs`; TLS and reentrancy state are above that usable region.

The diagnostic observed a worker usable size of 4 MiB minus 48 bytes, with a
mapped allocation of 4 MiB plus 4 KiB. A guard must remain within the usable
stack bounds and never include the entry arguments, TLS or reentrancy state.

The loader main thread is different: `__libnx_init_thread` queries its existing
mapping and records `stack_mem=NULL`. There is no publicly recorded source pair
to unmap/remap safely. The current JVM launcher runs JNI_CreateJavaVM on a
libnx-created worker, which does have the owned pair. Native attachment of an
unsupported loader stack must fail explicitly rather than silently omit guards.

## Public mapping implementation

The installed public `svc.h` describes MapMemory as mainly used to add guard
pages around a stack. The backend for SDK-created workers unmaps the exact
bottom mirror pages using their original backing pair, and remap that same
pair to reenable access. This uses the normal public memory-map contract;
changing permissions of a Stack mapping is not assumed to work.

Required implementation invariants:

- Validate the current thread's recorded mirror/backing pair, usable bounds and
  distance from its active SP before touching pages.
- Track only guard holes created by this backend; an unrelated unmapped range
  is not implicitly a valid stack guard.
- Reserve the virtual interval in libnx's virtual-memory allocator before
  unmapping it, so another allocation cannot reuse the guard hole.
- Allocate guard tracking/reservation state before faults are possible. Later
  exception-handler unguard/reguard operations must not allocate or acquire a
  process-wide allocator lock that interrupted code may already hold.
- Preserve split red/yellow/reserved-zone semantics and roll back partial
  failures. An unmap guard changes real access, not just accounting state.
- Restore every hole before SDK `threadClose`, which unmaps the entire original
  mirror allocation. Release reserved VA only once it cannot expose live holes.
- MapMemory restores RW. Do not claim a read-only stack permission mode unless
  separately supported and verified.

`nxvm_stack_guard_prepare` allocates per-thread tracking and reserves the VA
while the complete SDK stack mapping is still present. Guard/unguard operations
validate each mirror/backing page pair and use real public unmap/map operations.
They retain the original bitmap until all changes succeed, using it as the
rollback plan on ordinary failure. A failed rollback aborts rather than leaving
the VM with inconsistent accessibility. `nxvm_stack_guard_cleanup` restores all
remaining holes before removing the VA reservation. Native OS thread creation,
attachment and teardown call these lifecycle hooks; Zero does not.

## Device proof

The root-controlled run
`logs/mineswitch-stack-guard-probe-20261006T043128Z.log` passed **28 checks with
zero failures** and returned to the loader. Tested NRO SHA256:
`b14daf75aead0bb92e0abe0228c411636db23916ccc894daef46e619d261cadb`.

The probe established actual inaccessible guard pages, partial and mixed
red/yellow remapping with preserved contents, rejection of the active stack and
backing alias, cleanup/reprepare, and successful SDK whole-stack `threadClose`.
Its registered faulting store entered the full alternate-stack C dispatcher,
remapped the real guard page there, and retried the same store successfully.
The unsupported loader stack was explicitly rejected as intended.

The complete native JVM subsequently passed 31 C2 contract checks in
`logs/mineswitch-jit-20261006T053717Z.log`: two real StackOverflow recoveries and
reguards while the recursion method remained compiled, FP edge cases,
polymorphic dispatch, and GC plus stack walking of a live compiled loop with
preserved FP/object values. JVM destruction returned zero. This adds actual
generated-Java runtime evidence to the isolated public mapping/exception proof.
