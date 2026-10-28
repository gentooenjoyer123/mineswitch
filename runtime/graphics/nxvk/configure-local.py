#!/usr/bin/env python3
"""Stage the inspected NXVK fork against local SDK/tools; never install over SDK."""
from pathlib import Path
import os
import shutil
import subprocess
import sys
import shlex

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[2]
SRC = ROOT / ".upstream/nxvk"
BUILD = HERE / "build"
SDK = ROOT / ".toolchain/opt/devkitpro"
VENV = ROOT / ".toolchain/nxvk-python"

if os.environ.get("MINESWITCH_BUILD_LIMITED") != "1":
    os.execv(sys.executable, [sys.executable, str(ROOT / "runtime/limit-build.py"), "--", sys.executable, __file__])

def executable(path, content):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(content)
    path.chmod(0o755)

native = BUILD / "native-tools/bin"
native.mkdir(parents=True, exist_ok=True)
for target in ("src/compiler/clc/mesa_clc", "src/compiler/spirv/vtn_bindgen2"):
    source = BUILD / "native" / target
    if not source.is_file():
        raise SystemExit("Build the host shader tools first: " + str(source))
    shutil.copy2(source, native / (source.name + ".real" if source.name == "mesa_clc" else source.name))
clang_include = subprocess.check_output(["clang", "-print-resource-dir"], text=True).strip() + "/include"
executable(native / "mesa_clc", "#!/usr/bin/env bash\nexec " + shlex.quote(str(native / "mesa_clc.real")) +
           ' "$@" -I' + shlex.quote(clang_include) + "\n")

target = "aarch64-switch-horizon"
std_deps = BUILD / "rust-std/target" / target / "release/deps"
if not list(std_deps.glob("libstd-*.rlib")):
    raise SystemExit("Build the matching Rust std first")
sysroot = BUILD / "rust-sysroot"
target_libs = sysroot / "lib/rustlib" / target / "lib"
target_libs.mkdir(parents=True, exist_ok=True)
for source in std_deps.iterdir():
    if source.suffix in (".rlib", ".rmeta"):
        shutil.copy2(source, target_libs / source.name)
host_sysroot = Path(subprocess.check_output(["rustc", "--print", "sysroot"], text=True).strip())
for source in (host_sysroot / "lib/rustlib").iterdir():
    destination = sysroot / "lib/rustlib" / source.name
    if not destination.exists() and not destination.is_symlink():
        destination.symlink_to(source, target_is_directory=source.is_dir())

includes = BUILD / "cross-include"
includes.mkdir(parents=True, exist_ok=True)
shutil.copytree("/usr/include/libdrm", includes / "libdrm", dirs_exist_ok=True)
for name in ("xf86drm.h", "xf86drmMode.h"):
    shutil.copy2(Path("/usr/include") / name, includes / name)
for source in Path("/usr/include/libdrm").glob("*.h"):
    shutil.copy2(source, includes / source.name)
shutil.copytree(SRC / "switch/docker/cross-include", includes, dirs_exist_ok=True)

pkg = BUILD / "pkgconfig"
pkg.mkdir(exist_ok=True)
for source in (SDK / "portlibs/switch/lib/pkgconfig").glob("*.pc"):
    (pkg / source.name).write_text(source.read_text().replace("/opt/devkitpro", str(SDK)))
for name in ("libclc", "LLVMSPIRVLib", "SPIRV-Tools", "SPIRV-Tools-shared", "SPIRV-Headers"):
    env = dict(os.environ, PKG_CONFIG_PATH="/usr/lib/llvm/22/lib64/pkgconfig")
    path = subprocess.check_output(["pkg-config", "--variable=pcfiledir", name], text=True, env=env).strip()
    shutil.copy2(Path(path) / (name + ".pc"), pkg / (name + ".pc"))
pkg_wrapper = BUILD / "bin/pkg-config-switch"
executable(pkg_wrapper, "#!/usr/bin/env bash\nexport PKG_CONFIG_DIR= PKG_CONFIG_PATH= PKG_CONFIG_SYSROOT_DIR=\n"
           "export PKG_CONFIG_LIBDIR=" + shlex.quote(str(pkg)) + '\nexec /usr/bin/pkg-config --static "$@"\n')

replacements = {"/opt/devkitpro": str(SDK), "/opt/switch-cross-include": str(includes),
                "/work": str(SRC)}
def rebase(text):
    for old, new in replacements.items():
        text = text.replace(old, new)
    return text
cross = rebase((SRC / "switch/crossfiles/switch.cross").read_text())
cross = cross.replace(str(SDK / "portlibs/switch/bin/aarch64-none-elf-pkg-config"), str(pkg_wrapper))
(BUILD / "switch.cross").write_text(cross)
rust_wrapper = BUILD / "bin/rustc-switch"
rust_script = rebase((SRC / "switch/rust/rustc-switch.sh").read_text())
rust_script = rust_script.replace(str(SRC / "switch/rust/sysroot"), str(sysroot))
executable(rust_wrapper, rust_script)
rust_cross = rebase((SRC / "switch/crossfiles/rust.cross").read_text())
rust_cross = rust_cross.replace(str(SRC / "switch/rust/rustc-switch.sh"), str(rust_wrapper))
(BUILD / "rust.cross").write_text(rust_cross)

env = dict(os.environ, DEVKITPRO=str(SDK), DEVKITA64=str(SDK / "devkitA64"),
    PATH=os.pathsep.join([str(native), str(VENV / "bin"), "/usr/lib/llvm/22/bin", "/usr/bin", "/bin"]),
    PKG_CONFIG_PATH="/usr/lib/llvm/22/lib64/pkgconfig", LIBCLANG_PATH="/usr/lib/llvm/22/lib64", CARGO_BUILD_JOBS="6")
command = [str(VENV / "bin/meson"), "setup", str(BUILD / "cross-zink"), str(SRC),
    "--cross-file", str(BUILD / "switch.cross"), "--cross-file", str(BUILD / "rust.cross"),
    "--native-file", str(BUILD / "native.ini"), "--buildtype=release", "--default-library=static",
    "-Dmesa-clc=system", "-Dprecomp-compiler=system", "-Dllvm=disabled", "-Dvulkan-drivers=nouveau",
    "-Dgallium-drivers=zink", "-Dopengl=true", "-Dgles1=disabled", "-Dgles2=enabled", "-Degl=enabled",
    "-Dglvnd=disabled", "-Dplatforms=horizon", "-Dglx=disabled", "-Dgbm=disabled", "-Dvideo-codecs=",
    "-Dvulkan-layers=", "-Dvulkan-beta=false", "-Dshader-cache=enabled", "-Dzstd=disabled",
    "-Dlibunwind=disabled", "-Dlmsensors=disabled", "-Dvalgrind=disabled", "-Dperfetto=false",
    "-Dandroid-libbacktrace=disabled", "-Dbuild-tests=false"]
subprocess.run(command, env=env, check=True)
