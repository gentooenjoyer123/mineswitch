# Launcher validation — 6 October 2026

The native launcher has displayed its interface on the actual Switch OLED.
The root agent observed the initial library and focus navigation. The retained
[HDMI screenshot](../logs/switch-ui-keyboard-fabo-20261006.png) shows the Horizon
software keyboard with `fabo` entered and the `o` key focused. This confirms
basic interface rendering, navigation and SDK text entry. Later runs also
downloaded and persisted the user's Fabulously Optimized 1.21.1 instance from
Modrinth, and started Fabric 0.19.3 with 151 mods. Later launcher runs reached the modded game and the owned LAN server; see the
current performance evidence below. The active successful configuration is now
NXVK + Zink with Xms/Xmx1500m; larger heaps are deferred. Normal-world measurements
and the memory profiling plan are in [NXVK_MEMORY.md](NXVK_MEMORY.md).

That device observation belongs to the earlier UI NRO with SHA256
`e6493d281183c1738166a8079075ee5fe7149c8a36ac29b0faacdb96c1b73200`.
The screenshot is a stable copy of the observed capture, not a new device test.

| Requirement | Evidence available | Remaining functional validation |
| --- | --- | --- |
| Native launcher interface | Actual Switch library/focus observation and SDK keyboard screenshot; host UI smoke and ASan/UBSan smoke passed. | Complete device workflows, cancellation, restart and clean exit. |
| Offline profiles and SD instances | Shared storage and UI tests cover profiles/settings, per-instance paths and persistence logic. | Create/change a profile and reopen the NRO to confirm SD persistence on the device. Microsoft login is outside the offline-profile scope. |
| ZIP, MRPACK, CurseForge and MultiMC imports | Host importer suite passed 52 checks; catalog/manifest suite passed 167. | Import representative archives from the actual SD card, including missing/manual-file completion. |
| Modrinth and CurseForge catalogs | Official provider implementations and host catalog tests; device Modrinth pack import/persistence observed. An absent CurseForge key is rejected before its API request. | Live CurseForge requires an authorized application key; none is supplied or borrowed by the launcher. |
| Java 21 game preparation | Preparer suite passed 559 standard checks and 561 with real `read()` counters. Published-digest/stat receipts reuse unchanged assets, client and libraries; explicit resource validation bypasses them. | Cold/warm preparation and modded game launch verified; latest 2 GiB memory changes need a complete repeat. |
| Forge and NeoForge | Official Forge 52.1.16 (3 processors) and NeoForge 21.1.256 (6 processors) completed on host Java 21, followed by verified publication and normal loader plans. Original client hashes stayed unchanged. | Run the helper/reopen/game sequence on Switch. Host preparation did not execute either game. |
| Controller support in Minecraft | GLFW backend logic/replay work and documented mod/backend analysis exist. Launcher focus movement is actual device evidence for the launcher only. | Verify physical layouts, sticks/buttons, reconnects and a selected controller mod inside Minecraft. |

The previously completed direct-game MVP already rendered Minecraft 1.21.1 on
Horizon; its separate artifact and evidence are recorded in
[MVP_VERIFIED.md](MVP_VERIFIED.md). That result must not be attributed to the new
launcher artifact or to untested modpacks/controllers.

The owner corrected the console date from 2051 to 2026. A fresh process now
agrees with the Horizon USER/LOCAL clocks. An older process retains libnx's
startup time basis after a system-clock change, so the frontend requires a
restart when it detects that mismatch. TLS verification remains enabled.

## Current performance and memory work

All builds use `runtime/limit-build.py`: six jobs, six CPUs, nice 15.
Target is stable 30 FPS, controlled inside Minecraft/Sodium. The native GLFW
wait correction honors application waits and does not impose an FPS cap.
The agent has not changed clocks; device captures show user-selected custom
clocks, so these are not stock-clock comparisons.

| Run (UTC) | Preparation | Java startup | Result |
| --- | ---: | ---: | --- |
| [12:15](../logs/MineSwitch-Launcher-20261006T121517Z.log) | 80.351 s cold | 193.862 s | Modded game/server reached; subsequent Sodium settings crash diagnosed. |
| [12:37](../logs/MineSwitch-Launcher-20261006T123734Z.log) | 15.927 s warm | 192.725 s | Owner verified Sodium settings open/change/save/reopen after corrected companion. |
| [13:19](../logs/MineSwitch-Launcher-20261006T131907Z.log) | 15.889 s | 159.303 s | Forward JAR cache and real GLFW waits; owner confirmed CPU issue fixed. |
| [14:06](../logs/MineSwitch-Launcher-20261006T140648Z.log) | 11.060 s | Failed before main | SDK allocator rejected a 2 GiB reservation; repaired with verified newlib overlay. |
| [15:13](../logs/MineSwitch-Launcher-20261006T151305Z.log) | 11.029 s | Incomplete | Genuine Xmx2048m JVM started; new 64 MiB metaspace reservation failed at 103.798 s. |
| [15:27](../logs/MineSwitch-Launcher-20261006T152744Z.log) | 10.997 s | Incomplete | Native instruction trap at about 61 s; preceding available memory 719,777,232 bytes, no reserve failure. Extended fault diagnostics added for repeat. |

Warm receipts reduced verification reads from 915,888,430 to 1,845,368 bytes
with 3942 receipt hits. The final JAR cache uses eight aligned 4 KiB LRU pages
per read-only backing, a lazy 32 KiB slab and an 8 MiB global cap. It passed
3089 ASan/UBSan checks. At 101 s of the 15:13 run it had read 165,917,150 native
bytes for 92,467,550 logical bytes, compared with 1.468 GB native reads by
startup with the earlier forward cache. That is I/O evidence, not a completed
startup time for the new cache.

The 15:13 failure is native memory reservation, not Java heap exhaustion:
metaspace used 127.34 MiB of 128 MiB reserved, with no free chunks; another
64 MiB node could not be allocated. Approximately 217 MiB total native space
was available in the preceding heartbeat, but it need not be contiguous.
The complete `hs_err_pid155.log` was retrieved read-only through the diagnostic
NRO (`--jvm-error=155`), archived in [report transfer](../logs/diag-report-transfer.log).
Horizon now uses one 16 MiB root per metaspace reservation and requests aligned
reservations directly. No Java Xmx reduction or fake lazy backing is used.

The newlib overlay initially passed 640 host checks including an actual fragmented heap
regression, and the [large device probe](../logs/mineswitch-large-heap-probe-20261006T145921Z.log)
passed 39 checks for allocations over 2 GiB and real page permission/lifetime
contracts. `Xms` is 256 MiB; the selected `Xmx` is 2048 MiB. Serial GC reports
an effective heap maximum of 2,075,918,336 bytes because a survivor space is
excluded from its reported capacity.

Launcher RAM policy uses actual Horizon process allowance: at most 2 GiB for
normal/unknown allowances up to 4 GiB; expanded budgets reserve 1 GiB for native
use, round down to 512 MiB steps and cap at 6 GiB. Host policy tests pass; no
physical 8 GiB modified console has been tested. Gameplay/native headroom with
the latest 2 GiB configuration still requires device validation.


The [15:34 repeat](../logs/MineSwitch-Launcher-20261006T153437Z.log) did not
reproduce the instruction trap, but failed reserving a 16 MiB/16 MiB-aligned
metaspace node after resource loading. Failure diagnostics reported 135,848,048
free arena bytes plus 19,894,272 unclaimed heap bytes. The original Newlib
memalign path asks malloc for the worst-case alignment padding first.

An additional audited `malignr.o` overlay now reuses exact aligned intervals in
existing free chunks after the padded request fails. It preserves original
split/coalescing routines and allocator locks. The complete suite passed
1619 ASan/UBSan checks, including original-source negative controls for prefix
and tail splitting. The [device probe](../logs/mineswitch-large-heap-probe-20261006T154301Z.log)
passed 48 checks including an actual aligned reservation while 3150 neighboring
1 MiB blocks remained allocated. The [15:44 game retry](../logs/MineSwitch-Launcher-20261006T154453Z.log)
is under observation; probe success alone does not prove game stability.

## Earlier device diagnosis

- [File-sharing probe](../logs/MineSwitch-FileSharing-20261006T101814Z.log):
  all 19 checks passed on Switch. Independent Java handles can share a real
  SD backing file, including concurrent read/write use and reads at EOF.
- [Following pack run](../logs/MineSwitch-Launcher-20261006T102342Z.log):
  the previous file-sharing failures disappeared. Startup then failed because
  Debugify required the standard `java.net.http.HttpClient` module, which was
  absent from the reduced image. Loading-screen activation is logged, but
  startup failed before the GLFW graphics handoff.
- [Standard API probe](../logs/MineSwitch-StandardApi-20261006T112643Z.log):
  all 7 checks passed, including Java 21, module resolution, the genuine Unix
  `Insets.initIDs` initializer, and a real `HttpClient` GET to the owned LAN
  fixture. JFR remains excluded. Probe SHA256:
  `cd7b73328a08dc7edeeae54e00ae6f300b1b91cf763aea8bb4a6ef59be5121c1`.

The first HTTP probe's connection failure is not runtime-defect evidence:
its host fixture had already exited. The successful repeat keeps the fixture
and serialized NXLink session alive in one runner.

Slow preparation previously rehashed 3,888 unique asset objects totaling
824,771,773 bytes and repeatedly inspected parent directories. Persisted
validation receipts and the instance action **Проверить ресурсы** are now
implemented. Host syscall counters prove zero content reads for an unchanged
cached asset and real reads in forced mode. Metadata-preserving changes require
the explicit full resource check. Runtime overlays remain fully hashed.

The [first device run with the cache](../logs/MineSwitch-Launcher-20261006T121517Z.log)
reported `total_ms=80351`, `runtime_ms=2454`, `bytes_hashed=915888430`,
`hash_reads=3946`, `receipt_hits=0` and `metadata_checks=12412`. This is a cold
pass that populates the receipts, not evidence of warm-start acceleration.
The [loading screen capture](../logs/switch-loading-cache-build-20261006.png)
confirms visible game/mod initialization on the actual Switch. Later runs above verified the handoff and reached the game/server.

The separate [Sodium companion](../launcher/compat/README.md) passed 81 host
checks for recoverable configuration replacement and interrupted saves.
Its actual mixin application and successful save still require device evidence.

## Current device candidate

`runtime/launcher/MineSwitch-Launcher.nro`, SHA256
`fe98a4e35bfd3a9c2e72269eb0272749358a66b51facce00041ac580d96c0d24`.
The [build receipt](../logs/launcher-cache-build.json) confirms successful
completion and unchanged tracked inputs across the build. The serialized
upload is recorded in the device log above. Builds used six jobs/six CPUs and
nice 15. [UI checks](../logs/launcher-cache-ui-check.log) and
[host regression checks](../logs/launcher-cache-host-check.log) passed.

## Earlier build-only candidate (superseded)

Command: `bash runtime/build-ui-launcher.sh -s`.
The script used the shared build limiter: six jobs, CPUs 10–15, nice 15.
Build exited successfully on 2026-10-06 at 09:00:04 UTC.

- Artifact: `runtime/launcher/MineSwitch-Launcher.nro`
- SHA256: `1c1204bcd5bb382595d5a72e4a91d8ed27c5c3a2fb4ca6b0865b1b5a78d3228d`
- [Build output](../logs/launcher-ui-early-nxlink-build.log) and
  [build receipt](../logs/launcher-ui-early-nxlink-build.json).

This candidate opens NXLink before the frontend and contains the
`MINESWITCH_FRONTEND_BEGIN` / `MINESWITCH_FRONTEND_END` markers. The receipt binds
the artifact to the unchanged `main.c` source hash across this build. The build
agent did not upload it or contact the console; device confirmation of these
new logging paths belongs to the next root-owned run.

## Host evidence

- [Final host checks](../launcher/build-host/final-host-checks.log): catalog 167,
  storage 27, importer 52, preparer 174, repair 30; UI smoke passed.
- [ASan/UBSan UI smoke](../launcher/build-host/final-ui-asan.log) passed.
- [Forge final integration](../launcher/build-host/official-forge-final.log) and
  [NeoForge final integration](../launcher/build-host/official-neoforge-final.log)
  ran the real official tools with bounded per-instance logs.
- [Forge result](../launcher/build-official-sd/forge-validation.json) and
  [NeoForge result](../launcher/build-official-sd/neoforge-validation.json)
  explicitly record `minecraft_executed: false` and `device_access: false`.
- [Loader behavior and limits](../launcher/LOADER-PREPARATION.md),
  [controller mod review](LAUNCHER_CONTROLLERS.md), and
  [GLFW controller contract](../runtime/glfw/CONTROLLERS.md).

Earlier host-only evidence above does not replace the dated device measurements.
The performance/memory goal remains incomplete pending the current device run.
