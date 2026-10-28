#!/usr/bin/env python3
"""Numerical placement replay of a complete native page-pool trace.

This models addresses/capacity, not kernel permissions, GPU execution or changes
in allocator decisions after a different placement. Reference mode must match
every recorded offset before comparing alternatives.
"""
import argparse
import bisect
import json
from pathlib import Path
import re


def read_trace(path):
    text = path.read_text(errors='replace')
    header = re.search(r'MINESWITCH_POOL_TRACE_BEGIN base=(0x[0-9a-f]+) capacity=(\d+) count=(\d+) complete=(\d+)', text)
    if not header or header[4] != '1':
        raise ValueError('A complete trace is required')
    rows = []
    for match in re.finditer(r'MINESWITCH_POOL_TRACE seq=(\d+) op=(\d+) offset=(\d+) bytes=(\d+) alignment=(\d+) commit=(\d+)', text):
        rows.append(dict(zip(('seq','op','offset','bytes','alignment','commit'),map(int,match.groups()))))
    if len(rows) != int(header[3]) or [r['seq'] for r in rows] != list(range(len(rows))):
        raise ValueError('Trace has missing, duplicated or reordered records')
    return int(header[1],16), int(header[2]), rows


def replay(base, capacity, rows, policy, threshold=1024**2, verify=False, native=None):
    free = [(0,capacity)]
    live = {}  # original interval start -> (original end, remapped start)
    starts = []
    peak = used = 0
    for row in rows:
        n = row['bytes']; old = row['offset']
        if row['op'] in (1,2):
            a = row['alignment']
            high = policy in ('best_high','first_high') or (policy in ('split','split_best') and n > threshold)
            best = policy in ('best_high','best_low','split_best') or (policy == 'split' and high)
            choices = []
            for index,(lo,hi) in enumerate(free):
                if hi-lo < n:
                    continue
                p = (((base+hi-n)&~(a-1))-base if high else ((base+lo+a-1)&~(a-1))-base)
                if p < lo or p+n > hi:
                    continue
                priority = (hi-lo, -p if high else p) if best else (-p if high else p,)
                choices.append((priority,index,p))
            chosen = min(choices) if choices else None
            if native is not None:
                actual=native.allocate(n,a,bool(row['commit']))
                expected=None if chosen is None else chosen[2]
                if actual!=expected:
                    raise ValueError(f'C/model mismatch at {row["seq"]}: {actual} != {expected}')
            if row['op'] == 2 or chosen is None:
                if verify and row['op'] != 2:
                    raise ValueError(f'Reference failed early at record {row["seq"]}')
                if verify and chosen is not None:
                    raise ValueError('Reference unexpectedly satisfies recorded failed request')
                return {'policy':policy,'threshold':threshold,'record':row['seq'],
                        'reached_recorded_failure':row['op']==2,'can_satisfy':chosen is not None,
                        'requested':n,'total_free':sum(hi-lo for lo,hi in free),
                        'largest_free':max((hi-lo for lo,hi in free),default=0),
                        'free_extents':len(free),'peak_backing':peak}
            _,index,p = chosen
            if verify and p != old:
                raise ValueError(f'Reference offset mismatch at {row["seq"]}: {p} != {old}')
            lo,hi = free.pop(index)
            pieces = ([] if lo==p else [(lo,p)]) + ([] if p+n==hi else [(p+n,hi)])
            free[index:index] = pieces
            if old in live:
                raise ValueError('Overlapping trace allocation start')
            bisect.insort(starts,old);live[old]=(old+n,p)
            used += n;peak=max(peak,used)
        elif row['op'] == 3:
            index = bisect.bisect_right(starts,old)-1
            if index < 0:
                raise ValueError('Free of unknown original allocation')
            owner=starts.pop(index);end,new=live.pop(owner)
            if not owner <= old < old+n <= end:
                raise ValueError('Free spans unknown ownership')
            if owner < old:
                bisect.insort(starts,owner);live[owner]=(old,new)
            if old+n < end:
                bisect.insort(starts,old+n);live[old+n]=(end,new+old+n-owner)
            p=new+old-owner;at=bisect.bisect_left(free,(p,p+n))
            if native is not None:
                native.free(p,n)
            free.insert(at,(p,p+n))
            if at and free[at-1][1] == p:
                free[at-1:at+1]=[(free[at-1][0],p+n)];at-=1
            if at+1<len(free) and free[at][1]==free[at+1][0]:
                free[at:at+2]=[(free[at][0],free[at+1][1])]
            used -= n
        else:
            raise ValueError('Unknown operation')
    raise ValueError('No failed request in trace')


if __name__ == '__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('log',type=Path);parser.add_argument('--output',type=Path,required=True)
    args=parser.parse_args();base,capacity,rows=read_trace(args.log)
    reference=replay(base,capacity,rows,'split',verify=True)
    candidates=[replay(base,capacity,rows,p) for p in ('first_low','first_high','best_low','best_high')]
    candidates += [replay(base,capacity,rows,'split',t) for t in (65536,262144,524288,2**20,2**22,2**24,2**26)]
    candidates += [replay(base,capacity,rows,'split_best',t) for t in (65536,262144,2**20,2**22,2**24)]
    result={'source':str(args.log),'base':hex(base),'capacity':capacity,'records':len(rows),
            'reference_offsets_match':True,'reference':reference,'candidates':candidates,
            'scope':'Numerical replay of identical primitive allocation/free requests until first recorded failure; not a gameplay or kernel test.'}
    args.output.write_text(json.dumps(result,indent=2)+'\n')
    print(json.dumps(result,indent=2))
