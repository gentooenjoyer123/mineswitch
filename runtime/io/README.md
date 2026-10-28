# Shared Horizon file I/O for OpenJDK

`horizon_file_io.c` supplies the same descriptor-position locks to the VM,
libjava and libnio. Its `nxio_pread/pwrite` use seek, one read/write, and cursor
restoration under the same lock as ordinary IO, seek, truncate, stat and close.
`horizon_file_share.c` supplies independent logical SD descriptors over a
reference-counted native file backing. Each logical descriptor keeps its own
position and access flags. Positioned and vector operations also hold the
backing transaction lock, so other local descriptors cannot interleave an
append record or disturb its native cursor.
It does not emulate descriptor duplication, and socket operations must remain
in the socket layer. Vector operations preserve a single regular-file position
transaction and report actual short transfers or failures.

The pinned newlib descriptor table has **1024 entries**. This was checked in
`libgloss/libsysbase/handle_manager.c` at
`57bd8e1ade3cf46d2e61a72516ac8bfaf8fdfd62` and in installed `libsysbase.a`
(`handles` is 0x2000 bytes; `__get_handle` bounds-checks against 0x3ff).
`nxio_descriptor_limit` exposes that verified SDK capacity because `sysconf` is
declared but not implemented in the target libraries. Recheck on SDK upgrades.

The filesystem has no inode identifiers in the inspected libnx fsdev backend:
`stat`/`fstat` leave `st_dev` and `st_ino` zero. Horizon attributes therefore
return a null `fileKey`. Path-based identity uses canonical names obtained by
walking actual SD directory entries; ASCII case aliases resolve to the stored
spelling. Non-ASCII names match exactly; ambiguous or unverifiable case aliases
fail explicitly. Descriptor-only identity is unsupported without a stable key.
These checks prevent treating every zero-inode file as the same file during
copy/replace. Canonicalization is not an atomic transaction against concurrent
directory renames, just as the surrounding path APIs are not atomic.

Canonicalization deliberately avoids newlib `stat`/`lstat`/`realpath`: fsdev
implements file metadata by opening another read handle, which fails while a
writer holds the file. The implementation uses `fsdevTranslatePath`,
`fsFsGetEntryType` and directory enumeration instead; `.` and `..` are resolved
without opening a second data handle.

The Java default filesystem is the SD mount: native `sdmc:/` prefixes are
removed when returning an absolute path to Java, while `/...` native inputs use
the configured SD default device. Other devices are not silently interpreted as
Unix path elements. File-store capacity and flags come from `statvfs`; current
libnx fsdev reports `ST_NOSUID` without a read-only bit, so those flags cannot
establish writability for arbitrary other mounts.

The initial libnio port uses real read/write/seek/force/truncate/stat/directory
operations. Unsupported mappings, shared/region locks, descriptor aliases, Unix
domain sockets, inherited channels and POSIX-style native IO signalling remain
explicitly unavailable. Exclusive whole-file and selector transport bridges have
passed their native device probes; Java/Minecraft integration still needs its
own checks. IPv4 vector sockets use a single send/recv call with
gather/scatter buffers, preserving datagram boundaries. IPv6 multicast is not
implemented. NativeThread reports no native signalling capability via its
documented Java ABI; ordinary file close may wait for an in-flight SDK operation.

`python3 runtime/limit-build.py -- make -C runtime/io/probe -j6` builds the
regular-file integration probe within the shared six-thread budget. It creates
an exclusive uniquely named directory beneath `sdmc:/switch/`, writes at most
32 KiB to its own files, tests shared descriptor positions, canonical identity
and cleanup, then removes the owned files and directory. A worker timeout exits
the process and leaves only that named test directory for later cleanup.

Device verification on 6 October 2026: **47 passed, 0 failed**, including
canonical exact/case-alias/distinct identity while write handles were held,
concurrent sequential and positioned IO, flush/truncate and complete cleanup.
Log: `logs/mineswitch-file-io-probe-20261006T003733Z.log`. The first run exposed
three canonicalization failures caused by fsdev's extra read handles; its
44-pass/3-fail log is retained as `logs/mineswitch-file-io-probe-20261006T003309Z.log`.
Actual directory EOF has errno 0; actual SDK file dev/inode values are both 0.

The first real Java 21 run also passed `nio_size`, `nio_file_roundtrip`,
`nio_positioned_read`, `nio_cursor_unchanged`, `nio_positioned_data` and owned-file
cleanup through the actual JVM/JNI paths. See
`logs/mineswitch-jvm-20261006T004427Z.log` (21 Java checks total). This covers
basic file IO, not the newly prepared Java FileLock/selector changes below.

## Metadata while a writer remains open

The first actual Minecraft run hit `Files.walkFileTree` on its open download
journal, `downloads/log.json`. The SDK's path stat tried to open a second reader
and returned `TargetLocked`, surfaced as `EIO`. The failing stack is retained in
`logs/mineswitch-jvm-20261006T022345Z.log`.

`nxio_stat_path` and `nxio_lstat_path` now use the normal SDK operation first.
Only its exact `TargetLocked` failure enables a fallback: resolve the actual
canonical spelling, find a matching live shared backing, and call its real
fsdev `fstat`. Canonicalization happens before taking the registry/backing
locks, which keep close and backing replacement from racing the query.
If no backing matches, the real path query is
retried once to handle a concurrently completed close. Untracked/foreign writer
handles remain errors; no file sizes, inode IDs, or successful results are
fabricated.

The file-handle SDK caches timestamps at open. When its filesystem timestamp
metadata command provides fresh values, the bridge uses those with the same
timezone conversion as fsdev. Otherwise it retains the actual `fstat` values.
It never replaces file timestamps with the current clock. The device's observed
2051 timestamps also appear in native results; this change does not adjust them.

NIO stat/lstat, `java.io.File` path attributes, and HotSpot's public `os::stat`
wrapper use the bridge. The earlier descriptor-indexed implementation passed
its expanded native probe with **62 checks, 0 failed**,
including size after append/truncate, stat/lstat aliases, unchanged positions,
distinct writers, close/reopen, and rejection of an untracked writer. Cleanup
and loader return succeeded. Evidence:
`logs/mineswitch-file-io-probe-20261006T025559Z.log`; tested NRO SHA256:
`f30bb5984b58da8cfe90efe7dff94e9bd21fc992b3a5f232adc07ced5bac119b`.

`HelloHorizon --writer-metadata-only` provides a staged Java integration fixture
covering `Files.readAttributes`, `Files.size`, last-modified timestamps,
`java.io.File`, aliases, and the same FileTreeWalker path used by the cache
vacuum. It compiles; device verification for this change used the focused native
probe, leaving the actual client retry as the integration check.

## Exclusive whole-file locks

`runtime/io/probe/locks/mineswitch-fs-lock-probe.nro` 0.1.0 observed real file
handles through two independent SD filesystem sessions. With a writer held,
another writer, reader, rename and delete all failed with FS `TargetLocked`
(`0x00000e02`, module 2 / description 7). Readers can coexist, but exclude a
writer. Closing the writer permits a new writer and rename. All 12 administrative
checks and cleanup passed: `logs/mineswitch-fs-lock-probe-20261006T004216Z.log`.

The prepared whole-file adapter uses this **mandatory write-open exclusion**,
not a process-local list as a substitute for an OS lock. Writable SD descriptors
record a canonical pathname string. A grant checks the live descriptor and
revalidates `TargetLocked` for an attempted writer from an independent filesystem
session; success or any other error refuses the grant. Canonical strings are
used by Horizon's Java `FileKey` only for overlap checks, never as inode numbers.
Unknown descriptors and unsupported lock forms throw errors.

Java `FileLock.release()` removes the Java claim; Horizon's separate mandatory
write-open restriction continues until `FileChannel.close()`. This stronger
native lifetime must be kept explicit. Minecraft's session-lock owner releases
then immediately closes its channel, which matches that lifetime. Closing
without an explicit release also clears descriptor bookkeeping. Native
`TargetLocked` on SD open is translated to `EACCES`, allowing Java's filesystem
provider to report `AccessDeniedException` instead of an uninformative `EIO`.

The 0.2.0 probe verified this bridge using Minecraft's **WRITE-only** open mode:
acquire/release/reacquire, competing open, close/reopen and unknown-descriptor
behavior all passed, along with the preceding native observations: **29 passed,
0 failed**, complete cleanup and loader return. Evidence:
`logs/mineswitch-fs-lock-probe-20261006T010439Z.log`. Java FileChannel lock tests
and Minecraft world-load/save tests are still required; a native probe is not
evidence of those behaviors.

## Multiple Java opens of one SD file

The Fabulously Optimized startup log
`logs/MineSwitch-Launcher-20261006T084040Z.log` exposed two ordinary operations
that the original one-native-handle-per-open implementation could not support:

- FerriteCore 7.0.3 loads properties from an input stream, retains that stream,
  and then rewrites the same file with `Files.write`. Its exact
  [tagged source](https://github.com/malte0811/FerriteCore/blob/fa7e105e8f21578765c94d9a07e45ddbc606b0db/Fabric/src/main/java/malte0811/ferritecore/mixin/platform/ConfigFileHandler.java)
  shows the read at line 36 and write at line 53.
- CrashAssistant nests writable channels around a configuration lock. The
  second channel must open before Java can report `OverlappingFileLockException`.
  That expected overlap is handled by its
  [configuration helper](https://github.com/KostromDan/Crash-Assistant/blob/93fd8960ed4f9e55a9f5c34ab2f8ce2c4308fc14/common_config/src/main/java/dev/kostromdan/mods/crash_assistant/common_config/config/CrashAssistantConfig.java#L623).
  The device log's 1.11.9 build failed at the earlier native open instead.

The bridge now allocates real newlib descriptors through a private devoptab.
Several local opens share one genuine fsdev file object; that native object
does not consume an extra descriptor slot. Each logical open enforces its own
read/write permissions and has its own position. Truncate, append, flush and
data access call the real filesystem operations. Close ownership is in the
devoptab callback, so both `nxio_close` and ordinary libc `close`/`fclose` release
references correctly. Path metadata and lock identities come from that live
registry, not stale bookkeeping indexed by a reused descriptor number.

A read backing is opened read-only. When a local writer arrives it is upgraded
under the file lock to read/write access, retaining the existing logical
positions. Horizon offers no atomic cross-process upgrade: the old read handle
must close before the write handle can open. If the upgrade fails, the bridge
restores native read access and reports the actual writer-open failure. If a
foreign owner also prevents restoration, reads report real errors until native
read access can be reacquired. No cached snapshot or successful result is used
to conceal that limitation. A claimed writable backing is never closed or
reopened for another local open.

Whole-file locks still require the independent-session `TargetLocked` check.
The registry additionally tracks which local logical descriptor owns the
claim, so a second local claim cannot succeed merely because both descriptors
share the same native handle. Closing an inner channel does not release an
outer channel's claim. Java's canonical `FileKey` continues to report overlap
through the normal JDK lock table. Releasing a Java claim does not remove
Horizon's stronger mandatory exclusion: it persists until the final local
descriptor using that writable backing closes. Shared/region locks, open-file
rename/delete semantics and native IO signalling are not newly advertised.

Validation for this change:

```sh
python3 runtime/limit-build.py -- python3 runtime/tests/file-sharing/run.py
```

The host harness compiles both production C sources with ASan/UBSan and models
only the previously observed SDK sharing policy; all contents, seek, truncate
and flush operations use owned real temporary files. It passed **506 checks**,
including nested writers, read-to-write upgrade, independent cursors, vector
append concurrency, failed upgrade/restoration, genuine error propagation and
libc-close/descriptor-reuse lifetime and EOF after another channel truncates.
Both C sources also cross-compiled with
the installed SDK and `-Werror`.

`runtime/launcher/java/mineswitch/probe/FileSharingProbe.java` exercises the
same ordinary operations through Java NIO and `InputStream`, including expected
overlapping locks and final cleanup. Its **19 reference checks** passed on
Temurin Java 21.0.11 on the host. Its default owned parent on the console is
`/switch/mineswitch/tests`; an optional parent argument supports host runs.
Device execution and the modpack retry belong to the root coordinator and
remain separate evidence. The preceding 0.1/0.2 native probe binaries and logs
are retained unchanged as historical baselines.

The first actual Java device probe passed its initial **13 checks**, including
the held-input rewrite, nested writer/overlapping-lock behavior and independent
positioned IO. It then exposed an SDK difference: reading at a retained
position beyond a newly truncated EOF returns an I/O error. Evidence is
`logs/MineSwitch-FileSharing-20261006T100647Z.log`. Shared reads now query the
backing's real size under its lock, return EOF at/past that size and clamp a
crossing read to the remaining bytes before calling the SDK. Failed size
queries and actual in-range read failures remain errors. The host SDK model
now includes the observed past-EOF rejection.

The coordinator's next actual Switch run passed **all 19 Java checks**, including
EOF after truncate, concurrent append and owned-file cleanup, with JVM result 0
and a successful full application exit. Exact evidence:
`logs/MineSwitch-FileSharing-20261006T101814Z.log`. The following unchanged
Fabulously Optimized modpack run passed the CrashAssistant/FerriteCore sharing
failures and reached client initialization; its separate module/UI errors are
recorded in `logs/MineSwitch-Launcher-20261006T102342Z.log`. This establishes the
tested IO behaviors, not unrestricted POSIX filesystem or complete modpack
compatibility.

## Read-only JAR read-ahead

The shared backing caches bytes only for canonical `.jar` paths (ASCII case
insensitive) while its genuine native handle is open read-only. The measured
SDK sharing rules exclude an independent writer for that whole native-handle
lifetime. Mutable backings, including readers sharing a previously upgraded
read/write handle, keep the direct read and real size-query path.

A successful real size query can be reused for reads and `SEEK_END` during that
uninterrupted read-only JAR lifetime. Explicit metadata queries still call the
native backend. The first small read lazily allocates one 32 KiB slab containing
eight 4 KiB pages. Each missing page is read from its aligned boundary, clipped
to the verified EOF. Least-recently-used page replacement retains separated
header/directory ranges and supports reads before the original requested
offset. Cross-page requests copy the actual fragments in order. Requests larger
than 32 KiB stay direct. The process reserves at most 8 MiB for page slabs; a
full budget or allocation failure falls back to direct data reads. There is no
cross-backing eviction or background worker.

The backing mutex protects size, pages, native cursor and logical positions.
Upgrade, native close, reopen, write and truncate discard cached size/data
before attempting the operation, including failed upgrades or failed reader
restoration. A final logical close frees the slab; closing one of several
readers retains the still-open native lifetime. Reads on a backing whose native
access could not be restored continue to report the real error. Reopened
handles verify size and fetch data anew. Real size/seek/read errors and actual
short reads are not converted into a successful full transfer. An incomplete
page publishes only its initialized prefix. If that prefix ends before the
requested offset, one direct read at the actual requested position replaces
the attempt; there is no refill loop or EOF inferred from unrelated prefix
bytes. A later-page error or short transfer returns only the actual earlier
progress. A real zero returned at the requested page boundary remains zero and
is never cached as an EOF result.

`nxio_shared_io_snapshot` exposes cumulative, relaxed atomic counters without
filesystem calls or registry/backing locks. `logical_reads` counts shared read
attempts, including EOF/errors; `native_reads` and `native_stats` count actual
backend callback attempts. `cache_hits` counts whole requests served entirely
from pages already present;
`size_cache_hits` counts reused lengths. `logical_bytes` counts bytes returned
to callers and `native_bytes` counts actual fetched bytes, including read-ahead.
`cache_bytes` and `cache_peak_bytes` report current/peak allocated capacity.
Snapshot fields may represent slightly different instants during concurrent IO.
These counters support measuring this optimization on the device; host tests
do not establish a Minecraft startup-time improvement.

The expanded host suite passed **3089 ASan/UBSan checks**, retaining the prior
506 sharing checks. Independent backend counters verify that seven reads after
one page fill issue zero additional native read/seek/stat calls, and 27 tiny
alternating/backward header/directory reads fetch only three pages (12 KiB),
with 24 whole-request cache hits. Coverage also includes eight-slot LRU,
cross-page requests, 600 concurrent positioned reads, actual short fills and failures,
EOF, independent writer exclusion, changed contents after native lifetime
breaks, mutable JAR backings, and filling/releasing the entire 8 MiB quota.
The production source cross-compiles with the installed SDK and `-Werror`.
Its containing server-JDK archive was rebuilt with:

```sh
python3 runtime/limit-build.py -- env MINESWITCH_JVM_VARIANT=server \
    bash runtime/build-horizon.sh java.base-libs-only
```

The earlier single forward-window implementation reached client startup in
159.303 seconds in `logs/MineSwitch-Launcher-20261006T131907Z.log`, but its
startup snapshot fetched 1.47 GB for 93.5 MB delivered. The same run included a
GLFW wait correction, so its improvement over the preceding run is not an
isolated cache benchmark. Aligned pages address the measured excessive reads;
their Minecraft startup time still requires a new device run.

## Pipe and selector transport

Horizon has no POSIX pipe/socketpair implementation. `horizon_pipe.c` creates
an actual connected IPv4 loopback pair with a five-second setup deadline,
verifies the accepted peer matches its own client, and supports blocking and
nonblocking modes. Java's existing Pipe implementation and PollSelector use
these source/sink descriptors through `IOUtil.makePipe`.

The default SDK has three BSD IPC sessions. Blocking polls hold those sessions,
so several infinite polls could prevent another thread from sending a wakeup.
`nxio_socket_poll` releases the session every 50 ms while retaining the original
deadline; it returns only real readiness, an actual error, or the requested
timeout. PollSelector and Net.poll use this wrapper. Connected TCP close first
calls shutdown to release a blocked receive, then closes the descriptor.
Other cancellation cases (for example a blocked listener accept) are not yet
covered by the device evidence.

The device probe at `runtime/io/probe/pipe/mineswitch-pipe-probe.nro` passed
**74 checks, 0 failures**, including a real 120 ms timeout, a worker wakeup in
100.10 ms, vector payloads, EOF, shutdown releasing a blocked receive, and four
concurrent pollers sharing the three-session pool. All workers/descriptors
closed and the probe returned to Sphaira. Log:
`logs/mineswitch-pipe-probe-20261006T013104Z.log`. Production JNI wiring has
compiled; Java Selector/Pipe checks are the next integration gate.
