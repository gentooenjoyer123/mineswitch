#!/usr/bin/env python3
"""Authenticated, narrowly scoped deployment to the owner's running helper."""
import argparse,hashlib,json,socket,struct
from pathlib import Path
HERE=Path(__file__).resolve().parent
def recv_exact(s,n):
 chunks=[]
 while n:
  data=s.recv(min(n,65536))
  if not data:raise RuntimeError('Maintenance connection closed')
  chunks.append(data);n-=len(data)
 return b''.join(chunks)
def response(s):
 n=struct.unpack('!I',recv_exact(s,4))[0]
 if n>2*1024*1024:raise RuntimeError('Oversized response')
 return json.loads(recv_exact(s,n))
def request(op,*,file=None,**fields):
 config=json.loads((HERE/'client.json').read_text())
 if file:
  p=Path(file);fields.update(bytes=p.stat().st_size,sha256=hashlib.sha256(p.read_bytes()).hexdigest())
 data=json.dumps({'op':op,'token':config['token'],**fields}).encode()
 with socket.create_connection((config['host'],config['port']),timeout=10) as s:
  s.settimeout(240 if op in ('inventory','delete') else 30)
  s.sendall(struct.pack('!I',len(data))+data)
  result=response(s)
  if file and result.get('ready'):
   with p.open('rb') as stream:
    while block:=stream.read(65536):s.sendall(block)
   result=response(s)
  return result
if __name__=='__main__':
 parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('operation',choices=['status','inventory','title_check','put','delete','exit']);parser.add_argument('--kind');parser.add_argument('--file');parser.add_argument('--path');parser.add_argument('--sha256');parser.add_argument('--output',type=Path);a=parser.parse_args()
 fields={k:v for k,v in vars(a).items() if k in ('kind','path','sha256') and v is not None}
 result=request(a.operation,file=a.file,**fields)
 if a.output:a.output.write_text(json.dumps(result,indent=2)+'\n')
 print(json.dumps(result,indent=2))
