#!/usr/bin/env python3
"""Diagnostic ownership controls from source snapshots.

Default: remove only two telemetry increments. --typed: retain those increments
and add per-type counters, compiling rc.c with SHEN_RC_STATS_COUNTERS=1 (release
builds omit the increments since #7; the define is harmless for older sources). The product runtime and RcStats contract stay unchanged.
These executables are experiments, not production candidates or acceptance results.
"""
import argparse
import json
from pathlib import Path
import shutil
import subprocess
from release_manifest import digest

p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--checkpoint',type=Path,required=True)
p.add_argument('--out',type=Path,required=True)
p.add_argument('--typed',action='store_true',help='Keep counters and additionally count ownership operations by object type (diagnostic only)')
a=p.parse_args();root=a.checkpoint.resolve();out=a.out.resolve();out.mkdir(parents=True,exist_ok=False)
sources=out/'sources';sources.mkdir();inputs={};commands=[]
for src in (root/'src/rc').glob('*.[ch]'):
    inputs[str(src)]=digest(src);shutil.copy2(src,sources/src.name)
source=sources/'rc.c';text=source.read_text()
for kind in ('retain','release'):
    statement='v->ctx->stats.'+kind+'_calls++;'
    if text.count(statement)!=1:raise ValueError('changed counter template')
    replacement=(statement+'\nsp_'+kind+'[v->type]++;\nif(v->type==RC_SYMBOL && (v==v->ctx->true_symbol || v==v->ctx->false_symbol))sp_'+kind+'_bool++;') if a.typed else '/* Diagnostic control: '+statement+' omitted. */'
    text=text.replace(statement,replacement)
if a.typed:
    text=text.replace('#include "rc.h"','#include "rc.h"\nstatic size_t sp_retain[10],sp_release[10],sp_retain_bool,sp_release_bool;',1)
    text+='''
__attribute__((destructor)) static void sp_type_counts(void) {
    fputs("RC_TYPE_COUNTS {\\\"retain\\\":[",stderr);
    for(size_t i=0;i<10;i++)fprintf(stderr,"%s%zu",i?",":"",sp_retain[i]);
    fputs("],\\\"release\\\":[",stderr);
    for(size_t i=0;i<10;i++)fprintf(stderr,"%s%zu",i?",":"",sp_release[i]);
    fprintf(stderr,"],\\\"retain_bool\\\":%zu,\\\"release_bool\\\":%zu}\\n",sp_retain_bool,sp_release_bool);
}
'''
source.write_text(text)
flags=['-O3','-g','-flto=thin','-std=c17','-Wall','-Wextra','-Wpedantic','-I'+str(sources)]+(['-DSHEN_RC_STATS_COUNTERS=1'] if a.typed else [])
def run(argv):
    commands.append(argv)
    with (out/'build.log').open('ab') as log:subprocess.run(argv,stdout=log,stderr=subprocess.STDOUT,check=True,timeout=180)
obj=out/'rc.o';run(['/usr/bin/cc']+flags+['-c',str(source),'-o',str(obj)])
objects=[]
for name in ('main','kernel','primitives','eval','read'):
    path=root/'obj/rc'/(name+'.o');inputs[str(path)]=digest(path);objects.append(str(path))
binary=out/('shen-type-counts' if a.typed else 'shen-counter-control')
run(['/usr/bin/cc']+flags+objects+[str(obj),'-lm','-o',str(binary)])
baseline=out/'shen-checkpoint';shutil.copy2(root/'bin/shen-rc',baseline)
inputs[str(root/'bin/shen-rc')]=digest(baseline)
inputs[str(root/'obj/rc/kernel.c')]=digest(root/'obj/rc/kernel.c')
record={'scope':__doc__,'mode':'diagnostic additional per-type counters' if a.typed else 'diagnostic removal of two counters','type_order':['nil','number','string','symbol','pair','vector','closure','stream','tail','env'],'inputs':inputs,'commands':commands,'source_sha256':digest(source),
        'binaries':{str(p):digest(p) for p in (binary,baseline)}}
(out/'build.json').write_text(json.dumps(record,indent=2)+'\n')
config={'repetitions':5,'source_identity':{'counter_control_receipt_sha256':digest(out/'build.json')},
        'cases':[{'label':label,'argv':[str(b),'script','runme.shen'],'env':{},
                  'dependency_sha256':{str(out/'build.json'):digest(out/'build.json')}}
                 for label,b in [('checkpoint',baseline),('counter_control',binary)]]}
if not a.typed:(out/'benchmark.json').write_text(json.dumps(config,indent=2)+'\n')
