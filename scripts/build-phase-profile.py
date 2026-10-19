#!/usr/bin/env python3
"""Build diagnostic RC/Boehm executables from frozen generated C, never vendor edits.

Only selected NON-TAIL call boundaries receive phase scopes. Load callbacks return
a literal after their work; evaluator entry points drive evaluation completely.
Generated C and runtime sources are copied, transformed and hashed in --out.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import shutil
import subprocess
import platform
from release_manifest import ROOT, digest, load_release

PHASES={'read-file':'read', 'shen.shen->kl':'translate', 'shen.typecheck':'typecheck'}

def wrap(text,name,signature,arguments,before,after='sp_leave();'):
    pattern=re.escape(signature)+r'\s*\{'
    match=re.search(pattern,text)
    if not match: raise ValueError('missing function '+name)
    # Lex C strings/comments so braces in error messages are not structural.
    tokens=re.compile(r'/\*.*?\*/|//[^\n]*|"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'|[{}]',re.S)
    level=1;end=None
    for token in tokens.finditer(text,match.end()):
        if token.group()=='{':level+=1
        if token.group()=='}':level-=1
        if level==0:end=token.end();break
    if end is None:raise ValueError('unclosed '+name)
    original=text[match.start():end].replace(name+'(',name+'_profile_impl(',1).replace(name+' (',name+'_profile_impl (',1)
    result_type='RcValue *' if signature.startswith('static RcValue') or signature.startswith('RcValue') else 'KLObject* '
    wrapper=signature+' {\n'+before+'\n'+result_type+'sp_result='+name+'_profile_impl('+arguments+');\n'+after+'\nreturn sp_result;\n}\n'
    return text[:match.start()]+original+'\n'+wrapper+text[end:]

def rc_kernel(text):
    names=json.loads('['+re.search(r'rc_module_symbol_names\[\]=\{(.*?)\};',text,re.S)[1].rstrip().rstrip(',')+']')
    symbols={}
    for index,native in re.findall(r't\[0\]=(?:rc_retain\()?symbols\[(\d+)\]\)?;\s*if\(!t\[0\]\) goto cleanup;\s*t\[1\]=(?:rc_closure_static\()?rc_closure(?:_owned)?\(ctx,\d+,rc_native_(\d+),0,NULL\)\)?;',text):
        symbols['rc_native_body_'+native]=names[int(index)]
    for match in re.finditer(r'static RcValue \*rc_module_call_(\d+)\([^\n]+\) \{(.*?)\n\}',text,re.S):
        body=re.search(r'rc_native_body_(\d+)\(',match[2])
        if body:symbols['rc_native_body_'+body[1]]=names[int(match[1])]
        symbols['rc_module_call_'+match[1]]=names[int(match[1])]
    for symbol,name in list(symbols.items()):symbols[symbol.replace('_body','')]=name
    for name,phase in PHASES.items():
        n=names.index(name);fn='rc_module_call_'+str(n)
        signature='static RcValue *'+fn+'(RcContext *ctx,RcValue *fn,size_t argc,RcValue **argv)'
        text=wrap(text,fn,signature,'ctx,fn,argc,argv','sp_enter('+json.dumps(phase)+',NULL);')
    load=[k for k,v in symbols.items() if v=='load' and '_body_' in k]
    if len(load)!=1:raise ValueError('ambiguous load')
    fn=load[0];signature='static RcValue *'+fn+'(RcContext *ctx,RcValue *self,RcValue **argv)'
    text=wrap(text,fn,signature,'ctx,self,argv','sp_enter("other",rc_text(argv[0]));')
    return '#include "rc_phase_profile.h"\n'+text,symbols

def build(args):
    release=load_release();out=args.out.resolve();out.mkdir(parents=True,exist_ok=False)
    sources=out/'sources';sources.mkdir()
    commands=[];inputs={};outputs={};maps={};reused=[]
    inputs[str(args.gc_lib.resolve())]=digest(args.gc_lib)
    inputs[str(Path(__file__).resolve())]=digest(__file__)
    for module in release['modules']:
        name=release['kernel_directory']+'/'+module+'.kl';expected=release['kernel_sha256'][name]
        source=args.boehm_root/name
        if digest(source)!=expected:raise ValueError('Boehm boot kernel differs: '+str(source))
        inputs[str(source.resolve())]=expected
    previous=json.loads((args.reuse_build/'build.json').read_text()) if args.reuse_build else None
    def copy(src,dst):
        inputs[str(src.resolve())]=digest(src);shutil.copy2(src,dst)
    for name in ('rc_phase_profile.c','rc_phase_profile.h'):
        copy(ROOT/'scripts'/name,sources/name)
    flags=['-O3','-g','-flto=thin','-std=c17','-I'+str(sources)]
    def run(argv):
        commands.append(argv)
        with (out/'build.log').open('ab') as log:
            subprocess.run(argv,stdout=log,stderr=subprocess.STDOUT,check=True,timeout=180)
    for backend in ('rc','boehm'):
        directory=sources/backend;directory.mkdir();objects=[]
        if backend=='rc':
            for src in (ROOT/'src/rc').glob('*.[ch]'):copy(src,directory/src.name)
            original=args.rc_generated.resolve();inputs[str(original)]=digest(original)
            text,maps[backend]=rc_kernel(original.read_text())
            (directory/'kernel.c').write_text(text)
            path=directory/'eval.c';text=path.read_text()
            text=wrap(text,'rc_eval','RcValue *rc_eval(RcContext *ctx, RcValue *expression)','ctx,expression','sp_enter("execute",NULL);')
            path.write_text('#include "rc_phase_profile.h"\n'+text)
            cfiles=[directory/(n+'.c') for n in ('rc','eval','primitives','read','main','kernel')]
            extra=[];libs=['-lm']
        else:
            for src in (args.boehm_root/'src/c').glob('*.[ch]'):copy(src,directory/src.name)
            generated=directory/'kernel';generated.mkdir();maps[backend]={}
            for src in (args.boehm_root/'obj/kernel_aot').glob('*.c'):
                copy(src,generated/src.name)
                text=src.read_text()
                for number,(name,phase) in enumerate(PHASES.items()):
                    text=text.replace('shen_apply_direct(ctx, '+json.dumps(name)+',','sp_boehm_phase_'+str(number)+'(ctx, '+json.dumps(name)+',')
                for name,symbol in re.findall(r'shen_register_defun\(ctx, "([^"]+)", \d+, &(\w+)\)',text):
                    maps[backend][symbol]=name
                if src.name=='load.c':
                    name='native_kl_load_0'
                    signature='static KLObject* '+name+' (KLObject* function_object, Vector* arguments, Environment* function_environment, Environment* variable_environment)'
                    text=wrap(text,name,signature,'function_object,arguments,function_environment,variable_environment','sp_enter("other",kl_untag(arguments->objects[0])->value.string);')
                (generated/src.name).write_text('#include "rc_phase_profile.h"\n'+text)
            path=directory/'abi.c';text=path.read_text()
            # Only literal selected call sites change. Testing every ABI call's
            # name would impose asymmetric overhead on the Boehm implementation.
            declarations=[]
            for number,(name,phase) in enumerate(PHASES.items()):
                signature='KLObject* sp_boehm_phase_'+str(number)+'(shen_context* ctx, const char* name, long n, KLObject** arguments)'
                declarations.append(signature+';')
                text+='\n'+signature+' {\nsp_enter('+json.dumps(phase)+',NULL);\nKLObject *v=shen_apply_direct(ctx,name,n,arguments);\nsp_leave();return v;\n}\n'
            header=directory/'abi.h';header.write_text(header.read_text()+'\n'+'\n'.join(declarations)+'\n')
            text=wrap(text,'shen_eval_kl','KLObject* shen_eval_kl (shen_context* ctx, KLObject* object)','ctx,object','sp_enter("execute",NULL);')
            # Error handlers restore diagnostic scopes abandoned by longjmp.
            text=text.replace('int saved_depth = apply_depth;','unsigned sp_saved=sp_mark();\n  int saved_depth = apply_depth;')
            text=text.replace('apply_depth = saved_depth;','sp_rewind(sp_saved);\n    apply_depth = saved_depth;')
            path.write_text('#include "rc_phase_profile.h"\n'+text)
            cfiles=sorted(directory.glob('*.c'))+sorted(generated.glob('*.c'))
            extra=['-fno-optimize-sibling-calls','-fsigned-char','-I'+str(args.gc_include)]
            libs=[str(args.gc_lib)]
        for i,source in enumerate(cfiles+[sources/'rc_phase_profile.c']):
            obj=directory/('part-%d.o'%i);objects.append(str(obj))
            command=['/usr/bin/cc']+flags+extra+['-iquote',str(directory),'-c',str(source),'-o',str(obj)]
            old_source=args.reuse_build/source.relative_to(out) if previous else None
            old_obj=args.reuse_build/obj.relative_to(out) if previous else None
            old_command=[x.replace(str(out),str(args.reuse_build)) for x in command] if previous else None
            headers_match=previous and all(digest(p)==digest(args.reuse_build/p.relative_to(out)) for p in sources.rglob('*.h'))
            if previous and old_obj.is_file() and old_source.is_file() and digest(old_source)==digest(source) and old_command in previous['commands'] and headers_match and previous.get('objects_sha256',{}).get(str(old_obj))==digest(old_obj):
                shutil.copy2(old_obj,obj);reused.append({'object':str(obj),'from':str(old_obj),'sha256':digest(obj)});commands.append(command)
            else:run(command)
        binary=out/('shen-'+backend)
        run(['/usr/bin/cc','-g','-flto=thin']+objects+libs+['-o',str(binary)])
        outputs[str(binary)]=digest(binary)
    record={'scope':'diagnostic only; transformed generated C and selected non-tail boundaries',
            'phase_boundaries':PHASES,'inputs':inputs,'binaries':outputs,'commands':commands,
            'sources':{str(p.relative_to(out)):digest(p) for p in sources.rglob('*') if p.suffix in ('.c','.h')},
            'symbol_maps':maps,'manifest_sha256':digest(release['_path']),'reused_objects':reused,
            'reuse_receipt_sha256':digest(args.reuse_build/'build.json') if previous else None,
            'objects_sha256':{str(p):digest(p) for p in sources.rglob('*.o')},
            'host':platform.uname()._asdict(),
            'compiler':subprocess.check_output(['/usr/bin/cc','--version'],text=True).strip(),
            'sdk':subprocess.check_output(['/usr/bin/xcrun','--show-sdk-version'],text=True).strip()}
    (out/'build.json').write_text(json.dumps(record,indent=2)+'\n')

if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--rc-generated',type=Path,required=True)
    p.add_argument('--boehm-root',type=Path,required=True)
    p.add_argument('--gc-include',type=Path,required=True)
    p.add_argument('--gc-lib',type=Path,required=True)
    p.add_argument('--out',type=Path,required=True)
    p.add_argument('--reuse-build',type=lambda p:Path(p).resolve())
    build(p.parse_args())
