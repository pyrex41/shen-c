#!/usr/bin/env python3
"""Bounded comparative Darwin profiles; every process runs the unchanged suite.

The focus protocol stops at load entry and exit. A sampler attaches while the
process is stopped, then it runs only that load until the second stop. Paused
stack samples are reported separately, never treated as application CPU samples.
All times here are diagnostic and ineligible for performance acceptance.
"""
import argparse
from collections import Counter,defaultdict
import json
import os
from pathlib import Path
import re
import signal
import statistics
import subprocess
import time
from release_manifest import DEFAULT,digest,load_release,prepare_suite,validate_suite
OUTPUT_LIMIT=192*1024*1024

def sample_tree(text,symbols):
    # Unlike sample's >=5-hit flat summary, reconstruct self counts from the
    # complete call tree, including low-frequency leaves. Never sum inclusive
    # counts: recursion can put one function on a stack more than once.
    body=text.split('Call graph:',1)[1].split('Total number in stack',1)[0]
    roots=[];stack=[]
    for line in body.splitlines():
        m=re.match(r'^([ +!:|]*)(\d+) (.+)$',line)
        if not m:continue
        node={'depth':len(m[1]),'count':int(m[2]),'name':m[3].split('  (in ',1)[0], 'children':[]}
        while stack and stack[-1]['depth']>=node['depth']:stack.pop()
        if stack:stack[-1]['children'].append(node)
        else:roots.append(node)
        stack.append(node)
    def flatten(node,flat,paused=False):
        paused=paused or node['name'].startswith(('sp_enter','sp_leave','leave','raise','__kill'))
        own=node['count']-sum(c['count'] for c in node['children'])
        if own<0:raise ValueError('inconsistent call tree')
        name=node['name'];shen=symbols.get(name.replace('_profile_impl',''))
        if own:flat['PAUSED_OR_PROFILER' if paused else name+(' ['+shen+']' if shen else '')]+=own
        for child in node['children']:flatten(child,flat,paused)
    threads=[]
    for root in roots:
        flat=Counter();flatten(root,flat)
        threads.append({'thread':root['name'],'samples':root['count'],'self_samples':dict(flat.most_common())})
    return threads

def phases(path):
    events=[json.loads(line) for line in path.read_text().splitlines()]
    if events[-1]!={'event':'finished','open_scopes':1}:raise ValueError('unbalanced scopes')
    totals=Counter();loads=defaultdict(Counter);aborted=0
    for e in events:
        if e['event']!='span':continue
        if e['exclusive_cpu']<-.000001:raise ValueError('negative exclusive time')
        totals[e['phase']]+=e['exclusive_cpu'];loads[e['load']][e['phase']]+=e['exclusive_cpu']
        aborted+=e['aborted']
    root=[e for e in events if e.get('depth')==0]
    if len(root)!=1 or abs(sum(totals.values())-root[0]['cpu'])>.00001:raise ValueError('time not conserved')
    return {'exclusive_cpu':dict(totals),'per_load_exclusive_cpu':dict(loads),'aborted_scopes':aborted,'process_cpu':root[0]['cpu']}

def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--build',type=Path,required=True)
    p.add_argument('--out',type=Path,required=True)
    p.add_argument('--boehm-home',type=Path,required=True)
    p.add_argument('--repetitions',type=int,default=3,choices=range(1,6))
    a=p.parse_args();a.build=a.build.resolve();a.out=a.out.resolve();a.out.mkdir(parents=True,exist_ok=False)
    build=json.loads((a.build/'build.json').read_text());r=load_release();runs=[]
    env=dict(os.environ)
    for key in list(env):
        if key.startswith(('SHEN_','RC_','DYLD_','GC_')):env.pop(key)
    stdin=a.out/'stdin.txt';stdin.write_bytes(b'y\n'*100000)
    record={'scope':'DIAGNOSTIC ONLY: profiler overhead and phase stops; no performance credit',
            'build_sha256':digest(a.build/'build.json'),'manifest_sha256':digest(DEFAULT),'stdin_sha256':digest(stdin),'runs':runs,'repetitions':a.repetitions,
            'harness_sha256':{str(Path(__file__).resolve()):digest(__file__),str(Path(__file__).with_name('release_manifest.py')):digest(Path(__file__).with_name('release_manifest.py'))}}
    def save():(a.out/'result.json').write_text(json.dumps(record,indent=2)+'\n')
    save()
    for mode in ('accounting','whole','c-minus.shen','interpreter.shen'):
        for rep in range(a.repetitions):
            for backend in (('rc','boehm') if rep%2==0 else ('boehm','rc')):
                prefix='%s-%02d-%s'%(mode,rep+1,backend);directory=a.out/prefix;directory.mkdir()
                work=prepare_suite(r,directory/'suite');binary=a.build/('shen-'+backend)
                assert digest(binary)==build['binaries'][str(binary)]
                phasefile=directory/'phases.jsonl';runenv={**env,'SHEN_PROFILE_FILE':str(phasefile)}
                focus=mode not in ('accounting','whole')
                if backend=='boehm':runenv['SHEN_C_HOME']=str(a.boehm_home.resolve())
                if focus:runenv['SHEN_PROFILE_FOCUS']=mode
                command=[str(binary),'script',r['suite_entrypoint']]
                with stdin.open('rb') as inp,(directory/'stdout.txt').open('wb') as log,(directory/'stderr.txt').open('wb') as err,(directory/'sampler.log').open('wb') as slog:
                    child=subprocess.Popen(command,cwd=work,env=runenv,stdin=inp,stdout=log,stderr=err,start_new_session=True)
                    start=time.monotonic();sampler=None;stage=0;resume_at=None;reason=None
                    def sample():
                        nonlocal sampler
                        samplecmd=['/usr/bin/sample',str(child.pid),'5' if focus else '30','5','-mayDie','-file',str(directory/'sample.txt')]
                        sampler=subprocess.Popen(samplecmd,stdout=slog,stderr=slog,start_new_session=True)
                    if mode=='whole':sample()
                    try:
                        while child.poll() is None:
                            now=time.monotonic()
                            if now-start>90:reason='TIMEOUT'
                            if sum(f.stat().st_size for f in directory.glob('*') if f.is_file())>OUTPUT_LIMIT:reason='OUTPUT_LIMIT'
                            if reason:raise RuntimeError(reason)
                            if focus and phasefile.exists():
                                events=phasefile.read_text(errors='replace')
                                if stage==0 and '"event":"focus-start"' in events:
                                    sample();resume_at=now+.5;stage=1
                                if stage==1 and now>=resume_at:
                                    os.kill(child.pid,signal.SIGCONT);stage=2
                                if stage==2 and '"event":"focus-end"' in events:stage=3
                                if stage==3 and sampler.poll() is not None:
                                    os.kill(child.pid,signal.SIGCONT);stage=4
                            time.sleep(.02)
                        if sampler:
                            # sample writes its expanded tree AFTER the child
                            # exits too. Bound symbol processing and that output.
                            deadline=time.monotonic()+45
                            while sampler.poll() is None:
                                if time.monotonic()>deadline:raise RuntimeError('sampler analysis timeout')
                                report=directory/'sample.txt'
                                if report.exists() and report.stat().st_size>OUTPUT_LIMIT:raise RuntimeError('sampler output limit')
                                time.sleep(.02)
                            if (directory/'sample.txt').stat().st_size>OUTPUT_LIMIT:raise RuntimeError('sampler final output limit')
                    finally:
                        for process in (child,sampler):
                            if process and process.poll() is None:os.killpg(process.pid,signal.SIGKILL);process.wait()
                text=(directory/'stdout.txt').read_text(errors='replace')+'\n'+(directory/'stderr.txt').read_text(errors='replace')
                assert digest(binary)==build['binaries'][str(binary)],'binary changed during profile'
                assert digest(a.build/'build.json')==record['build_sha256'],'build receipt changed'
                assert all(digest(path)==sha for path,sha in record['harness_sha256'].items()),'harness changed'
                validation=validate_suite(r,text,work)
                run={'backend':backend,'mode':mode,'repetition':rep+1,'exit':child.returncode,'suite':validation,'binary_sha256':digest(binary),'command':command,'environment_overrides':{k:v for k,v in runenv.items() if k.startswith('SHEN_')},
                     'phase_accounting':phases(phasefile),'focus_completed':not focus or stage==4}
                if sampler:
                    run['sampler_exit']=sampler.returncode
                    run['threads']=sample_tree((directory/'sample.txt').read_text(errors='replace'),build['symbol_maps'][backend])
                run['status']='PASS' if child.returncode==0 and validation['status']=='PASS' and run['focus_completed'] and (not sampler or sampler.returncode==0) else 'FAIL'
                runs.append(run);save();print(prefix,run['status'],flush=True)
                if run['status']!='PASS':raise SystemExit(1)
    summary={}
    for backend in ('rc','boehm'):
        accounting=[x['phase_accounting'] for x in runs if x['backend']==backend and x['mode']=='accounting']
        summary[backend]={phase:statistics.median(x['exclusive_cpu'].get(phase,0) for x in accounting) for phase in ('read','translate','typecheck','execute','other')}
    record['accounting_medians']=summary;record['status']='PASS';save()

if __name__=='__main__':main()
