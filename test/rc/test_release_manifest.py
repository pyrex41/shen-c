#!/usr/bin/env python3
"""Adversarial validator controls: final green reports cannot hide failure."""
from pathlib import Path
import sys
import tempfile
sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'scripts'))
from release_manifest import load_release, prepare_suite, validate_suite
r = load_release()
with tempfile.TemporaryDirectory() as tmp:
    work = prepare_suite(r, Path(tmp) / 'suite')
    count, lines = 0, []
    for group in r['reports']:
        count += group['assertions']
        lines += ['passed ... '+str(count), 'failed ... 0']
    text = '\n'.join(lines)
    total = 'passed ... ' + str(r['expected_tests'])
    assert total in text
    outputs = r['expected_outputs']
    for name in outputs: # A shipped expected output must be written by this run.
        assert validate_suite(r,text,work)['status']=='FAIL'
        (work/name).write_bytes(b'hello')
    assert validate_suite(r,text,work)['status']=='PASS'
    for bad in [text.replace('failed ... 0','failed ... 1',1), text.replace(total,'passed ... '+str(r['expected_tests']-2)), '\n'.join(lines[2:]), text+'\nfailed with error hidden', text+'\n'+text]:
        assert validate_suite(r,bad,work)['status']=='FAIL'
    for name in outputs:
        (work/name).write_bytes(b'wrong')
        assert validate_suite(r,text,work)['status']=='FAIL'
        (work/name).write_bytes(b'hello')
    (work/'runme.shen').write_text('changed')
    assert validate_suite(r,text,work)['status']=='FAIL'
    (work/'runme.shen').unlink()
    assert validate_suite(r,text,work)['status']=='FAIL'
    (work/'stray.txt').write_text('unexpected')
    assert validate_suite(r,text,work)['status']=='FAIL'
print('Pinned release and adversarial suite validator controls PASS')

import importlib.util
spec=importlib.util.spec_from_file_location('benchmark_rc',Path(__file__).resolve().parents[2]/'scripts/benchmark-rc.py')
module=importlib.util.module_from_spec(spec);spec.loader.exec_module(module)
acceptance=module.acceptance
runs=[]
for i in range(1,11):
    for label,user,rss in [('rc',3.5,30),('boehm',4,70)]:
        runs.append({'label':label,'repetition':i,'status':'PASS','user_seconds':user,'wall_seconds':4,'max_rss_bytes':rss})
config={'repetitions':10,'rc_baseline_rss_bytes':30}
assert acceptance(runs,config)['status']=='PASS'
assert acceptance(runs[:-1],config)['status']=='OPEN'
for r in runs:
    if r['label']=='rc' and r['repetition']>5:r['user_seconds']=3.8
assert acceptance(runs,config)['status']=='OPEN' # one batch above .90
print('Two-batch performance gate controls PASS')
