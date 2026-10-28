# libjimage Horizon adapter

The port uses the actual OpenJDK 21 image reader. Its new platform source is
`src/java.base/horizon/native/libjimage/osSupport_horizon.cpp` in the upstream
checkout. The main changes are:

- Private image descriptors have per-descriptor locks. Positioned reads use
  seek/read/restore under the lock because target newlib does not provide
  `pread`. Close uses the same lock and removes the descriptor from a registry.
  This is local to libjimage; it does not claim generic POSIX descriptor/dup
  compatibility.
- Read-only index snapshots use the tested `nxvm` memory backend and read the
  original file descriptor, preserving file identity. Horizon uses the existing
  index-only image mode and loads resources as needed, avoiding an eager copy of
  the entire `lib/modules` file.
- The Horizon launcher must set `-Djdk.image.map.all=false` so the Java
  `BasicImageReader` uses the same index-only policy and reads resources through
  its file channel. Its upstream default assumes a full mapping on 64-bit VMs.
- Decompression resolves `ZIP_InflateFully` through `JVM_FindLibraryEntry`, using
  the port's static native-library symbol resolver. It still needs the real
  libzip implementation in the final launcher.
- Mapping failure returns a failed image open instead of proceeding with a null
  index in release builds.

`osSupport`, `endian`, `imageFile`, `imageDecompressor` and `jimage` have compiled
as AArch64 target objects with warnings treated as errors. This does not prove
the parser loads a runtime image or that Java runs.

Build the bounded integration probe with `make -C runtime/jimage/probe` and run
`runtime/jimage/probe/mineswitch-jimage-io-probe.nro` through the normal NXLink
script. It tests the actual OS adapter on a ROMFS fixture, including concurrent
positioned reads on one image descriptor, read-only snapshots, short reads and
cleanup. No fake JVM or dynamic-library symbols are linked into the probe.

On the user's Switch OLED in application mode, 6 October 2026, **30 checks
passed, 0 failed**, including concurrent reads and loader return. Evidence:
`logs/mineswitch-jimage-io-probe-20261006T001327Z.log`. This validates the OS
adapter's tested contracts; it still does not establish JVM/class execution.

The subsequent real JVM run executed ordinary Java and passed
`jrt_resource_open`, `jrt_class_magic` and `java21_class_version` through this
image path: `logs/mineswitch-jvm-20261006T004427Z.log`. That verifies loading the
tested Java 21 runtime resource, in addition to the earlier C adapter probe.
