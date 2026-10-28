# Minecraft 1.21.1 configuration timeout on Horizon Zero

The second real-client run reached OpenAL initialization, resource atlas creation
and an actual connection to the owned test server. The server recognized the
MineSwitch profile and disconnected with `Timed out`. No native crash occurred.

Evidence:

- Client: `logs/mineswitch-jvm-20261006T025810Z.log`.
- Server: `minecraft/test-server/run/logs/latest.log`, 06:14:51 on 6 October 2026.
- Official mappings: `.minecraft/1.21.1/client-mappings.txt`.
- Bytecode audit: `runtime/generated/client-audit/*.javap`.

## Required work during the protocol transition

The unchanged client code follows this chain on the render thread:

1. `ClientConfigurationPacketListenerImpl.handleConfigurationFinished`
   (`fzd.a(abd)`) collects the received game registries.
2. `RegistryDataCollector.collectGameRegistries` (`fzs.a`) applies server tags
   through `TagCollector.updateTags` (`fzx.a`).
3. For a remote connection, `refreshBuiltInTagDependentData` invokes
   `Blocks.rebuildCache` (`dga.a()`).
4. This iterates the block-state registry and calls `BlockStateBase.initCache`
   for every state. Each nondynamic state gets a new cache containing its
   actual collision, occlusion, face-support and light-dependent results.
5. Only after this returns does `fzd` install the PLAY inbound protocol and
   listener, send `ServerboundFinishConfiguration`, and install PLAY outbound.

The live sampler saw `BitSetDiscreteVoxelShape.getIndex` (`exf.a`, line 72),
`VoxelShape.getFaceShape` (`exv.a`, line 193), `VoxelShape.max` (`exv.c`, line 43),
`CubePointRange.size` and Guava shape-cache work from roughly 971 to 1092 seconds.
The current image's known Zero stack-walk bug limits these samples to one frame,
so the complete live call chain has not yet been captured. Its operations agree
with the audited vanilla path. At about 1106 seconds, two protocol-change
operations report that the connection has already closed, and the client
displays the timeout.

This cache rebuild is performed on every remote join. A warm retry may save
some class loading or reuse some shape objects, but it does not remove the
rebuild. Skipping the operation changes tag-dependent gameplay semantics.

## Three different timeout mechanisms

The relevant server deadline is **15,000 ms after a terminal packet is sent**:

- `JoinWorldTask.start` (`asb.a`) sends `ClientboundFinishConfiguration` (`abd`).
- `abd.isTerminal()` is true.
- `ServerCommonPacketListenerImpl.send` (`arr.a`) calls `close` (`arr.l`) for
  terminal packets. This records `closedListenerTime` and marks the listener
  closed.
- Configuration `tick` calls `keepConnectionAlive`; its `checkIfClosed`
  disconnects if the closed listener still exists after 15,000 ms.

The bytecode text for `arr`, `ars`, `asb` and `abd` was independently extracted
from the actual isolated server JAR and is identical to the corresponding
classes in the client JAR. The timeout is a constant in the method; there is
no system-property lookup for it in this implementation.

On the client, `PacketDecoder` calls
`ProtocolSwapHandler.handleInboundTerminalPacket` (`wi.a`). Before the render
thread processes the terminal packet, this sets Netty `AUTO_READ=false`, adds
`inbound_config`, and removes the old decoder. Installing the new inbound
protocol through `UnconfiguredPipelineHandler` (`wo`) reenables reads. The
client also installs a separate Netty `ReadTimeoutHandler(30)`.

Normal keepalive replies do not require the render thread: the client common
listener replies immediately from the packet handler unless
`RenderSystem.isFrozenAtPollEvents()` is true. That predicate requires an active
GLFW event poll lasting over 200 ms, rather than general slow game work.
During the terminal transition the read/protocol boundary and closed-listener
deadline are the obstacle. Sending periodic ordinary keepalives cannot extend
the closed-listener deadline. This is not the initial login 600-tick limit.

## Retry and existing input

The GLFW source maps D-pad directions to arrow keys and Plus to Enter. However,
**the current launcher intercepts Plus and exits the entire process**, before
the game's use of that key can be relied on. Do not instruct the owner to use
Plus to retry the current live build. Sticks are joystick axes, which vanilla
Minecraft does not use. There is no hardware-keyboard polling in this backend.
Touch input generates real cursor movement and left-click events in handheld
mode; this still requires a responsive render thread.

QuickPlay creates its server-history entry with `ServerList.add(server,true)`,
meaning **hidden history**. It cannot be assumed to appear as a selectable saved
server after returning to the multiplayer list. A normal LAN advertisement from
the owned fixture may offer a touch-selectable row if Horizon multicast receive
works; that path has not been tested.

A bounded automatic retry in a future bootstrap can preserve original game code:

- Obtain `Minecraft.getInstance()` (`fgo.Q`) and post checks through its real
  `execute(Runnable)` event-loop method.
- Inspect `Minecraft.screen` (`fgo.y`) only on the game thread. Require the
  actual `DisconnectedScreen` (`fnk`), an appropriate timeout reason, and no
  loaded world (`fgo.r`).
- Invoke the original `QuickPlay.joinMultiplayerWorld` (`gei.b(fgo,String)`)
  reflectively, or call public `ConnectScreen.startConnecting` (`fnc.a`) with
  the original address and server data. Keep the configured fixture address,
  cap retries, and recheck screen identity when the delayed task runs.
- Stop on world entry, a different screen, cancellation, or exhausted attempts.

The existing live JVM has no bootstrap command channel for adding this behavior.
The current run must not be described as supporting a programmatic retry.

## Performance implication

The production Zero interpreter is already a product build compiled with `-O3`
for Cortex-A57. The GCC computed-goto dispatcher is enabled. This is not a
debug-build oversight. Public-server compatibility requires bringing the actual
configuration work inside the vanilla deadline through real runtime/performance
work, while preserving the tag-cache results and packet ordering.

An explicitly labeled larger-timeout isolated fixture could separate world
rendering correctness from interpreter performance. It would require accounting
for both the server closed-listener deadline and the client read timeout, and
would not prove compatibility with an unchanged public server.

`minecraft/diagnostics/ShapeCacheTiming.java` measures three real rebuilds in the
untouched client under a host interpreter. Its unchanged default tags and host
hardware make it directional cold/warm evidence only, not target performance.

Completed host `-Xint` evidence, `logs/minecraft-shape-cache-host-xint.log`:
26,684 block states; bootstrap 37,482 ms; rebuilds 7,984 / 7,967 / 7,999 ms.
Repeated rebuild time did not decrease in this experiment. This supports
prioritizing real runtime execution speed over assuming a warm retry will help.
