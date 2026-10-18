#!/bin/sh
# Exercise ownership and calls in native functions whose lexical frames are large.
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
builder=${1:-"$root/bin/rc-build"}
case "$builder" in /*) ;; *) builder="$PWD/$builder" ;; esac
export RC_FRAME_ROOT="$root" RC_FRAME_BUILDER="$builder"
exec python3 - <<'PY'
import os
from pathlib import Path
import resource
import signal
import time
import subprocess
import tempfile

root = Path(os.environ['RC_FRAME_ROOT'])
def lets(n, body):
    for i in reversed(range(n)):
        body = '(let V%d %d %s)' % (i, i, body)
    return body

with tempfile.TemporaryDirectory(prefix='shen-rc-frames.') as work:
    work = Path(work)
    source = work / 'input'; source.mkdir()
    (source / 'kernel.kl').write_text('')
    (source / 'user.kl').write_text('\n'.join([
        '(defun frame-rec (N) ' + lets(300, '(if (= N 0) 0 (+ 1 (frame-rec (- N 1))))') + ')',
        '(defun frame-capture (X) ' + lets(80, '(lambda Y (+ X Y))') + ')',
        '(defun frame-fault () ' + lets(80, '(trap-error (cons 1 (simple-error "frame-fault")) (lambda E 42))') + ')',
        '(defun frame-tail (N) ' + lets(80, '(if (= N 0) 42 (frame-tail (- N 1)))') + ')',
        '(frame-rec 256)', '((frame-capture 20) 22)', '(frame-fault)', '(frame-tail 5000)',
    ]) + '\n')
    env = dict(os.environ, SHEN_C_HOME=str(root))
    build_timeout = int(os.environ.get('RC_FRAME_BUILD_TIMEOUT', '300'))
    if not 1 <= build_timeout <= 1800:
        raise SystemExit('RC_FRAME_BUILD_TIMEOUT must be within 1..1800 seconds')
    started = time.monotonic()
    build = subprocess.Popen([os.environ['RC_FRAME_BUILDER'], str(source), str(work / 'app')],
                             env=env, start_new_session=True)
    try:
        status = build.wait(timeout=build_timeout)
    except subprocess.TimeoutExpired:
        # The builder forks make/compiler processes; stop the whole test group.
        try:
            os.killpg(build.pid, signal.SIGTERM)
        except ProcessLookupError:
            pass
        try:
            build.wait(timeout=5)
        except subprocess.TimeoutExpired:
            pass
        try:
            os.killpg(build.pid, signal.SIGKILL)
        except ProcessLookupError:
            pass
        build.wait()
        raise SystemExit('RC large frames: build UNKNOWN after %ss timeout' % build_timeout)
    generated_size = (work / 'app/app.c').stat().st_size if (work / 'app/app.c').exists() else 0
    print('RC large frames: generated C bytes=%d build_seconds=%.3f' %
          (generated_size, time.monotonic()-started), flush=True)
    if status:
        raise SystemExit('RC large frames: builder failed with status %s' % status)
    def stack_limit():
        requested = os.environ.get('RC_FRAME_STACK_KB')
        if requested:
            _, hard = resource.getrlimit(resource.RLIMIT_STACK)
            resource.setrlimit(resource.RLIMIT_STACK, (int(requested) * 1024, hard))
    p = subprocess.run([str(work / 'app/app')], env=dict(env, SHEN_RC_PRINT='1'),
                       stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
                       timeout=30, preexec_fn=stack_limit)
    if p.returncode or p.stdout != '256\n42\n42\n42\n':
        raise SystemExit('RC large frames failed: exit=%s stdout=%r stderr=%s' % (p.returncode,p.stdout,p.stderr))
    print('RC large frames: recursion, captures, caught faults, and tail calls PASS')
PY
