#!/bin/sh
# Build a fresh independent compiler/CLI, exercise emitted code, retain a new receipt.
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
export RC_TOOLCHAIN_ROOT="$root"
exec python3 - "$@" <<'PY'
import argparse
import datetime
import hashlib
import json
import os
from pathlib import Path
import re
import shlex
import signal
import subprocess
import sys
import time

root = Path(os.environ['RC_TOOLCHAIN_ROOT']).resolve()
sys.dont_write_bytecode = True
sys.path.insert(0, str(root / 'scripts'))
from rc_native_evidence import native_definition_evidence
from release_manifest import load_release, digest as release_digest
release = load_release(Path(os.environ.get('SHEN_RELEASE_MANIFEST', root / 'shen/releases/s43.1.json')))
parser = argparse.ArgumentParser(prog='scripts/test-rc-toolchain.sh', description='Fresh standalone RC toolchain acceptance; never runs canonical suite itself.')
parser.add_argument('--out', type=Path, help='New evidence directory; must not already exist.')
parser.add_argument('--gc-negative-control', type=Path, default=Path(os.environ['RC_GC_NEGATIVE']) if os.environ.get('RC_GC_NEGATIVE') else None,
                    help='Optional actual GC-linked executable that the auditor must reject.')
gate = parser.add_mutually_exclusive_group(required=True)
gate.add_argument('--skip-canonical', action='store_true', help='Explicit focused scope; canonical result remains UNKNOWN.')
gate.add_argument('--canonical-receipt', type=Path, help='Completed fresh-host canonical receipt from scripts/certify-rc.sh.')
args = parser.parse_args()
stamp = datetime.datetime.now(datetime.timezone.utc).strftime('%Y%m%dT%H%M%S.%fZ')
out = (args.out or root / 'evidence/rc' / ('toolchain-' + stamp)).resolve()
out.mkdir(parents=True, exist_ok=False)
record = {'started_utc': datetime.datetime.now(datetime.timezone.utc).isoformat(),
          'scope': 'fresh standalone compiler and CLI; native ownership optimizer acceptance',
          'status': 'UNKNOWN', 'canonical': 'UNKNOWN', 'evidence': str(out), 'checks': []}
sequence = 0
base_env = dict(os.environ)
for inherited_debug in ('SHEN_RC_PRINT', 'SHEN_RC_STATS'):
    base_env.pop(inherited_debug, None)
# Keep environment values out of receipts except the small explicit compiler surface.
for key in ('CC', 'CFLAGS', 'RC_CFLAGS', 'RC_LDFLAGS', 'ASAN_OPTIONS', 'UBSAN_OPTIONS'):
    if key in base_env:
        record[key.lower()] = base_env[key]
if 'CFLAGS' in base_env and 'RC_CFLAGS' not in base_env:
    base_env['RC_CFLAGS'] = base_env['CFLAGS']
base_env['SHEN_C_HOME'] = str(root)
base_env['PKG_CONFIG'] = '/nonexistent/rc-toolchain-pkg-config'
record['pkg_config'] = base_env['PKG_CONFIG']


def save():
    (out / 'result.json').write_text(json.dumps(record, indent=2) + '\n')


def digest(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def snapshot():
    paths = sorted((root / 'src/rc').glob('*.[ch]'))
    paths += [root / name for name in ('tools/rc-build.c', 'Makefile', 'Makefile.rc', 'scripts/compile-shen-rc.sh', 'scripts/test-rc.sh', 'scripts/test-rc-toolchain.sh', 'scripts/rc_native_evidence.py', 'test/rc/test_native_evidence.py')]
    paths += sorted((root / 'shen/src/kl').glob('*.kl'))
    return {str(p.relative_to(root)): digest(p) for p in paths}


def check(condition, message):
    if not condition:
        raise RuntimeError(message)
    record['checks'].append(message)
    save()


def run(label, argv, *, env=None, cwd=root, data=None, timeout=120, expected=0):
    global sequence
    sequence += 1
    label = '%02d-%s' % (sequence, label)
    stdout_path, stderr_path = out / (label + '.out'), out / (label + '.err')
    started = time.monotonic()
    with stdout_path.open('wb') as stdout, stderr_path.open('wb') as stderr:
        child = subprocess.Popen([str(a) for a in argv], cwd=cwd, env=env or base_env,
                                 stdin=subprocess.PIPE if data is not None else subprocess.DEVNULL,
                                 stdout=stdout, stderr=stderr, start_new_session=True)
        if data is not None:
            child.stdin.write(data)
            child.stdin.close()
        while child.poll() is None:
            if time.monotonic() - started > timeout or stdout.tell() + stderr.tell() > 64 * 1024 * 1024:
                os.killpg(child.pid, signal.SIGKILL)
                child.wait()
                raise RuntimeError('%s exceeded timeout/output budget' % label)
            time.sleep(0.05)
    result = {'label': label, 'command': [str(a) for a in argv], 'exit': child.returncode,
              'seconds': round(time.monotonic() - started, 3),
              'stdout': stdout_path.name, 'stderr': stderr_path.name}
    record.setdefault('commands', []).append(result)
    save()
    if expected is not None and child.returncode != expected:
        raise RuntimeError('%s: exit %s, expected %s; see %s' % (label, child.returncode, expected, stderr_path))
    return child.returncode, stdout_path.read_text(errors='replace'), stderr_path.read_text(errors='replace')


def audit(label, binary, expect_gc=False, synthetic=False):
    code, stdout, stderr = run(label, [root / 'scripts/test-rc.sh', '--audit-only', binary], expected=None, timeout=30)
    if expect_gc:
        check(code != 0 and ('Boehm symbol' in stderr or 'Boehm dependency' in stderr),
              'auditor symbol control rejected (not a runtime collector test)' if synthetic else
              'actual GC-linked executable rejected by artifact audit')
    else:
        check(code == 0 and 'RC artifact audit PASS:' in stdout,
              '%s has no Boehm global symbols or direct linked dependency' % label)


def native_evidence(label, source_path, symbol=None):
    try:
        evidence = native_definition_evidence(source_path.read_text(), symbol)
    except ValueError as error:
        raise RuntimeError('%s native evidence rejected: %s' % (label, error)) from error
    if symbol is not None:
        record.setdefault('native_definition_evidence', {})[symbol] = evidence
    check(True, '%s selected native C body has value operations and no direct source-evaluator forwarding' % label)


def cli_case(name, argv, expected=None, *, fail=False, data=None, contains=None):
    # Empty working directory and no source/home override prove embedded-kernel startup.
    env = dict(base_env)
    env.pop('SHEN_C_HOME', None)
    env.pop('SHEN_RC_HOME', None)
    code, stdout, stderr = run('cli-' + name, [cli] + argv, env=env, cwd=empty, data=data,
                               expected=None if fail else 0, timeout=30)
    if fail:
        check(code != 0 and stderr.strip() != '', 'CLI %s fails observably' % name)
    else:
        check(not stderr.strip(), 'CLI %s has no diagnostic' % name)
        if expected is not None:
            check(stdout.strip() == expected, 'CLI %s exact result' % name)
        if contains is not None:
            check(contains in stdout, 'CLI %s result is present' % name)
    return stdout


save()
try:
    run('native-evidence-controls', [sys.executable, root / 'test/rc/test_native_evidence.py'], timeout=30)
    record['source_sha256'] = snapshot()
    _, revision, _ = run('checkout-revision', ['git', 'rev-parse', 'HEAD'], timeout=10)
    record['checkout_revision'] = revision.strip()
    _, compiler_version, compiler_errors = run('compiler-version', shlex.split(base_env.get('CC', 'cc')) + ['--version'], timeout=10)
    record['compiler_version'] = (compiler_version + compiler_errors)[:8192]
    save()
    scanner_control = out / 'auditor-control.c'
    scanner_control.write_text('__attribute__((used)) volatile int GC_auditor_control = 1; int main(void) { return GC_auditor_control - 1; }\n')
    scanner_binary = out / 'auditor-control'
    run('build-auditor-control', shlex.split(base_env.get('CC', 'cc')) + shlex.split(base_env.get('CFLAGS', '-O2')) +
        [scanner_control, '-o', scanner_binary], timeout=60)
    audit('auditor-negative-control', scanner_binary, expect_gc=True, synthetic=True)
    if args.gc_negative_control:
        check(args.gc_negative_control.is_file(), 'optional actual GC compiler negative control exists')
        record['gc_negative_control_sha256'] = digest(args.gc_negative_control)
        audit('actual-gc-negative-control', args.gc_negative_control, expect_gc=True)
    fresh = out / 'fresh'
    obj, runtime_bin, tools = fresh / 'obj', fresh / 'runtime', fresh / 'bin'
    builder, cli = tools / 'rc-build', tools / 'shen-rc'
    make = shlex.split(base_env.get('MAKE', 'make'))
    settings = ['RC_BUILD=' + str(obj), 'RC_BIN=' + str(runtime_bin),
                'RC_TOOL_BIN=' + str(tools)]
    run('fresh-host-build', make + settings + [str(builder)], timeout=300)
    check(builder.is_file(), 'fresh host produced by recorded build command')
    record['host_build'] = 'PASS'
    record['fresh_builder'] = str(builder)
    record['builder_sha256'] = digest(builder)
    audit('fresh-compiler-audit', builder)
    record['compiler_audit'] = 'PASS'
    # This is deliberately an explicit bounded CLI build, not reuse of bin/shen-rc.
    run('fresh-default-build', make + settings, timeout=600)
    check(cli.is_file(), 'fresh CLI produced from same isolated build graph')
    record['cli_build'] = 'PASS'
    record['default_make_without_pkg_config'] = 'PASS'
    compatibility_cli = tools / 'shen-c'
    check(compatibility_cli.is_file() and digest(compatibility_cli) == digest(cli),
          'default build creates shen-c compatibility executable matching standalone CLI')
    record['cli_sha256'] = digest(cli)
    audit('fresh-cli-audit', cli)
    empty = out / 'empty-cwd'
    empty.mkdir()
    cli_case('version-flag', ['--version'], contains='Shen-C RC')
    cli_case('kernel-version', ['eval', '-e', '(version)'], release['kernel_version'])
    cli_case('arithmetic', ['eval', '-e', '(+ 20 22)'], '42')
    cli_case('strings', ['eval', '-e', '(= (@s "foo" "bar") "foobar")'], 'true')
    # The standard library is part of the image, with its type signatures.
    cli_case('stlib-sort', ['eval', '-e', '(sort (/. X Y (< X Y)) [3 1 2])'], '[1 2 3]')
    cli_case('numeric-hash-equality', ['eval', '-e',
             '(and (= (hash 1 1000000) (hash 1.0 1000000)) '
             '(and (= (hash 0 1000000) (hash -0.0 1000000)) '
             '(= (hash 9007199254740992 1000000) (hash 9007199254740992.0 1000000))))'], 'true')
    cli_case('numeric-property-key', ['eval', '-e',
             '(do (put 1.0 shen-c.hash-test 42 (value *property-vector*)) '
             '(get 1 shen-c.hash-test (value *property-vector*)))'], '42')
    cli_case('string-tail', ['eval', '-e', '(= (tlstr "hello") "ello")'], 'true')
    cli_case('mutable-self-cycle', ['eval', '-e', '(let V (absvector 1) (do (address-> V 0 V) (= V (<-address V 0))))'], 'true')
    cli_case('trap', ['eval', '-e', '(trap-error (simple-error "boom") (lambda E (= (error-to-string E) "boom")))'], 'true')
    cli_case('quiet', ['eval', '-q', '-e', '(+ 20 22)'], '')
    cli_case('multiple-forms', ['eval', '-e', '(+ 1 1) (+ 20 22)'], '2\n42')
    cli_case('eof', ['repl'], data=b'')
    cli_case('list-without-final-newline', ['repl'], data=b'[314159 271828]', contains='[314159 271828]')
    cli_case('expression-without-final-newline', ['repl'], data=b'(+ 314000 159)', contains='314159')
    cli_case('missing-eval-option-argument', ['eval', '-e'], fail=True)
    cli_case('unhandled-error', ['eval', '-e', '(simple-error "toolchain-unhandled")'], fail=True)
    cli_case('missing-file', ['script', str(empty / 'missing.shen')], fail=True)
    stream_path = empty / 'written.txt'
    stream_expr = '(let S (open %s out) (do (pr "toolchain-stream" S) (close S)))' % json.dumps(str(stream_path))
    cli_case('stream-write', ['eval', '-q', '-e', stream_expr], '')
    check(stream_path.read_text() == 'toolchain-stream', 'CLI stream contains exact written bytes')
    script = empty / 'script with spaces.shen'
    script.write_text('(output "CLI-SCRIPT-ARG=~A~%" (hd (tl (value *argv*))))\n')
    cli_case('script-argv', ['script', str(script), 'argument-marker'], contains='CLI-SCRIPT-ARG=argument-marker')
    quiet_script = empty / 'script-result-quiet.shen'
    quiet_script.write_text('(output "CLI-SCRIPT-OUTPUT~%" )\n')
    quiet_output = cli_case('script-result-quiet', ['script', str(quiet_script)], contains='CLI-SCRIPT-OUTPUT')
    check(quiet_output.count('CLI-SCRIPT-OUTPUT') == 1 and '"CLI-SCRIPT-OUTPUT' not in quiet_output
          and 'run time' not in quiet_output and 'loaded' not in quiet_output,
          'CLI script does not echo its final string result, timing or load result')
    redefine_script = empty / 'script-redefinition.shen'
    redefine_script.write_text('(define shen.shen->kl X -> 42)\n(output "SHOULD-NOT-RUN~%")\n')
    redefine_output = cli_case('script-redefinition', ['script', str(redefine_script)])
    check('SHOULD-NOT-RUN' not in redefine_output,
          'CLI script resolves shen.shen->kl after a preceding redefinition')
    nested = empty / 'nested.shen'
    nested.write_text('(output "NESTED~%")\n')
    outer = empty / 'outer.shen'
    outer.write_text('(load "nested.shen")\n(output "OUTER~%")\n')
    nested_output = cli_case('script-nested-load', ['script', str(outer)], contains='NESTED')
    # As in the standard launcher, only the script's own forms are quiet; a
    # load it calls prints its results and timing like any other load.
    check(nested_output.count('NESTED') == 2 and '"NESTED' in nested_output
          and nested_output.count('OUTER') == 1 and '"OUTER' not in nested_output
          and nested_output.count('run time') == 1,
          'CLI script is quiet while its nested load prints as an ordinary load')
    # Deep non-tail recursion runs on the large evaluator stack; running out
    # of it is an ordinary trappable error rather than a crash.
    deep_script = empty / 'script-deep-recursion.shen'
    deep_script.write_text('(define deep 0 -> 0 N -> (+ 1 (deep (- N 1))))\n(output "DEEP=~A~%" (deep 300000))\n')
    cli_case('script-deep-recursion', ['script', str(deep_script)], contains='DEEP=300000')
    runaway_script = empty / 'script-stack-exhausted.shen'
    runaway_script.write_text('(define runaway X -> (+ 1 (runaway X)))\n'
                              '(output "CAUGHT=~A~%" (trap-error (runaway 1) (/. E (error-to-string E))))\n')
    cli_case('script-stack-exhausted', ['script', str(runaway_script)], contains='CAUGHT=stack exhausted')
    error_script = empty / 'script-error.shen'
    error_script.write_text('(simple-error "script-error")\n')
    cli_case('script-error', ['script', str(error_script)], fail=True)
    hash_script = empty / 'hash-formatting.shen'
    hash_script.write_text('(define shen-c.hash-guard-control -> (output "HASH-GUARD=~A~%" (shen.hashkey 1.0)))\n'
                           '(eval-kl [defun explode [x] [if [= [str x] "1.0"] [cons "A" []] [cons "B" []]]])\n'
                           '(shen-c.hash-guard-control)\n')
    cli_case('hash-redefined-explode', ['script', str(hash_script)], contains='HASH-GUARD=65')
    # A same-named custom kernel body must keep its own semantics in both ABIs.
    for conservative in (False, True):
        mode = 'borrowed' if conservative else 'owned'
        case = out / ('custom-hash-' + mode)
        source = case / 'input'
        source.mkdir(parents=True)
        (source / 'kernel.kl').write_text('(defun shen.hashkey (x) 99)\n')
        (source / 'user.kl').write_text('(shen.hashkey 1.0)\n')
        command = [builder] + (['--conservative-ownership'] if conservative else []) + [source, case / 'app']
        run('custom-hash-' + mode + '-build', command, timeout=120)
        _, stdout, _ = run('custom-hash-' + mode + '-run', [case / 'app/app'],
                           env=dict(base_env, SHEN_RC_PRINT='1'), timeout=30)
        check(stdout.strip() == '99', 'custom kernel hash body preserved in ' + mode + ' emitter')
    stlib_typed = empty / 'stlib-typed.shen'
    stlib_typed.write_text('(sort (/. X Y (< X Y)) [3 1 2])\n')
    cli_case('stlib-typed', ['eval', '-e', '(tc +)', '-l', str(stlib_typed)], contains='[1 2 3] : (list number)')
    loaded = empty / 'loaded.shen'
    loaded.write_text('(set *toolchain-loaded* 42)\n')
    # Load may print its own normal progress; the subsequent expression is decisive.
    cli_case('load-file', ['eval', '-q', '-l', str(loaded), '-e', '(if (= (value *toolchain-loaded*) 42) true (simple-error "load failed"))'])
    record['cli_smoke'] = 'PASS'
    shen_source = out / 'pattern-sum.shen'
    shen_source.write_text('(define toolchain-sum [] -> 0 [X | Xs] -> (+ X (toolchain-sum Xs)))\n'
                           '(define toolchain-invoke F X -> ((function F) X))\n'
                           '(define toolchain-add X -> (+ X 2))\n'
                           '(toolchain-sum [])\n(toolchain-sum [7])\n(toolchain-sum [3 4 5])\n'
                           '(toolchain-sum [20 22])\n(toolchain-invoke toolchain-add 40)\n')
    shen_app = out / 'native-shen'
    frontend_env = dict(base_env, SHEN_RC_CLI=str(cli), RC_BUILDER=str(builder))
    run('shen-to-native-build', [root / 'scripts/compile-shen-rc.sh', shen_source, shen_app], env=frontend_env, timeout=650)
    check((shen_app / 'source.kl').is_file(), 'real Shen compilation retains generated KL')
    native_evidence('Shen pattern-matching source application', shen_app / 'app.c', 'toolchain-sum')
    native_evidence('Shen dynamic function source application', shen_app / 'app.c', 'toolchain-invoke')
    audit('shen-source-app-audit', shen_app / 'app')
    _, shen_output, shen_stderr = run('shen-to-native-run', [shen_app / 'app'], env=dict(base_env, SHEN_RC_PRINT='1'), timeout=30)
    check(not shen_stderr.strip(), 'Shen source application has no runtime diagnostics')
    check(shen_output.strip() == '0\n7\n12\n42\n42',
          'real Shen pattern cases and dynamic function lookup produce exact results 0/7/12/42/42')
    frontend_receipt = json.loads((shen_app / 'source-build.json').read_text())
    check(frontend_receipt.get('compiler_sha256') == {'cli': digest(cli), 'builder': digest(builder)},
          'Shen frontend receipt identifies the fresh CLI and compiler binaries')
    check(frontend_receipt.get('source_sha256') == digest(shen_source) and
          frontend_receipt.get('kl_sha256') == digest(shen_app / 'source.kl') and
          frontend_receipt.get('artifact_sha256') == digest(shen_app / 'app'),
          'Shen frontend receipt matches actual source, KL, and executable bytes')
    check(frontend_receipt.get('source_transport') == 'escaped-v1' and
          frontend_receipt.get('source_events_sha256') == digest(shen_app / 'source-events.kl'),
          'Shen frontend receipt identifies escaped-v1 transport and actual reader event bytes')
    output_hashes = frontend_receipt.get('output_sha256', {})
    required_outputs = {'app.c', 'Makefile', 'app', 'source.kl', 'source-events.kl'}
    check(isinstance(output_hashes, dict) and required_outputs.issubset(output_hashes),
          'Shen frontend receipt binds every required compiler output')
    for filename, expected_hash in output_hashes.items():
        check(isinstance(filename, str) and Path(filename).name == filename and filename not in ('.', '..'),
              'Shen receipt output uses a local artifact filename: ' + str(filename))
        artifact = shen_app / filename
        check(artifact.is_file() and digest(artifact) == expected_hash,
              'Shen receipt output hash matches: ' + filename)
    record['shen_pipeline'] = {'status': 'PASS',
                               'native_scope': 'registered toolchain-sum and toolchain-invoke callback bodies',
                               'input_sha256': digest(shen_source),
                               'kl_sha256': digest(shen_app / 'source.kl'),
                               'events_sha256': digest(shen_app / 'source-events.kl'),
                               'c_sha256': digest(shen_app / 'app.c'), 'artifact_sha256': digest(shen_app / 'app'),
                               'frontend_receipt': frontend_receipt}
    save()
    # These are emitted KL programs, not hand-written C models of the pass.
    list_functions = """(defun make-list (N) (if (= N 0) () (cons N (make-list (- N 1)))))
(defun rebuild (Xs) (if (cons? Xs) (cons (+ 1 (hd Xs)) (rebuild (tl Xs))) Xs))
(defun sum-list (Xs) (if (cons? Xs) (+ (hd Xs) (sum-list (tl Xs))) 0))
(defun list-length (Xs N) (if (cons? Xs) (list-length (tl Xs) (+ N 1)) N))
(defun verify-list (Xs N Offset) (if (= N 0) (= Xs ()) (and (cons? Xs) (and (= (hd Xs) (+ N Offset)) (verify-list (tl Xs) (- N 1) Offset)))))
"""
    programs = {}
    for size in (100, 1000):
        expected_sum = size * (size + 1) // 2 + size
        programs['unique-%d' % size] = (list_functions +
            '(let Ys (rebuild (make-list %d)) (and (verify-list Ys %d 1) (and (= (list-length Ys 0) %d) (= (sum-list Ys) %d))))\n'
            % (size, size, size, expected_sum), 'true')
    programs['aliased-1000'] = (list_functions +
        '(let Xs (make-list 1000) (let Ys (rebuild Xs) (and (verify-list Xs 1000 0) (verify-list Ys 1000 1))))\n', 'true')
    programs['capture'] = (list_functions +
        '(defun make-capture (Used Huge) (freeze Used))\n'
        '(let F (make-capture 42 (make-list 1000)) (F))\n', '42')
    programs['last-use'] = (list_functions +
        '(defun consume (Xs) (let H (hd Xs) (let New (make-list 1000) (+ H (hd New)))))\n'
        '(consume (make-list 1000))\n', '2000')
    measurements = {}
    for name, (program, expected_output) in programs.items():
        measurements[name] = {}
        for optimized in (False, True):
            mode = 'optimized' if optimized else 'conservative'
            case = out / ('native-' + name + '-' + mode)
            source = case / 'input'
            source.mkdir(parents=True)
            (source / 'kernel.kl').write_text('')
            (source / 'user.kl').write_text(program)
            command = [builder] + ([] if optimized else ['--conservative-ownership']) + [source, case / 'app']
            run(name + '-' + mode + '-build', command, timeout=120)
            app = case / 'app/app'
            native_evidence('%s %s' % (name, mode), case / 'app/app.c')
            audit(name + '-' + mode + '-audit', app)
            env = dict(base_env, SHEN_RC_PRINT='1', SHEN_RC_STATS='1')
            _, stdout, stderr = run(name + '-' + mode + '-run', [app], env=env, timeout=60)
            check(stdout.strip() == expected_output, '%s %s exact functional result' % (name, mode))
            check(not any(line.strip() and not line.startswith('RC_STATS ') for line in stderr.splitlines()),
                  '%s %s has no runtime diagnostic beyond stats' % (name, mode))
            metrics = [json.loads(line[len('RC_STATS '):]) for line in stderr.splitlines() if line.startswith('RC_STATS ')]
            check(len(metrics) == 1, '%s %s emits one real-runtime stats record' % (name, mode))
            required = ('pair_allocations', 'pair_reuse_attempts', 'pair_reuses', 'pair_reuse_misses',
                        'capture_edges_created', 'peak_objects', 'retain_calls', 'release_calls')
            check(all(isinstance(metrics[0].get(field), int) for field in required),
                  '%s %s stats contain required integer counters' % (name, mode))
            measurements[name][mode] = dict(metrics[0], artifact_sha256=digest(app), source_sha256=digest(case / 'app/app.c'))
            record['native_measurements'] = measurements
            save()
    for size in (100, 1000):
        result = measurements['unique-%d' % size]
        opt, baseline = result['optimized'], result['conservative']
        check(baseline['pair_reuses'] == 0, 'conservative unique-%d is an actual reuse-off control' % size)
        check(opt['pair_reuses'] >= size, 'unique-%d reuses at least one real pair per element' % size)
        check(opt['pair_allocations'] <= baseline['pair_allocations'] - size,
              'unique-%d avoids at least one allocation per element' % size)
    alias = measurements['aliased-1000']['optimized']
    check(alias['pair_reuses'] == 0, 'retained alias prevents destructive pair reuse')
    check(alias['pair_reuse_misses'] > 0, 'aliased program exercises checked reuse fallback')
    capture = measurements['capture']
    check(capture['optimized']['capture_edges_created'] < capture['conservative']['capture_edges_created'],
          'free-variable analysis creates fewer actual closure capture edges')
    liveness = measurements['last-use']
    check(liveness['optimized']['peak_objects'] + 1000 <= liveness['conservative']['peak_objects'],
          'last-use release removes at least 1000 objects from overlapping list lifetimes')
    record['native_measurements'] = measurements
    record['optimizer'] = 'PASS'
    if args.canonical_receipt:
        canonical = json.loads(args.canonical_receipt.read_text())
        for field in ('host_build', 'compiler_audit', 'build', 'suite', 'artifact_audit'):
            check(canonical.get(field) == 'PASS', 'canonical %s gate passed' % field)
        check(all(canonical.get(stage + '_exit') == 0 for stage in ('host_build', 'compiler_audit', 'build', 'suite', 'audit')),
              'canonical host, compiler audit, app build, suite, and app audit all exited zero')
        check(canonical.get('suite_exit') == 0 and canonical.get('suite_matches_manifest') is True and
              canonical.get('source_unchanged') is True and canonical.get('release_manifest_sha256') == release_digest(release['_path']) and canonical.get('suite_validation', {}).get('status') == 'PASS' and canonical.get('last_passed') == 'passed ... ' + str(release['expected_tests']) and
              canonical.get('last_failed') == 'failed ... 0', 'canonical pinned suite completed all assertions at exit zero')
        canonical_sources = canonical.get('source_sha256', {})
        for path, expected_hash in canonical_sources.items():
            check((root / path).is_file() and digest(root / path) == expected_hash,
                  'canonical source still matches: ' + path)
        # Canonical execution compiles the host and generated main, not CLI main.c.
        # This runner separately builds and hashes the complete standalone CLI.
        core = [str(p.relative_to(root)) for p in (root / 'src/rc').glob('*.[ch]') if p.name != 'main.c']
        core += ['tools/rc-build.c', 'Makefile.rc']
        check(all(path in canonical_sources for path in core), 'canonical receipt binds all current compiler/runtime core sources')
        canonical_host = Path(canonical['builder_path'])
        if not canonical_host.is_absolute():
            canonical_host = root / canonical_host
        check(digest(canonical_host) == canonical['builder_sha256'], 'canonical fresh host binary still matches receipt')
        audit('canonical-host-audit', canonical_host)
        build_command = canonical.get('build_command')
        check(isinstance(build_command, list) and len(build_command) == 3 and
              all(isinstance(value, str) and value for value in build_command),
              'canonical build command identifies its retained output directory')
        check((root / build_command[0]).resolve() == canonical_host.resolve(),
              'canonical build command uses the retained audited compiler host')
        canonical_output = Path(build_command[2])
        if not canonical_output.is_absolute():
            canonical_output = root / canonical_output
        canonical_output = canonical_output.resolve()
        canonical_app = canonical_output / 'app'
        suite_command = canonical.get('suite_command')
        check(isinstance(suite_command, list) and len(suite_command) == 1 and
              isinstance(suite_command[0], str) and
              (root / suite_command[0]).resolve() == canonical_app,
              'canonical suite command executes the retained build output app')
        def canonical_hash(path, expected, label):
            check(isinstance(expected, str) and re.fullmatch(r'[0-9a-f]{64}', expected) is not None and
                  path.is_file() and digest(path) == expected,
                  'canonical retained artifact matches receipt: ' + label)
        canonical_hash(canonical_app, canonical.get('artifact_sha256'), 'app')
        generated = canonical.get('generated_sha256')
        check(isinstance(generated, dict) and {'app.c', 'Makefile'}.issubset(generated),
              'canonical receipt binds generated app.c and Makefile')
        for filename, expected_hash in generated.items():
            check(isinstance(filename, str) and Path(filename).name == filename and
                  filename not in ('.', '..'),
                  'canonical generated artifact uses a local filename: ' + str(filename))
            canonical_hash(canonical_output / filename, expected_hash, filename)
        audit('canonical-app-audit', canonical_app)
        record['canonical'] = 'PASS'
        record['canonical_receipt'] = str(args.canonical_receipt.resolve())
        record['canonical_receipt_sha256'] = digest(args.canonical_receipt)
    else:
        record['canonical'] = 'UNKNOWN (explicit --skip-canonical scope)'
    record['source_unchanged'] = record['source_sha256'] == snapshot()
    check(record['source_unchanged'], 'compiler/runtime/kernel sources unchanged throughout fresh acceptance run')
    record['status'] = 'PASS'
    record['ended_utc'] = datetime.datetime.now(datetime.timezone.utc).isoformat()
    save()
    print('RC toolchain PASS: fresh audited compiler/CLI, CLI smoke, native optimizer controls')
    print('Canonical:', record['canonical'])
    print('Evidence:', out)
except Exception as error:
    record['status'] = 'FAIL'
    record['reason'] = str(error)
    try:
        record['source_unchanged'] = record.get('source_sha256') == snapshot()
    except OSError:
        record['source_unchanged'] = False
    save()
    print('RC toolchain FAIL:', error, file=sys.stderr)
    print('Evidence:', out, file=sys.stderr)
    raise SystemExit(1)
PY
