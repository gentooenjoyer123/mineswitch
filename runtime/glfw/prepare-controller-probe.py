#!/usr/bin/env python3
"""Compile a separate real LWJGL controller probe; never stages/deploys it."""
import hashlib
import os
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]
if os.environ.get("MINESWITCH_BUILD_LIMITED") != "1":
    os.execv(sys.executable, [sys.executable, str(ROOT / "runtime/limit-build.py"),
                             "--", sys.executable, __file__, *sys.argv[1:]])
JAVA = Path(os.environ.get("MINESWITCH_HOST_JAVA", "/opt/openjdk-bin-21")) / "bin"
BUILD = ROOT / "runtime/glfw/build/controller-probe"
CLASSES = BUILD / "classes"
CLASSES.mkdir(parents=True, exist_ok=True)
CP = [ROOT / "runtime/lwjgl/build/lwjgl-3.3.3-horizon.jar",
      ROOT / ".minecraft/1.21.1/libraries/org/lwjgl/lwjgl-glfw/3.3.3/lwjgl-glfw-3.3.3.jar"]
subprocess.run([str(JAVA / "javac"), "--release", "21", "-proc:none", "-encoding", "UTF-8",
                "-cp", os.pathsep.join(map(str, CP)), "-d", str(CLASSES),
                str(ROOT / "runtime/lwjgl/tests/ControllerProbe.java")], check=True)
JAR = BUILD / "controller-probe.jar"
subprocess.run([str(JAVA / "jar"), "--create", "--file", str(JAR), "-C", str(CLASSES), "."], check=True)
print(JAR.relative_to(ROOT))
print("sha256=" + hashlib.sha256(JAR.read_bytes()).hexdigest())
