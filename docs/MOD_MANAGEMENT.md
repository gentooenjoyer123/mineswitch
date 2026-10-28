# Per-instance mod management

The instance page now opens a mod list, with real `.jar`/`.jar.disabled`
renames for enable/disable. Names/IDs/versions are read from bounded Fabric or
Quilt metadata; other JARs remain manageable by filename. Broken archives are
shown with their error so an owner can disable them. Known duplicate enabled
mod IDs and case-insensitive filename collisions are rejected.

Individual Modrinth/CurseForge searches use the selected Minecraft version and
loader. Version selection is filtered again; downloaded JAR metadata is checked
before publication. Downloads use the existing bounded HTTP and hash verifier,
stage in owned transaction directories, then publish with a non-replacing
rename. Cancellation, bad checksums and conflicting files preserve existing mods.
Required libraries are installed separately; the version screen says so.

CurseForge uses an explicitly configured key when present, otherwise the public
resolver used by PolyMC (`https://cf.polymc.org/api`). The response/token is never
logged and a personal key is never sent to that resolver. Author download
restrictions remain respected; no guessed CDN bypass is added.

Host evidence:

- `logs/mod-manager-verified-tests.log`: 271 catalog checks and 35 real JAR/file
  checks; SDL UI smoke includes mod toggles and catalog navigation.
- `logs/mod-ui-final-check.log`: final UI check after isolation of asynchronous
  mod/modpack search results and captured destination instances.
- `logs/mod-live-downloads.log`: actual Mod Menu JAR downloaded independently
  from Modrinth and CurseForge, all provider checksums verified, toggle roundtrip.

RAM ceilings are 1900 MiB for normal/unknown process budgets and up to5900 MiB
for expanded budgets, retaining at least1 GiB for native allocations. Exact
ceiling values are reachable in the settings cycle. Legacy6144 MiB preferences
migrate instead of blocking startup. Minecraft defaults toXms256m; precommit is
only an explicit diagnostic flag. No8 GiB hardware was available for a live test.

The temporary launch-cache experiment saved about1.2seconds on a61.2second run
and was removed at the owner's request. Normal4 KiB pages/32 KiB per-JAR slabs
with an8 MiB global budget remain. The owner postponed new Switch UI/game/NSP
tests; host checks and SDK compilation must not be described as device testing.
