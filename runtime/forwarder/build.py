#!/usr/bin/env python3
"""Build MineSwitch's own HOME forwarder; never embed a keyset in artifacts."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import struct
import subprocess
import sys
import tempfile
from PIL import Image
from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes

ROOT=Path(__file__).resolve().parents[2]
if os.environ.get('MINESWITCH_BUILD_LIMITED')!='1':
    os.execv(sys.executable,[sys.executable,str(ROOT/'runtime/limit-build.py'),'--',sys.executable,__file__,*sys.argv[1:]])
parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument('--keys',type=Path,required=True)
args=parser.parse_args()
TITLE=0x01004D5357490000
NRO='sdmc:/switch/mineswitch/MineSwitch.nro'
SDK=ROOT/'.toolchain/opt/devkitpro'
NTON=ROOT/'.upstream/nton'
PACK=ROOT/'.upstream/hacbrewpack'
pins={NTON:'c5d543a91b7cd1df880f98c9ab831be9a40d024f',PACK:'56698df456eac4d5076e85499fe045a90fba790b'}
for path,commit in pins.items():
    assert subprocess.check_output(['git','-C',str(path),'rev-parse','HEAD'],text=True).strip()==commit
keys={}
for line in args.keys.expanduser().read_text().splitlines():
    if '=' in line:
        name,value=line.split('=',1)
        if name.strip() in ('header_key','key_area_key_application_00'):
            keys[name.strip()]=value.split('#',1)[0].strip()
for name,length in [('header_key',64),('key_area_key_application_00',32)]:
    if not re.fullmatch('[0-9a-fA-F]{'+str(length)+'}',keys.get(name,'')):
        raise SystemExit('Required packaging key is missing or malformed: '+name)

build=ROOT/'runtime/forwarder/build';build.mkdir(parents=True,exist_ok=True)
for name in ('exefs','romfs','control','nca','temp','nsp'):
    folder=build/name
    if folder.exists():shutil.rmtree(folder)
    folder.mkdir()
def run(command,**kwargs):subprocess.run([str(x) for x in command],check=True,**kwargs)
cc=SDK/'devkitA64/bin/aarch64-none-elf-gcc'
arch=['-march=armv8-a','-mtune=cortex-a57','-mtp=soft','-fPIE']
flags=arch+['-O2','-g','-Wall','-ffunction-sections','-fdata-sections','-D__SWITCH__','-DVERSION="v2.4.5 MineSwitch"','-I'+str(SDK/'libnx/include')]
run([cc,*flags,'-c',NTON/'rom/source/main.c','-o',build/'main.o'])
run([cc,*arch,'-c',NTON/'rom/source/trampoline.s','-o',build/'trampoline.o'])
env=dict(os.environ,DEVKITPRO=str(SDK),DEVKITA64=str(SDK/'devkitA64'))
run([cc,*arch,'-specs='+str(SDK/'libnx/switch.specs'),'-Wl,--gc-sections,--wrap=exit',
     '-Wl,-Map,'+str(build/'forwarder.map'),build/'main.o',build/'trampoline.o',
     '-L'+str(SDK/'libnx/lib'),'-lnx','-o',build/'forwarder.elf'],env=env)
run([SDK/'tools/bin/elf2nso',build/'forwarder.elf',build/'exefs/main'])
meta=json.loads((NTON/'rom/hbl.json').read_text())
meta.update(name='MineSwitch',title_id=hex(TITLE),title_id_range_min=hex(TITLE),title_id_range_max=hex(TITLE),
            program_id=hex(TITLE),program_id_range_min=hex(TITLE),program_id_range_max=hex(TITLE),address_space_type=3,
            system_resource_size='0x0',version='0x0',optimize_memory_allocation=False,disable_device_address_space_merge=False,
            enable_alias_region_extra_size=False,prevent_code_reads=False,signature_key_generation=0)
# Allow core 3 like other homebrew ports do. libnx pthreads take the whole
# process core mask, and HotSpot sizes its pools from the same mask.
for capability in meta['kernel_capabilities']:
    if capability['type']=='kernel_flags':
        capability['value']['highest_cpu_id']=3
(build/'mineswitch.json').write_text(json.dumps(meta,indent=2)+'\n')
run([SDK/'tools/bin/npdmtool',build/'mineswitch.json',build/'exefs/main.npdm'])
run([SDK/'tools/bin/nacptool','--create','MineSwitch','MineSwitch','0.2.1',build/'control/control.nacp',f'--titleid={TITLE:016x}'])
nacp=bytearray((build/'control/control.nacp').read_bytes())
nacp[0x3035]=0 # No automatic gameplay-recording reservation stealing96MiB from the JVM.
(build/'control/control.nacp').write_bytes(nacp)
with Image.open(ROOT/'launcher/assets/icon.jpg') as original:
    clean=Image.new('RGB',(256,256));clean.paste(original.convert('RGB').resize((256,256)))
    clean.save(build/'control/icon_AmericanEnglish.dat',format='JPEG',quality=95)
(build/'romfs/nextNroPath').write_text(NRO)
(build/'romfs/nextArgv').write_text(NRO) # Settings, not a forwarder argument, choose Xmx.
with tempfile.TemporaryDirectory(prefix='mineswitch-packaging-') as private:
    keyfile=Path(private)/'keys.dat'
    with keyfile.open('x') as stream:
        os.chmod(keyfile,0o600)
        for name,value in keys.items():stream.write(name+' = '+value+'\n')
    with (build/'packager.log').open('w') as output:
        run([PACK/'hacbrewpack','-k',keyfile,'--nologo','--exefsdir',build/'exefs',
             '--romfsdir',build/'romfs','--controldir',build/'control','--ncadir',build/'nca',
             '--tempdir',build/'temp','--nspdir',build/'nsp'],cwd=build,stdout=output,stderr=subprocess.STDOUT)
packages=list((build/'nsp').glob('*.nsp'))
assert len(packages)==1,'Expected one package'
package=packages[0].read_bytes();assert package[:4]==b'PFS0'
count,names_bytes=struct.unpack_from('<II',package,4);names_start=16+24*count;data_start=names_start+names_bytes
contents=[]
for index in range(count):
    offset,size,name_offset=struct.unpack_from('<QQI',package,16+24*index)
    end=package.index(0,names_start+name_offset,names_start+names_bytes)
    name=package[names_start+name_offset:end].decode()
    data=package[data_start+offset:data_start+offset+size];assert len(data)==size
    assert name.startswith(hashlib.sha256(data).hexdigest()[:32]),'NCA content ID mismatch'
    header=b''
    for sector in range(6):
        cipher=Cipher(algorithms.AES(bytes.fromhex(keys['header_key'])),modes.XTS(sector.to_bytes(16,'big'))).decryptor()
        header+=cipher.update(data[sector*512:(sector+1)*512])+cipher.finalize()
    assert header[0x200:0x204] in (b'NCA2',b'NCA3')
    program_id=struct.unpack_from('<Q',header,0x210)[0]
    assert program_id==TITLE and struct.unpack_from('<Q',header,0x208)[0]==size
    for section in range(4):
        start,end=struct.unpack_from('<II',header,0x240+16*section)
        if end:
            assert start<end and end*512<=size
            assert hashlib.sha256(header[0x400+512*section:0x600+512*section]).digest()==header[0x280+32*section:0x2a0+32*section]
    contents.append({'name':name,'bytes':size,'type':header[0x205],'program_id':f'{program_id:016X}'})
assert sorted(x['type'] for x in contents)==[0,1,2],'Expected program,meta,control content only'
dest=ROOT/'dist/MineSwitch.nsp';dest.parent.mkdir(exist_ok=True);dest.write_bytes(package)
receipt={'nsp':str(dest),'sha256':hashlib.sha256(package).hexdigest(),'title_id':f'{TITLE:016X}','nro_path':NRO,
         'contents':contents,'sources':{str(p.relative_to(ROOT)):c for p,c in pins.items()},'address_space_type':3,
         'video_capture':'disabled;no96MiB automatic-recording reserve','cpu_cores':'0-3','validation':'PFS0 bounds,NCA encrypted headers,program IDs,content hashes,section-header hashes passed;device install not tested'}
(ROOT/'logs/mineswitch-nsp-receipt.json').write_text(json.dumps(receipt,indent=2)+'\n')
print('NSP_READY',dest,'bytes='+str(len(package)),'sha256='+receipt['sha256'])
