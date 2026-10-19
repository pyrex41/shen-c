#!/usr/bin/env python3
"""Package a previously built native toolchain; record every shipped file."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import re
import shlex
import shutil
import subprocess
import tarfile
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def capture(command):
    return subprocess.check_output(command, cwd=ROOT, text=True, stderr=subprocess.STDOUT, timeout=30).strip()


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(__doc__)
    parser.add_argument('--bin-dir', type=Path, default=ROOT / 'bin')
    parser.add_argument('--output', type=Path, default=ROOT / 'dist')
    args = parser.parse_args()
    version = re.search(r'#define SHEN_RC_VERSION "([0-9.]+)"', (ROOT / 'src/rc/main.c').read_text()).group(1)
    binary_dir = args.bin_dir.resolve()
    output = args.output.resolve()
    system = {'Darwin': 'macos', 'Linux': 'linux'}[platform.system()]
    arch = {'arm64': 'arm64', 'aarch64': 'arm64', 'x86_64': 'x86_64'}[platform.machine()]
    name = 'shen-c-%s-%s-%s' % (version, system, arch)
    output.mkdir(parents=True, exist_ok=True)
    archive = output / (name + '.tar.gz')
    if archive.exists():
        raise RuntimeError('refusing to replace existing archive: %s' % archive)
    if capture([str(binary_dir / 'shen-rc'), '--version']) != 'Shen-C RC %s (native compiled kernel)' % version:
        raise RuntimeError('binary version does not match release source')
    dirty = capture(['git', 'status', '--porcelain', '--untracked-files=no'])
    if dirty:
        raise RuntimeError('release source has tracked modifications')
    with tempfile.TemporaryDirectory(prefix='shen-c-package-') as temporary:
        package = Path(temporary) / name
        (package / 'bin').mkdir(parents=True)
        (package / 'libexec/shen-c').mkdir(parents=True)
        share = package / 'share/shen-c'
        (share / 'scripts').mkdir(parents=True)
        shutil.copy2(binary_dir / 'shen-rc', package / 'bin/shen-rc')
        shutil.copy2(binary_dir / 'shen-rc', package / 'bin/shen-c')
        shutil.copy2(binary_dir / 'rc-build', package / 'libexec/shen-c/rc-build')
        # Only tracked files: never copy build outputs, caches or local evidence.
        paths = capture(['git', 'ls-files', 'src/rc', 'shen']).splitlines()
        for relative in paths:
            destination = share / relative
            destination.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(ROOT / relative, destination)
        for script in ('embed-tier-config.py', 'release_manifest.py'):
            shutil.copy2(ROOT / 'scripts' / script, share / 'scripts' / script)
        shutil.copy2(ROOT / 'scripts/compile-shen-rc.sh', package / 'bin/compile-shen-rc.sh')
        wrapper = package / 'bin/rc-build'
        wrapper.write_text('#!/bin/sh\nset -eu\n'
                           'prefix=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)\n'
                           ': "${SHEN_C_HOME:=$prefix/share/shen-c}"\n'
                           'export SHEN_C_HOME\n'
                           'exec "$prefix/libexec/shen-c/rc-build" "$@"\n')
        wrapper.chmod(0o755)
        for document in ('README.md', 'CHANGELOG.md', 'LICENSE'):
            shutil.copy2(ROOT / document, package / document)
        dependencies = capture(['otool', '-L', str(binary_dir / 'shen-rc')]) if system == 'macos' else capture(['ldd', str(binary_dir / 'shen-rc')])
        # The first otool line identifies the build file, not a dependency.
        if system == 'macos':
            dependencies = '\n'.join(dependencies.splitlines()[1:])
        receipt = dict(version=version, revision=capture(['git', 'rev-parse', 'HEAD']),
                       system=platform.platform(), architecture=arch,
                       compiler=capture(shlex.split(os.environ.get('CC', 'cc')) + ['--version']),
                       cflags=os.environ.get('RC_CFLAGS', os.environ.get('CFLAGS', 'Makefile.rc default')),
                       lto_policy=os.environ.get('RC_LTO', 'auto'),
                       makefile_sha256=digest(ROOT / 'Makefile.rc'),
                       dependencies=dependencies,
                       files={str(p.relative_to(package)): digest(p) for p in sorted(package.rglob('*')) if p.is_file()})
        if system == 'macos':
            receipt['sdk'] = capture(['xcrun', '--show-sdk-version'])
            receipt['deployment_target'] = os.environ.get('MACOSX_DEPLOYMENT_TARGET', 'toolchain default')
        else:
            receipt['glibc'] = platform.libc_ver()
        (package / 'build-receipt.json').write_text(json.dumps(receipt, indent=2) + '\n')
        with tarfile.open(archive, 'w:gz') as bundle:
            bundle.add(package, arcname=name)
    archive.with_suffix('.sha256').write_text(digest(archive) + '  ' + archive.name + '\n')
    print(archive)


if __name__ == '__main__':
    main()
