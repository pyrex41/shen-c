#!/usr/bin/env python3
"""Bounded Darwin sampling of a pinned complete suite; never timing evidence."""
import argparse
import json
import os
from pathlib import Path
import signal
import subprocess
import time
from release_manifest import DEFAULT, digest, load_release, prepare_suite, validate_suite

p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--cli',type=Path,required=True)
p.add_argument('--manifest',type=Path,default=DEFAULT)
p.add_argument('--out',type=Path,required=True)
a=p.parse_args();r=load_release(a.manifest);cli=a.cli.resolve();out=a.out.resolve()
out.mkdir(parents=True,exist_ok=False);work=prepare_suite(r,out/'suite')
env=dict(os.environ)
for key in list(env):
    if key.startswith(('SHEN_','RC_','DYLD_')):env.pop(key)
record={'scope':'diagnostic sampling only; no performance credit','binary_sha256':digest(cli),
        'manifest_sha256':digest(a.manifest),'samples':[]}
# Bootstrap has its own process interval. Complete suite load/typecheck/execution
# retain their individual nested Shen intervals in the benchmark stage artifacts.
cmd=['/usr/bin/time','-l',str(cli),'eval','-e','(version)']
boot=subprocess.run(cmd,cwd=out,env=env,stdin=subprocess.DEVNULL,capture_output=True,timeout=30)
(out/'startup.out').write_bytes(boot.stdout);(out/'startup.err').write_bytes(boot.stderr)
record['startup']={'command':cmd,'exit':boot.returncode}
with (out/'stdin.txt').open('w+b') as inp,(out/'suite.log').open('wb') as log:
    inp.write(b'y\n'*1024);inp.seek(0)
    command=[str(cli),'script',r['suite_entrypoint']]
    child=subprocess.Popen(command,cwd=work,env=env,stdin=inp,stdout=log,stderr=subprocess.STDOUT,start_new_session=True)
    started=time.monotonic();record['command']=command
    try:
        for i in range(3):
            if child.poll() is not None:break
            path=out/('sample-%d.txt'%i)
            cmd=['/usr/bin/sample',str(child.pid),'1','5','-file',str(path)]
            sample=subprocess.Popen(cmd,stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL,start_new_session=True)
            sample_start=time.monotonic();reason=None
            while sample.poll() is None:
                if time.monotonic()-sample_start>15:reason='TIMEOUT'
                if path.exists() and path.stat().st_size>64*1024*1024:reason='OUTPUT_LIMIT'
                if reason:os.killpg(sample.pid,signal.SIGKILL);break
                time.sleep(.1)
            sample.wait()
            record['samples'].append({'command':cmd,'exit':sample.returncode,'reason':reason,
              'start_offset_seconds':round(sample_start-started,3),'bytes':path.stat().st_size if path.exists() else 0,
              'sha256':digest(path) if path.exists() else None})
        while child.poll() is None:
            if time.monotonic()-started>120 or log.tell()>64*1024*1024:
                os.killpg(child.pid,signal.SIGKILL);break
            time.sleep(.1)
        child.wait()
    finally:
        if child.poll() is None:os.killpg(child.pid,signal.SIGKILL);child.wait()
record['exit']=child.returncode
record['suite_validation']=validate_suite(r,(out/'suite.log').read_text(errors='replace'),work)
record['binary_unchanged']=digest(cli)==record['binary_sha256']
(out/'result.json').write_text(json.dumps(record,indent=2)+'\n')
raise SystemExit(0 if child.returncode==0 and record['suite_validation']['status']=='PASS' and record['binary_unchanged'] else 1)
