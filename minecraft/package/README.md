# Standalone MineSwitch installer

This small native installer embeds exactly one final game launcher, writes it
to `/switch/mineswitch/MineSwitch.nro`, and returns to Sphaira after its installer
thread has exited and been joined. It does not start a JVM. The existing
Minecraft client/libraries/assets under `/switch/mineswitch/minecraft` stay on
SD and are not bundled or retransmitted.

Build from an explicitly selected, completed standalone launcher:

```sh
bash minecraft/package/build.sh --nro /absolute/path/to/MineSwitch.nro \
  --expect-sha256 FINAL_CANDIDATE_SHA256
```

The optional expected hash pins a reviewed candidate. `prepare.py` validates
the NRO/embedded ROMFS bounds, snapshots the file, and produces a one-entry
SHA256 manifest. It rejects inputs over 128 MiB to prevent accidentally bundling
the separate game-data installer. The result is
`minecraft/package/MineSwitch-Installer.nro`; provenance is in
`generated/package-summary.json`. Every build uses the shared six-job limiter.

The installer compiles the existing `runtime/launcher/install.c` with an owned
target directory. That code checks size/SHA256, reuses matching files and uses
the existing verified `.mineswitch-part` / `.mineswitch-old` replacement and
recovery procedure. Unlisted files are untouched. The worker does the SD I/O;
the supervisor yields 16 ms per iteration, pumps optional NXLink logs and owns
console updates. It joins/closes the worker before restoring stdio, closing
networking/ROMFS/console and returning normally to the loader. A failed worker
cleanup takes an explicit process-exit fallback, never an unsafe loader return.

The installed game NRO has its own runtime installer and default Minecraft
entry/options. This package does not change those defaults or the user profile.
Installing via NXLink and confirming a Sphaira launch without CLI arguments are
separate device checks owned by the integration workflow.
