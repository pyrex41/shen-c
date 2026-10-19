#!/usr/bin/env python3
"""Qualify compile-before-execute against the pinned suite; retain all evidence."""
import argparse, datetime, hashlib, json, os, platform, re, resource, signal, subprocess, sys, time
from pathlib import Path
from release_manifest import load_release, prepare_suite, validate_suite
ROOT = Path(__file__).resolve().parents[1]

def sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()

def run(command, cwd, env, timeout, destination):
    before = resource.getrusage(resource.RUSAGE_CHILDREN)
    start = time.monotonic()
    with destination.open('wb') as log:
        p = subprocess.Popen(command, cwd=cwd, env=env, stdout=log, stderr=subprocess.STDOUT, start_new_session=True)
        reason = None
        while p.poll() is None:
            if time.monotonic()-start > timeout:
                reason = 'TIMEOUT'
            elif log.tell() > 64*1024*1024:
                reason = 'OUTPUT_LIMIT'
            if reason:
                os.killpg(p.pid, signal.SIGKILL); p.wait(); break
            time.sleep(.1)
    after = resource.getrusage(resource.RUSAGE_CHILDREN)
    text = destination.read_text(errors='replace')
    stats = [{k: float(v) if '.' in v else int(v) for k,v in re.findall(r'(\w+)=([0-9.]+)', line)}
             for line in text.splitlines() if line.startswith('RC_NATIVE ')]
    return dict(exit=p.returncode, termination=reason, wall=time.monotonic()-start,
                user=after.ru_utime-before.ru_utime, system=after.ru_stime-before.ru_stime,
                stats=stats, log_sha256=sha(destination)), text

def normalized(text):
    text = re.sub(r'run time: \S+ secs', 'run time: X secs', text)
    return '\n'.join(x for x in text.splitlines() if not x.startswith('RC_NATIVE '))

def main():
    ap=argparse.ArgumentParser(__doc__)
    ap.add_argument('binary',type=Path)
    ap.add_argument('--timeout',type=float,default=1200)
    ap.add_argument('--output',type=Path)
    ap.add_argument('--configs',default='interpret,native-cold,native-warm')
    ap.add_argument('--cache',type=Path)
    ap.add_argument('--eager',action='store_true')
    args=ap.parse_args();binary=args.binary.resolve()
    output=args.output or ROOT/'evidence/rc'/('compile-first-'+datetime.datetime.now(datetime.timezone.utc).strftime('%Y%m%dT%H%M%SZ'))
    output=output.resolve();output.mkdir(parents=True,exist_ok=False)
    cache=(args.cache or output/'cache').resolve()
    release=load_release()
    paths=list((ROOT/'src/rc').glob('*.[ch]'))+[ROOT/'Makefile.rc',ROOT/'CMakeLists.txt',Path(__file__).resolve(),release['_path']]
    sources={str(p.relative_to(ROOT)):sha(p) for p in paths}
    record=dict(binary_sha256=sha(binary),source_sha256=sources,host=platform.platform(),machine=platform.machine(),
                release_manifest_sha256=sha(release['_path']),expected_tests=release['expected_tests'],
                compiler=os.environ.get('SHEN_RC_NATIVE_CC','embedded build default'),
                cflags=os.environ.get('SHEN_RC_NATIVE_CFLAGS','-O1'),eager=args.eager,results={})
    def save(): (output/'result.json').write_text(json.dumps(record,indent=2)+'\n')
    save();print('Compile-first evidence:',output,flush=True)
    baseline=None;ok=True
    for index,config in enumerate(args.configs.split(',')):
        destination=output/(str(index)+'-'+config);destination.mkdir()
        suite=prepare_suite(release,destination/'suite')
        env={k:v for k,v in os.environ.items() if not k.startswith(('SHEN_RC_NATIVE','SHEN_RC_EXECUTION','SHEN_RC_VERIFY_NATIVE'))}
        env.update(SHEN_RC_EXECUTION='interpret' if config=='interpret' else 'native',SHEN_RC_NATIVE_CACHE=str(cache),SHEN_RC_NATIVE_STATS='1')
        for name in ('SHEN_RC_NATIVE_CC','SHEN_RC_NATIVE_CFLAGS'): 
            if name in os.environ:env[name]=os.environ[name]
        if config!='interpret':env['SHEN_RC_VERIFY_NATIVE']='1'
        if args.eager:env['SHEN_RC_NATIVE_EAGER']='1'
        command=[str(binary),'eval','-l',release['suite_entrypoint']]
        result,text=run(command,suite,env,args.timeout,destination/'run.log')
        result['command']=command;result['environment']={k:v for k,v in env.items() if k.startswith(('SHEN_RC_','ASAN_OPTIONS','UBSAN_OPTIONS'))}
        result['suite']=validate_suite(release,text,suite)
        result['checks']={'exit':result['exit']==0 and result['termination'] is None,
                          'suite':result['suite']['status']=='PASS'}
        if config!='interpret':
            result['checks']['strict_native']=bool(result['stats']) and all(s['interpreter_entries']==0 and s['failures']==0 for s in result['stats'])
        if config.endswith('warm'):result['checks']['no_compilation']=bool(result['stats']) and all(s['compiles']==0 for s in result['stats'])
        current=normalized(text)
        if baseline is None:baseline=current
        else:result['checks']['output_matches']=current==baseline
        record['results'][str(index)+'-'+config]=result;save();passed=all(result['checks'].values());ok &= passed
        print(config,'PASS' if passed else 'FAIL',json.dumps(result['checks']),flush=True)
        if result['exit']!=0:break
    record['sources_unchanged']=all(sha(ROOT/p)==h for p,h in sources.items());ok &= record['sources_unchanged']
    record['status']='PASS' if ok else 'FAIL';save();return 0 if ok else 1
if __name__=='__main__':raise SystemExit(main())
