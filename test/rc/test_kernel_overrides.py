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
program = ['(define kernel-overrides.result F X -> (trap-error (F X) (/. E (cn "error: " (error-to-string E)))))']
for n in unary:
    for x in inputs:
        program.append('(let A (kernel-overrides.result (/. Y (%s Y)) %s) B (kernel-overrides.result (/. Y (kernel.%s Y)) %s) '
                       '(output "~A ~A~%%" (if (= A B) same [A B]) %s))' % (n, x, n, x, '"' + n + ' ' + x.replace('"', 'c#34;') + '"'))
with tempfile.TemporaryDirectory(prefix='shen-rc-overrides.') as tmp:
    work = Path(tmp)
    (work / 'kernel.kl').write_text('\n'.join(definitions) + '\n')
    (work / 'checks.shen').write_text('\n'.join(program) + '\n')
    load = ('(do (map (/. F (do (eval-kl F) (update-lambda-table (hd (tl F)) (length (hd (tl (tl F)))))))'
            ' (read-file "kernel.kl")) (load "checks.shen"))')
    env = dict(os.environ, SHEN_RC_NATIVE_CACHE=os.environ.get('SHEN_RC_NATIVE_CACHE', str(work / 'cache')))
    run = subprocess.run([str(binary), 'eval', '-e', load], cwd=work, env=env, capture_output=True, text=True, timeout=1200)
lines = [l for l in run.stdout.splitlines() if re.match(r'^(same|\[)', l)]
bad = [l for l in lines if not l.startswith('same ')]
assert run.returncode == 0, (run.returncode, run.stderr[-2000:])
assert len(lines) == len(unary) * len(inputs), (len(lines), run.stdout[-2000:])
assert not bad, '\n'.join(bad)
print('RC kernel overrides: %d defuns x %d inputs identical to the pinned %s kernel PASS'
      % (len(unary), len(inputs), release['kernel_version']))
