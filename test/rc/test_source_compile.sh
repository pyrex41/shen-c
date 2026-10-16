#!/bin/sh
# Real Shen syntax, standard package/macro processing, and native application code.
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
export RC_SOURCE_TEST_ROOT="$root"
exec python3 - <<'PY'
import contextlib
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import tempfile

root = Path(os.environ['RC_SOURCE_TEST_ROOT'])
retained = os.environ.get('RC_SOURCE_TEST_DIR')
if retained:
    Path(retained).resolve().mkdir(parents=True, exist_ok=False)
workspace = contextlib.nullcontext(str(Path(retained).resolve())) if retained else tempfile.TemporaryDirectory(prefix='shen-rc-source-test.')
with workspace as temp:
    temp = Path(temp)
    cwd = temp / "working directory's spaces"; cwd.mkdir()
    source = cwd / "source file's spaces.shen"
    source.write_text('''\\* Standard package exports, list patterns, and a captured lambda. *\\
(package native-proof [native-proof-run]
  (define sum-list
    [] -> 0
    [X | Xs] -> (+ X (sum-list Xs)))
  (define map-list
    F [] -> []
    F [X | Xs] -> [(F X) | (map-list F Xs)])
  (define native-proof-run
    Xs Offset -> (sum-list (map-list (/. X (+ X Offset)) Xs))))
(package empty-proof [])
(defmacro proof-bytes-macro
  [proof-bytes!] -> (cn (n->string 34) (cn (n->string 0) (n->string 92))))
(define proof-bytes -> (proof-bytes!))
(defmacro proof-symbol-macro [proof-symbol!] -> (intern "1e999"))
(define proof-symbol -> (proof-symbol!))
(defmacro proof-inc-macro
  [proof-inc! X] -> (do (set *native-proof-reader* 91)
                      (do (put native-proof-note reader-value 73 (value *property-vector*))
                          [+ X 1])))
(datatype native-proof-type
  __________
  42 : native-proof-type;)
(synonyms proof-number number)
(proof-inc! 41)
(if (= (arity late-raw) -1) true (simple-error "raw defun predeclared too early"))
(defun late-raw (X) X)
(defun shen.rc-source-arity (X Y) (+ X Y))
(if (and (= (arity native-proof-run) 2)
         (= (hd (get native-proof-run shen.source (value *property-vector*))) defun))
    (do (if (and (= (proof-bytes) (cn (n->string 34) (cn (n->string 0) (n->string 92))))
                 (= (hd (tl (tl (tl (get proof-bytes shen.source (value *property-vector*)))))) (proof-bytes)))
            true (simple-error "quoted/NUL literal or source reflection changed"))
      (do (if (= (proof-symbol) (intern "1e999"))
              true (simple-error "numeric-looking symbol changed"))
        (do (if (and (= (value *native-proof-reader*) 91)
                 (= (get native-proof-note reader-value (value *property-vector*)) 73))
            true (simple-error "reader macro side effects missing"))
      (do (if (and (package? empty-proof) (= (external empty-proof) []))
            true (simple-error "empty package metadata missing"))
      (do (if (and (element? native-proof-run (external native-proof))
                   (element? native-proof.sum-list (internal native-proof)))
              true (simple-error "package metadata missing"))
        (do (if (= (eval [proof-inc! 41]) 42)
                true (simple-error "macro metadata missing"))
          (do (if (= (shen.typecheck 42 native-proof-type) native-proof-type)
                  true (simple-error "datatype metadata missing"))
            (do (if (= (shen.rectify-type proof-number) number)
                    true (simple-error "synonym metadata missing"))
              (do (if (and (= (arity late-raw) 1) (= (shen.rc-source-arity 20 22) 42))
                      true (simple-error "raw defun or metadata symbol collision"))
                (let Invoke (fn native-proof-run)
                  (let Marker (write-to-file "application marker.txt" "executed")
                    (Invoke [10 12] 10))))))))))))
    (simple-error "source definition metadata missing"))
''')
    original = source.read_bytes()
    output = cwd / 'native output'
    command = [str(root / 'scripts/compile-shen-rc.sh'), str(source), str(output)]
    subprocess.run(command, cwd=cwd, check=True, timeout=360)
    assert source.read_bytes() == original, 'compiler changed source'
    assert not (cwd / 'application marker.txt').exists(), 'application top form executed during compilation'
    kl = (output / 'source.kl').read_text()
    assert '(defun native-proof.sum-list ' in kl, 'source definition was not lowered to KL'
    assert '(defun native-proof.map-list ' in kl, 'list pattern definition missing from KL'
    assert 'eval-kl' not in kl, 'source lowering introduced dynamic eval-kl forwarding'
    generated = (output / 'app.c').read_text()
    assert 'native-proof.sum-list' in generated and re.search(r'static RcValue \*rc_native_\d+\(',generated), 'native definition missing'
    assert not re.search(r'\brc_eval\s*\(',generated), 'generated application delegates to evaluator'
    receipt = json.loads((output / 'source-build.json').read_text())
    assert receipt['source_sha256'] == hashlib.sha256(original).hexdigest()
    assert receipt['source_transport'] == 'escaped-v1'
    assert receipt['source_events_sha256'] == hashlib.sha256((output / 'source-events.kl').read_bytes()).hexdigest()
    for name, expected in receipt['output_sha256'].items():
        assert hashlib.sha256((output / name).read_bytes()).hexdigest() == expected, name
    result = subprocess.check_output([str(output / 'app')], cwd=cwd,
                                    env=dict(os.environ, SHEN_RC_PRINT='1'), text=True, timeout=30)
    lines = result.splitlines()
    assert lines and lines[-1] == '42', repr(result)
    assert all(line in ('proof-inc-macro', 'proof-bytes-macro', 'proof-symbol-macro', 'native-proof-type#type', 'synonyms', 'true', '42') for line in lines[:-1]), repr(result)
    assert 'executed' in (cwd / 'application marker.txt').read_text(), 'application top form did not execute at runtime'
    subprocess.run([str(root / 'scripts/test-rc.sh'),'--audit-only',str(output / 'app')], check=True, timeout=30)
    broken = cwd / 'broken source.shen'
    broken.write_text('(define broken X ->')
    failed_output = cwd / 'failed native output'
    failed = subprocess.run([str(root / 'scripts/compile-shen-rc.sh'),str(broken),str(failed_output)],
                            cwd=cwd,stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True,timeout=60)
    assert failed.returncode != 0 and not (failed_output / 'app').exists(), 'invalid source produced an executable'
    aliases = cwd / 'alias outputs'; aliases.mkdir()
    alias_source = aliases / 'source.kl'; alias_source.write_text('42')
    alias = subprocess.run([str(root / 'scripts/compile-shen-rc.sh'),str(alias_source),str(aliases)],
                           cwd=cwd,stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True,timeout=30)
    assert alias.returncode != 0 and 'aliases compiler output' in alias.stderr and alias_source.read_text() == '42'
    external_source = cwd / 'hardlinked source.shen'; external_source.write_text('42')
    os.link(external_source, aliases / 'frontend.log')
    alias = subprocess.run([str(root / 'scripts/compile-shen-rc.sh'),str(external_source),str(aliases)],
                           cwd=cwd,stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True,timeout=30)
    assert alias.returncode != 0 and 'aliases compiler output' in alias.stderr and external_source.read_text() == '42'
    if retained:
        print('Retained source compiler proof:', temp)
    print('RC Shen source compilation: package code/reflection, empty package, macros/global/property replay, datatypes, synonyms, quoted/NUL literals/reflection, numeric symbols, raw defun timing, list patterns, closures, native output, deferred top effects, and invalid-source failure PASS')
PY
