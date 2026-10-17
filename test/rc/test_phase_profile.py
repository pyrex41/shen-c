#!/usr/bin/env python3
"""Independent checks against misleading diagnostic attribution."""
import importlib.util
import json
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT=Path(__file__).resolve().parents[2]
def module(name):
    import sys
    sys.path.insert(0,str(ROOT/'scripts'))
    spec=importlib.util.spec_from_file_location(name,ROOT/'scripts'/(name+'.py'))
    result=importlib.util.module_from_spec(spec);spec.loader.exec_module(result);return result

profile=module('compare-phase-profile')
builder=module('build-phase-profile')
analyzer=module('analyze-rc-profile')

class Diagnostics(unittest.TestCase):
    def test_recursive_tree_and_small_leaves(self):
        text='''Call graph:
    10 Thread_main
      6 recursive  (in app)
        4 recursive  (in app)
          3 leaf  (in app)
      4 __kill  (in kernel)
Total number in stack (recursive counted multiple, when >=5):
 999 fake_summary
'''
        result=profile.sample_tree(text,{'recursive':'shen.recursive'})[0]
        self.assertEqual(result['samples'],10)
        self.assertEqual(result['self_samples'],{'recursive [shen.recursive]':3,'leaf':3,'PAUSED_OR_PROFILER':4})

    def test_inconsistent_tree_rejected(self):
        with self.assertRaises(ValueError):profile.sample_tree('Call graph:\n    1 thread\n      2 child\nTotal number in stack',{})

    def test_matched_ratios_and_failed_execution(self):
        runs=[]
        for rep,values in enumerate(((9,10),(8,10),(30,10)),1):
            for label,value in zip(('candidate','baseline'),values):
                runs.append({'label':label,'repetition':rep,'status':'PASS',**{k:value for k in ('user_seconds','wall_seconds','instructions','max_rss_bytes')}})
        record={'repetitions':3,'runs':runs}
        result=analyzer.paired(record,'candidate','baseline')
        self.assertAlmostEqual(result['median_ratios']['user_seconds'],.9)
        self.assertEqual(result['cpu_wins'],2)
        runs[-1]['status']='FAIL'
        with self.assertRaises(ValueError):analyzer.paired(record,'candidate','baseline')

    def test_c_strings_do_not_end_function(self):
        source='RcValue *fn(int x) { f("} {", x); /* } */ return x; }\nint next;'
        wrapped=builder.wrap(source,'fn','RcValue *fn(int x)','x','enter();','leave();')
        self.assertIn('fn_profile_impl(int x) { f("} {", x); /* } */ return x; }',wrapped)
        self.assertTrue(wrapped.endswith('\nint next;'))

    def test_balanced_cpu_and_exception_rewind(self):
        with tempfile.TemporaryDirectory() as tmp:
            d=Path(tmp);source=d/'test.c'
            source.write_text('''#include "rc_phase_profile.h"
int main(void) {
  unsigned root=sp_mark();
  sp_enter("execute",0);
  unsigned handler=sp_mark();
  sp_enter("typecheck",0);
  sp_rewind(handler);
  sp_enter("read",0); sp_leave();
  sp_leave(); sp_rewind(root);
  return 0;
}''')
            subprocess.run(['/usr/bin/cc','-std=c17','-fsanitize=address,undefined','-fno-sanitize-recover=all','-I'+str(ROOT/'scripts'),str(source),str(ROOT/'scripts/rc_phase_profile.c'),'-o',str(d/'test')],check=True,timeout=30)
            path=d/'phases.jsonl'
            subprocess.run([str(d/'test')],env={**os.environ,'SHEN_PROFILE_FILE':str(path),'ASAN_OPTIONS':'abort_on_error=1','UBSAN_OPTIONS':'halt_on_error=1'},check=True,timeout=10)
            result=profile.phases(path)
            self.assertEqual(result['aborted_scopes'],1)
            self.assertAlmostEqual(sum(result['exclusive_cpu'].values()),result['process_cpu'],places=7)
            events=[json.loads(s) for s in path.read_text().splitlines()]
            events[-1]['open_scopes']=2;path.write_text('\n'.join(json.dumps(e) for e in events)+'\n')
            with self.assertRaises(ValueError):profile.phases(path)

if __name__=='__main__':unittest.main()
