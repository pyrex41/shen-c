#!/usr/bin/env python3
"""Recompute exploratory comparisons; no release acceptance claim."""
import hashlib,json,re,statistics
from pathlib import Path
D=Path(__file__).resolve().parent
sha=lambda p:hashlib.sha256(p.read_bytes()).hexdigest()
result={"scope":"Exploratory bounded probes. Host had competing jobs; no quiet-host, Boehm, release or Linux/Nix qualification claim.","suite":{},"micro":{},"diagnostic":{},"inputs_sha256":{}}
for label,folder in [("A_vs_baseline","suite-five-pairs"),("B_vs_A","suite-b-five-pairs")]:
 p=D/folder/'result.json';j=json.loads(p.read_text());result['inputs_sha256'][str(p.relative_to(D))]=sha(p)
 assert j['execution_status']=='PASS' and len(j['runs'])==10
 for run in j['runs']:
  assert run['status']=='PASS' and run['exit']==0
  assert all(run[k] for k in ['binary_unchanged','dependencies_unchanged','suite_unchanged','inputs_unchanged'])
  v=run['suite_validation'];assert v['status']=='PASS' and len(v['passed_reports'])==36 and v['passed_reports'][-1]==136 and not any(v['failed_reports'])
  assert v['output_sha256']['test.txt']=='2cf24dba5fb0a30e26e83b2ac5b9e29e1b161e5c1fa7425e73043362938b9824'
  assert sha(p.parent/run['stdout'])==run['stdout_sha256'] and sha(p.parent/run['stderr'])==run['stderr_sha256']
 metrics=['user_seconds','sys_seconds','wall_seconds','max_rss_bytes','instructions','cycles']
 pairs={k:[next(r[k] for r in j['runs'] if r['label']=='candidate' and r['repetition']==i)/next(r[k] for r in j['runs'] if r['label']=='baseline' and r['repetition']==i) for i in range(1,6)] for k in metrics}
 result['suite'][label]={'all_ten_executions':'136/0 with all reports and redirect validated','paired_ratios':pairs,'median_paired_ratios':{k:statistics.median(v) for k,v in pairs.items()},'cpu_wins':sum(v<1 for v in pairs['user_seconds']),'cpu_ties':sum(v==1 for v in pairs['user_seconds']),'distribution_medians':{label:{k:statistics.median(r[k] for r in j['runs'] if r['label']==label) for k in metrics} for label in ['baseline','candidate']}}
for label,folder in [('A_vs_baseline','micro-five-pairs'),('B_vs_A','micro-b-five-pairs')]:
 p=D/folder/'result.json';j=json.loads(p.read_text());assert j['status']=='PASS' and len(j['runs'])==80
 for run in j['runs']:
  assert run['status']=='PASS' and run['exit']==0
  assert all(v==0 for v in run['result']['counters'].values())
  assert sha(p.parent/(run['case']+'-'+str(run['size'])+'-%02d-'%run['repetition']+run['label']+'.out'))==run['stdout_sha256']
 result['micro'][label]=j['summary'];result['inputs_sha256'][str(p.relative_to(D))]=sha(p)
for label in ['baseline','candidate-a','candidate-b']:
 p=D/label/'suite-diagnostic/result.json';j=json.loads(p.read_text());assert j['execution_status']=='PASS'
 stderr=(p.parent/j['runs'][0]['stderr']).read_text()
 result['diagnostic'][label]=json.loads(re.search(r'RC_FUSION (\{[^\n]*\})',stderr).group(1))
 result['inputs_sha256'][str(p.relative_to(D))]=sha(p)
result['decision']={'A':'Keep primitive copy elimination as a reviewable branch candidate; suite gain not established.','B':'Keep suffix views default off: bytes saved in long scans, no full-suite CPU gain in observed busy host window.','next':'Guarded fresh string/vector builder fusion; vector views require mutation and escape proofs.'}
(D/'assessment.json').write_text(json.dumps(result,indent=2)+'\n')
print(json.dumps({k:v['median_paired_ratios'] for k,v in result['suite'].items()},indent=2))
