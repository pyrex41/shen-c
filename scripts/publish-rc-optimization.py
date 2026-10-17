#!/usr/bin/env python3
"""Publish bounded portable evidence copies; retain original and published hashes.

Raw binaries, objects, vendor suite copies and generated build trees stay local.
Published text replaces workspace/home paths; original bytes remain bound by SHA.
"""
import argparse
import gzip
import hashlib
import json
from pathlib import Path
import re

p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--input',type=Path,action='append',required=True)
p.add_argument('--out',type=Path,required=True)
a=p.parse_args();root=Path(__file__).resolve().parents[1];out=a.out.resolve()
out.mkdir(parents=True,exist_ok=False)
allowed={'.json','.jsonl','.log','.out','.err','.txt','.patch','.c','.h','.py','.sh'}
skip={'suite','suite-work','sources','__pycache__'}
replacements=[(str(root),'<OPT_ROOT>'),(str(root.parent/'shen-c-43.1'),'<RC_BASELINE_ROOT>'),
 (str(root.parent/'shen-c-boehm-43.1'),'<BOEHM_ROOT>'),(str(root.parent/'shen-c-profile-43.1'),'<PROFILE_ROOT>'),
 (str(Path.home()),'<HOME>')]
files=[];total=0
sha=lambda data:hashlib.sha256(data).hexdigest()
for directory in a.input:
 directory=directory.resolve();relative=directory.relative_to(root/'evidence/rc')
 for path in sorted(directory.rglob('*')):
  rel=path.relative_to(directory)
  if not path.is_file() or path.is_symlink() or path.suffix not in allowed:continue
  if any(part in skip or part.endswith('-suite') for part in rel.parts):continue
  if path.name in {'kernel.c','app.c'}:continue
  size=path.stat().st_size
  if size>64*1024*1024:raise ValueError('oversized evidence: '+str(path))
  total+=size
  if total>512*1024*1024:raise ValueError('publication input budget exceeded')
  raw=path.read_bytes();text=raw.decode('utf-8',errors='backslashreplace')
  for old,new in replacements:text=text.replace(old,new)
  text=re.sub(r'/var/folders/[^\s\"\']+', '<TMP>',text)
  portable=text.encode();target=out/relative/rel
  if path.suffix in {'.json','.jsonl','.py','.sh'}:
   published=portable
  else:
   target=target.with_name(target.name+'.gz');published=gzip.compress(portable,mtime=0)
  target.parent.mkdir(parents=True,exist_ok=True);target.write_bytes(published)
  files.append({'raw_path':str(path.relative_to(root)),'raw_sha256':sha(raw),
   'raw_bytes':len(raw),'published_path':str(target.relative_to(out)),
   'published_sha256':sha(published),'published_bytes':len(published)})
record={'scope':__doc__,'files':files,'raw_bytes':total,'published_bytes':sum(x['published_bytes'] for x in files)}
(out/'publication.json').write_text(json.dumps(record,indent=2)+'\n')
print(json.dumps({k:record[k] for k in ('raw_bytes','published_bytes')}));print('files',len(files))
