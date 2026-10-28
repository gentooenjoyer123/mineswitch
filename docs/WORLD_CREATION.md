# Local world creation on Horizon (Minecraft 1.21.1)

The 7 October 2026 device failure occurred before integrated-server startup:
`FileChannel.open(.../saves/Новый мир/session.lock)` reported `Not supported`.
The fresh log is `logs/mineswitch-diag-20261007T075535Z.log`.

A native reproduction (`runtime/io/probe/world`) creates only uniquely named
owned test directories, then removes them. On this console, both `readdir` and
raw `fsDirRead` omit the Cyrillic directory entries, while direct creation,
lookup and deletion by those names succeed. ASCII directories enumerate and
pass create/write/force/whole-file-lock/unlock/close/reopen. This was reproduced
with Cyrillic created first and with all directories retained until cleanup;
it is not an old directory iterator or Java-only encoding failure.
Evidence: `logs/mineswitch-world-io-probe-20261007T075925Z.log`.
Do not assume all consoles or firmware versions have the same behavior.

The file bridge deliberately keeps rejecting unverifiable canonical identities;
removing that check could make case aliases bypass shared-handle and lock
identity. The workaround is confined to new-world storage-folder selection:

- Fabric Minecraft 1.21.1 on Horizon, independent of Sodium being installed.
- `WorldCreationUiState.findResultFolder` (intermediary
  `class_8100.method_49704`) passes an ASCII candidate to vanilla
  `FileUtil.findAvailableName` (`class_4239.method_19773`).
- A candidate containing non-ASCII characters becomes `World`; vanilla still
  checks and assigns `World (1)`, etc. Existing paths are never overwritten.
- The UI state retains the original world title, which is written into world
  metadata. Existing save directories are not renamed or migrated.
- Empty-name localized defaults are covered because interception occurs after
  vanilla selects the display-name fallback. Both invocation sites are required.
- The regular session lock, chunk IO and save machinery remain unchanged.

Companion version: 0.1.1. Host compatibility suite: 96 checks passed.
On-console creation and entry passed: the owner confirmed «Мир создался, я внутри».
`logs/MineSwitch-20261007T080336Z.log` confirms the mixin, integrated server and
local player login. The owner also confirmed save/exit, appearance in the world list and successful reopen.
The installed NSP is an unchanged forwarder; only the canonical NRO is updated.
