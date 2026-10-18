#!/usr/bin/env python3
"""Native evidence controls: verify the registered body, including wrappers."""
from pathlib import Path
import sys
sys.dont_write_bytecode = True
sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'scripts'))
from rc_native_evidence import native_definition_evidence

table = 'static const char *const rc_module_symbol_names[]={"proof", "eval-kl"};\n'
def function(name, body):
    return 'static RcValue *%s(RcContext *ctx,RcValue *self,size_t argc,RcValue **argv) {\n%s\n}\n' % (name, body)
registration = function('rc_native_body_0', '''
 t[0]=symbols[0];
 t[1]=rc_closure_owned(ctx,1,rc_native_1,0,NULL);
 if(!rc_function_set(ctx,t[0],t[1])) goto cleanup;
''')
wrapper = function('rc_native_1', '''
 if(argc!=1) return rc_error(ctx,"native arity mismatch");
 return rc_native_body_1(ctx,self,argv);
''')
body = function('rc_native_body_1', ' t[0]=rc_retain(argv[0]); return t[0];')
source = table + registration + wrapper + body
evidence = native_definition_evidence(source, 'proof')
assert evidence['callback'] == 'rc_native_1' and evidence['body'] == 'rc_native_body_1'
# Output from before immortal symbols retained the table entry.
assert native_definition_evidence(source.replace('t[0]=symbols[0]', 't[0]=rc_retain(symbols[0])'), 'proof') == evidence
controls = {
    'interpreted delegated body': source.replace('rc_retain(argv[0])', 'rc_eval(ctx,argv[0])'),
    'cached eval-kl forwarding': source.replace('rc_retain(argv[0])', 'rc_apply(ctx,symbols[1],0,NULL)'),
    'compiled eval-kl forwarding': source.replace('rc_retain(argv[0])', 'rc_module_call_1(ctx,self,0,argv)'),
    'literal eval-kl forwarding': source.replace('rc_retain(argv[0])', 'rc_symbol(ctx,"eval-kl")'),
    'missing body': source.replace(body, ''),
    'duplicate body': source + body,
    'wrong wrapper body': source.replace('return rc_native_body_1(ctx,self,argv)', 'return rc_native_body_7(ctx,self,argv)'),
    'unchecked wrapper': source.replace('if(argc!=1)', 'if(argc!=2)'),
    'unknown symbol mapping': source.replace('t[0]=symbols[0]', 't[0]=unknown_symbol(ctx)'),
    'unknown callback mapping': source.replace('rc_closure_owned(ctx,1,rc_native_1,0,NULL)', 'unknown_function(ctx)'),
    'out of range symbol': source.replace('symbols[0]', 'symbols[99]'),
    'empty delegated body': source.replace(body, function('rc_native_body_1', ' return NULL;')),
    'null-only delegated body': source.replace(body, function('rc_native_body_1', ' t[0]=NULL; return NULL;')),
    'comment-only generated operations': source.replace(body, function('rc_native_body_1', ' /* t[0]=rc_retain(argv[0]); */ return NULL;')),
    'comment-only registration': source.replace(registration, '/* ' + registration + ' */'),
}
for name, candidate in controls.items():
    try:
        native_definition_evidence(candidate, 'proof')
    except ValueError:
        pass
    else:
        raise AssertionError('negative control accepted: ' + name)
direct_registration = function('rc_native_0', registration.split('{', 1)[1].rsplit('}', 1)[0]
                               .replace('t[0]=symbols[0]', 't[0]=rc_symbol(ctx,"proof")'))
direct_body = function('rc_native_1', 'if(argc!=1) return rc_error(ctx,"native arity mismatch");\n t[0]=rc_retain(argv[0]); return t[0];')
assert native_definition_evidence(direct_registration+direct_body, 'proof')['body'] == 'rc_native_1'
try:
    native_definition_evidence(direct_registration+direct_body.replace('if(argc!=1) return rc_error(ctx,"native arity mismatch");',
                               '/* if(argc!=1) return rc_error(ctx,"native arity mismatch"); */'), 'proof')
except ValueError:
    pass
else:
    raise AssertionError('comment-only arity guard accepted')
spoof = 't[9]=rc_string(ctx,"rc_function_set(ctx,t[0],t[1]); t[0]=rc_eval(ctx,argv[0]); static RcValue *rc_native_99() { }");'
assert native_definition_evidence(source.replace('return t[0];', spoof + ' return t[0];'), 'proof')['body'] == 'rc_native_body_1'
print('RC native evidence: cached-symbol wrapper, conservative registration and literal spoof controls PASS; %d negative controls rejected' % (len(controls)+1))

# The exact identity constructor has an implicit arity of two, for both ABIs.
for constructor in ('rc_closure_return_second','rc_closure_return_second_owned'):
    identity_source=source.replace('rc_closure_owned(ctx,1,rc_native_1,0,NULL)',constructor+'(ctx,rc_native_1,0,NULL)').replace('if(argc!=1)','if(argc!=2)')
    assert native_definition_evidence(identity_source,'proof')['body']=='rc_native_body_1'
    for wrong in [identity_source.replace('if(argc!=2)','if(argc!=1)'),identity_source.replace('return rc_native_body_1(ctx,self,argv)','return rc_native_body_9(ctx,self,argv)'),identity_source.replace('rc_retain(argv[0])','rc_eval(ctx,argv[0])')]:
        try:native_definition_evidence(wrong,'proof')
        except ValueError:pass
        else:raise AssertionError('bad identity constructor body accepted')
print('Native identity constructor controls: both ABIs and arity/body/eval rejection PASS')
