#!/usr/bin/env python3
"""Generate JNI headers for the JDK 8 native libraries from the pinned rt.jar.

JDK 8's own build emits headers from javac -h while compiling the class
library. The Horizon JDK 8 natives are compiled against the same 8u492 class
files (rt.jar), so `javah` over every class of the packages that have native
code produces the same declarations and @Native constants.
"""
import argparse
import os
from pathlib import Path
import subprocess
import zipfile

PACKAGES = (
    "java/lang/", "java/io/", "java/util/", "java/net/", "java/nio/", "java/security/",
    "sun/misc/", "sun/reflect/", "sun/nio/", "sun/net/", "sun/security/provider/",
    "sun/util/locale/provider/", "java/util/zip/", "java/util/prefs/", "java/util/logging/",
    "sun/management/", "com/sun/management/", "java/lang/management/", "jdk/internal/platform/",
    "jdk/net/",
)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--rt-jar", type=Path, required=True)
    parser.add_argument("--javah", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    names = []
    with zipfile.ZipFile(args.rt_jar) as jar:
        for entry in jar.namelist():
            if not entry.endswith(".class") or not entry.startswith(PACKAGES):
                continue
            stem = entry[:-6]
            last = stem.rsplit("/", 1)[-1]
            # Anonymous/local classes never declare natives.
            if any(part[:1].isdigit() for part in last.split("$")[1:]):
                continue
            names.append(stem.replace("/", "."))
    args.output.mkdir(parents=True, exist_ok=True)
    env = dict(os.environ)
    env.pop("_JAVA_OPTIONS", None)
    batch = 400
    failures = 0
    for i in range(0, len(names), batch):
        chunk = names[i:i + batch]
        result = subprocess.run([str(args.javah), "-J-Xmx512m", "-bootclasspath", str(args.rt_jar),
                                 "-d", str(args.output), *chunk], env=env,
                                capture_output=True, text=True)
        if result.returncode != 0:
            # Retry one by one so a single unloadable class does not drop a batch.
            for name in chunk:
                r = subprocess.run([str(args.javah), "-bootclasspath", str(args.rt_jar),
                                    "-d", str(args.output), name], env=env, capture_output=True, text=True)
                failures += r.returncode != 0
    print(f"javah: {len(names)} classes, {failures} not processed, "
          f"{len(list(args.output.glob('*.h')))} headers in {args.output}")


if __name__ == "__main__":
    main()
