# Verified native Minecraft MVP — 6 October 2026

The owner requested ending the goal after Minecraft Java 1.21.1 entered and
rendered a multiplayer world on the Switch OLED in Horizon/Atmosphere. Controls
were explicitly deferred. The tested client uses a local MineSwitch profile;
Microsoft login is deferred.

## Authoritative evidence

| Requirement | Evidence |
| --- | --- |
| Native Horizon execution | Actual Switch run `logs/mineswitch-native-minecraft-20261006T054818Z.log`; real OpenJDK AArch64 Server VM and original client |
| Genuine JIT | C1 and C2 each passed 45 device checks, including actual nmethod/entry/code inspection and execution: `logs/mineswitch-jit-20261006T051055Z.log`, `logs/mineswitch-jit-20261006T051252Z.log` |
| Native VM correctness gates | C2 and C1 each passed 31 extended checks including FP, repeated StackOverflow/reguard and live compiled-loop GC/stackwalk: `logs/mineswitch-jit-20261006T053717Z.log`, `logs/mineswitch-jit-20261006T053838Z.log` |
| JNI and GPU integration | 24 checks and 90 frames on native HotSpot: `logs/mineswitch-native-minecraft-20261006T053948Z.log` |
| Local profile and offline-server world | Server identified MineSwitch from `192.168.3.177`, logged in and joined at 08:49:51; retained server log `logs/minecraft-native-world-server-20261006.log` |
| Rendered Minecraft world | Owner's supplied screenshot retained as `logs/minecraft-native-world-owner-20261006.png`, plus explicit confirmation “подключился” |
| Session stability | Server session lasted 17 minutes 17 seconds until the owner deliberately closed the client at 09:07:08. Successful client log has no `HORIZON_NATIVE_UNHANDLED` |
| Orderly process exit | AM self-exit request is logged; owner confirmed HOME without the prior system error dialog |

Tested artifact: `runtime/launcher/mineswitch-native-minecraft.nro`.
SHA256: `9a9adcaca839f6100b4b2694f553531ab8bb1a53c1f2ccb874bcca88779d7f02`.
The archived ELF and hash beside the successful run log identify the executed
build. Its tested invocation is:

```sh
bash scripts/run-nro.sh 192.168.3.177 runtime/launcher/mineswitch-native-minecraft.nro \
  --graphics --system-main --class=mineswitch.launcher.ClientEntry \
  --jvm-options=romfs:/launch/minecraft.options
```

The isolated fixture at `192.168.3.163:25576` was gracefully stopped and saved
when the owner requested finishing the goal. Restart it with
`bash minecraft/test-server/launch.sh` for another test. EULA acceptance is
already recorded. No unrelated user server was changed.

## Explicit limits and deferred follow-ups

- Controls and Microsoft login remain deferred as requested. The tested server
  is configured for offline mode; this does not provide access to authenticated
  servers.
- Numeric FPS has not been measured. The last tested run kept rendering, but
  an idle flat fixture does not establish performance for arbitrary worlds.
- `runtime/launcher/MineSwitch.nro` is a separately built standalone candidate
  with default game entry, a mutable SD profile and a real FPS metrics helper.
  It was not deployed or manually validated before the owner ended this goal.
  Its SHA256 is `f0221adfe6a7ee06f7f717899ee152df98f72af0edf10772548f443fc17237b3`.
- Standalone installer work was stopped at the owner's request; do not treat
  preparation as a completed SD installation.
- The console's actual user/local clocks report 2051; network time is
  unavailable. This causes TLS certificate-expiry warnings. No clock or TLS
  verification setting was changed.
- The verified game target is 1.21.1. Other game versions and complete Java SE
  compatibility are not established by these results.
