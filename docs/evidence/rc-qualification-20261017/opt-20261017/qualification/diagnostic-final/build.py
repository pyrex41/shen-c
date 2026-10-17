import json,subprocess,shutil,hashlib
from pathlib import Path
root=Path.cwd();out=root/'evidence/rc/opt-20261017/diagnostic-final';src=out/'sources';src.mkdir()
for p in (root/'src/rc').glob('*.[ch]'):shutil.copy2(p,src/p.name)
s=(src/'rc.c').read_text().replace('#define RC_OBJECT_CACHE_LIMIT 1024','#define RC_OBJECT_CACHE_LIMIT 1024\nstatic RcNative sp_plan_native;')
s=s.replace('RcValue *old=expression->as.pair.eval_plan;', 'sp_plan_native=plan->as.closure.native;\n    RcValue *old=expression->as.pair.eval_plan;')
s=s.replace('    ctx->destroying = true;', '''    size_t plans=0,planbytes=0,codebytes=0,cachepairs=0;
    for(RcValue *v=ctx->objects;v;v=v->next) {
        if(v->type==RC_PAIR&&v->as.pair.eval_plan)cachepairs++;
        if(v->type==RC_CLOSURE&&v->as.closure.native==sp_plan_native&&sp_plan_native) {
            plans++;planbytes+=v->bytes;codebytes+=v->as.closure.captures[0]->bytes;
        }
    }
    fprintf(stderr,"PLAN_STATS {\\\"plans\\\":%zu,\\\"plan_bytes\\\":%zu,\\\"code_bytes\\\":%zu,\\\"cache_pairs\\\":%zu}\\n",plans,planbytes,codebytes,cachepairs);
    ctx->destroying = true;''')
(src/'rc.c').write_text(s)
shutil.copy2(root/'evidence/rc/opt-20261017/count-build/main.c',src/'main.c')
commands=[]
def run(cmd):
 commands.append(cmd)
 with (out/'build.log').open('ab') as log:subprocess.run(cmd,stdout=log,stderr=subprocess.STDOUT,check=True,timeout=180)
flags=['-O2','-g','-std=c17','-I'+str(src)]
objects=[]
for name in ['rc','primitives','eval','read','main']:
 obj=out/(name+'.o');run(['/usr/bin/cc']+flags+['-c',str(src/(name+'.c')),'-o',str(obj)]);objects.append(str(obj))
# Diagnostic only: no LTO so repeated counter builds can link quickly. Native
# emitted operations and all ownership counters remain unchanged.
kernel=root/'obj/rooted-e2-release/kernel.c';obj=out/'kernel.o'
run(['/usr/bin/cc','-O0','-g','-std=c17','-I'+str(src),'-c',str(kernel),'-o',str(obj)])
run(['/usr/bin/cc']+objects+[str(obj),'-lm','-o',str(out/'shen-counts')])
(out/'build.json').write_text(json.dumps({'commands':commands,'source_sha256':{p.name:hashlib.sha256(p.read_bytes()).hexdigest() for p in src.iterdir()},'kernel_sha256':hashlib.sha256(kernel.read_bytes()).hexdigest()},indent=2)+'\n')
