# Loader preparation

The launcher prepares Java 21 instances using official Minecraft and loader
metadata. Vanilla, Fabric, and Quilt produce a game launch plan directly.
Forge and NeoForge use a separate preparation launch to run their official
installer processors.

For Forge and NeoForge:

1. Native preparation verifies the installer, original client, tool jars, and
   processor inputs, then writes `preparation/processor-plan.properties`.
2. Starting preparation runs `mineswitch.launcher.ProcessorEntry` in a separate
   Java session. The instance remains `NeedsPreparation`. When that session
   exits, reopen the launcher.
3. Native preparation checks the completion marker against the exact plan and
   verifies every generated output. Only then does it publish the generated
   libraries and return the normal game launch plan.

The console shows short progress messages. Detailed processor stdout, stderr,
and failure stacks go to `preparation/processor.log`, replaced on each run and
limited to 8 MiB. Space is reserved for failure diagnostics when verbose output
reaches the limit.

Processors write to the instance's `preparation/generated` directory. Failed
or interrupted runs preserve the original client and previously published
libraries. Generated staging copies remain available for later verification,
so they occupy additional SD space. Shared assets and already verified
downloads are reused. The first installation can require network access from
the official `DOWNLOAD_MOJMAPS` processor; a completed preparation can be
verified and reused offline.

Forge includes its verified generated client in the launch classpath.
NeoForge loads its generated SRG, extra, and patched client jars from the
canonical Maven library directory. Its module options retain the actual
module names declared by the downloaded jars.

The current Horizon runtime requires Java 21 and LWJGL 3.3.3. Metadata requiring
another Java major or LWJGL ABI is rejected. JVM instrumentation is unavailable,
and JFR remains excluded. A valid preparation plan does not establish that
every mod or loader works on Horizon; native dependencies and actual game
startup still require platform testing.
