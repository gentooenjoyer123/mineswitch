# Horizon SHA-384 / SHA-512 compression candidate

The SUN provider's `SHA5` class retains the implementation of SHA-384,
SHA-512, SHA-512/224, and SHA-512/256. Only compression of complete 128-byte
blocks is accelerated on Horizon. Initial values, buffering, padding, byte
counts, digest output/truncation, reset, clone, provider identity, and every
JAR signature check remain in their original Java paths.

## Implementation

- `DigestBase.implCompressMultiBlock` retains its existing range checks and
  intrinsic entry point, adding a Horizon-only dispatch for `SHA5` instances.
- `SHA5.implCompress` uses the same native primitive for a buffered or final
  single block. Other operating systems retain the original Java compressor.
- `SHA5_horizon.c` imports the eight Java state words, calls the installed
  mbedTLS 2.28.10 `mbedtls_internal_sha512_process`, and commits the state only
  after processing all blocks successfully.
- One native call handles a full multi-block update. Input is copied with
  `GetByteArrayRegion` in at most 8 KiB batches. No critical/pinned Java array
  is held across a long digest, and no pointer into the Java heap survives a
  JNI call. Offset arithmetic is checked in `jlong` before narrowing.
- Temporary native state/input buffers are cleared on completion or failure.
  Native failures preserve a pending exception or throw `ProviderException`.
- `CoreLibraries.gmk` adds the Horizon portlibs include directory. The launcher
  already links the real `libmbedcrypto.a`; a fresh native registry must discover
  `Java_sun_security_provider_SHA5_implCompressBlocksHorizon0` in `libjava.a`.

The SDK configuration does not enable `MBEDTLS_SHA512_ALT` or
`MBEDTLS_SHA512_PROCESS_ALT`. The matching upstream compression function
accesses only `ctx->state`, not `total`, `buffer`, or `is384`. The four algorithms
therefore use their unchanged Java initial states with the same primitive.
This is C acceleration; Cortex-A57 has no SHA-512 architectural instruction
extension.

Reviewed source:
<https://raw.githubusercontent.com/Mbed-TLS/mbedtls/mbedtls-2.28.10/library/sha512.c>

SHA-256 of that source:
`f031e4397aab648361e82eb6d798ff620551c412136fa5f903e4d821677aa2dc`.

## Isolated host validation

`runtime/tests/sha5/validate-native.py` builds a separate AArch64 object with
the real target libjava flags, a host native library from the same C source,
and test copies of `SHA5`/`DigestBase`. Only the copies' Horizon predicates
are forced true. Production platform identity and image contents are untouched.

The host adapter explicitly registers the candidate on the boot `SHA5` class
and counts native invocations. Tests run with `-Xint -Xcheck:jni`, so a silent
Java fallback cannot count as native validation. Host mbedTLS is also 2.28.10.

The full untouched official Minecraft 1.21.1 client has passed native-path
`JarFile` verification: **23,516 signed entries / 41,812,158 signed bytes**,
with **46,597 native calls**, in **3.437 seconds** on this host. This is
correctness evidence and a host timing, not a prediction of Switch performance.

```sh
python3 runtime/limit-build.py -- python3 runtime/tests/sha5/validate-native.py \
  --native-only --probe runtime/tests/sha5/Sha5OfficialJarProbe.java \
  --class-name Sha5OfficialJarProbe .minecraft/1.21.1/client.jar
```

The result is recorded in `logs/sha5-official-client-native.log`. All builds and
host Java invocations use the shared six-worker/CPU limit, nice level 15, and
global build lock. No launcher, target image, or device run is changed by this
validation script.

Target execution remains required before enabling this candidate in a client
deployment. A fresh Java/native image pair and native registry are required.
