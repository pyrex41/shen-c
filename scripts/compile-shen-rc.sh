#!/bin/sh
# Shen source -> kernel frontend KL -> native C and independent RC executable.
set -eu
export SHEN_RC_SOURCE_DRIVER="$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)/$(basename -- "$0")"
exec python3 - "$@" <<'PY'
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import signal
import subprocess
import sys
import tempfile
import time

parser = argparse.ArgumentParser(description='Compile Shen source to native C using the compiled standard kernel frontend.',
    epilog='Each file starts from a fresh bundled kernel context. Standard package and macro processing runs during compilation in the caller working directory. Ordinary top forms run in the generated application. Explicit load/eval operations and their external files remain runtime dependencies.')
parser.add_argument('--conservative-ownership', action='store_true', help='use the explicit ownership baseline')
parser.add_argument('--acyclic', action='store_true', help='compile out cycle collection and reject new reference cycles')
parser.add_argument('source', type=Path)
parser.add_argument('output', type=Path)
args = parser.parse_args()
script = Path(os.environ['SHEN_RC_SOURCE_DRIVER']).resolve()
prefix = script.parent.parent
home = Path(os.environ.get('SHEN_C_HOME', prefix if (prefix / 'src/rc/rc.h').is_file() else prefix / 'share/shen-c')).resolve()
cli = Path(os.environ.get('SHEN_RC_CLI', prefix / 'bin/shen-rc')).resolve()
builder = Path(os.environ.get('RC_BUILDER', prefix / 'bin/rc-build')).resolve()
source = args.source.resolve()
output = args.output.resolve()
sys.path.insert(0, str(home / 'scripts'))
from release_manifest import load_release, kernel_files
release = load_release(Path(os.environ.get('SHEN_RELEASE_MANIFEST', home / 'shen/releases/shenoskernel-43.1.2.json')), home)
kernel = [home / name for name in kernel_files(release)]

def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()

def require_file(path, executable=False):
    if not path.is_file() or (executable and not os.access(path, os.X_OK)):
        raise RuntimeError('required %s unavailable: %s' % ('executable' if executable else 'file', path))

def run(label, command, log, timeout, env):
    start = time.monotonic()
    with log.open('wb') as stream:
        process = subprocess.Popen(command, cwd=Path.cwd(), env=env, stdin=subprocess.DEVNULL,
                                   stdout=stream, stderr=subprocess.STDOUT, start_new_session=True)
        reason = None
        while process.poll() is None:
            if time.monotonic() - start > timeout:
                reason = 'timeout after %ss' % timeout
            elif stream.tell() > 64 * 1024 * 1024:
                reason = 'output exceeds 64 MiB'
            if reason:
                os.killpg(process.pid, signal.SIGKILL)
                process.wait()
                break
            time.sleep(0.1)
    if process.returncode or reason:
        tail = log.read_bytes()[-8192:].decode(errors='replace')
        raise RuntimeError('%s failed (%s); log: %s\n%s' % (label, reason or process.returncode, log, tail))
    return round(time.monotonic() - start, 3)

try:
    require_file(cli, True); require_file(builder, True); require_file(source)
    for path in kernel:
        require_file(path)
    require_file(home / 'src/rc/rc.h')
    # Publishing outputs or opening logs must never overwrite the input, even
    # through a symlink or an existing hard link.
    published = ('app.c', 'Makefile', 'app', 'source.kl', 'source-events.kl')
    destinations = published + tuple('.' + name + '.new' for name in published)
    destinations += ('source-build.json', 'frontend.log', 'build.log')
    for name in destinations:
        destination = output / name
        if source == destination.resolve() or (destination.exists() and os.path.samefile(source, destination)):
            raise RuntimeError('source aliases compiler output: %s' % destination)
    output.parent.mkdir(parents=True, exist_ok=True)
    output.mkdir(exist_ok=True)
    original_hash = digest(source)
    env = dict(os.environ, SHEN_C_HOME=str(home))
    record = {'release_manifest_sha256': digest(release['_path']), 'kernel_version': release['kernel_version'], 'source': str(source), 'source_sha256': original_hash, 'cwd': str(Path.cwd()),
              'kernel_sha256': {str(path.relative_to(home)): digest(path) for path in kernel},
              'compiler_sha256': {'cli': digest(cli), 'builder': digest(builder)},
              'conservative_ownership': args.conservative_ownership, 'acyclic_requested': args.acyclic,
              'source_transport': 'escaped-v1',
              'frontend': 'compiled standard kernel read-file then shen.shen->kl-h; fresh bootstrap context'}
    with tempfile.TemporaryDirectory(prefix='.shen-rc-source-', dir=output.parent) as temp:
        temp = Path(temp); inputs = temp / 'input'; inputs.mkdir()
        (inputs / 'kernel.kl').write_bytes(b'\n'.join(path.read_bytes() for path in kernel))
        (inputs / 'rc.manifest.txt').write_text('kernel=kernel.kl\nuser=user.kl\nsource-events=source-events.kl\nsource-format=escaped-v1\n')
        print('Compiling Shen source with the standard kernel frontend...', flush=True)
        command = [str(cli), 'compile-kl', str(source), str(inputs / 'user.kl'), str(inputs / 'source-events.kl')]
        record['frontend_seconds'] = run('Shen frontend', command, output / 'frontend.log',
                                         float(os.environ.get('RC_SOURCE_COMPILE_TIMEOUT', '300')), env)
        require_file(inputs / 'user.kl'); require_file(inputs / 'source-events.kl')
        print('Emitting and compiling native C...', flush=True)
        command = [str(builder), '--source-metadata']
        if args.conservative_ownership:
            command.append('--conservative-ownership')
        if args.acyclic:
            command.append('--acyclic')
        command += [str(inputs), str(temp / 'app')]
        record['build_seconds'] = run('native compiler', command, output / 'build.log',
                                      float(os.environ.get('RC_SOURCE_BUILD_TIMEOUT', '300')), env)
        if digest(source) != original_hash:
            raise RuntimeError('source changed while compilation was running; no executable published')
        for filename in ('app.c', 'Makefile', 'app'):
            require_file(temp / 'app' / filename)
        record['kl_sha256'] = digest(inputs / 'user.kl')
        record['source_events_sha256'] = digest(inputs / 'source-events.kl')
        record['artifact_sha256'] = digest(temp / 'app/app')
        record['output_sha256'] = {name: digest(temp / 'app' / name) for name in ('app.c', 'Makefile', 'app')}
        record['output_sha256']['source.kl'] = record['kl_sha256']
        record['output_sha256']['source-events.kl'] = record['source_events_sha256']
        record['acyclic_forced_by_makefile'] = 'SHEN_RC_ACYCLIC_ONLY' in (temp / 'app/Makefile').read_text()
        # The executable is published last, after every compilation stage passed.
        shutil.copy2(inputs / 'user.kl', temp / 'source.kl')
        for filename, origin in [('source.kl', temp / 'source.kl'), ('source-events.kl', inputs / 'source-events.kl'), ('app.c', temp / 'app/app.c'),
                                 ('Makefile', temp / 'app/Makefile'), ('app', temp / 'app/app')]:
            staged = output / ('.' + filename + '.new')
            shutil.copy2(origin, staged); os.replace(staged, output / filename)
    (output / 'source-build.json').write_text(json.dumps(record, indent=2) + '\n')
    print('Native executable: %s' % (output / 'app'))
except (OSError, RuntimeError, ValueError) as error:
    print('compile-shen-rc: %s' % error, file=sys.stderr)
    raise SystemExit(1)
PY
