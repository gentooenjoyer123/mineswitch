# Whole-application exit after native JVM shutdown

The C1 and C2 probes returned success from Java main and `DestroyJavaVM`, but the
owner observed a real Horizon "software closed because an error occurred"
dialog. The old launcher called `svcExitProcess` directly. A zero Java result
does not prove a normal application-manager exit.

Returning into hbloader is also unsuitable while HotSpot daemon/reaper or
other native threads can still execute the NRO's code. The exit bridge must
terminate the entire application without unloading code or tearing down shared
services beneath those threads.

## SDK contract

The installed libnx implementation and pinned source agree:

- Application/SystemApplication self-exit uses `ISelfController::Exit`, command
  0, reached through the public SelfController session getter.
- LibraryApplet self-exit uses `ILibraryAppletSelfAccessor::ExitProcessAndReturn`,
  command 10, through the corresponding public session getter.
- After an accepted command, libnx waits for the application manager to
  terminate the process. It calls `svcExitProcess` only if the command fails.
- NRO return normally skips these whole-application commands and returns to its
  loader. The `__nx_applet_exit_mode` path also runs global service teardown;
  that path cannot simply replace our process exit while VM threads survive.

Source evidence is in
`runtime/generated/native-aarch64-audit/libnx-applet.c`, especially
`_appletCleanup` and `_appletExitProcess`. The public LibraryApplet
`appletRequestExitToSelf` notification is a different command, not the
application self-exit operation.

## Implemented bridge

`horizon_exit_process` now chooses the same SDK self-exit operation through the
public service session. It preserves all global services and native code until
the manager ends the process, without `appletExit`, `__appExit`, libc destructors
or hbloader return. An atomic one-shot owner prevents concurrent VM/supervisor
exit calls from racing a successful request with a later syscall fallback.

Clean status zero explicitly publishes `appletSetTerminateResult(0)`. Other C
statuses remain in diagnostics; they are not cast to packed Horizon Results
and are never explicitly published as success. The request/result markers
distinguish manager exit from the fallback on a genuine IPC failure. The
`report=false` path performs no stdio logging.

An optional launcher flush hook drains the NXLink queue only on the supervisor
thread. An exiting VM thread instead gives the independent supervisor a bounded
opportunity to drain. All launcher terminal paths use this bridge, including
successful JVM completion and intentional cancellation.

The bridge and native launcher objects compile with `-Werror`. Device run
`logs/mineswitch-jit-20261006T053717Z.log` then passed all 31 C2 native-contract
checks, completed `DestroyJavaVM` with zero, and issued application command 0
with clean terminate-result publication. The application manager terminated the
process before the optional post-request marker was printed. The owner
explicitly confirmed return to HOME **without the error dialog**, and reopened
Sphaira. This establishes the tested normal whole-application exit path.
Tested NRO SHA256:
`ee87509b46b02bd60f2533a5fb9e3590f6a854bc9540d69f81cf39baf77f0455`.
