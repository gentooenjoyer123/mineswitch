#!/usr/bin/env python3
"""Exercise the real installer against non-replacing rename and failures.

Host libc/mbedcrypto validate transaction logic; these tests do not replace
hardware tests of libnx, SD persistence, or the JVM.
"""
import hashlib
import os
from pathlib import Path
import subprocess
import tempfile

root=Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix="mineswitch-installer-") as directory:
    work=Path(directory)
    executable=work/"installer-test"
    subprocess.run(["cc","-std=gnu11","-O2","-Wall","-Wextra","-Werror",
                    "-I",str(root),str(root/"install.c"),
                    str(root/"tests/installer_entry.c"),str(root/"tests/rename_strict.c"),
                    "-Wl,--wrap=rename","-lmbedcrypto","-o",str(executable)],check=True)
    source=work/"romfs:/jdk"
    target=work/"sdmc:/switch/mineswitch/runtime"
    source.mkdir(parents=True)
    target.mkdir(parents=True)
    name="lib/modules"
    old=b"old runtime image"
    new=b"new runtime image\0"*8192
    src=source/name
    dst=target/name
    src.parent.mkdir(parents=True)
    dst.parent.mkdir(parents=True)
    manifest=source/"files.manifest"
    def write_manifest(path=name,data=new,newline=True):
        line=f"{hashlib.sha256(data).hexdigest()} {len(data)} {path}"
        manifest.write_text(line+("\n" if newline else ""))
    def run(expected=0,fail_replace=False):
        env=os.environ.copy()
        env.pop("MINESWITCH_INSTALL_TEST_REPLACE_FAIL",None)
        if fail_replace: env["MINESWITCH_INSTALL_TEST_REPLACE_FAIL"]="1"
        result=subprocess.run([str(executable)],cwd=work,env=env,text=True,capture_output=True)
        assert result.returncode==expected,(result.returncode,result.stdout,result.stderr)
        return result.stdout
    src.write_bytes(new)
    write_manifest()
    dst.write_bytes(old)
    run()
    assert dst.read_bytes()==new
    assert not Path(str(dst)+".mineswitch-old").exists()
    print("PASS replacement with non-replacing rename")
    timestamp=dst.stat().st_mtime_ns
    assert "copied=0" in run()
    assert dst.stat().st_mtime_ns==timestamp
    print("PASS unchanged hash avoids replacement")
    dst.write_bytes(old)
    run(1,fail_replace=True)
    assert dst.read_bytes()==old
    print("PASS failed replacement rolls back old file")
    dst.rename(str(dst)+".mineswitch-old")
    Path(str(dst)+".mineswitch-part").write_bytes(b"interrupted copy")
    run()
    assert dst.read_bytes()==new
    assert not Path(str(dst)+".mineswitch-old").exists()
    assert not Path(str(dst)+".mineswitch-part").exists()
    print("PASS interrupted backup recovered and replaced")
    dst.write_bytes(old)
    src.write_bytes(b"corrupt source")
    run(1)
    assert dst.read_bytes()==old
    print("PASS bad source hash preserves previous file")
    src.write_bytes(new)
    for bad in ("../outside","/absolute","sdmc:/outside","lib/../outside","lib/modules.mineswitch-old"):
        write_manifest(path=bad)
        run(1)
        assert dst.read_bytes()==old
    print("PASS traversal and installer scratch paths rejected")
    write_manifest(newline=False)
    run(1)
    assert dst.read_bytes()==old
    print("PASS truncated manifest rejected")
