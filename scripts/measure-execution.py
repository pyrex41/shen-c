#!/usr/bin/env python3
"""Measure warmed execution separately from kernel boot, source load and warmup.

These representative controls are diagnostic, not suite/Boehm acceptance.
Each sample owns a fresh process; mode order alternates by round.
"""
import argparse, datetime, hashlib, json, os, platform, re, signal, statistics, subprocess, time
from pathlib import Path
from release_manifest import load_release, prepare_suite
ROOT=Path(__file__).resolve().parents[1]
DEFAULT={'tail-count':300,'list-build':1000,'queens':40,'prolog':3000,'typecheck':15000}

def sha(path):return hashlib.sha256(path.read_bytes()).hexdigest()

def main():
    ap=argparse.ArgumentParser(__doc__)
    ap.add_argument('binary',type=Path);ap.add_argument('--cache',type=Path,required=True)
    ap.add_argument('--output',type=Path);ap.add_argument('--rounds',type=int,default=3)
    ap.add_argument('--cases',default=','.join(DEFAULT));ap.add_argument('--timeout',type=float,default=180)
    args=ap.parse_args();binary=args.binary.resolve();cache=args.cache.resolve();release=load_release()
    if args.rounds<1 or args.rounds>10 or args.timeout<=0:ap.error('invalid budget')
    output=(args.output or ROOT/'evidence/rc'/('execution-'+datetime.datetime.now(datetime.timezone.utc).strftime('%Y%m%dT%H%M%SZ'))).resolve()
    output.mkdir(parents=True,exist_ok=False)
    sources=[ROOT/'test/rc/bench_execution.c',Path(__file__).resolve(),release['_path']]+list((ROOT/'src/rc').glob('*.[ch]'))
    record={'status':'RUNNING','host':platform.platform(),'machine':platform.machine(),'binary_sha256':sha(binary),
            'source_sha256':{str(p.relative_to(ROOT)):sha(p) for p in sources},
            'scope':'warmed representative execution; not whole-suite or Boehm acceptance','samples':[]}
    def save():(output/'result.json').write_text(json.dumps(record,indent=2)+'\n')
    save();print('Execution evidence:',output,flush=True)
    base={k:v for k,v in os.environ.items() if not k.startswith(('SHEN_RC_','ASAN_OPTIONS','UBSAN_OPTIONS'))}
    base.update(SHEN_RC_NATIVE_CACHE=str(cache),SHEN_RC_NATIVE_STATS='1')
    try:
        for round in range(args.rounds):
            for case in args.cases.split(','):
                count=DEFAULT[case]
                for mode in (('native','interpret') if round%2==0 else ('interpret','native')):
                    name=f'{round}-{case}-{mode}';destination=output/name;destination.mkdir()
                    suite=prepare_suite(release,destination/'suite')
                    env=dict(base,SHEN_RC_EXECUTION=mode)
                    if mode=='native':env['SHEN_RC_VERIFY_NATIVE']='1'
                    command=[str(binary),case,str(count),str(suite)]
                    with (destination/'run.log').open('wb') as log:
                        process=subprocess.Popen(command,env=env,stdout=log,stderr=subprocess.STDOUT,start_new_session=True)
                        try:process.wait(timeout=args.timeout)
                        except subprocess.TimeoutExpired:
                            os.killpg(process.pid,signal.SIGKILL);process.wait();raise RuntimeError('timeout: '+name)
                    text=(destination/'run.log').read_text(errors='replace')
                    metrics=[json.loads(line[len('RC_EXEC_BENCH '):]) for line in text.splitlines() if line.startswith('RC_EXEC_BENCH ')]
                    if process.returncode or len(metrics)!=1:raise RuntimeError('failed: '+name)
                    result=metrics[0]
                    if result['compiler_child_cpu'] or (mode=='native' and result['interpreter_entries']):raise RuntimeError('impure timing: '+name)
                    record['samples'].append(dict(name=name,mode=mode,round=round,command=command,
                        log_sha256=sha(destination/'run.log'),metrics=result))
                    save();print(name,round_metric(result),flush=True)
        record['comparisons']={}
        for case in args.cases.split(','):
            ratios=[]
            for index in range(args.rounds):
                pair={s['mode']:s['metrics'] for s in record['samples'] if s['round']==index and s['metrics']['case']==case}
                ratios.append(pair['native']['user']/pair['interpret']['user'])
            record['comparisons'][case]={'paired_user_ratios':ratios,'median_user_ratio':statistics.median(ratios)}
        record['binary_unchanged']=sha(binary)==record['binary_sha256']
        record['sources_unchanged']=all(sha(ROOT/p)==digest for p,digest in record['source_sha256'].items())
        if not record['binary_unchanged'] or not record['sources_unchanged']:raise RuntimeError('changed artifacts')
        record['status']='PASS';save();print(json.dumps(record['comparisons'],indent=2),flush=True)
        return 0
    except Exception as error:
        record.update(status='FAIL',error=str(error));save();raise

def round_metric(result):return {k:result[k] for k in ('wall','user','system','retain_calls','release_calls','system_allocations','collections')}
if __name__=='__main__':raise SystemExit(main())
