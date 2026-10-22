#!/usr/bin/env python3
"""Failure and concurrent cache controls for the strict native backend and its compiler-failure fallback."""
import argparse, json, os, re, subprocess, sys, tempfile, time
from pathlib import Path

def main():
    ap=argparse.ArgumentParser(__doc__);ap.add_argument('probe',type=Path);args=ap.parse_args();probe=args.probe.resolve()
    with tempfile.TemporaryDirectory(prefix='shen-compile-cache-') as directory:
        root=Path(directory);cache=root/'cache'
        base=dict(os.environ,SHEN_RC_EXECUTION='native',SHEN_RC_VERIFY_NATIVE='1',SHEN_RC_NATIVE_STATS='1',SHEN_RC_NATIVE_CACHE=str(cache))
        def run(extra=None,success=True,expression='(+ 1 2)',expected='3\n',strict=True):
            forms=expression if isinstance(expression,list) else [expression]
            env=dict(base,**(extra or {}))
            if not strict:env['SHEN_RC_VERIFY_NATIVE']='0'
            p=subprocess.run([str(probe)]+forms,env=env,capture_output=True,text=True,timeout=30)
            assert (p.returncode==0)==success,(p.returncode,p.stdout,p.stderr)
            if success:assert p.stdout==expected,p.stdout
            metrics=[dict(re.findall(r'(\w+)=([0-9.]+)',x)) for x in p.stderr.splitlines() if x.startswith('RC_NATIVE ')]
            assert metrics,p.stderr
            if strict:assert all(x['interpreter_entries']=='0' for x in metrics),metrics
            return metrics[-1],p
        s,_=run();assert s['compiles']=='1',s
        # The embedded default compiler, recorded by the first run's cache.
        config=dict(x.split(' ',1) for x in next(cache.glob('compile-*.config')).read_text().splitlines()[1:])
        default_cc=bytes.fromhex(config['cc']).decode()
        s,_=run();assert s['compiles']=='0' and s['disk_hits']=='1',s
        # The packed image must bind current literals, reject damaged images,
        # and preserve ordinary native compilation for an unseen code shape.
        packer=Path(__file__).resolve().with_name('bundle-native-cache.py')
        subprocess.run([sys.executable,str(packer),'--cache',str(cache)],env=base,check=True,timeout=60,capture_output=True)
        s,_=run(expression='(+ 20 22)',expected='42\n')
        assert s['compiles']=='0' and s['disk_hits']=='0' and s['bundle_hits']=='1',s
        # Constant-table helpers and typedefs from separate native modules must
        # have distinct names when the packer combines them into one C image.
        giant='(cons 1 '*80+'()'+')'*80
        table_expressions=[f'(let X {giant} (+ 41 (hd X)))',f'(let X {giant} (+ 42 (+ 0 (hd X))))']
        for marker,expression in zip((41,42),table_expressions):
            s,_=run(expression=expression,expected=str(marker+1)+'\n')
            assert s['compiles']=='1',s
        subprocess.run([sys.executable,str(packer),'--cache',str(cache)],env=base,check=True,timeout=60,capture_output=True)
        for marker,expression in zip((41,42),table_expressions):
            s,_=run(expression=expression,expected=str(marker+1)+'\n')
            assert s['compiles']=='0' and s['bundle_hits']=='1',s
        usage=root/'used-keys.txt'
        subprocess.run([sys.executable,str(packer),'--cache',str(cache),'--record-usage',str(usage)],env=base,check=True,timeout=60,capture_output=True)
        s,_=run();assert s['bundle_hits']=='1' and len(usage.read_text().splitlines())==1,s
        subprocess.run([sys.executable,str(packer),'--cache',str(cache),'--only-used',str(usage)],env=base,check=True,timeout=60,capture_output=True)
        s,_=run();assert s['bundle_hits']=='1' and s['compiles']=='0',s
        writers=[subprocess.Popen([sys.executable,str(packer),'--cache',str(cache),'--only-used',str(usage)],env=base,stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True) for _ in range(2)]
        for writer in writers:
            out,err=writer.communicate(timeout=60);assert writer.returncode==0,(out,err)
        s,_=run();assert s['bundle_hits']=='1' and s['compiles']=='0',s
        # Rebuilding another image from canonical source requires neither the
        # original machine code nor individual cached source files.
        for path in cache.glob('*.c'):
            if re.fullmatch('[0-9a-f]{32}',path.stem):path.unlink()
        manifest=next(cache.glob('bundle-*.json'))
        subprocess.run([sys.executable,str(packer),'--cache',str(cache),'--source-bundle',str(manifest)],env=base,check=True,timeout=60,capture_output=True)
        s,_=run();assert s['bundle_hits']=='1' and s['compiles']=='0',s
        s,_=run({'SHEN_RC_NATIVE_BUNDLE':'off'});assert s['compiles']=='1' and s['bundle_hits']=='0',s
        s,_=run({'SHEN_RC_NATIVE_BUNDLE':'off'});assert s['disk_hits']=='1' and s['bundle_hits']=='0',s
        s,_=run(expression='(if true 41 42)',expected='41\n');assert s['compiles']=='1' and s['bundle_hits']=='0',s
        packed=next(cache.glob('bundle-*.so'));receipt=packed.with_suffix('.receipt')
        valid_receipt=receipt.read_bytes();receipt.write_bytes(b'bad receipt')
        s,_=run();assert s['disk_hits']=='1' and s['bundle_hits']=='0',s
        receipt.write_bytes(valid_receipt);packed.write_bytes(b'bad native image')
        s,_=run();assert s['disk_hits']=='1' and s['bundle_hits']=='0',s
        for obj in cache.glob('*.so'):obj.write_bytes(b'corrupt object')
        s,_=run();assert s['compiles']=='1',s
        # Two writers must publish a complete matching object/source/receipt.
        shared=dict(base,SHEN_RC_NATIVE_CACHE=str(root/'shared'))
        children=[subprocess.Popen([str(probe),'(+ 1 2)'],env=shared,stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True) for _ in range(2)]
        stats=[]
        for child in children:
            out,err=child.communicate(timeout=30);assert child.returncode==0 and out=='3\n',(out,err)
            stats.append(dict(re.findall(r'(\w+)=([0-9.]+)',err)))
        assert sum(int(x['compiles']) for x in stats)==1,stats
        # Deliberately different compiler settings cannot reuse the old object.
        s,_=run({'SHEN_RC_NATIVE_CFLAGS':'-O0'});assert s['compiles']=='1',s
        for cc,expected in ((str(root/'missing-cc'),'127'),('/usr/bin/false','1')):
            s,p=run({'SHEN_RC_NATIVE_CC':cc},False);assert 'status '+expected in p.stderr,p.stderr
        slow=root/'slow-cc';slow.write_text('#!/bin/sh\nsleep 30\n');slow.chmod(0o700)
        start=time.monotonic();s,p=run({'SHEN_RC_NATIVE_CC':str(slow),'SHEN_RC_COMPILE_TIMEOUT':'0.2'},False)
        assert 'status 124' in p.stderr and time.monotonic()-start<10,p.stderr
        # Outside SHEN_RC_VERIFY_NATIVE a failed or timed-out compile interprets
        # that form: same output, exit 0, one notice, and nothing published, so
        # the next process compiles it again. Definitions and closures cover the
        # lazy-body fallback as well as top-level forms.
        program=['(defun fallback-square (X) (* X X))','(defun fallback-sum (L) (if (= L ()) 0 (+ (hd L) (fallback-sum (tl L)))))',
                 '(defun fallback-map (F L) (if (= L ()) () (cons (F (hd L)) (fallback-map F (tl L)))))',
                 '(fallback-sum (fallback-map (lambda X (fallback-square X)) (cons 1 (cons 2 (cons 3 ())))))','((freeze (trap-error (hd 4) (lambda E 42))))']
        program_output='fallback-square\nfallback-sum\nfallback-map\n14\n42\n'
        # One compiler path for every mode keeps the cache key fixed, so the
        # working run after a failure proves the failure was not cached.
        switch=root/'switch-cc';switch.write_text('#!/bin/sh\ncase "$FALLBACK_MODE" in fail) exit 1;; slow) exec sleep 30;; esac\nexec "$FALLBACK_REAL_CC" "$@"\n');switch.chmod(0o700)
        switched={'SHEN_RC_NATIVE_CC':str(switch),'FALLBACK_REAL_CC':default_cc}
        reference,_=run(dict(switched,SHEN_RC_NATIVE_CACHE=str(root/'switch-reference')),expression=program,expected=program_output)
        for name,expected,extra in (('missing','127',{'SHEN_RC_NATIVE_CC':str(root/'missing-cc')}),('fail','1',dict(switched,FALLBACK_MODE='fail')),
                                    ('slow','124',dict(switched,FALLBACK_MODE='slow',SHEN_RC_COMPILE_TIMEOUT='0.2'))):
            retry=root/('retry-'+name);extra=dict(extra,SHEN_RC_NATIVE_CACHE=str(retry))
            start=time.monotonic()
            s,p=run(extra,strict=False)
            assert p.stderr.count('interpreting instead')==1 and 'status '+expected in p.stderr,p.stderr
            assert s['fallbacks']=='1' and s['failures']=='0' and int(s['interpreter_entries'])>0,s
            s,p=run(extra,expression=program,expected=program_output,strict=False)
            assert p.stderr.count('interpreting instead')==1 and int(s['fallbacks'])>=2 and s['failures']=='0',s
            assert time.monotonic()-start<30,'fallback compiles were not bounded'
            assert not list(retry.glob('*.so')) and not list(retry.glob('*.receipt')),sorted(retry.iterdir())
            if name!='missing':
                s,_=run(dict(switched,SHEN_RC_NATIVE_CACHE=str(retry)),expression=program,expected=program_output)
                assert s['compiles']==reference['compiles'] and s['disk_hits']=='0',(s,reference)
        s,_=run({'SHEN_RC_NATIVE_CACHE':str(root/'bounded'),'SHEN_RC_NATIVE_CACHE_BYTES':'0'})
        assert not list((root/'bounded').glob('*.so'))
        private=root/'not-private';private.mkdir(mode=0o777);private.chmod(0o777)
        p=subprocess.run([str(probe),'(+ 1 2)'],env=dict(base,SHEN_RC_NATIVE_CACHE=str(private)),capture_output=True,text=True,timeout=30)
        assert p.returncode!=0 and 'private cache' in p.stderr,p.stderr
        print('RC strict cache: cold/warm, packed literals/misses/corruption, concurrency, flags, compiler failures and interpreter fallback, pruning and private directory PASS')
if __name__=='__main__':main()
