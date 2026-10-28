#!/usr/bin/env python3
"""Build actual LWJGL 3.3.3 native bindings for Horizon/newlib."""
import concurrent.futures
import json
import os
from pathlib import Path
import subprocess
import sys

root=Path(__file__).resolve().parents[2]
if os.environ.get('MINESWITCH_BUILD_LIMITED')!='1':
    os.execv(sys.executable,[sys.executable,str(root/'runtime/limit-build.py'),'--',sys.executable,__file__,*sys.argv[1:]])
source=root/'.upstream/lwjgl'
build=root/'runtime/lwjgl/build'
sdk=Path(os.environ.get('DEVKITPRO',root/'.toolchain/opt/devkitpro'))
target=sdk/'devkitA64/bin'
jdk=root/'.upstream/openjdk21/build/horizon-aarch64-zero-release'
core=source/'modules/lwjgl/core/src/main/c'
includes=[core,root/'runtime/lwjgl/native',root/'.research/vm-probe-libffi/stage/include',
          jdk/'support/modules_include/java.base',jdk/'support/modules_include/java.base/horizon',
          root/'.upstream/openjdk21/src/hotspot/share/include',root/'.upstream/openjdk21/src/hotspot/os/posix/include',
          sdk/'libnx/include',sdk/'portlibs/switch/include']
base=[str(target/'aarch64-none-elf-gcc'),'-std=gnu11','-O2','-g','-Wall','-Wextra',
      '-Werror=implicit-function-declaration','-Wno-unused-parameter','-D_GNU_SOURCE','-D_REENTRANT',
      '-D__SWITCH__','-DLWJGL_HORIZON','-march=armv8-a+crc+crypto','-mtune=cortex-a57','-mtp=soft',
      '-fPIC','-ftls-model=local-exec','-ffunction-sections','-fdata-sections']+['-I'+str(x) for x in includes]
main=[p for p in core.glob('*.c') if p.name!='org_lwjgl_system_SharedLibraryUtil.c']
modules={
 'lwjgl':main+list((source/'modules/lwjgl/core/src/generated/c').glob('*.c'))+
          list((root/'runtime/lwjgl/native').glob('*.c')),
 'lwjgl_stb':list((source/'modules/lwjgl/stb/src/generated/c').glob('*.c')),
 'lwjgl_opengl':[p for p in (source/'modules/lwjgl/opengl/src/generated/c').glob('*.c') if not p.name.startswith(('org_lwjgl_opengl_WGL','org_lwjgl_opengl_CGL'))],
}
failed=[]
requested=set(sys.argv[1:])
for module,files in modules.items():
    if requested and module not in requested: continue
    directory=build/module
    directory.mkdir(parents=True,exist_ok=True)
    def compile_one(path):
        obj=directory/(path.stem+'.o')
        extra=[]
        if module=='lwjgl_opengl':extra=['-I'+str(source/'modules/lwjgl/opengl/src/main/c')]
        if module=='lwjgl_stb':extra=['-I'+str(source/'modules/lwjgl/stb/src/main/c')]
        arguments=[*base,*extra,'-c',str(path),'-o',str(obj)]
        result=subprocess.run(arguments,text=True,capture_output=True)
        (directory/(path.stem+'.log')).write_text(result.stdout+result.stderr)
        (directory/(path.stem+'.cmd.json')).write_text(json.dumps(arguments)+'\n')
        return path,obj,result
    objects=[]
    with concurrent.futures.ThreadPoolExecutor(max_workers=min(6,max(1,int(os.environ.get('MINESWITCH_BUILD_JOBS','6'))))) as workers:
        for path,obj,result in workers.map(compile_one,files):
            if result.returncode:
                failed.append(str(path))
                print(path.relative_to(root),result.stderr[:6000],file=sys.stderr)
            else:objects.append(obj)
    if len(objects)==len(files):
        archive=build/('lib'+module+'.a')
        if archive.exists():archive.unlink()
        subprocess.run([str(target/'aarch64-none-elf-ar'),'rcs',str(archive),*map(str,objects)],check=True)
        print(f'{archive.relative_to(root)}: {len(objects)} genuine binding objects')
if failed:
    raise SystemExit(f'{len(failed)} native sources failed; see runtime/lwjgl/build/*/*.log')
