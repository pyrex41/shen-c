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
    assert validate_suite(r,text,work)['status']=='FAIL' # missing redirect output
    (work/'test.txt').write_bytes(b'hello')
    assert validate_suite(r,text,work)['status']=='PASS'
    for bad in [text.replace('failed ... 0','failed ... 1',1), text.replace('passed ... 136','passed ... 134'), '\n'.join(lines[2:]), text+'\nfailed with error hidden', text+'\n'+text]:
        assert validate_suite(r,bad,work)['status']=='FAIL'
    (work/'test.txt').write_bytes(b'wrong')
    assert validate_suite(r,text,work)['status']=='FAIL'
    (work/'test.txt').write_bytes(b'hello')
    (work/'runme.shen').write_text('changed')
    assert validate_suite(r,text,work)['status']=='FAIL'
print('Pinned release and adversarial suite validator controls PASS')
