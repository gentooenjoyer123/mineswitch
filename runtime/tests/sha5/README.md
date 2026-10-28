# SHA5 independent correctness fixtures

`HorizonSha5Probe.java` uses only public JCA/JAR APIs and explicitly selects the
SUN digest provider. Its expected values are fixed Python `hashlib`/OpenSSL
answers, independent of the Java implementation under test. The empty and
`abc` cases also match published SHA-family known-answer vectors.

Coverage includes SHA-384, SHA-512, SHA-512/224 and SHA-512/256; padding boundaries
111/112/113 and 127/128/129; lengths through 100003 bytes including native batch
boundaries 8191/8192/8193 and 16383/16384/16385; offset 31 with non-aligned
remainders; chunked updates; clones with a compressed block plus partial buffer
and divergent suffixes; explicit/implicit reset; direct/read-only ByteBuffers;
and digest output offsets/guards. The input pattern is
`(i * 37 + (i >>> 3) + 11) & 255`; clone branches change the last byte by XOR 0x5a
(empty input instead forks to byte 0x42).

Generate signed JAR fixtures with:

```sh
python3 runtime/tests/sha5/make-fixtures.py
```

The generator automatically enters `runtime/limit-build.py`. It creates a
disposable self-signed RSA-2048 test key and signs the owned fixture using real
`jarsigner`, SHA-512 entry digests and SHA512withRSA. Certificate validity spans
2000 through 2109, covering both the host and the console's observed clock.
No TSA, network trust, production identity or system trust-store changes are
involved. These files and the test key stay under the ignored `build/` directory.
Key generation is random; fixture content and the tamper recipe are repeatable,
while artifact hashes are recorded per run in `fixtures.json`.

The tampered JAR has one changed payload byte with a valid ZIP CRC and unchanged
signature metadata. The probe fully reads the intact signed payload and checks
that a real `CodeSigner` is attached; it then requires `SecurityException` for
the changed JAR. An IO/decompression failure does not count as signature
rejection. This tests JAR integrity, not PKIX certificate trust.

Run the isolated native-candidate harness (which also runs an unmodified host
baseline unless `--native-only` is supplied):

```sh
python3 runtime/tests/sha5/validate-native.py \
  runtime/tests/sha5/build/fixtures/intact.jar \
  runtime/tests/sha5/build/fixtures/tampered.jar
```

The Java probe accepts zero arguments for digest-only testing or exactly those
two JAR paths. It has no resource-file dependency and introduces no custom
provider that could accidentally bypass the implementation under test.

Frozen checkpoint: the unmodified host JDK passed **2058 checks** with
`-Xint -Xcheck:jni`, including signed-JAR authentication and changed-payload
rejection. Log: `logs/sha5-independent-baseline.log`. The same independent fixture subsequently passed all **2058 checks** against
the host native candidate with **3467 real native compression calls**, including
tamper rejection: `logs/sha5-independent-native.log`. Device validation is still
pending. Separate official-client native verification is recorded by the
implementation owner.
