#!/usr/bin/env python3
"""Stream bounded portable diagnostic evidence; preserve raw and published hashes."""
import argparse
import gzip
import hashlib
import json
from pathlib import Path
from release_manifest import ROOT

def wanted(path):
    parts=path.parts
    if 'suite' in parts or any(x.endswith('-suite') for x in parts):return False
    if path.suffix in ('.o','.a') or 'default' in parts or 'boxed' in parts:return False
    if 'sources' in parts:
        if parts[0]=='build-v6':return path.suffix in ('.c','.h')
        if parts[0] in ('counter-control','type-counts'):return path.name=='rc.c'
        return str(path) in ('build-v4/sources/boehm/abi.c','build-v5/sources/boehm/kernel/load.c')
    return path.suffix in ('.json','.jsonl','.out','.err','.log','.txt','.py','.c','.h')

def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--raw',type=Path,required=True);p.add_argument('--out',type=Path,required=True)
    a=p.parse_args();raw=a.raw.resolve();out=a.out.resolve();out.mkdir(parents=True,exist_ok=False)
    files=sorted(p for p in raw.rglob('*') if p.is_file() and wanted(p.relative_to(raw)))
    if len(files)>1000 or sum(p.stat().st_size for p in files)>8*1024**3:raise ValueError('raw publication budget')
    replacements=[(str(ROOT).encode(),b'<PROFILE_ROOT>'),(str(ROOT.parent/'shen-c-boehm-43.1').encode(),b'<BOEHM_ROOT>'),(str(ROOT.parent/'shen-c-43.1').encode(),b'<CHECKPOINT_ROOT>'),(str(Path.home()).encode(),b'<HOME>')]
    records=[];size=0
    for path in files:
        relative=path.relative_to(raw);compressed=path.suffix!='.json'
        target=out/(str(relative)+('.gz' if compressed else ''));target.parent.mkdir(parents=True,exist_ok=True)
        original=hashlib.sha256()
        with path.open('rb') as source,target.open('wb') as destination:
            stream=gzip.GzipFile(fileobj=destination,mode='wb',mtime=0,filename='',compresslevel=6) if compressed else destination
            try:
                for line in source:
                    original.update(line)
                    for old,new in replacements:line=line.replace(old,new)
                    stream.write(line)
            finally:
                if compressed:stream.close()
        size+=target.stat().st_size
        if target.stat().st_size>20*1024**2 or size>128*1024**2:raise ValueError('published size budget')
        records.append({'raw_path':str(path.relative_to(ROOT)),'raw_sha256':original.hexdigest(),'published_path':str(target.relative_to(out)),'published_sha256':hashlib.sha256(target.read_bytes()).hexdigest()})
    (out/'publication.json').write_text(json.dumps({'note':'Portable copies replace local paths. Raw and published hashes differ intentionally. Failed/superseded attempts retained. No binaries, object files, or suite copies published.','files':records,'published_bytes':size},indent=2)+'\n')
    print(len(records),'files;',size,'published bytes')

if __name__=='__main__':main()
