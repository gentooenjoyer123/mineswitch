# Horizon thread placement and launcher scheduling

The process allows cores 0–2 in the existing device probes. There are two
different creation paths in this project:

- The launcher calls `threadCreate(..., 0x3B, -2)` directly for the 4 MiB Java
  bootstrap/render worker. It does not set a broader affinity mask afterward.
- HotSpot creates its subsequent native threads with `pthread_create`. The
  installed SDK's `__syscall_thread_create` queries `InfoType_CoreMask`, creates
  a thread with priority `0x3B` and default core `-2`, then calls
  `svcSetThreadCoreMask(handle, -1, allowed_mask)` before starting it. Thus these
  pthreads are already permitted to migrate across the process's allowed cores.

The second behavior is present both in the audited libnx source and installed
binary. Evidence: `runtime/generated/native-aarch64-audit/libnx-newlib.c`
(`__syscall_thread_create`) and `pthread-create.asm`. Do not assume all HotSpot
threads are confined to the default core.

The native port currently defaults `UseThreadPriorities=false`, so Java/HotSpot
priority requests do not alter these native SDK priorities. The loader main
thread inherits its priority/core mask. Its exact values require a read-only
`svcGetThreadPriority`/`svcGetThreadCoreMask` query; they have not yet been
established by these source checks.

The second native JVM attempt,
`logs/mineswitch-jit-20261006T050636Z.log`, showed startup log fragments advancing
at the supervisor's five-second heartbeat cadence. Small measured tasks such as
interpreter generation took milliseconds, but logging around them took seconds.
This occurred after normal console/log-pump mutex acquisitions became try-locks
for fatal-context safety, while the pre-graphics supervisor loop had no sleep.
That is strong evidence of busy-loop scheduling contention, rather than slow
code generation or a frozen process.

The root subsequently added a real 16 ms sleep to every ordinary supervisor-loop
iteration, retaining the existing graphics behavior and bounded fatal-report
path. The next C1 run created the VM in about 1.12 seconds and finished its real
compiled-method tests and shutdown in about 2.7 seconds, with no five-second
logging stalls. Evidence: `logs/mineswitch-jit-20261006T051055Z.log`, 45 checks
passed and JNI shutdown/process exit zero. C2 independently passed 45 checks in
`logs/mineswitch-jit-20261006T051252Z.log` with normal shutdown.

Broadening the direct Java worker's core mask is a separate future decision;
it is not needed to justify removing the supervisor busy loop. No priority or
affinity changes were made by this audit.
