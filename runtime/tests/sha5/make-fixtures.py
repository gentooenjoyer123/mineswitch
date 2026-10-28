#!/usr/bin/env python3
"""Build owned, offline signed-JAR integrity fixtures; never edits the runtime image."""
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import zipfile

ROOT = Path(__file__).resolve().parents[3]
HERE = Path(__file__).resolve().parent
JDK = Path(os.environ.get("MINESWITCH_BOOT_JDK", "/opt/openjdk-bin-21"))
OUTPUT = HERE / "build/fixtures"
# Public test-only password for a disposable, untrusted local signing key.
PASSWORD = "mineswitch-local-fixture-only"


def run(tool, *args):
    subprocess.run([str(JDK / "bin" / tool), *map(str, args)], check=True)


def main():
    if os.environ.get("MINESWITCH_BUILD_LIMITED") != "1":
        os.execv(sys.executable, [sys.executable, str(ROOT / "runtime/limit-build.py"),
                                 "--", sys.executable, str(Path(__file__).resolve())])
    OUTPUT.mkdir(parents=True, exist_ok=True)
    unsigned, intact, altered = (OUTPUT / name for name in ("unsigned.jar", "intact.jar", "tampered.jar"))
    payload = bytes((i * 37 + (i >> 3) + 11) & 255 for i in range(32769))
    with zipfile.ZipFile(unsigned, "w", zipfile.ZIP_DEFLATED) as archive:
        for name, data in [
            ("META-INF/MANIFEST.MF", b"Manifest-Version: 1.0\r\n\r\n"),
            ("payload.bin", payload),
            ("README.txt", b"MineSwitch SHA5 local integrity test. No production signing identity.\n"),
        ]:
            entry = zipfile.ZipInfo(name, (2020, 1, 1, 0, 0, 0))
            entry.compress_type = zipfile.ZIP_DEFLATED
            archive.writestr(entry, data)
    keystore = OUTPUT / "local-test-key.p12"
    if not keystore.exists():
        run("keytool", "-genkeypair", "-alias", "local-test", "-keyalg", "RSA", "-keysize", "2048",
            "-sigalg", "SHA512withRSA", "-dname", "CN=MineSwitch SHA5 Offline Test",
            "-startdate", "2000/01/01 00:00:00", "-validity", "40000", "-storetype", "PKCS12",
            "-keystore", keystore, "-storepass", PASSWORD, "-keypass", PASSWORD, "-noprompt")
    run("jarsigner", "-keystore", keystore, "-storepass", PASSWORD, "-keypass", PASSWORD,
        "-digestalg", "SHA-512", "-sigalg", "SHA512withRSA", "-sigfile", "LOCALTEST",
        "-signedjar", intact, unsigned, "local-test")
    # Repack with valid ZIP CRC/structure and identical signed metadata. Only
    # payload bytes change, so rejection must come from JAR verification.
    with zipfile.ZipFile(intact) as source, zipfile.ZipFile(altered, "w") as target:
        for entry in source.infolist():
            value = source.read(entry.filename)
            if entry.filename == "payload.bin":
                value = value[:12345] + bytes([value[12345] ^ 0x40]) + value[12346:]
            target.writestr(entry, value)
    manifest = {
        "purpose": "Owned offline signed-JAR integrity test; self-signed certificate is not trusted production identity",
        "certificate_start": "2000-01-01", "certificate_validity_days": 40000,
        "signature_algorithm": "SHA512withRSA", "entry_digest": "SHA-512",
        "payload_bytes": len(payload), "payload_sha512": hashlib.sha512(payload).hexdigest(),
        "tamper": "payload.bin byte 12345 xor 0x40; ZIP CRC recomputed, signatures unmodified",
        "artifacts": {path.name: {"bytes": path.stat().st_size,
                     "sha256": hashlib.sha256(path.read_bytes()).hexdigest()} for path in (intact, altered)},
    }
    (OUTPUT / "fixtures.json").write_text(json.dumps(manifest, indent=2) + "\n")
    print("SHA5_JAR_FIXTURES", intact, altered)


if __name__ == "__main__":
    main()
