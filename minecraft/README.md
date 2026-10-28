# Minecraft 1.21.1 inputs and launcher

The active target is the original Minecraft Java client on Horizon OS. Controls
are deferred by the owner. The MVP uses a local profile and connections to
servers configured for offline mode; Microsoft sign-in is a later feature.

The client, 49 library artifacts and 3888 distinct asset objects were downloaded
from Mojang endpoints with their published SHA1/size checks. The game files stay
under ignored `.minecraft/` and generated build directories; source changes do
not include a redistributed game jar.

```sh
python3 minecraft/prepare.py --assets
python3 minecraft/prepare-payload.py
make -C minecraft/installer -j2
bash scripts/run-nro.sh 192.168.3.177 minecraft/installer/mineswitch-game-data.nro
```

The one-time installer packs about 0.9 GB into RomFS, verifies SHA256 manifests,
and writes the owned SD namespace `/switch/mineswitch/minecraft`. It does not
launch the game. Its successful device run is recorded in
`logs/mineswitch-game-data-20261006T011605Z.log`: **3941 files, 912723284 bytes**,
all installed and verified. The installer then exits the process; HOME is
expected before reopening Sphaira.

`profile.json` selects the local username, initial heap and resolution. The
default username is `MineSwitch`; the UUID is the ordinary name-based
`OfflinePlayer:MineSwitch` UUID. Vanilla's non-MSA user type selects its actual
`UserApiService.OFFLINE` path. No Microsoft access token is created or stored.
Online-authenticated servers require the later Microsoft login implementation.

`prepare-launch.py` builds the namespaced `mineswitch.launcher.ClientEntry`
bootstrap, stages genuine patched Horizon LWJGL jars, and creates a VM options
file. Long classpaths are carried in the options file rather than the limited
NXLink argument buffer. Original Minecraft classes retain their jar signatures;
unsigned helper code uses a separate package.

```sh
python3 minecraft/prepare-launch.py
```

This preparation must follow each rebuilt runtime image, since the image build
replaces generated RomFS data. Classpath entries select platform-independent
Java artifacts and actual Horizon bindings; desktop native binaries are not
Horizon libraries. Initial allocator selection is LWJGL's real system allocator.
The JFR boot-layer policy is documented in [MODULE_POLICY.md](MODULE_POLICY.md).

Java LWJGL callbacks and GPU output passed 16 checks and 90 frames in
`logs/mineswitch-jvm-20261006T021608Z.log`. The original signed Minecraft client
then reached graphics initialization in
`logs/mineswitch-jvm-20261006T022345Z.log`, but exited at about 625 seconds because
one CodeMemory object per callback exhausted a kernel resource. Shared callback
slabs passed 24 checks and 90 frames in
`logs/mineswitch-jvm-20261006T025317Z.log`, including 128 simultaneous callbacks,
sparse frees, continued execution of live callbacks and slot reuse. A rendered
Minecraft world and connection to the isolated server remain unverified.

The development launch enables `mineswitch.progress`: a daemon samples the main
thread every ten seconds, recording stack frames, class counts and heap use.
Separate markers identify loading the original Main class and invoking it.
Remove `-Dmineswitch.progress=true` from the generated options to disable this
diagnostic. Existing options are preserved except the invalid
`simulationDistance:4` written by the initial launcher, which migrates to the
supported minimum of 5.
