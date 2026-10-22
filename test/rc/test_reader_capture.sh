#!/bin/sh
# Frontend-only checks: explicit CLI, real compiled kernel, no native C rebuild.
set -eu
if [ "$#" -ne 1 ]; then
  echo "usage: test_reader_capture.sh /path/to/shen-rc" >&2
  exit 2
fi
exec python3 - "$1" <<'PY'
from pathlib import Path
import hashlib
import os
import signal
import subprocess
import sys
import tempfile
import time

cli = Path(sys.argv[1]).resolve()
if not cli.is_file() or not os.access(cli, os.X_OK):
    raise SystemExit('reader capture: explicit CLI is not executable')
print('CLI_SHA256=' + hashlib.sha256(cli.read_bytes()).hexdigest(), flush=True)
checks = 0
base_env = dict(os.environ)
for key in ('SHEN_C_HOME', 'SHEN_RC_HOME'):
    base_env.pop(key, None)

with tempfile.TemporaryDirectory(prefix='shen-rc-reader-capture-') as temporary:
    root = Path(temporary)

    def run(name, source, *, error=None, verify=None):
        global checks
        case = root / ('%02d' % (checks + 1))
        case.mkdir()
        input_path = case / 'source.shen'
        program, events = case / 'program.kl', case / 'events.kl'
        input_path.write_text(source)
        program.write_bytes(b'prior program\n')
        events.write_bytes(b'prior events\n')
        with (case / 'stdout').open('wb') as stdout, (case / 'stderr').open('wb') as stderr:
            child = subprocess.Popen([str(cli), 'compile-kl', str(input_path), str(program), str(events)],
                                     cwd=case, env=base_env, stdin=subprocess.DEVNULL,
                                     stdout=stdout, stderr=stderr, start_new_session=True)
            start = time.monotonic()
            while child.poll() is None:
                if time.monotonic() - start > 30 or stdout.tell() + stderr.tell() > 8 * 1024 * 1024:
                    os.killpg(child.pid, signal.SIGKILL)
                    child.wait()
                    raise AssertionError(name + ': frontend exceeded time/output budget')
                time.sleep(0.025)
        diagnostic = (case / 'stderr').read_text(errors='replace')
        if error is not None:
            assert child.returncode != 0, name + ': unsupported input succeeded'
            assert error in diagnostic, (name, diagnostic)
            assert program.read_bytes() == b'prior program\n', name + ': changed prior program'
            assert events.read_bytes() == b'prior events\n', name + ': changed prior events'
        else:
            assert child.returncode == 0, (name, child.returncode, diagnostic)
            assert not diagnostic.strip(), (name, diagnostic)
            assert not (case / 'runtime-only-marker').exists(), name + ': application top executed during compilation'
            if verify is not None:
                verify(program.read_text(), events.read_text())
        checks += 1
        print('PASS ' + name, flush=True)

    def registrations(program, events):
        for marker in ('(call shen.record-external (capture-package ',
                       '(call shen.record-internal (capture-package ',
                       '(call shen.record-external (capture-empty ',
                       '(call shen.store-arity (capture-macro 1))',
                       '(call shen.record-kl (capture-macro ',
                       '(definition (defun capture-macro ',
                       '(macro capture-macro)',
                       '(datatype capture-type#type)',
                       '(global shen.*synonyms* '):
            assert marker in events, ('missing metadata', marker)
        assert events.index('(call shen.record-kl (capture-macro ') < events.index('(definition (defun capture-macro ') < events.index('(macro capture-macro)')
        assert 'write-to-file' in program

    run('standard package macro datatype synonym metadata', '''
(package capture-package [capture-export]
  (define helper X -> X)
  (define capture-export X -> (helper X)))
(package capture-empty [])
(defmacro capture-macro [capture-trigger] -> 42)
(datatype capture-type __________ 42 : capture-type;)
(synonyms capture-number number)
(capture-trigger)
(write-to-file "runtime-only-marker" "runtime")
''', verify=registrations)

    def persistent_state(program, events):
        assert '(call shen.store-arity (+ 7))' not in events, 'replayed a caught failure'
        assert '(global *capture-state* 42)' in events
        assert '(property capture-key capture-property 43)' in events
        assert '(global shen.*gensym* ' in events
        assert events.index('(global *capture-state* 42)') < events.index('(property capture-key capture-property 43)')
        assert program.rstrip().endswith('(value *capture-state*)')

    run('successful scalar property effects and caught failure', '''
(defmacro capture-caught [capture-caught!] ->
  (trap-error (shen.store-arity + 7) (/. E 42)))
(defmacro capture-state [capture-state!] ->
  (do (set *capture-state* 42) (do (put capture-key capture-property 43) 42)))
(capture-caught!)
(capture-state!)
(value *capture-state*)
''', verify=persistent_state)

    def property_removal(program, events):
        put = '(property capture-gone capture-property 44)'
        gone = '(unproperty capture-gone capture-property)'
        assert put in events and gone in events and events.index(put) < events.index(gone), events
        # update-lambda-table to arity 0 removes the stale lambda form.
        assert '(property capture-zero arity 0)' in events, events
        assert '(unproperty capture-zero shen.lambda-form)' in events, events

    run('reader-time property removal and arity-0 lambda table update', '''
(define capture-zero X -> X)
(defmacro capture-remove [capture-remove!] ->
  (do (put capture-gone capture-property 44)
      (do (unput capture-gone capture-property) 42)))
(defmacro capture-zero-arity [capture-zero!] ->
  (do (update-lambda-table capture-zero 0) 42))
(capture-remove!)
(capture-zero!)
''', verify=property_removal)

    def escaped_atoms(program, events):
        literal = '"\\\"\\x00\\\\\\xff"'
        assert '(defun capture-bytes () ' + literal + ')' in program, program
        assert '#sym"1e9999"' in program
        assert '#sym"1e-9999"' in program
        assert '#sym"a b("' in program
        assert '(macro capture-byte-macro)' in events
        assert '(cn ' not in program, 'literal string became application AST'

    run('lossless escaped string and symbol atoms', '''
(defmacro capture-byte-macro [capture-bytes!] ->
  (cn (n->string 34) (cn (n->string 0) (cn (n->string 92) (n->string 255)))))
(defmacro capture-big-symbol [capture-big!] -> (intern "1e9999"))
(defmacro capture-small-symbol [capture-small!] -> (intern "1e-9999"))
(defmacro capture-space-symbol [capture-space!] -> (intern "a b("))
(define capture-bytes -> (capture-bytes!))
(capture-big!)
(capture-small!)
(capture-space!)
''', verify=escaped_atoms)

    def stages(program, events):
        assert '(call shen.store-arity (capture-early 2))' in events
        assert '(call shen.store-arity (capture-late ' not in events
        assert program.index('(arity capture-late)') < program.index('(defun capture-late ')
        assert 'shen.rc-source-arity' in program, 'ordinary function name was treated as a directive'

    run('define versus raw defun reader timing', '''
(arity capture-late)
(defun capture-late (X) X)
(define capture-early X Y -> (+ X Y))
(defun shen.rc-source-arity (X Y) (+ X Y))
(shen.rc-source-arity 20 22)
''', verify=stages)

    negatives = [
        ('nested eval-kl definition boundary', '''
(defmacro capture-invalid [capture-invalid!] ->
  (do (eval-kl [do [defun capture-hidden [] 42] 0]) 42))
(capture-invalid!)
(capture-hidden)
''', 'nested persistent definitions'),
        ('nested auxiliary definition boundary', '''
(defmacro capture-invalid [capture-invalid!] ->
  (do (defun capture-hidden () 42) 42))
(capture-invalid!)
(capture-hidden)
''', 'nested definitions in reader-installed'),
        ('actual capture hook replacement boundary', '''
(defmacro capture-invalid [capture-invalid!] ->
  (do (eval-kl (hd (read-from-string "(defun shen.store-arity (A B) A)"))) 42))
(capture-invalid!)
''', 'replaced a capture hook'),
        ('noncanonical macro callback boundary', '''
(defmacro capture-invalid [capture-invalid!] ->
  (do (shen.record-macro custom-capture (/. X X)) 42))
(capture-invalid!)
''', 'custom macro/datatype callback'),
        ('noncanonical datatype callback boundary', '''
(defmacro capture-invalid [capture-invalid!] ->
  (do (shen.remember-datatype custom-capture (/. X X)) 42))
(capture-invalid!)
''', 'custom macro/datatype callback'),
        ('persistent property address mutation is sticky', '''
(defmacro capture-invalid [capture-invalid!] ->
  (trap-error (do (address-> (value *property-vector*) 1 []) 42) (/. E 42)))
(capture-invalid!)
''', 'direct reader mutation'),
        ('persistent closure payload boundary', '''
(defmacro capture-invalid [capture-invalid!] ->
  (do (set *capture-closure* (/. X X)) 42))
(capture-invalid!)
''', 'unsupported literal'),
        ('persistent vector payload boundary', '''
(defmacro capture-invalid [capture-invalid!] ->
  (do (set *capture-vector* (absvector 1)) 42))
(capture-invalid!)
''', 'unsupported literal'),
        ('malformed source preserves prior outputs', '(define capture-broken X ->', 'shen-rc:'),
    ]
    for name, source, error in negatives:
        run(name, source, error=error)

print('RC reader capture: PASS (%d cases)' % checks, flush=True)
PY
