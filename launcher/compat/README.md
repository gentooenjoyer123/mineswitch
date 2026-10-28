# MineSwitch Fabric companion

`mineswitch_compat` is a separate client mod. It leaves Minecraft, downloaded mod
jars and the instance's `mods` directory unchanged. The launcher supplies it with
`-Dfabric.addMods=/switch/mineswitch/runtime/mineswitch/mineswitch-compat.jar`.

The only mixin redirects the `Files.move(Path, Path, CopyOption[])` invocation in
Sodium's `FileUtil.writeTextRobustly(String, Path)`. It applies only on Horizon,
Minecraft 1.21.1 and Sodium `0.6.13+mc1.21.1`, after checking the original class
name, method descriptor and single matching invocation. Other platforms,
versions, absent Sodium and unmatched bytecode are skipped. A successful mixin
application prints one `MINESWITCH_COMPAT sodium_config=enabled` log marker.
No Fabric API dependency is required.

Sodium requests atomic replacement, which Horizon's SDK rename cannot provide
when the target exists. This adapter changes that **Sodium call site's save
policy** to recoverable replacement; it does not change Java's `ATOMIC_MOVE`
contract or claim cross-process atomicity.

The source must be Sodium's sibling `.tmp` file, and every path must remain
inside the real game directory without symbolic-link traversal. The adapter
flushes the new data, publishes an immutable SHA256-bound journal, stages the
new file, moves an existing config to its reserved backup, then promotes the
stage using only non-replacing moves. It verifies each result. Ordinary IO
failure restores the previous config where possible; an unsuccessful payload
is retained as Sodium's temp file when that name remains free.

Reserved siblings are `<config>.mineswitch-compat.stage`, `.backup`, `.journal`
and `.journal.tmp`. A valid journal binds their hashes to the relative target
and this adapter's protocol. Unowned backups, unexpected content, invalid
journals and ambiguous states are retained and reported, never silently deleted.
A real file lock serializes access to an active journal. Cleanup failure after
a verified commit leaves a recoverable journal while keeping the saved target.

A narrowly gated Fabric `preLaunch` entrypoint recovers only the owned
`sodium-options.json` journal before Sodium reads settings. This prevents a
temporarily absent config from causing defaults to overwrite a recovered
backup. If that recovery is first reached during a save instead, restoring an
absent old config causes an honest reload-required error. Native filesystem
errors remain errors. Torn or unrecognizable journals can require inspection;
the protocol does not promise power-loss durability.

Build with a real Java 21 host JDK:

```sh
python3 launcher/compat/build.py
python3 launcher/compat/build.py --output /absolute/staging/mineswitch-compat.jar
```

The script enters the repository's six-thread build limiter, uses
`--release 21 -proc:none`, and runs host tests before publishing the jar. Its
compile-only dependencies are pinned by Maven coordinates and SHA256 in
`compile-dependencies.json`: Fabric Loader 0.19.3, Mixin 0.8.7 (Fabric wrapper
0.17.3), and ASM/ASM-tree 9.10.1, matching the target runtime. Only existing
cached jars are read; the build downloads nothing and bundles none of these
dependencies. The companion's `build/dependencies` directory and launcher
library caches are searched by default; `--libraries DIR` adds another Maven
cache root.

The Fabric wrapper version matters even when the Mixin version stays 0.8.7.
Wrapper 0.17.4 changed `Redirect.at()` from `At` to `At[]`, so compiling the same
source against it emits an array in the annotation. Loader 0.19.3 includes
MixinExtras 0.5.4, which casts that value to a single `AnnotationNode` and rejects
the array during transformation. The pinned 0.17.3 emits the compatible scalar
annotation; the host suite reads the actual compiled companion class to check
that shape, the exact method/call target, remapping flags and match count.
It also runs that class through the actual MixinExtras transformer, extracted
from the pinned Loader jar as a test-only dependency.

Default output: `launcher/compat/build/mineswitch-compat.jar`. Unchanged tested
inputs reuse that verified artifact. The host suite passed **92 checks**
using ordinary config files and a strict non-replacing move adapter, covering
save/rollback, interrupted phases, prelaunch recovery, ambiguous files, active
journals, deferred cleanup, confinement, platform/version/bytecode gating and
compiled-annotation compatibility. The annotation regression also rejected the
original companion class compiled against wrapper 0.17.4.
Actual Fabric mixin application and Horizon saves require the device log marker
and resulting configuration evidence; host tests alone do not establish them.

## Current device status and removed memory experiment

The owner verified opening Sodium settings, applying a change and reopening
without a crash. The active companion contains only `SodiumFileUtilMixin`.

The earlier experimental upload-ring (16→2 MiB) and initial mesh-capacity
reductions were removed at the owner's request. Current NXVK tests use standard
Sodium buffers. Historical investigation remains in `MODLOG.md`; memory work
now measures native/runtime/driver ownership (see `docs/NXVK_MEMORY.md`).
