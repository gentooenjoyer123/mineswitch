#!/usr/bin/env python3
"""Build/test the narrow Fabric companion using existing pinned compile jars."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import zipfile

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    if os.environ.get("MINESWITCH_BUILD_LIMITED") != "1":
        os.execv(sys.executable, [sys.executable, str(ROOT / "runtime/limit-build.py"), "--", sys.executable, str(Path(__file__).resolve()), *sys.argv[1:]])
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, default=HERE / "build/mineswitch-compat.jar")
    parser.add_argument("--libraries", type=Path, action="append", default=[], help="Existing Maven library root; no downloads are performed")
    args = parser.parse_args()
    version = subprocess.check_output(["javac", "-version"], text=True).strip()
    if not version.startswith("javac 21."):
        raise SystemExit("The companion build requires a real Java21 host JDK")
    lock_path = HERE / "compile-dependencies.json"
    lock = json.loads(lock_path.read_text())
    roots = args.libraries + [HERE / "build/dependencies", ROOT / "launcher/build-live-sd/cache/libraries", ROOT / "launcher/build-official-sd/cache/libraries", ROOT / ".minecraft/1.21.1/libraries"]
    dependencies = {}
    for name, entry in lock["dependencies"].items():
        found = next((root / entry["path"] for root in roots if (root / entry["path"]).is_file() and sha(root / entry["path"]) == entry["sha256"]), None)
        if found is None:
            raise SystemExit("Missing pinned compile-only dependency " + entry["coordinate"] + "; provide its existing library root with --libraries")
        dependencies[name] = found
    build = HERE / "build"
    build.mkdir(exist_ok=True)
    jar = build / "mineswitch-compat.jar"
    state_path = build / "state.json"
    inputs = sorted(path for path in (HERE / "src").rglob("*") if path.is_file()) + [lock_path, Path(__file__).resolve()]
    fingerprint = hashlib.sha256((version + "\n" + "\n".join(str(path.relative_to(HERE)) + "=" + sha(path) for path in inputs)).encode()).hexdigest()
    state = json.loads(state_path.read_text()) if state_path.exists() else {}
    if not jar.is_file() or state.get("inputs") != fingerprint or state.get("jar_sha256") != sha(jar):
        classes, tests = build / "classes", build / "test-classes"
        for directory in [classes, tests]:
            if directory.exists(): shutil.rmtree(directory)
            directory.mkdir()
        classpath = os.pathsep.join(str(path) for path in dependencies.values())
        flags = ["javac", "--release", "21", "-proc:none", "-Xlint:all,-try,-processing", "-Werror"]
        subprocess.run(flags + ["-cp", classpath, "-d", str(classes)] + [str(path) for path in sorted((HERE / "src/main/java").rglob("*.java"))], check=True)
        # Exercise the same transformer that consumes Redirect.at on the device.
        # The containing Loader jar is SHA256-pinned; this member is test-only.
        test_dependencies = build / "test-dependencies"
        test_dependencies.mkdir(exist_ok=True)
        mixinextras = test_dependencies / "mixinextras-fabric-0.5.4.jar"
        with zipfile.ZipFile(dependencies["fabric_loader"]) as loader:
            mixinextras.write_bytes(loader.read("META-INF/jars/" + mixinextras.name))
        test_classpath = os.pathsep.join([str(classes), str(mixinextras), classpath])
        test_sources = sorted((HERE / "src/test/java").rglob("*.java")) + sorted((HERE / "src/test/fixtures").rglob("*.java"))
        subprocess.run(flags + ["-cp", test_classpath, "-d", str(tests)] + [str(path) for path in test_sources], check=True)
        fixture = tests / "net/caffeinemc/mods/sodium/client/util/FileUtil.class"
        subprocess.run(["java", "-cp", os.pathsep.join([str(tests), test_classpath]), "mineswitch.compat.CompatibilityTests", str(fixture)], check=True, timeout=30)
        subprocess.run(["jar", "--create", "--file", str(jar), "--date=2026-01-01T00:00:00Z", "-C", str(classes), ".", "-C", str(HERE / "src/main/resources"), "."], check=True)
        with zipfile.ZipFile(jar) as archive:
            metadata = json.loads(archive.read("fabric.mod.json"))
            assert metadata["id"] == "mineswitch_compat"
            assert not any(name.startswith(("net/", "org/", "com/")) for name in archive.namelist()), "Compile/test dependencies or fixtures must not be bundled"
        state_path.write_text(json.dumps({"inputs": fingerprint, "jar_sha256": sha(jar), "compiler": version, "tests": "passed"}, indent=2) + "\n")
    output = args.output.resolve()
    output.parent.mkdir(parents=True, exist_ok=True)
    if output != jar.resolve(): shutil.copyfile(jar, output)
    print("COMPAT_JAR_READY path=" + str(output) + " sha256=" + sha(output))


if __name__ == "__main__":
    main()
