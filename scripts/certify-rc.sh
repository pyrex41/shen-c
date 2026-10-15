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
import shlex
import subprocess
import time

root = Path(os.environ['RC_CERTIFY_ROOT'])
stamp = datetime.datetime.now(datetime.timezone.utc).strftime('%Y%m%dT%H%M%S.%fZ')
evidence = root / 'evidence' / 'rc' / ('certify-' + stamp)
evidence.mkdir(parents=True)
source = root / 'obj' / ('rc-certify-' + stamp)
source.mkdir(parents=True)
output = root / 'bin' / ('rc-certify-' + stamp)
modules = 'sys writer core reader declarations toplevel macros load prolog sequent track t-star yacc types'.split()
kernel = [root / 'shen/src/kl' / (name + '.kl') for name in modules]
(source / 'kernel.kl').write_bytes(b'\n'.join(path.read_bytes() for path in kernel))
(source / 'user.kl').write_text('(load "runme.shen")\n')
(source / 'rc.manifest.txt').write_text('kernel=kernel.kl\nuser=user.kl\n')
# Bind every input compiled by the host/generated application. The standalone
# CLI main is separately built and qualified by test-rc-toolchain.sh.
paths = kernel + sorted(p for p in (root / 'src/rc').glob('*.[ch]') if p.name != 'main.c')
paths += [root / name for name in ('tools/rc-build.c', 'Makefile.rc',
                                  'scripts/certify-rc.sh', 'scripts/test-rc.sh')]
tracked = subprocess.check_output(['git', 'ls-files', '-z', 'shen/test/s42'], cwd=root)
paths += [root / name for name in tracked.decode().split('\0') if name]
hashes = {str(path.relative_to(root)): hashlib.sha256(path.read_bytes()).hexdigest() for path in paths}
revision = subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=root, text=True).strip()
canonical = subprocess.run(['git', 'diff', '--quiet', revision, '--', 'shen/test/s42'], cwd=root).returncode == 0
external_builder = os.environ['RC_CERTIFY_BUILDER']
builder = Path(external_builder).resolve() if external_builder else source / 'host' / 'bin' / 'rc-build'
record = {'source_sha256': hashes, 'scope': 'complete bundled kernel; unmodified S42 runme.shen',
          'checkout_revision': revision, 'suite_matches_revision': canonical,
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
if not canonical:
    record['suite'] = 'FAIL'; record['reason'] = 'S42 suite differs from checkout revision'; save(); raise SystemExit(1)
if not external_builder:
    host = source / 'host'
    command = shlex.split(os.environ.get('MAKE', 'make')) + ['-f', 'Makefile.rc',
               'RC_BUILD=' + str(host / 'obj'), 'RC_BIN=' + str(host / 'lib'),
               'RC_TOOL_BIN=' + str(host / 'bin'), 'RC_CFLAGS=' + record['cflags'], str(builder)]
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
with (evidence / 'answers.txt').open('w+b') as answers:
    answers.write(b'y\n' * 1024); answers.seek(0)
    ok = run('suite', [str(output / 'app')], root / 'shen/test/s42',
             float(os.environ.get('RC_SUITE_TIMEOUT', '600')), answers)
lines = (evidence / 'suite.log').read_text(errors='replace').splitlines()
passed = [line.strip() for line in lines if line.strip().startswith('passed ... ')]
failed = [line.strip() for line in lines if line.strip().startswith('failed ... ')]
record['last_passed'] = passed[-1] if passed else None
record['last_failed'] = failed[-1] if failed else None
record['suite'] = ('UNKNOWN' if 'suite_termination' in record else
                   'PASS' if ok and record['last_passed'] == 'passed ... 134' and record['last_failed'] == 'failed ... 0' else 'FAIL')
record['artifact_sha256'] = hashlib.sha256((output / 'app').read_bytes()).hexdigest()
record['artifact_audit'] = 'PASS' if run('audit', [str(root / 'scripts/test-rc.sh'), '--audit-only', str(output / 'app')], root, 30) else 'FAIL'
record['source_unchanged'] = all(hashlib.sha256((root / path).read_bytes()).hexdigest() == digest for path, digest in hashes.items())
save()
print(json.dumps({key: value for key, value in record.items() if key != 'source_sha256'}, indent=2))
raise SystemExit(0 if record['suite'] == record['artifact_audit'] == record['compiler_audit'] == 'PASS' and record['source_unchanged'] else 1)
PY
