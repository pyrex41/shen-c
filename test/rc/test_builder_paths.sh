#!/bin/sh
# Generated Makefiles must preserve checkout, input, and output path spaces.
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
builder=${1:-"$root/bin/rc-build"}
case "$builder" in /*) ;; *) builder="$PWD/$builder" ;; esac
export RC_PATH_ROOT="$root" RC_PATH_BUILDER="$builder"
exec python3 - <<'PY'
import os
from pathlib import Path
import subprocess
import tempfile

root = Path(os.environ['RC_PATH_ROOT'])
with tempfile.TemporaryDirectory(prefix='shen-rc-paths.') as temp:
    temp = Path(temp)
    home = temp / "checkout with spaces and 'quote'"; home.mkdir()
    (home / 'src').symlink_to(root / 'src', target_is_directory=True)
    (home / 'shen').symlink_to(root / 'shen', target_is_directory=True)
    source = temp / 'input with spaces'; source.mkdir()
    (source / 'kernel.kl').write_text('')
    (source / 'user.kl').write_text('(+ 20 22)\n')
    output = temp / 'output with spaces'
    env = dict(os.environ, SHEN_C_HOME=str(home))
    subprocess.run([os.environ['RC_PATH_BUILDER'], str(source), str(output)], env=env, check=True, timeout=60)
    result = subprocess.check_output([str(output / 'app')], env=dict(env, SHEN_RC_PRINT='1'), text=True, timeout=10)
    if result != '42\n':
        raise SystemExit('RC path build returned %r' % result)
    # Exercise the literal Makefile default as well as the environment override.
    env.pop('SHEN_C_HOME')
    subprocess.run(['make', '-B', 'app'], cwd=output, env=env, check=True, timeout=60)
    print('RC builder paths: spaces and shell quotes PASS')
PY
