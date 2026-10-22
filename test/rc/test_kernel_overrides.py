#!/usr/bin/env python3
"""Native overrides of kernel defuns against the pinned kernel's own bodies.

usage: test/rc/test_kernel_overrides.py [SHEN_C_BINARY]

Each kernel defun that a native primitive replaces is installed again under
kernel.NAME from the pinned KLambda, then both are applied to the same inputs.
Results, including error messages, must be identical.
"""
import os, re, subprocess, sys, tempfile
from pathlib import Path
root = Path(__file__).resolve().parents[2]
binary = Path(sys.argv[1] if len(sys.argv) > 1 else root / 'bin/shen-c').resolve()
sys.path.insert(0, str(root / 'scripts'))
from release_manifest import load_release, kernel_files
release = load_release()
text = ''.join((root / f).read_text() for f in kernel_files(release))

def defun(name):
    i = text.index('(defun ' + name + ' '); depth = 0; quoted = False
    for k in range(i, len(text)):
        ch = text[k]
        if quoted: quoted = ch != '"'
        elif ch == '"': quoted = True
        elif ch == '(': depth += 1
        elif ch == ')':
            depth -= 1
            if depth == 0: return text[i:k + 1]
    raise ValueError(name)

# Kernel defuns replaced by src/rc/primitives.c "overwrites" (pr is a
# documented deviation and is not compared).
unary = ['symbol?', 'boolean?', 'integer?', 'variable?', 'not']
inputs = ['abc', '(intern "Abc")', '(intern "123")', '(intern "A b")', '(intern "a b")', '(intern "")',
          '(intern "{")', '(intern "}")', '(intern ":")', '(intern ";")', '(intern ",")', '(intern "|")', '(intern "-")',
          '(intern "_x")', '(intern "X1")', '(intern "A^")', '(intern "a^")', '(intern "Xy-z")', '(intern "1.5e3")', '(intern "é")', '(intern "aé")',
          '(intern "Éa")', '"str"', '""', '0', '1', '-7', '1.0', '1.5', '-2.0', '(* 1000000000.0 1000000000.0)',
          '(/ 1.0 1000000000000)', '(/ 1 3)', 'true', 'false', '[]', '[1 2]', '(@p 1 2)', '(vector 0)',
          '(absvector 0)', '(stinput)', '(/. X X)', 'shen.pvar']
# The Shen reader treats { and } as tokens; spell those symbols with intern.
definitions = [defun(n).replace('(defun ' + n + ' ', '(defun kernel.' + n + ' ', 1)
               .replace('(cons { (cons } ', '(cons (intern "{") (cons (intern "}") ') for n in unary]
# hash is replaced by a native port (primitives.c). Its kernel copy calls the
# installed shen.hashkey, so it includes the numeric hashkey wrapper too.
definitions.append(defun('hash').replace('(defun hash ', '(defun kernel.hash ', 1))
keys = inputs + ['(intern "shen.fail!")', '"shen.fail!"', '"..."', '(intern "...")', '"a"', '(n->string 0)',
                 '(cn "x" (cn (n->string 0) "y"))', '(n->string 255)', '"héllo wörld"', '-1', '-9223372036854775807',
                 '9223372036854775807', '1234567890123', '(intern "-")', '(fail)', '[]', '[a b]', '(@v 1 2 <>)',
                 '(intern "a b c d e f g h i j k l m n o p q r s t u v w x y z 0 1 2 3 4 5 6 7 8 9")',
                 '"The quick brown fox jumps over the lazy dog, repeatedly, until the product passes ten billion."']
moduli = ['1', '2', '7', '97', '20000', '1000003', '10000000019', '9223372036854775807', '2.0', '20000.0']
program = ['(define kernel-overrides.result F X -> (trap-error (F X) (/. E (cn "error: " (error-to-string E)))))']
for n in unary:
    for x in inputs:
        program.append('(let A (kernel-overrides.result (/. Y (%s Y)) %s) B (kernel-overrides.result (/. Y (kernel.%s Y)) %s) '
                       '(output "~A ~A~%%" (if (= A B) same [A B]) %s))' % (n, x, n, x, '"' + n + ' ' + x.replace('"', 'c#34;') + '"'))
for x in keys:
    for m in moduli:
        program.append('(let A (trap-error (hash %s %s) (/. E (cn "error: " (error-to-string E)))) B (trap-error (kernel.hash %s %s) (/. E (cn "error: " (error-to-string E)))) '
                       '(output "~A ~A~%%" (if (= A B) same [A B]) %s))' % (x, m, x, m, '"hash ' + x.replace('"', 'c#34;') + ' ' + m + '"'))
with tempfile.TemporaryDirectory(prefix='shen-rc-overrides.') as tmp:
    work = Path(tmp)
    (work / 'kernel.kl').write_text('\n'.join(definitions) + '\n')
    (work / 'checks.shen').write_text('\n'.join(program) + '\n')
    load = ('(do (map (/. F (do (eval-kl F) (update-lambda-table (hd (tl F)) (length (hd (tl (tl F)))))))'
            ' (read-file "kernel.kl")) (load "checks.shen"))')
    env = dict(os.environ, SHEN_RC_NATIVE_CACHE=os.environ.get('SHEN_RC_NATIVE_CACHE', str(work / 'cache')))
    run = subprocess.run([str(binary), 'eval', '-e', load], cwd=work, env=env, capture_output=True, text=True, timeout=1200)
    # Redefining a function the kernel hash path reaches must reach native
    # hash too: it falls back to the kernel closure.
    redefined = []
    for dep, body in [('shen.prodbutzero', '(defun shen.prodbutzero (X Y) 3)'),
                      ('string->n', '(defun string->n (S) 2)'),
                      ('shen.mod', '(defun shen.mod (X Y) 5)')]:
        check = ('(do %s (output "~A~%%" [(hash abc 97) (hash "xyz" 1000) (hash 42 97)]))' % body)
        r = subprocess.run([str(binary), 'eval', '-e', check], cwd=work, env=env, capture_output=True, text=True, timeout=600)
        redefined.append((dep, r.returncode, r.stdout.strip().splitlines()[-1:] , r.stderr[-300:]))
lines = [l for l in run.stdout.splitlines() if re.match(r'^(same|\[)', l)]
bad = [l for l in lines if not l.startswith('same ')]
assert run.returncode == 0, (run.returncode, run.stderr[-2000:])
assert len(lines) == len(unary) * len(inputs) + len(keys) * len(moduli), (len(lines), run.stdout[-2000:])
assert not bad, '\n'.join(bad)
expected = {'shen.prodbutzero': '[3 3 3]', 'string->n': None, 'shen.mod': '[5 5 5]'}
for dep, code, out, err in redefined:
    assert code == 0, (dep, err)
    if expected[dep]: assert out == [expected[dep]], (dep, out)
# string->n returning 2 for every byte gives 2^len, below the threshold here.
assert redefined[1][2] == ['[%d %d %d]' % (2**3 % 97, 2**3 % 1000, 2**2 % 97)], redefined[1]
print('RC kernel overrides: %d defuns x %d inputs, and hash x %d keys x %d moduli, identical to the pinned %s kernel PASS'
      % (len(unary), len(inputs), len(keys), len(moduli), release['kernel_version']))
