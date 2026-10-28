# Minecraft 1.21.1 module policy

The physical Horizon runtime contains the JDK module closure derived from the
official client and Java libraries, plus the DNS, TLS, extended management and
ZIP filesystem providers that the application selects through services or
reflection. The inputs, hashes and `jdeps` results are recorded in
`runtime/minecraft-1.21.1-modules.json`; the actual target descriptor closure is
recorded in `runtime/generated/module-image.json`.

The current VM is built without native Flight Recorder. The genuine JDK JFR
classes remain in the diagnostic image, and `FlightRecorder.isAvailable()`
reports false through upstream `JVMSupport` handling. No event, sampler or
recording implementation is simulated.

Minecraft requires a narrower **resolved boot layer** while this VM feature is
unavailable. In the official 1.21.1 client (Mojang mappings),
`net.minecraft.util.profiling.jfr.JvmProfiler` is `bnl`. Its static initializer
tests only `ModuleLayer.findModule("jdk.jfr").isPresent()`, selecting
`JfrProfiler` (`bnk`) if true and its own `NoOpProfiler` (`bnl$a`) otherwise.
The active profiler's server tick path reads `ServerTickTimeEvent.TYPE`, whose
initializer calls `EventType.getEventType`; that API correctly rejects a VM
without JFR. The profiler's separate availability method is insufficient to
guard this initialization path.

The Minecraft launcher must therefore pass **both** `--limit-modules` and
`--add-modules` with the roots in `module-policy.json`, excluding `jdk.jfr`.
The stock client then selects its real NoOp implementation. Diagnostic JVM
tests may continue using `--add-modules=ALL-SYSTEM` to verify the JFR API's
unsupported capability result. This policy changes module resolution, not the
game's event classes or the VM's reported feature support.

The host check uses the actual verified client and libraries, loads the vanilla
profiler interface, and checks that the selected object is `bnl$a`. Its result
is `logs/minecraft-module-policy-probe.log`. This proves profiler selection and
module visibility on the host; it is not evidence of Minecraft rendering or
world loading on the Switch.
