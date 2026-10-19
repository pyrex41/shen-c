#!/usr/bin/env python3
"""Exercise the extracted release from a relocated path and empty cwd."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import tarfile
import tempfile


def main():
    parser = argparse.ArgumentParser(__doc__)
    parser.add_argument('archive', type=Path)
    parser.add_argument('--suite-output', type=Path, required=True)
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    with tempfile.TemporaryDirectory(prefix='shen-c-relocated-') as temporary:
        temporary = Path(temporary)
        with tarfile.open(args.archive) as bundle:
            # Only archives just produced by package-release.py enter this check.
            for entry in bundle.getmembers():
                if entry.name.startswith('/') or '..' in Path(entry.name).parts or entry.issym() or entry.islnk():
                    raise RuntimeError('unsafe release archive entry')
            bundle.extractall(temporary)
        package = next(temporary.glob('shen-c-*'))
        relocated = temporary / "relocated toolchain's directory"
        package.rename(relocated)
        receipt = json.loads((relocated / 'build-receipt.json').read_text())
        for name, expected in receipt['files'].items():
            assert hashlib.sha256((relocated / name).read_bytes()).hexdigest() == expected, name
        work = temporary / 'empty'
        work.mkdir()
        env = {k: v for k, v in os.environ.items() if not k.startswith(('SHEN_', 'RC_BUILDER'))}
        env.update(XDG_CACHE_HOME=str(temporary / 'cache'), SHEN_RC_VERIFY_NATIVE='1')
        for binary in ('shen-c', 'shen-rc'):
            command = [str(relocated / 'bin' / binary)]
            result = subprocess.check_output(command + ['eval', '-e', '(+ 20 22)'], cwd=work, env=env, text=True, timeout=120)
            assert result.strip() == '42', result
        source = work / 'input.shen'
        source.write_text('(define release-answer X -> (+ X 1))\n(release-answer 41)\n')
        subprocess.run([str(relocated / 'bin/compile-shen-rc.sh'), str(source), str(work / 'native')],
                       cwd=work, env=env, check=True, timeout=600)
        result = subprocess.check_output([str(work / 'native/app')], cwd=work,
                                         env=dict(env, SHEN_RC_PRINT='1'), text=True, timeout=120)
        assert result.strip() == '42', result
        for binary in ('bin/shen-rc', 'libexec/shen-c/rc-build'):
            subprocess.run([str(root / 'scripts/test-rc.sh'), '--audit-only', str(relocated / binary)],
                           cwd=work, env=env, check=True, timeout=30)
        # This validates all 136 assertions, redirect output, cold strict native
        # execution and a cached run with zero new compilations.
        subprocess.run(['python3', str(root / 'scripts/test-compile-first.py'), str(relocated / 'bin/shen-rc'),
                        '--timeout', '1200', '--output', str(args.suite_output.resolve())],
                       cwd=work, env=env, check=True, timeout=2700)
    print('Relocated release toolchain and complete suite PASS')


if __name__ == '__main__':
    main()
