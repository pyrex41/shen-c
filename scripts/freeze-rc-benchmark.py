#!/usr/bin/env python3
"""Bind source/generated/binary/toolchain identities before Darwin comparisons.

The binaries must already be built with the recorded flags. This records evidence;
it cannot retroactively prove an unknown build's correspondence to source.
"""
import argparse
import json
from pathlib import Path
import platform
import subprocess
from release_manifest import DEFAULT, ROOT, digest, load_release

p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--manifest',type=Path,default=DEFAULT)
p.add_argument('--rc',type=Path,required=True)
p.add_argument('--rc-generated',type=Path,default=ROOT/'obj/rc/kernel.c',help='Generated kernel C used by this exact RC binary')
p.add_argument('--boehm',type=Path,required=True)
p.add_argument('--boehm-root',type=Path,required=True)
p.add_argument('--boehm-lib',type=Path,required=True)
p.add_argument('--baseline',type=Path,required=True)
p.add_argument('--baseline-c',type=Path,required=True)
p.add_argument('--baseline-emitter',type=Path,required=True)
p.add_argument('--baseline-root',type=Path,help='Explicit source checkout used for the pre-optimization binary')
p.add_argument('--out',type=Path,required=True)
a=p.parse_args()
if not a.rc_generated.is_file():raise ValueError('missing RC generated C: '+str(a.rc_generated))
if a.baseline_root and not (a.baseline_root/'src/rc/rc.c').is_file():raise ValueError('missing baseline source checkout')
release=load_release(a.manifest);a.out=a.out.resolve();a.out.mkdir(parents=True,exist_ok=False)

def command(argv):
    return subprocess.check_output(argv,text=True,stderr=subprocess.STDOUT,timeout=30).strip()

def hashes(paths):
    return {str(f.resolve()):digest(f) for f in sorted(paths) if f.is_file()}

rc_paths=list((ROOT/'src/rc').glob('*.[ch]'))
rc_paths += [ROOT/n for n in ['tools/rc-build.c','Makefile','Makefile.rc','CMakeLists.txt','flake.lock','scripts/benchmark-rc.py','scripts/release_manifest.py','scripts/freeze-rc-benchmark.py']]
rc_paths += [ROOT/p for p in release['kernel_sha256']]
rc_paths += [a.rc_generated]
boehm_paths=list((a.boehm_root/'src/c').glob('*.[ch]'))
for module in release['modules']:
    source=a.boehm_root/'shen/src/kl'/(module+'.kl')
    if digest(source)!=digest(ROOT/release['kernel_directory']/(module+'.kl')):
        raise ValueError('comparator kernel differs: '+module)
    boehm_paths.append(source)
boehm_paths += [a.boehm_root/'Makefile',a.boehm_root/'tools/kernel-aot-build.c']
boehm_paths += list((a.boehm_root/'obj/kernel_aot').glob('*.c'))
record={'scope':'frozen complete implementations; all flags retained in build logs',
        'host':platform.uname()._asdict(),'compiler':command(['/usr/bin/cc','--version']),
        'os':command(['/usr/bin/sw_vers']),'sdk_path':command(['/usr/bin/xcrun','--show-sdk-path']),
        'sdk_version':command(['/usr/bin/xcrun','--show-sdk-version']),
        'checkout_revision':command(['git','rev-parse','HEAD']),
        'boehm_revision':command(['git','-C',str(a.boehm_root),'rev-parse','HEAD']),
        'release_manifest_sha256':digest(a.manifest),
        'rc_release_flags':'-O3 -g -std=c17 -Wall -Wextra -Wpedantic -Isrc/rc -flto=thin',
        'boehm_release_flags':'-O3 -g -flto=thin -std=c17 -fno-optimize-sibling-calls -fsigned-char; Nix Boehm GC include/lib paths',
        'rc_source_generated_sha256':hashes(rc_paths),'boehm_source_generated_sha256':hashes(boehm_paths),
        'baseline_c_sha256':digest(a.baseline_c),'baseline_emitter_sha256':digest(a.baseline_emitter),
        'baseline_correspondence':'explicit baseline source checkout and generated kernel; retain the corresponding build receipt' if a.baseline_root else 'same qualified RC runtime/main and 43.1 kernel; explicit pre-optimization emitter and generated C',
        'baseline_source_sha256':hashes((a.baseline_root/'src/rc').glob('*.[ch]')) if a.baseline_root else {},
        'artifacts':{},'dependencies':{str(a.boehm_lib.resolve()):digest(a.boehm_lib)}}
for label,binary in [('rc',a.rc),('boehm',a.boehm),('baseline',a.baseline)]:
    record['artifacts'][label]={'path':str(binary.resolve()),'sha256':digest(binary),
                              'linked_libraries':command(['/usr/bin/otool','-L',str(binary)])}
receipt=a.out/'freeze.json';receipt.write_text(json.dumps(record,indent=2)+'\n')
common={str(receipt):digest(receipt)}
rc_deps={**common,**record['rc_source_generated_sha256']}
boehm_deps={**common,**record['boehm_source_generated_sha256'],**record['dependencies']}
baseline_source=record['baseline_source_sha256'] if a.baseline_root else hashes(p for p in rc_paths if p.name!='emit.c')
baseline_deps={**common,**baseline_source,str(a.baseline_c.resolve()):digest(a.baseline_c),str(a.baseline_emitter.resolve()):digest(a.baseline_emitter)}
cases=[{'label':label,'argv':[str(binary.resolve()),'script',release['suite_entrypoint']],
        'env':{},'dependency_sha256':deps} for label,binary,deps in
       [('rc',a.rc,rc_deps),('boehm',a.boehm,boehm_deps)]]
config={'cases':cases,'repetitions':10,'source_identity':{'freeze_receipt':str(receipt),'sha256':digest(receipt)}}
(a.out/'final-template.json').write_text(json.dumps(config,indent=2)+'\n')
config['cases'][0]={'label':'rc','argv':[str(a.baseline.resolve()),'script',release['suite_entrypoint']],
                    'env':{},'dependency_sha256':baseline_deps}
config['repetitions']=3
(a.out/'baseline-config.json').write_text(json.dumps(config,indent=2)+'\n')
print(a.out)
