#!/usr/bin/env python3
"""Produce an additive diagnostic CPU budget and matched control summaries."""
import argparse
from collections import Counter
import json
from pathlib import Path
import statistics
from release_manifest import digest

def paired(record,numerator,denominator):
    runs=record['runs'];pairs=[]
    for rep in range(1,record['repetitions']+1):
        a=next(x for x in runs if x['repetition']==rep and x['label']==numerator)
        b=next(x for x in runs if x['repetition']==rep and x['label']==denominator)
        if a['status']!='PASS' or b['status']!='PASS':raise ValueError('failed pair')
        pairs.append({k:a[k]/b[k] for k in ('user_seconds','wall_seconds','instructions','max_rss_bytes')})
    return {'ratios':pairs,'median_ratios':{k:statistics.median(p[k] for p in pairs) for k in pairs[0]},
            'cpu_wins':sum(p['user_seconds']<1 for p in pairs)}

def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--profiles',type=Path,required=True)
    p.add_argument('--counter',type=Path)
    p.add_argument('--calibration',type=Path)
    p.add_argument('--types',type=Path)
    p.add_argument('--out',type=Path,required=True)
    a=p.parse_args();profiles=json.loads(a.profiles.read_text())
    expected={(b,m,i) for b in ('rc','boehm') for m in ('accounting','whole','c-minus.shen','interpreter.shen') for i in range(1,profiles.get('repetitions',3)+1)}
    observed={(x['backend'],x['mode'],x['repetition']) for x in profiles['runs'] if x['status']=='PASS' and x['suite']['status']=='PASS'}
    if profiles.get('status')!='PASS' or observed!=expected or len(profiles['runs'])!=len(expected):raise ValueError('incomplete profile set')
    result={'scope':'Diagnostic CPU accounting and stack samples, not performance acceptance',
            'inputs_sha256':{str(a.profiles):digest(a.profiles)},'accounting_cpu_means':{},'per_load_cpu_means':{},'samples':{}}
    for backend in ('rc','boehm'):
        runs=[x for x in profiles['runs'] if x['backend']==backend and x['mode']=='accounting']
        result['accounting_cpu_means'][backend]={k:statistics.mean(x['phase_accounting']['exclusive_cpu'].get(k,0) for x in runs) for k in ('read','translate','typecheck','execute','other')}
        result['per_load_cpu_means'][backend]={load:{k:statistics.mean(x['phase_accounting']['per_load_exclusive_cpu'].get(load,{}).get(k,0) for x in runs) for k in ('read','translate','typecheck','execute','other')}
          for load in sorted({load for x in runs for load in x['phase_accounting']['per_load_exclusive_cpu']})}
        for mode in ('whole','c-minus.shen','interpreter.shen'):
            counts=Counter();total=0
            for x in profiles['runs']:
                if x['backend']==backend and x['mode']==mode:
                    main=x['threads'][0];counts.update(main['self_samples']);total+=main['samples']
            if sum(counts.values())!=total:raise ValueError('sample counts not conserved')
            paused=counts.pop('PAUSED_OR_PROFILER',0)
            result['samples'][backend+'/'+mode]={'total':total,'paused_or_profiler':paused,'nonpaused':total-paused,'self_samples':dict(counts.most_common()),
                'note':'Wall-clock main-thread stack samples, including blocking syscalls; not exclusive CPU percentages. Inlining can hide ownership work within callers.'}
    result['cpu_delta_rc_minus_boehm']={k:result['accounting_cpu_means']['rc'][k]-result['accounting_cpu_means']['boehm'][k] for k in result['accounting_cpu_means']['rc']}
    for key,path in (('counter_control',a.counter),('instrumentation_calibration',a.calibration),('ownership_types',a.types)):
        if not path:continue
        result['inputs_sha256'][str(path)]=digest(path);record=json.loads(path.read_text())
        if key=='counter_control':result[key]=paired(record,'counter_control','checkpoint')
        elif key=='instrumentation_calibration':result[key]={b:paired(record,b+'_accounting',b+'_checkpoint') for b in ('rc','boehm')}
        else:result[key]=record
    a.out.write_text(json.dumps(result,indent=2)+'\n')

if __name__=='__main__':main()
