#!/bin/sh
# Replay reader metadata without parsing/evaluating user definitions at runtime.
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
builder=${1:-"$root/bin/rc-build"}
case "$builder" in /*) ;; *) builder="$PWD/$builder" ;; esac
export RC_METADATA_ROOT="$root" RC_METADATA_BUILDER="$builder"
exec python3 - <<'PY'
import os
from pathlib import Path
import subprocess
import tempfile

with tempfile.TemporaryDirectory(prefix='shen-rc-metadata.') as tmp:
    work = Path(tmp); source = work / 'input'; source.mkdir()
    (source / 'kernel.kl').write_text('''
(defun shen.sysfunc? (F) (= F reserved))
(defun shen.store-arity (F N) (set F N))
(defun shen.record-kl (F AST)
  (do (set *prior-exists* (trap-error (do (function F) true) (lambda E false)))
      (set *records* (cons AST (value *records*)))))
(defun fn (F) (lambda X (F X)))
(defun shen.record-macro (F Closure) (set *macro* Closure))
(defun shen.remember-datatype (F Closure) (set *datatype* Closure))
(defun shen.record-external (P Exports) (set *external* Exports))
(defun put (Owner Key Value Table) (address-> Table 0 Value))
(set *property-vector* (absvector 1))
(set *records* ())
''')
    (source / 'rc.manifest.txt').write_text('kernel=kernel.kl\nuser=user.kl\nsource-events=events.kl\n')
    (source / 'events.kl').write_text('''
(call shen.store-arity (forward 1))
(call shen.store-arity (forward 2))
(call shen.store-arity (auxiliary 1))
(call shen.record-kl (auxiliary (defun auxiliary (X) (+ X 1))))
(definition (defun auxiliary (X) (+ X 1)))
(macro auxiliary)
(datatype auxiliary)
(global *synonyms* (foo bar))
(call shen.record-external (pkg (auxiliary)))
(property owner key (property payload))
''')
    (source / 'user.kl').write_text('''
(value forward)
(trap-error (value late) (lambda E -1))
(= (value *records*) ())
(auxiliary 41)
((value *macro*) 41)
((value *datatype*) 41)
(= (value *synonyms*) (cons foo (cons bar ())))
(= (value *external*) (cons auxiliary ()))
(= (<-address (value *property-vector*) 0) (cons property (cons payload ())))
(trap-error (forward 42) (lambda E true))
(defun forward (X) X)
(value forward)
(value *prior-exists*)
(= (hd (hd (value *records*))) defun)
(= (hd (tl (hd (value *records*)))) forward)
(forward 42)
(defun forward (X Y) Y)
(value forward)
(value *prior-exists*)
(forward 1 42)
(defun late (X) X)
(value late)
(do (defun nested (Z) Z) (nested 42))
(trap-error (value nested) (lambda E -1))
(defun auxiliary (X) (+ X 2))
((value *macro*) 40)
((value *datatype*) 40)
(defun reserved (X) X)
''')
    expected = ['2','-1','false','42','42','42','true','true','true','true','1','false','true','true','42','2','true','42','1','42','-1','42','42']
    env = dict(os.environ, SHEN_C_HOME=os.environ['RC_METADATA_ROOT'])
    for mode, flags in [('optimized', []), ('conservative', ['--conservative-ownership'])]:
        output = work / mode
        p = subprocess.run([os.environ['RC_METADATA_BUILDER'], '--source-metadata', *flags, str(source), str(output)],
                           env=env, text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=90)
        if p.returncode:
            raise SystemExit('%s source metadata build failed:\n%s\n%s' % (mode,p.stdout,p.stderr))
        generated = (output / 'app.c').read_text()
        if 'rc_literal_' not in generated or 'rc_source_definition' not in generated:
            raise SystemExit('source metadata values/protocol missing from native artifact')
        if 'rc_eval(ctx' in generated or 'rc_read(ctx' in generated:
            raise SystemExit('source metadata emitted a user parse/eval wrapper')
        p = subprocess.run([str(output / 'app')], env=dict(env, SHEN_RC_PRINT='1'),
                           text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=30)
        sanitizer_error = any(token in p.stderr for token in ['ERROR: AddressSanitizer', 'LeakSanitizer', 'runtime error:', 'UndefinedBehaviorSanitizer'])
        if sanitizer_error or p.returncode != 1 or 'reserved is not a legitimate function name' not in p.stderr or p.stdout.splitlines() != expected:
            raise SystemExit('%s metadata failed exit=%s output=%r expected=%r\n%s' % (mode,p.returncode,p.stdout,expected,p.stderr))
    print('RC source metadata: ordered events, native auxiliaries, dynamic macro/type wrappers, raw defun timing, literal records, protected names PASS (optimized/conservative)')
PY
