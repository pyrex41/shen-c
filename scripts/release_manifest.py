#!/usr/bin/env python3
"""Pinned release inputs and fail-closed canonical-suite validation."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import shutil

ROOT = Path(__file__).resolve().parents[1]
DEFAULT = ROOT / 'shen/releases/s43.1.json'

def digest(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()

def inventory(directory):
    return {str(p.relative_to(directory)): digest(p) for p in sorted(directory.rglob('*')) if p.is_file()}

def forms(text):
    # Read only structure, without evaluating suite code. Preserve string tokens,
    # remove line/block comments, and distinguish vectors from applications.
    tokens = re.findall(r'\\\*.*?\*\\|\\\\[^\n]*|"[^"]*"|[()\[\]]|[^\s()\[\]"]+', text, re.S)
    stack, result = [], []
    for token in tokens:
        if token.startswith(('\\*', '\\\\')):
            continue
        if token in ('(', '['):
            stack.append((token, []))
        elif token in (')', ']'):
            if not stack or stack[-1][0] != {')':'(', ']':'['}[token]:
                raise ValueError('unbalanced suite source')
            opening, items = stack.pop()
            value = items if opening == '(' else ['#vector'] + items
            (stack[-1][1] if stack else result).append(value)
        else:
            (stack[-1][1] if stack else result).append(token)
    if stack:
        raise ValueError('incomplete suite source')
    return result

def reports(path):
    result = []
    for form in forms(path.read_text()):
        if isinstance(form, list) and form and form[0] == 'report':
            if len(form) < 4 or (len(form) - 2) % 2:
                raise ValueError('malformed report: ' + str(form[:2]))
            result.append({'label': form[1][1:-1], 'assertions': (len(form)-2)//2})
    if not result:
        raise ValueError('missing suite reports')
    return result

def load_release(path=DEFAULT, root=ROOT):
    path = Path(path).resolve()
    data = json.loads(path.read_text())
    for key in ('kernel_sha256', 'suite_sha256', 'source_sha256'):
        for name, expected in data[key].items():
            if digest(root / name) != expected:
                raise ValueError('release hash mismatch: ' + name)
    suite = root / data['suite_directory']
    actual = {str((suite / p).relative_to(root)): h for p,h in inventory(suite).items()}
    if actual != data['suite_sha256']:
        raise ValueError('suite inventory changed')
    groups = reports(suite / data['suite_test_source'])
    if groups != data['reports'] or sum(x['assertions'] for x in groups) != data['expected_tests']:
        raise ValueError('suite structure/count differs from release manifest')
    data['_path'] = path
    return data

def prepare_suite(release, destination, root=ROOT):
    shutil.copytree(root / release['suite_directory'], destination)
    # Upstream ships a previous output with the expected bytes. Remove it so
    # observing the expected file proves THIS execution actually wrote it.
    for name in release['expected_outputs']:
        (destination / name).unlink(missing_ok=True)
    return destination

def validate_suite(release, text, directory):
    passed = [int(x) for x in re.findall(r'^\s*passed \.\.\. (\d+)\s*$', text, re.M)]
    failed = [int(x) for x in re.findall(r'^\s*failed \.\.\. (\d+)\s*$', text, re.M)]
    expected, count = [], 0
    for group in release['reports']:
        count += group['assertions']; expected.append(count)
    errors = []
    if passed != expected: errors.append('missing, duplicate or wrong test denominator/report sequence')
    if failed != [0] * len(expected): errors.append('missing report or intermediate failure')
    if re.search(r'failed with error|failed; continue\?|Sanitizer|runtime error:', text):
        errors.append('suite or sanitizer diagnostic')
    suite_inputs = {str(Path(p).relative_to(release['suite_directory'])): h for p,h in release['suite_sha256'].items()}
    observed = inventory(directory)
    outputs = release['expected_outputs']
    for name,h in suite_inputs.items():
        if name not in outputs and observed.get(name) != h: errors.append('changed/missing input: '+name)
    for name, spec in outputs.items():
        if observed.get(name) != spec['sha256']: errors.append('missing/wrong output: '+name)
    if set(observed) != set(suite_inputs) | set(outputs): errors.append('unexpected suite files')
    return {'status': 'FAIL' if errors else 'PASS', 'errors': errors,
            'passed_reports': passed, 'failed_reports': failed,
            'output_sha256': {p: observed.get(p) for p in outputs}}

def header(release):
    return ('/* Generated from the checked release manifest. */\n'
            '#define SHEN_KERNEL_VERSION '+json.dumps(release['kernel_version'])+'\n'
            '#define SHEN_KERNEL_MODULES {'+','.join(json.dumps(m) for m in release['modules'])+'}\n')

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--manifest',type=Path,default=DEFAULT)
    parser.add_argument('--kernel-inputs',action='store_true')
    parser.add_argument('--header',type=Path)
    args=parser.parse_args(); release=load_release(args.manifest)
    if args.header:
        args.header.parent.mkdir(parents=True,exist_ok=True);args.header.write_text(header(release))
    elif args.kernel_inputs:
        print(' '.join(release['kernel_directory']+'/'+m+'.kl' for m in release['modules']))
    else:
        print(json.dumps({'version':release['kernel_version'],'tests':release['expected_tests'],'reports':release['reports']},indent=2))

if __name__=='__main__': main()
