# Native AArch64 HotSpot exception support

The implementation lives in
`.upstream/openjdk21/src/hotspot/os_cpu/horizon_aarch64`:

- `exception_horizon_aarch64.S` supplies the strong libnx entry.
- `horizon_aarch64_exception_support.c` owns a separate 128 KiB alternate stack
  per VM thread, with compile-time checks against the public libnx frame ABI.
- `horizon_aarch64_context.h` describes the complete saved context.
- `os_horizon_aarch64.cpp` routes known HotSpot stack guards, polling-page reads,
  implicit-null sites, nonentrant method traps and fast-JNI slow-case sites.

The SDK/kernel frame saves x0–x8, LR, SP, PC and PSTATE. Other GPRs and NEON
registers remain live on entry. The adapter saves them before calling C/C++,
uses its own thread's alternate stack, and restores all 31 GPRs, all 32 NEON
registers, FPCR/FPSR and saved PSTATE before `svcReturnFromException`. Neither
the default shared libnx exception dump nor its shared fatal-handler stack is
used. Unknown faults and recursive dispatch stay unhandled.

The installed GCC/libnx `__aarch64_read_tp` helper is a leaf containing only
`mrs x0,tpidrro_el0; ldr x0,[x0,#504]; ret`. The adapter calls that standard
toolchain helper, then resolves its own local-exec TLS symbol. Audit artifacts
are in `runtime/generated/native-aarch64-audit` and
`logs/native-exception-entry-linked.txt`. This helper's register contract must
be checked again if the pinned toolchain changes.

The original SafeFetch leaf loads are checked first, including before VM thread
initialization and after cleanup. `NXVM_HOTSPOT_AARCH64` excludes only the legacy
SafeFetch exception entry from the native build. Zero and the existing SafeFetch
probe keep the established handler. Native linkage explicitly forces
`__libnx_exception_entry`; the installed SDK fallback is weak and the new entry
is strong.

The standalone probe uses the same support and assembly with a test dispatcher
for one registered load and one exact `dcps1 #0xdead` instruction. It checks the full register state before
and after dispatch, verifies that the C callback runs outside the ordinary
thread stack, calls the public own-thread `svcGetThreadId` inside dispatch, and
performs 500 recoveries on each of two permitted cores. It does not claim to
exercise HotSpot's higher-level safepoint/GC/exception continuations.

```sh
python3 runtime/limit-build.py -- make -C runtime/native-exceptions/probe -j6
```

Output: `runtime/native-exceptions/probe/mineswitch-native-exceptions-probe.nro`.
The initial data-abort-only NRO SHA256 was
`9e1551dcff8f85ed5ef33c5ad9c8f5bb9ddb91466bfeba3f24d8a8649410e823`.
That initial two-core run passed 18 checks, no failures and `complete=1`.

The root-controlled device run on 6 October 2026 passed **18 checks, zero
failures, complete=1**, including all register state, alternate stack/SVC use,
and 1,000 recoveries across permitted cores 0 and 1. It returned to the loader.
Evidence: `logs/mineswitch-native-exceptions-probe-20261006T034628Z.log`.
The tested NRO hash is the value above. This establishes the tested native
fault ABI, not yet HotSpot's higher-level safepoint/GC continuations.

The extended trap probe passed **21 checks, zero failures, complete=1** in
`logs/mineswitch-native-exceptions-probe-20261006T035930Z.log`, again returning
to the loader. SHA256:
`9fce782c69c3537e7f7e83d518691973dc6386a961dcae80458a840537cf1a1e`.
The exact upstream HotSpot `dcps1 #0xdead` instruction reported ESR `0x02000000`
(exception class 0) and description `0x00000100`, with the full register state
restored. The native VM dispatcher already recognizes exception class 0 and the
exact nonentrant instruction. It does not infer the instruction class from the
SDK's broader description enum.

`runtime/tests/mineswitch/probe/NativeContractProbe.java` extends the root-owned
WhiteBox JIT proof with native FP semantics, repeated StackOverflow recovery,
polymorphic transitions, and GC/stackwalk of a running compiled loop with live
FP and object references. Real warmups resolve referenced classes before C2
compilation; the method must remain compiled at the requested level before and
after each live-loop GC/stackwalk. Host level-1 and level-4 runs each passed 31
checks (`logs/native-contract-host-level1.log` and `level4.log`). The native
Horizon C2 run `logs/mineswitch-jit-20261006T053717Z.log` subsequently passed all
31 checks, including actual SOE/reguard and live compiled-loop GC/stackwalk,
with successful JNI shutdown. It uses an idle-queue re-enqueue loop to avoid
WhiteBox's per-method compilation request coalescing across C1/C2 tiers; exact
requested-level assertions remain in place.

The root agent exclusively owns device execution. The native JVM uses the real
code-cache alias/publication backend and protected libnx stack pages. These
subsystem tests do not establish Minecraft world rendering or game performance.
