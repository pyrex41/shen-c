#!/bin/sh
# Compile the complete bundled KL kernel and load the untouched canonical suite.
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
cd "$root"
builder=${RC_BUILDER:-}
export RC_CERTIFY_ROOT="$root" RC_CERTIFY_BUILDER="$builder"
exec python3 - <<'PY'
import datetime
import hashlib
import json
import os
from pathlib import Path
import signal
import sys
import shlex
import subprocess
import time

root = Path(os.environ['RC_CERTIFY_ROOT'])
sys.path.insert(0, str(root / 'scripts'))
from release_manifest import load_release, kernel_files, digest, prepare_suite, validate_suite
release = load_release(Path(os.environ.get('SHEN_RELEASE_MANIFEST', root / 'shen/releases/shenoskernel-43.1-ec16a9f.json')))
stamp = datetime.datetime.now(datetime.timezone.utc).strftime('%Y%m%dT%H%M%S.%fZ')
evidence = root / 'evidence' / 'rc' / ('certify-' + stamp)
evidence.mkdir(parents=True)
source = root / 'obj' / ('rc-certify-' + stamp)
source.mkdir(parents=True)
output = root / 'bin' / ('rc-certify-' + stamp)
kernel = [root / name for name in kernel_files(release)]
(source / 'kernel.kl').write_bytes(b'\n'.join(path.read_bytes() for path in kernel))
(source / 'user.kl').write_text('(load ' + json.dumps(release['suite_entrypoint']) + ')\n')
(source / 'rc.manifest.txt').write_text('kernel=kernel.kl\nuser=user.kl\n')
# Bind every input compiled by the host/generated application. The standalone
# CLI main is separately built and qualified by test-rc-toolchain.sh.
paths = kernel + sorted(p for p in (root / 'src/rc').glob('*.[ch]') if p.name != 'main.c')
paths += [root / name for name in ('tools/rc-build.c', 'Makefile.rc',
                                  'scripts/certify-rc.sh', 'scripts/test-rc.sh')]
paths += [release['_path'], root / 'scripts/release_manifest.py']
paths += [root / p for p in release['suite_sha256']]
hashes = {str(path.relative_to(root)): digest(path) for path in paths}
revision = subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=root, text=True).strip()
canonical = True  # load_release already checked all pinned bytes and report structure.
external_builder = os.environ['RC_CERTIFY_BUILDER']
builder = Path(external_builder).resolve() if external_builder else source / 'host' / 'bin' / 'rc-build'
record = {'source_sha256': hashes, 'scope': 'complete pinned kernel; pristine ' + release['kernel_version'] + ' suite in fresh writable copy',
          'release_manifest_sha256': digest(release['_path']), 'kernel_version': release['kernel_version'], 'checkout_revision': revision, 'suite_matches_manifest': canonical,
          'builder_path': str(builder.relative_to(root)) if builder.is_relative_to(root) else str(builder),
          'builder_source_correspondence': 'external binary; not established' if external_builder else 'fresh standalone build from recorded sources',
          'host_build': 'EXTERNAL' if external_builder else 'UNKNOWN', 'compiler_audit': 'UNKNOWN',
          'cc': os.environ.get('CC', 'cc'), 'cflags': os.environ.get('CFLAGS', '-O2'),
          'asan_options': os.environ.get('ASAN_OPTIONS'), 'ubsan_options': os.environ.get('UBSAN_OPTIONS'),
          'build': 'UNKNOWN', 'suite': 'UNKNOWN', 'artifact_audit': 'UNKNOWN'}
try:
    version = subprocess.check_output(shlex.split(record['cc']) + ['--version'], stderr=subprocess.STDOUT,
                                      timeout=10, text=True)
    record['compiler_version'] = version[:8192]
except (OSError, subprocess.SubprocessError) as exc:
    record['compiler_version'] = 'UNKNOWN: ' + str(exc)


def save():
    (evidence / 'result.json').write_text(json.dumps(record, indent=2) + '\n')

def run(label, argv, cwd, timeout, stdin=None):
    env = dict(os.environ, SHEN_C_HOME=str(root))
    record[label + '_command'] = argv
    start = time.monotonic()
    with (evidence / (label + '.log')).open('wb') as log:
        process = subprocess.Popen(argv, cwd=cwd, env=env, stdin=stdin,
                                   stdout=log, stderr=subprocess.STDOUT, start_new_session=True)
        reason = None
        while process.poll() is None:
            if time.monotonic() - start > timeout:
                reason = 'TIMEOUT'
            elif log.tell() > 64 * 1024 * 1024:
                reason = 'OUTPUT_LIMIT'
            if reason:
                os.killpg(process.pid, signal.SIGKILL)
                process.wait()
                break
            time.sleep(0.1)
    record[label + '_seconds'] = round(time.monotonic() - start, 3)
    record[label + '_exit'] = process.returncode
    if reason:
        record[label] = reason
        record[label + '_termination'] = reason
    save()
    return process.returncode == 0 and reason is None

save()
print('RC certification evidence:', evidence, flush=True)
if not external_builder:
    host = source / 'host'
    command = shlex.split(os.environ.get('MAKE', 'make')) + ['-f', 'Makefile.rc',
               'RC_BUILD=' + str(host / 'obj'), 'RC_BIN=' + str(host / 'lib'),
               'RC_TOOL_BIN=' + str(host / 'bin'), 'RC_CFLAGS=' + record['cflags'], 'RC_RELEASE_MANIFEST=' + str(release['_path']), str(builder)]
    if not run('host_build', command, root, float(os.environ.get('RC_BUILD_TIMEOUT', '300'))):
        record['host_build'] = 'UNKNOWN' if 'host_build_termination' in record else 'FAIL'; save(); raise SystemExit(1)
    record['host_build'] = 'PASS'
record['builder_sha256'] = hashlib.sha256(builder.read_bytes()).hexdigest()
record['compiler_audit'] = 'PASS' if run('compiler_audit', [str(root / 'scripts/test-rc.sh'), '--audit-only', str(builder)], root, 30) else 'FAIL'
save()
if record['compiler_audit'] != 'PASS':
    raise SystemExit(1)
if not run('build', [str(builder), str(source), str(output)], root,
           float(os.environ.get('RC_BUILD_TIMEOUT', '300'))):
    record['build'] = 'UNKNOWN' if 'build_termination' in record else 'FAIL'; save(); raise SystemExit(1)
record['build'] = 'PASS'
record['generated_sha256'] = {name: hashlib.sha256((output / name).read_bytes()).hexdigest()
                              for name in ('app.c', 'Makefile')}
save()
suite_copy = prepare_suite(release, evidence / 'suite-work')
with (evidence / 'answers.txt').open('w+b') as answers:
    answers.write(b'y\n' * 1024); answers.seek(0)
    ok = run('suite', [str(output / 'app')], suite_copy,
             float(os.environ.get('RC_SUITE_TIMEOUT', '600')), answers)
lines = (evidence / 'suite.log').read_text(errors='replace').splitlines()
passed = [line.strip() for line in lines if line.strip().startswith('passed ... ')]
failed = [line.strip() for line in lines if line.strip().startswith('failed ... ')]
record['last_passed'] = passed[-1] if passed else None
record['last_failed'] = failed[-1] if failed else None
record['suite_validation'] = validate_suite(release, (evidence / 'suite.log').read_text(errors='replace'), suite_copy)
record['stdin_sha256'] = digest(evidence / 'answers.txt')
record['suite'] = ('UNKNOWN' if 'suite_termination' in record else
                   'PASS' if ok and record['suite_validation']['status'] == 'PASS' else 'FAIL')
record['dependencies'] = subprocess.check_output(['otool', '-L', str(output / 'app')] if sys.platform == 'darwin' else ['ldd', str(output / 'app')], text=True)
record['artifact_sha256'] = hashlib.sha256((output / 'app').read_bytes()).hexdigest()
record['artifact_audit'] = 'PASS' if run('audit', [str(root / 'scripts/test-rc.sh'), '--audit-only', str(output / 'app')], root, 30) else 'FAIL'
record['source_unchanged'] = all(hashlib.sha256((root / path).read_bytes()).hexdigest() == digest for path, digest in hashes.items())
save()
print(json.dumps({key: value for key, value in record.items() if key != 'source_sha256'}, indent=2))
raise SystemExit(0 if record['suite'] == record['artifact_audit'] == record['compiler_audit'] == 'PASS' and record['source_unchanged'] else 1)
PY
