import subprocess,json,hashlib
from pathlib import Path
root=Path.cwd();out=root/'evidence/rc/opt-20261017';sha=lambda p:hashlib.sha256(p.read_bytes()).hexdigest()
record={'code_revision':subprocess.check_output(['git','rev-parse','HEAD'],text=True).strip(),'scope':'fresh Make compiler/CLI and metadata/source pipeline; CMake Release; installed Darwin Nix; fatal runtime/emitted/full-kernel sanitizers','checks':{}}
for key,path in [('toolchain',out/'final-toolchain/result.json'),('full_generated_sanitizer',root/'evidence/rc/certify-20261017T224224.649249Z/result.json'),('nix_installed_suite',out/'nix-installed-suite/result.json')]:
 r=json.load(open(path));status=r.get('status') if key=='toolchain' else r.get('suite') if key=='full_generated_sanitizer' else r.get('execution_status');assert status=='PASS',(key,status);record['checks'][key]={'status':'PASS','receipt':str(path.relative_to(root)),'sha256':sha(path)}
for key,name,marker in [('cmake_24','final-cmake-tests.log','100% tests passed out of 24'),('runtime_boxed','final-runtime-boxed.log','RC private rooted calls:'),('runtime_and_emitted_fatal','final-all-emitted.log','RC guarded Prolog: 36 differential cases PASS'),('reader_capture','final-reader-controls.log','RC reader capture: PASS (13 cases)'),('distribution_audits','final-distribution-audits.log','RC artifact audit PASS')]:
 p=out/name;text=p.read_text();assert marker in text,(key,text[-300:]);record['checks'][key]={'status':'PASS','log':str(p.relative_to(root)),'sha256':sha(p)}
for name,binary in [('cmake',root/'build-final-43.1/shen-rc'),('nix',Path('/nix/store/1rcjcyjqa4s8nkvalkhimgn12nin6azy-shen-c-0.2.3/bin/shen-rc'))]:
 version=subprocess.check_output([str(binary),'eval','-e','(version)'],text=True).strip();value=subprocess.check_output([str(binary),'eval','-e','(+ 20 22)'],text=True).strip();assert version=='43.1' and value=='42';record['checks'][name+'_cli']={'status':'PASS','version':version,'value':value,'binary_sha256':sha(binary)}
record['source_correspondence']=json.load(open(out/'fresh-generated-correspondence.json'));assert record['source_correspondence']['identical']
ci=json.load(open(out/'ci-final.json'));assert ci['headSha']==record['code_revision'] and ci['conclusion']=='success' and all(j['conclusion']=='success' for j in ci['jobs']);record['checks']['ci']={'status':'PASS','run':ci['url'],'receipt_sha256':sha(out/'ci-final.json'),'jobs':[j['name'] for j in ci['jobs']]}
record['performance']=json.load(open(out/'final-ten-pairs/result.json'))['performance_acceptance'];assert record['performance']['status']=='PASS';record['performance_receipt_sha256']=sha(out/'final-ten-pairs/result.json')
record['host_condition']='No local build or profiling overlapped the final timings. Defender and desktop services appear in before/after snapshots; strict quiet-host provenance remains unestablished.'
record['status']='PASS for numerical performance gates and recorded correctness/platform checks; quiet-host condition UNKNOWN'
(out/'local-qualification.json').write_text(json.dumps(record,indent=2)+'\n');print(record['status'])
