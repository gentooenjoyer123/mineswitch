# MineSwitch launcher

Native SDL2/FreeType launcher in front of the existing Horizon Java 21 runtime.
The desktop build exercises the same storage, import, catalog and UI code. It does
not start the Switch JVM or prove that a modpack works on the console.

## Build on the development PC

```sh
bash launcher/build-host.sh
bash launcher/build-host.sh check ui-check http-check
launcher/build-host/mineswitch-launcher --root /absolute/path/to/test-sd
```

Dependencies: SDL2, FreeType, Jansson, libcurl, minizip, zlib and mbedcrypto headers
and libraries. `MINESWITCH_FONT` can select a TrueType font on the desktop.
Switch uses its shared system font. All project builds go through
`runtime/limit-build.py`: at most six jobs, six CPUs and nice 15.

```sh
bash runtime/build-ui-launcher.sh
```

This uses the genuine Java/LWJGL runtime image under
`runtime/generated-native-minecraft`, writes `runtime/launcher/MineSwitch-Launcher.nro`,
and does not deploy or contact a device. This candidate is separate from the
previous direct-game MVP NRO.

## Controls and workflow

- D-pad or left stick moves focus; A selects, B goes back, X opens the actions menu
  of the focused item, Y is the screen's quick action (play an instance, search).
- L/R switch instance tabs (or sidebar sections outside an instance); ZL/ZR jump a
  page in lists; the right stick scrolls. Touch: tap to select, drag to scroll.
  Plus exits, cancelling an active job and waiting for its file cleanup first.
- Desktop: arrows, Enter, Escape, X, Y, Tab/Shift+Tab, PageUp/PageDown, mouse wheel.
- Two background lanes: installs/preparation/copies (shown under «Задачи») and
  light queries (search, lists, version lists), so catalogs stay usable during a
  download. Errors appear as toasts and in the task history.

PolyMC-style features:

- New instance wizard: Mojang version list (releases/snapshots), loader choice
  (Vanilla, Fabric, Quilt, Forge, NeoForge) with live loader version lists. The
  same wizard changes the version/loader of an existing instance.
- Instance tabs: Overview (rename, icon, notes, copy, PolyMC/MultiMC ZIP export,
  full check, delete), Mods, Resource packs, Shaders, Worlds, Servers,
  Screenshots, Logs, Settings (memory override, extra JVM options).
- Mods/packs: enable/disable, delete, catalog install from Modrinth/CurseForge
  with automatic required dependencies (mods), update check by SHA-1 (Modrinth)
  and fingerprint (CurseForge), update one or all.
- Worlds: play directly (quick play), rename, ZIP backup in `game/backups`,
  copy, delete. Servers: add/edit/reorder/delete in `servers.dat`, join directly.
- Screenshot gallery and viewer, log viewer (gzip logs and crash reports).
- Catalog: icons (PNG/JPEG/WebP), sort by relevance/downloads/updated,
  incremental «Показать ещё».
- Settings: offline profiles (add, rename, delete, select), default memory,
  global JVM options, CurseForge key, animations, cache cleanup.
- Not implemented: Microsoft sign-in, instance groups, other Java versions (in
  progress separately), Technic/ATLauncher/FTB catalogs.

The interactive UI worker is joined before starting the JVM. A separate loading
screen remains visible while Java loads the client and mods. GLFW requests its
native window through a handoff: the SDK main thread first releases the loading
renderer, then lets the game create its window. SDL subsystem references are
paired so cleanup does not stop audio initialized by the game.

The existing SDK Java worker and full-process exit path remain in use. Returning
to the launcher after a JVM has run is not assumed safe; reopen the NRO after
leaving Minecraft.

## SD layout

Default root: `sdmc:/switch/mineswitch`.

| Path | Contents |
| --- | --- |
| `settings.json`, `profiles.json` | Launcher preferences and local profiles |
| `instances/<id>/instance.json` | Instance metadata; generated IDs are independent of names |
| `instances/<id>/game/` | Saves, mods, settings and resource packs |
| `instances/<id>/provenance/` | Original pack metadata |
| `instances/<id>/managed-files.json` | Imported-file integrity record |
| `instances/<id>/missing-files.json` | Files needing manual completion |
| `instances/<id>/preparation/processor.log` | Bounded log from loader preparation |
| `cache/` | Catalog archives, game metadata and shared artifacts |
| `transactions/` | Private staging; completed instances publish by rename |
| `runtime/` | Existing genuine Java 21 image and patched native bindings |

Startup removes abandoned generated transaction directories before workers start.
It does not publish staging or remove installed instances. Existing settings have
recoverable replacement journals because SD rename does not replace destinations.

## Online access and compatibility

Modrinth uses its public API. CurseForge uses the official API and a configured
application key, entered in Settings; an empty key produces a visible configuration
message before any catalog request. No other application's credentials are used.
Author-restricted downloads remain manual unless the same public artifact is found
on Modrinth with matching SHA-1 and size.

For a manual file, use its `manual_url` from `missing-files.json` and copy it to
the listed path below that instance's `game/` directory. Select **Проверить файлы**
in the instance. The launcher checks its published size and hashes; a filename
alone never unlocks preparation. Unresolved file IDs still need provider metadata.

HTTPS certificate checks remain enabled. The console was previously found to be
set to 2051; its date must be corrected before live HTTPS tests can succeed.

Java 21 and the current statically linked native library set bound compatibility.
Desktop JNI/SDL/JNA libraries are not automatically portable. See
`runtime/glfw/CONTROLLERS.md` for Joy-Con discovery and pending physical checks.
Forge/NeoForge use a separate preparation JVM followed by reopening the launcher.
Official Forge 52.1.16 and NeoForge 21.1.256 pipelines passed on host Java 21 with
the original client unchanged. See [loader preparation](LOADER-PREPARATION.md).
Real game startup, modpack behavior and physical controller/device validation
remain separate gates; host preparation success is not game validation.

FPS measurement, overclocking and additional Java ports are outside this stage.
Functional device validation is in progress. The host test scripts above do not
contact Switch; device uploads use the existing serialized `scripts/run-nro.sh`.

## Full initial-heap diagnostic

The NRO option `--precommit-heap` appends an Xms matching the selected Xmx for
one launch and logs `MINESWITCH_HEAP_PRECOMMIT`. It does not persist an Xms
preference or change Xmx. The owner requested2GiB initial/max tests. Equal
committed and maximum heap capacity proves full initial commitment, not2GiB
of live Java objects. The current Horizon reservation backend already retains
physical backing for the full Xmx even when the ordinary initial heap is smaller.
