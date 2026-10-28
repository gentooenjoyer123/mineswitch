#!/usr/bin/env python3
"""Build the pinned Horizon mimalloc port without modifying the installed SDK."""
import concurrent.futures
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys

HERE=Path(__file__).resolve().parent
ROOT=HERE.parents[2]
SRC=ROOT/'.upstream/mimalloc'
if os.environ.get('MINESWITCH_BUILD_LIMITED')!='1':
    os.execv(sys.executable,[sys.executable,str(ROOT/'runtime/limit-build.py'),'--',sys.executable,__file__,*sys.argv[1:]])
pin=json.loads((HERE/'provenance.json').read_text())
commit=subprocess.check_output(['git','-C',str(SRC),'rev-parse','HEAD'],text=True).strip()
if commit!=pin['commit']:
    raise SystemExit('mimalloc source differs from pinned commit')
if hashlib.sha256((SRC/'LICENSE').read_bytes()).hexdigest()!=pin['license_sha256']:
    raise SystemExit('Pinned mimalloc license changed')
diff=subprocess.check_output(['git','-C',str(SRC),'diff'])
if diff!=(HERE/'horizon.patch').read_bytes():
    raise SystemExit('Unrecorded mimalloc source changes; update/review horizon.patch')
segment_shift=20
for argument in sys.argv[1:]:
    if argument.startswith('--segment-shift='):
        segment_shift=int(argument.split('=',1)[1])
if segment_shift not in (18,19,20):
    raise SystemExit('Supported segment shifts:18,19,20 (slice>=4KiB,64slices)')
variant='' if segment_shift==20 else f'-s{segment_shift}'

def run(args,**kwargs):
    subprocess.run([str(x) for x in args],check=True,**kwargs)

common=['-std=gnu11','-D_GNU_SOURCE','-fno-builtin','-ffunction-sections','-fdata-sections',
        '-DMI_MINESWITCH=1','-DMI_STATIC_LIB=1','-DMI_STAT=1',
        '-DMI_DEFAULT_DISALLOW_ARENA_ALLOC=1',f'-DMI_SEGMENT_SHIFT={segment_shift}',f'-DMI_SEGMENT_SLICE_SHIFT={segment_shift-6}',
        '-I'+str(SRC/'include'),'-I'+str(HERE)]
sources=['core.c','page_pool.c','large_pages.c','newlib_adapter.c']

for debug in (2,0):
    build=HERE/(('build-host-debug' if debug else 'build-host-release')+variant)
    build.mkdir(exist_ok=True)
    flags=common+['-O1','-g',f'-DMI_DEBUG={debug}','-DMINESWITCH_MIMALLOC_HOST=1',
                  '-fsanitize=address,undefined','-fno-omit-frame-pointer','-pthread']
    def compile_host(name):
        target=build/(Path(name).stem+'.o')
        run(['cc',*flags,'-c',HERE/name,'-o',target]);return target
    with concurrent.futures.ThreadPoolExecutor(max_workers=3) as pool:
        objects=list(pool.map(compile_host,sources))
    run(['cc',*flags,HERE/'tests/main.c',*objects,'-o',build/'test'])
    result=subprocess.run([str(build/'test')],env=dict(os.environ,ASAN_OPTIONS='allocator_may_return_null=1'),
                          text=True,capture_output=True,timeout=90)
    print(result.stdout,end='');print(result.stderr,end='',file=sys.stderr)
    if result.returncode:raise SystemExit(f'mimalloc host debug={debug} failed ({result.returncode})')
    if debug:
        run(['cc',*flags,HERE/'tests/large_pages.c',build/'large_pages.o',build/'page_pool.o',
             '-o',build/'large-pages-test'])
        run([build/'large-pages-test'])
if '--host-only' in sys.argv:raise SystemExit(0)

sdk=ROOT/'.toolchain/opt/devkitpro'
cc=sdk/'devkitA64/bin/aarch64-none-elf-gcc'
ar=sdk/'devkitA64/bin/aarch64-none-elf-ar'
nm=sdk/'devkitA64/bin/aarch64-none-elf-nm'
build=HERE/('build'+variant);build.mkdir(exist_ok=True)
flags=common+['-O2','-g','-D__SWITCH__','-DMI_DEBUG=0','-march=armv8-a+crc+crypto',
              '-mtune=cortex-a57','-mtp=soft','-fPIC','-ftls-model=local-exec',
              '-I'+str(sdk/'libnx/include')]
def compile_target(name):
    target=build/(Path(name).stem+'.o')
    run([cc,*flags,'-c',HERE/name,'-o',target]);return target
with concurrent.futures.ThreadPoolExecutor(max_workers=3) as pool:
    objects=list(pool.map(compile_target,sources))
archive=build/'libmineswitch_mimalloc.a'
archive.unlink(missing_ok=True)
run([ar,'rcsD',archive,*objects])
symbols=subprocess.check_output([str(nm),'-g','--defined-only',str(archive)],text=True)
for name in ['_malloc_r','_free_r','_calloc_r','_realloc_r','_memalign_r',
             '_malloc_usable_size_r','_mallinfo_r','_malloc_trim_r','_sbrk_r']:
    if ' '+name+'\n' not in symbols:raise SystemExit('Missing ABI entrypoint: '+name)
if '__malloc_av_' in symbols:raise SystemExit('Old allocator state accidentally present')
receipt={'upstream':pin,'archive_sha256':hashlib.sha256(archive.read_bytes()).hexdigest(),
         'flags':flags,'host_tests':'ASan/UBSan debug and release passed',
         'sources':{name:hashlib.sha256((HERE/name).read_bytes()).hexdigest()
                    for name in sources+['page_pool.h','large_pages.h','newlib_adapter.h','prim_horizon.c','horizon.patch']}}
(build/'verification.json').write_text(json.dumps(receipt,indent=2)+'\n')
print('MIMALLOC_ARCHIVE_READY '+str(archive)+' sha256='+receipt['archive_sha256'])
