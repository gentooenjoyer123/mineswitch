#!/usr/bin/env python3
"""Overlay the Horizon-specific LWJGL sources on Mojang's pinned 3.3.3 jars."""
from pathlib import Path
import hashlib
import os
import subprocess
import sys
import urllib.request
import zipfile

root=Path(__file__).resolve().parents[2]
if os.environ.get('MINESWITCH_BUILD_LIMITED')!='1':
    os.execv(sys.executable,[sys.executable,str(root/'runtime/limit-build.py'),'--',sys.executable,__file__,*sys.argv[1:]])
source=root/'.upstream/lwjgl'
build=root/'runtime/lwjgl/build'
repo=root/'.minecraft/1.21.1/libraries/org/lwjgl'
annotation=root/'runtime/lwjgl/deps/jsr305-3.0.2.jar'
annotation.parent.mkdir(parents=True,exist_ok=True)
expected='766ad2a0783f2687962c8ad74ceecc38a28b9f72a2d085ee438b7813e928d0c7'
if not annotation.exists():
    urllib.request.urlretrieve('https://repo.maven.apache.org/maven2/com/google/code/findbugs/jsr305/3.0.2/jsr305-3.0.2.jar',annotation)
if hashlib.sha256(annotation.read_bytes()).hexdigest()!=expected:
    raise SystemExit('JSR305 compile-only dependency hash mismatch')
javac=Path(os.environ.get('MINESWITCH_HOST_JAVA','/opt/openjdk-bin-21'))/'bin/javac'
core_source=source/'modules/lwjgl/core/src/main/java/org/lwjgl/system'
sets={
    'lwjgl': [core_source/name for name in ['Platform.java','Library.java','APIUtil.java','HorizonLibrary.java']],
    'lwjgl-opengl': [source/'modules/lwjgl/opengl/src/main/java/org/lwjgl/opengl/GL.java'],
    'lwjgl-openal': [source/'modules/lwjgl/openal/src/main/java/org/lwjgl/openal/ALC.java'],
}
classes={}
for name,files in sets.items():
    original=repo/name/'3.3.3'/(name+'-3.3.3.jar')
    if not original.exists():raise SystemExit('Missing official Minecraft dependency '+str(original))
    output=build/(name+'-classes')
    output.mkdir(parents=True,exist_ok=True)
    paths=[annotation,original,repo/'lwjgl/3.3.3/lwjgl-3.3.3.jar']
    if 'lwjgl' in classes:paths.insert(0,classes['lwjgl'])
    subprocess.run([str(javac),'-J-XX:ActiveProcessorCount=6','--release','8','-proc:none','-encoding','UTF-8',
                    '-cp',os.pathsep.join(map(str,paths)),'-d',str(output),*map(str,files)],check=True)
    classes[name]=output
    changed={p.relative_to(output).as_posix():p for p in output.rglob('*.class')}
    destination=build/(name+'-3.3.3-horizon.jar')
    with zipfile.ZipFile(original) as old,zipfile.ZipFile(destination,'w',zipfile.ZIP_DEFLATED) as new:
        for entry in old.infolist():
            if entry.filename not in changed:new.writestr(entry,old.read(entry.filename))
        for relative,path in sorted(changed.items()):new.write(path,relative)
    print(destination.relative_to(root),f'{len(changed)} actual patched classes')
