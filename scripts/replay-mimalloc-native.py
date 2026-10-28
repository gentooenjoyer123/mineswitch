#!/usr/bin/env python3
"""Run the actual C page pool against recorded native requests in real host RAM mappings."""
import argparse
import ctypes
import importlib.util
import json
import mmap
from pathlib import Path

spec=importlib.util.spec_from_file_location('placement_replay',Path(__file__).with_name('replay-mimalloc-pool.py'))
model=importlib.util.module_from_spec(spec);spec.loader.exec_module(model)

class Pool:
    def __init__(self,library,base,capacity,alignment):
        self.lib=ctypes.CDLL(str(library.resolve()))
        self.memory=mmap.mmap(-1,capacity+2*alignment,flags=mmap.MAP_PRIVATE|mmap.MAP_ANONYMOUS,
                              prot=mmap.PROT_READ|mmap.PROT_WRITE)
        self.anchor=ctypes.c_char.from_buffer(self.memory)
        raw=ctypes.addressof(self.anchor)
        self.base=((raw+alignment-1)&~(alignment-1))+(base&(alignment-1))
        self.lib.nxmi_pool_init.argtypes=(ctypes.c_void_p,ctypes.c_size_t)
        self.lib.nxmi_pool_alloc.argtypes=(ctypes.c_size_t,ctypes.c_size_t,ctypes.c_bool,ctypes.POINTER(ctypes.c_void_p))
        self.lib.nxmi_pool_free.argtypes=(ctypes.c_void_p,ctypes.c_size_t)
        if self.lib.nxmi_pool_init(self.base,capacity):raise RuntimeError('C pool init failed')
    def allocate(self,size,alignment,commit):
        out=ctypes.c_void_p()
        result=self.lib.nxmi_pool_alloc(size,alignment,commit,ctypes.byref(out))
        if result:
            if result!=12:raise RuntimeError(f'Unexpected allocation error {result}')
            return None
        return out.value-self.base
    def free(self,offset,size):
        result=self.lib.nxmi_pool_free(self.base+offset,size)
        if result:raise RuntimeError(f'Invalid replay free {result}: {offset}+{size}')

parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument('log',type=Path);parser.add_argument('library',type=Path)
parser.add_argument('--policy',choices=['split','split_best'],required=True)
parser.add_argument('--output',type=Path,required=True)
args=parser.parse_args()
base,capacity,rows=model.read_trace(args.log)
model.replay(base,capacity,rows,'split',verify=True)
pool=Pool(args.library,base,capacity,max(r['alignment'] for r in rows))
result=model.replay(base,capacity,rows,args.policy,verify=args.policy=='split',native=pool)
result.update({'actual_c_matches_model':True,'records':len(rows),'source':str(args.log),
               'scope':'Real mmap/mprotect backing, actual page_pool.c; allocator request sequence fixed to recorded first failure, no game or GPU execution.'})
args.output.write_text(json.dumps(result,indent=2)+'\n');print(json.dumps(result,indent=2))
