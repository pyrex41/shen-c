#!/usr/bin/env python3
"""Bounded emitted controls; separate from all timing runs."""
from pathlib import Path
import hashlib,json,os,subprocess,time
root=Path.cwd();out=Path(__file__).resolve().parent/'candidate-b/emitted-controls'
out.mkdir(exist_ok=False)
builder=root.parent/'shen-c-release-0.3.0/bin/rc-build'
flags='-O1 -g -DSHEN_RC_STRING_SLICES=1 -DSHEN_RC_VERIFY_OWNERSHIP=1 -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer'
env=dict(os.environ,CC='/usr/bin/cc',CFLAGS=flags,ASAN_OPTIONS='abort_on_error=1',UBSAN_OPTIONS='halt_on_error=1')
sha=lambda p:hashlib.sha256(p.read_bytes()).hexdigest()
record={'scope':'Five existing emitted controls against unchanged released emitter and current opt-in runtime; not complete CLI/platform qualification','builder_sha256':sha(builder),'flags':flags,'sources_sha256':{str(p.relative_to(root)):sha(p) for p in (root/'src/rc').glob('*.[ch]')},'runs':[]}
for name in ['semantics','ownership','callee','primitive_fast','prolog_fast']:
 script=root/('test/rc/test_'+name+'.sh');command=['sh',str(script),str(builder)]
 start=time.monotonic()
 with (out/(name+'.log')).open('wb') as log:
  p=subprocess.run(command,env=env,stdout=log,stderr=subprocess.STDOUT,timeout=240)
 item={'name':name,'command':command,'exit':p.returncode,'seconds':time.monotonic()-start,'script_sha256':sha(script),'log_sha256':sha(out/(name+'.log')),'status':'PASS' if p.returncode==0 else 'FAIL'}
 record['runs'].append(item);(out/'result.json').write_text(json.dumps(record,indent=2)+'\n')
 print(name,item['status'],flush=True);assert p.returncode==0,item
record['status']='PASS';(out/'result.json').write_text(json.dumps(record,indent=2)+'\n')
