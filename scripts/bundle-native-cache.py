#!/usr/bin/env python3
"""Pack cached expression modules into one native image; preserve runtime bindings.

This is an explicit cache preparation step. Native execution automatically uses
an ABI-matching pack and continues compiling unseen expressions normally.
"""
import argparse, fcntl, hashlib, json, math, os, re, signal, stat, subprocess, time
from pathlib import Path
ROOT = Path(__file__).resolve().parents[1]


def digest(a, b=b''):
    h1, h2 = 14695981039346656037, 0x84222325cbf29ce4
    for byte in a+b:
        h1 = ((h1 ^ byte)*1099511628211) & ((1 << 64)-1)
        h2 = ((h2 ^ byte ^ (h2 >> 29))*0x100000001b3) & ((1 << 64)-1)
    return f'{h1:016x}{h2:016x}'


def sha(path): return hashlib.sha256(path.read_bytes()).hexdigest()


def config(path):
    lines=path.read_text().splitlines()
    if not lines or lines[0]!='SHEN_NATIVE_CONFIG_V1': raise ValueError('unknown native configuration')
    result={}
    for line in lines[1:]:
        key, value=line.split(' ',1)
        result[key]=value if key in ('abi','immediate','acyclic') else None if value=='-' else bytes.fromhex(value).decode()
    if not re.fullmatch('[0-9a-f]{32}',result['abi']): raise ValueError('invalid ABI identity')
    return result


TOKENS=re.compile(r'"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'|/\*.*?\*/|//[^\n]*|[A-Za-z_][A-Za-z_0-9]*',re.S)
PRIVATE=re.compile(r'rc_native_(?:body_)?[0-9]+|rc_module_call_[0-9]+|rc_drop_range|rc_certified_pvar|rc_transparent_do|rc_expression_module|RcConstantString|RcConstantData|rc_constant_data|rc_constant_(?:(?:code|strings|integers|reals)_)?[0-9]+')


def namespace(source, number):
    if source.count('RcValue *rc_expression_module(')!=1:
        raise ValueError('not an expression module')
    def replace(match):
        token=match.group()
        return f'pack_{number}_{token}' if PRIVATE.fullmatch(token) else token
    source=TOKENS.sub(replace,source)
    source=source.replace('__attribute__((visibility("default")))','static')
    return '#undef RC_LITERAL_SYMBOL_OFFSET\n'+source


def main():
    parser=argparse.ArgumentParser(__doc__)
    parser.add_argument('--cache',type=Path,required=True)
    parser.add_argument('--abi')
    parser.add_argument('--source-receipt',type=Path,help='Reuse canonical C from a qualified cache after loader-only changes')
    parser.add_argument('--source-bundle',type=Path,help='Reuse hash-verified canonical C from an existing bundle JSON; rebuild with current policy')
    parser.add_argument('--record-usage',type=Path,help='Diagnostic pack: append successful lookup keys to this file; do not time this image')
    parser.add_argument('--only-used',type=Path,help='Pack only keys observed by a diagnostic pack with this ABI')
    parser.add_argument('--timeout',type=float,default=600)
    args=parser.parse_args();cache=args.cache.resolve()
    if not math.isfinite(args.timeout) or args.timeout<=0:raise ValueError('timeout must be positive and finite')
    mode=cache.lstat()
    if not stat.S_ISDIR(mode.st_mode) or mode.st_uid!=os.geteuid() or mode.st_mode&0o022:
        raise ValueError('cache must be a private directory owned by this user')
    configs=list(cache.glob('compile-*.config'))
    if args.abi: configs=[p for p in configs if p.name==f'compile-{args.abi}.config']
    if len(configs)!=1: raise ValueError('select one compiler ABI with --abi (see RC_NATIVE_CONFIG diagnostics)')
    cfg=config(configs[0]);abi=cfg['abi'];sources=[]
    # Serialize complete image/receipt/metadata publication across packers.
    # Readers can still safely reject a transient mixed pair and compile.
    pack_lock=os.fdopen(os.open(cache/f'bundle-{abi}.lock',os.O_RDWR|os.O_CREAT|os.O_NOFOLLOW,0o600),'w')
    deadline=time.monotonic()+args.timeout
    while True:
        try:fcntl.flock(pack_lock,fcntl.LOCK_EX|fcntl.LOCK_NB);break
        except BlockingIOError:
            if time.monotonic()>deadline:raise TimeoutError('native pack lock timeout')
            time.sleep(.1)
    if args.source_receipt:
        receipt=json.loads(args.source_receipt.read_text());old=args.source_receipt.resolve().parent/'cache'
        # Canonical candidates are recompiled with CURRENT headers and flags.
        # The loader compares complete current emission before selecting one,
        # so changed compiler/runtime versions safely miss obsolete classes.
        for name, expected in receipt['generated_modules'].items():
            path=old/name
            if path.suffix=='.c':
                if sha(path)!=expected: raise ValueError('changed canonical C: '+name)
                sources.append(path)
    for path in cache.glob('*.c'):
        if re.fullmatch('[0-9a-f]{32}',path.stem) and digest(abi.encode(),path.read_bytes())==path.stem:
            sources.append(path)
    records={};raw_candidates=[]
    if args.source_bundle:
        previous=json.loads(args.source_bundle.read_text())
        build=args.source_bundle.with_suffix('.build.c')
        if sha(build)!=previous['source_sha256']:raise ValueError('changed source bundle')
        raw_candidates=[json.loads(m).encode() for m in re.findall(r'^static const char pack_source_\d+\[\]=("(?:\\.|[^"\\])*");$',build.read_text(),re.M)]
        actual=sorted(hashlib.sha256(raw).hexdigest() for raw in raw_candidates)
        if actual!=sorted(previous['canonical_sha256'].values()):raise ValueError('source bundle inventory mismatch')
    raw_candidates.extend(path.read_bytes() for path in sources)
    total=0
    for raw in raw_candidates:
        # Older generated C can acquire the current function-entry guard.
        # Complete current-emission comparison remains the selection gate.
        if b'rc_stack_check(ctx)' not in raw:
            raw=raw.replace(b'  RcValue *result=NULL;\n',b'  if(!rc_stack_check(ctx)) return NULL;\n  RcValue *result=NULL;\n')
        key=digest(abi.encode(),raw)
        if (key,raw) not in records:
            total+=len(raw);records[key,raw]=None
    if not records: raise ValueError('no matching expression modules')
    records=sorted(records)
    if args.only_used:
        if args.only_used.stat().st_size>1024*1024:raise ValueError('usage record exceeds 1 MiB')
        used=set(args.only_used.read_text().splitlines())
        if not used or any(not re.fullmatch('[0-9a-f]{32}',key) for key in used):raise ValueError('invalid usage keys')
        records=[r for r in records if r[0] in used]
        if {key for key,raw in records}!=used:raise ValueError('usage keys do not match canonical sources and this ABI')
        total=sum(len(raw) for key,raw in records)
    if len(records)>512 or total>32*1024*1024: raise ValueError('pack exceeds 512 modules or 32 MiB canonical C budget')
    base=cache/f'bundle-{abi}';source=base.with_suffix('.build.c');obj=base.with_suffix('.so');log=base.with_suffix('.log')
    temporary=cache/f'.bundle-{abi}-{os.getpid()}.so'
    pieces=[]
    for i, (key,raw) in enumerate(records):
        pieces.append(namespace(raw.decode(),i))
        pieces.append('static const char pack_source_%d[]=%s;\n' % (i,json.dumps(raw.decode(),ensure_ascii=False)))
    pieces.append('typedef struct {const char *key,*source;size_t length;RcValue *(*entry)(RcContext*,RcValue*);} RcPackedEntry;\n')
    pieces.append('static const RcPackedEntry pack_entries[]={\n')
    for i,(key,raw) in enumerate(records):
        pieces.append('{"%s",pack_source_%d,%d,pack_%d_rc_expression_module},\n' % (key,i,len(raw),i))
    pieces.append('};\n__attribute__((visibility("default"))) const char *rc_bundle_abi(void){return "%s";}\n' % abi)
    observe=''
    if args.record_usage:
        observe='''static FILE *pack_usage;static int pack_attempted;
   if(!pack_attempted){pack_attempted=1;pack_usage=fopen(%s,"a");}
   if(pack_usage)fprintf(pack_usage,"%%s\\n",key);
   ''' % json.dumps(str(args.record_usage.resolve()))
    pieces.append('''__attribute__((visibility("default"))) const RcPackedEntry *rc_bundle_lookup(const char *key,const char *source,size_t length){
 size_t lo=0,hi=sizeof(pack_entries)/sizeof(*pack_entries);
 while(lo<hi){size_t mid=lo+(hi-lo)/2;if(strcmp(pack_entries[mid].key,key)<0)lo=mid+1;else hi=mid;}
 for(;lo<sizeof(pack_entries)/sizeof(*pack_entries)&&!strcmp(pack_entries[lo].key,key);lo++)
  if(pack_entries[lo].length==length&&!memcmp(pack_entries[lo].source,source,length)){OBSERVEreturn &pack_entries[lo];}
 return NULL;
}\n'''.replace('OBSERVE',observe))
    source.write_text(''.join(pieces))
    # Match the backend's direct argv convention: flags are whitespace words,
    # never a shell command. The host's ABI/policy flags take precedence.
    flags=(cfg['cflags']+' '+cfg['host_flags']).replace('\t',' ').split(' ')
    argv=[cfg['cc']]+[word for word in flags if word]+['-DSHEN_RC_IMMEDIATE_INTEGERS='+cfg['immediate']]
    if cfg['acyclic']=='1':argv+=['-DSHEN_RC_ACYCLIC_ONLY']
    argv+=['-std=c17','-w','-fPIC','-shared','-fvisibility=hidden']
    if os.uname().sysname=='Darwin':argv+=['-undefined','dynamic_lookup']
    argv+=['-I'+str(cache/('include-'+abi)),str(source),'-o',str(temporary)]
    environment=dict(os.environ)
    for name,value in cfg.items():
        if name.startswith('env_'):
            if value is None:environment.pop(name[4:],None)
            else:environment[name[4:]]=value
    start=time.monotonic()
    with log.open('wb') as output:
        process=subprocess.Popen(argv,stdout=output,stderr=subprocess.STDOUT,start_new_session=True,env=environment)
        while process.poll() is None:
            if time.monotonic()>deadline or output.tell()>8*1024*1024:
                os.killpg(process.pid,signal.SIGKILL);process.wait();temporary.unlink(missing_ok=True)
                raise RuntimeError('bounded pack compilation failed: '+str(log))
            time.sleep(.1)
    if process.returncode:raise RuntimeError('pack compiler failed: '+str(log))
    checksum=digest(temporary.read_bytes()).encode()
    # A pack replacement is optional acceleration: a reader seeing a mixed
    # pair rejects it and uses ordinary native modules for that execution.
    temporary.replace(obj);base.with_suffix('.receipt').write_bytes(checksum)
    record=dict(status='PASS',abi=abi,modules=len(records),canonical_bytes=total,
                source_sha256=sha(source),binary_sha256=sha(obj),command=argv,
                wall=time.monotonic()-start,compiler_configuration=cfg,
                canonical_sha256={key:hashlib.sha256(raw).hexdigest() for key,raw in records},
                diagnostic_usage=str(args.record_usage.resolve()) if args.record_usage else None,
                source_bundle_sha256=sha(args.source_bundle) if args.source_bundle else None,
                usage_sha256=sha(args.only_used) if args.only_used else None)
    base.with_suffix('.json').write_text(json.dumps(record,indent=2)+'\n')
    log.unlink(missing_ok=True)
    print(json.dumps({k:record[k] for k in ('status','abi','modules','canonical_bytes','wall')}))


if __name__=='__main__':main()
