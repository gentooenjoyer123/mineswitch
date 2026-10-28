#!/usr/bin/env python3
import hashlib,json,os,re,secrets,subprocess,sys
from pathlib import Path
ROOT=Path(__file__).resolve().parents[2];HERE=Path(__file__).resolve().parent
if os.environ.get('MINESWITCH_BUILD_LIMITED')!='1':
 os.execv(sys.executable,[sys.executable,str(ROOT/'runtime/limit-build.py'),'--',sys.executable,__file__])
build=HERE/'build';build.mkdir(exist_ok=True)
config=HERE/'client.json'
if config.exists():state=json.loads(config.read_text())
else:
 state={'token':secrets.token_hex(24),'suffix':secrets.token_hex(4),'host':'192.168.3.177','port':31339}
 state['filename']='MineSwitch-Maintenance-'+state['suffix']+'.nro'
 config.write_text(json.dumps(state,indent=2)+'\n');config.chmod(0o600)
owned=set()
for path in (ROOT/'logs').glob('*.sha256'):
 for line in path.read_text().splitlines():
  m=re.fullmatch(r'([0-9a-f]{64})\s+\*?(.+\.nro)',line)
  if m:owned.add((Path(m[2]).name,m[1]))
assert owned
header='#pragma once\nstruct OwnedNro {const char* name;const char* sha256;};\n'
header+='static const OwnedNro owned_nros[]={\n'+''.join('{'+json.dumps(name)+','+json.dumps(digest)+'},\n' for name,digest in sorted(owned))+'};\n'
for name,value in [('TOKEN',state['token']),('SUFFIX',state['suffix']),('FILENAME',state['filename'])]:header+='#define MAINTENANCE_'+name+' '+json.dumps(value)+'\n'
(build/'owned_nros.h').write_text(header)
sdk=ROOT/'.toolchain/opt/devkitpro';cc=sdk/'devkitA64/bin/aarch64-none-elf-g++'
base=HERE/state['filename'].removesuffix('.nro')
arch=['-march=armv8-a+crc+crypto','-mtune=cortex-a57','-mtp=soft','-fPIE','-ftls-model=local-exec']
flags=arch+['-D__SWITCH__','-std=gnu++17','-O2','-g','-Wall','-Wextra','-Werror','-ffunction-sections','-fdata-sections','-I'+str(build),'-isystem',str(sdk/'libnx/include'),'-isystem',str(sdk/'portlibs/switch/include')]
def run(args,**kw):subprocess.run([str(x) for x in args],check=True,**kw)
run([cc,*flags,'-c',HERE/'main.cpp','-o',build/'main.o'])
run([cc,*arch,'-specs='+str(sdk/'libnx/switch.specs'),'-Wl,--gc-sections',build/'main.o','-L'+str(sdk/'portlibs/switch/lib'),'-ljansson','-lmbedcrypto','-L'+str(sdk/'libnx/lib'),'-lnx','-o',str(base)+'.elf'],env=dict(os.environ,DEVKITPRO=str(sdk),DEVKITA64=str(sdk/'devkitA64')))
run([sdk/'tools/bin/nacptool','--create','MineSwitch maintenance','MineSwitch','0.1.0',str(base)+'.nacp'])
run([sdk/'tools/bin/elf2nro',str(base)+'.elf',str(base)+'.nro','--nacp='+str(base)+'.nacp'])
print('MAINTENANCE_READY',str(base)+'.nro','owned_records='+str(len(owned)),'sha256='+hashlib.sha256(Path(str(base)+'.nro').read_bytes()).hexdigest())
