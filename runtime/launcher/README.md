# Actual OpenJDK 21 JNI launcher

Build with `runtime/build-launcher.sh` after `runtime/build-image.sh` and the
native registry generator have produced `runtime/generated/`. The result is
`runtime/launcher/mineswitch-jvm.nro`. This contains the real HotSpot Zero
interpreter and Java 21 runtime image; a successful native link does not prove
that the JVM starts or that Minecraft runs.

The separate native paths produce `mineswitch-jit.nro` (JVM probes) and
`mineswitch-native-minecraft.nro` (the full Minecraft runtime). They use the
actual AArch64 interpreter/C1/C2 server archive and do not force `-Xint`.
Their device evidence is recorded in `docs/JIT_BRINGUP.md`.

The executable mounts its ROMFS and checks `jdk/files.manifest`. Each line is:

```
<sha256 hex> <decimal file size> <relative path>
```

Only those manifest paths beneath `sdmc:/switch/mineswitch/runtime/` are
installed. Existing correct files are retained by size/SHA256 comparison. New
contents are copied, hashed, flushed and closed before replacement. libnx
rename does not replace an existing file: the installer preserves it as an
owned `.mineswitch-old` backup, commits the verified `.mineswitch-part` file,
and rolls back on failure. An interrupted backup is recovered on the next run.
This is recoverable file-by-file installation, not a POSIX atomic replacement
of the whole directory. The VM starts only after every manifest entry passes.
Absolute paths, traversal components, device prefixes and installer scratch
names are rejected. Unlisted files are not removed.

A 4 MiB libnx thread calls the actual `JNI_CreateJavaVM` with Zero/Serial GC,
64 MiB maximum Java heap, explicit signal/perf/NMT settings, uncompressed
pointers and `-Djdk.image.map.all=false`. The last option matches the native
image reader's index-only mapping. The worker defines the actual Java 21
`HelloHorizon.class`, registers a JNI callback and invokes its `main` method.
It then calls `DestroyJavaVM`. Java assertions and pending JNI exceptions are
reported as failures; no stand-in VM or fabricated native result is used.

`nativeRoundTrip(long)` calls the Java `nativeCallback(long)` and adds seven,
so the supplied callback's `value * 3` yields 376 for an input of 123. Both
Java and native diagnostics go to the console and a queued NXLink stream.
The main thread renders the console, sends queued output and emits a heartbeat
every five seconds. Plus requests process exit; there is no arbitrary timeout
on slow Zero startup or test execution. The native worker's final exit wait is
bounded after it has reported completion.

HotSpot native daemon/reaper threads can survive `DestroyJavaVM`. After draining
logs the launcher requests normal termination through the public applet/AM
session and waits for the manager to terminate the complete process. Native
code, service handles and daemon storage remain alive meanwhile; it does not
run `appletExit`/`atexit` teardown or return into an unloaded NRO.
`svcExitProcess` is reserved for a failed manager request. The owner confirmed
graceful HOME exit on this path; reopen Sphaira for the next test.

Evidence of successful bootstrap requires the Java `MINESWITCH_JAVA_PASS`
marker, `JAVA_MAIN_END result=0`, `JNI_DESTROY_END result=0`, and the launcher
end marker with result zero. These checks cover the Java test source; they do
not demonstrate Minecraft, graphics integration, or usable performance.

The installer transaction tests can run on a host with a C compiler and
mbedcrypto installed:

```
python3 runtime/launcher/tests/installer_host.py
```

They compile the actual installer and model libnx's non-replacing rename plus
commit failures. They validate replacement, cached hashes, rollback, interrupted
recovery, corruption preservation, path validation and incomplete manifests.
They do not replace checks on SD hardware or in the actual JVM.

## Verified on the device

On 6 October 2026 the real OLED/Horizon 21.1.0 run completed all 21 checks in
`runtime/tests/HelloHorizon.java`. The log is
`logs/mineswitch-jvm-20261006T004427Z.log`. It reports Java
`21.0.13-internal`, class-file version 65, `JNI_CreateJavaVM=0`, both JNI callback
results, Unicode/exception checks, 128 MiB of allocation with 30 observed Serial
GC cycles under a 64 MiB maximum heap, a Java worker thread, the CSRNG-backed
provider, JRT class reads, NIO ordinary/positioned I/O and temporary-file cleanup.
`MINESWITCH_JAVA_PASS checks=21`, `JNI_DESTROY_END result=0` and the process-exit
end marker are all present. The tested NRO SHA256 is
`7968b0fdcb5525493933dde31861eee455143e046d1871ab31811c7b51b4fabb`.

This proves this Java bootstrap/integration test, not Java SE conformance,
LWJGL compatibility, Minecraft startup or game performance.

Subsequent native device runs passed C1/C2 WhiteBox JIT tests (45 checks each),
native runtime contracts (31 each) and LWJGL (24 checks, 90 frames). Those runs
all report successful VM creation/destruction. Exact logs and the scope of
each result are in `docs/JIT_BRINGUP.md`; they are separate from the historical
21-check Zero bootstrap described above.
