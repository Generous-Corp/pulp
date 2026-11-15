#!/usr/bin/env python3
"""Offline P2 proof: compile assessor AST only; never import/run process fixtures."""
import ast
import copy
import hashlib
import json
import os
from pathlib import Path
import signal
import subprocess

BASE = '2c1e6201feb2d26d980c71457f5865825848511b'
HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[2]
SOURCE = 'test/fixtures/g9_native_surviving_owner/run.py'
ORIGINAL = ROOT / 'docs/evidence/g9-native-surviving-owner-20261110'
CASES = ('healthy', 'host_eof', 'host_kill', 'retry', 'leak', 'leak_restored',
         'census_error', 'anchor_stop', 'anchor_kill')

def sha(data):
    return hashlib.sha256(data).hexdigest()

def encoded(value):
    return json.dumps(value, sort_keys=True, separators=(',', ':')).encode()

def write(path, value):
    path.write_text(json.dumps(value, indent=2, sort_keys=True) + '\n')

def load_assessor(source):
    tree = ast.parse(source)
    functions = [n for n in tree.body if isinstance(n, ast.FunctionDef)
                 and n.name in ('selected', 'one', 'assess')]
    assert {n.name for n in functions} == {'selected', 'one', 'assess'}
    scope = {'os': os, 'signal': signal}
    exec(compile(ast.Module(body=functions, type_ignores=[]), '<assessor-only>', 'exec'), scope)
    return scope['assess']

def context(case):
    directory = ORIGINAL / 'raw' / case
    read = lambda name: json.loads((directory / name).read_text())
    receipt = read('receipt.json')
    return dict(data=read('verdict-events.json'), baseline=read('baseline.json'),
                final=read('pre-hygiene-census.json'), mode=receipt['native_mode'],
                action=receipt['action'], hygiene_at=receipt['hygiene_started_ns'],
                deadline=receipt['deadline_ns'])

def score(assessor, record):
    checks, containment = assessor(**record)
    return dict(passed=all(checks.values()), containment_oracle=containment,
                failed=[k for k, v in checks.items() if not v], assertions=checks)

def mutation(record, name):
    if name.startswith('nonretained_wait'):
        witness = next(e for e in record['data'] if e['event'] == 'retained_wait')
        witness['event'] = 'nonretained_wait'
        if name.endswith('_other_role'):
            witness['role'] = 'W'
        return
    w = next(e for e in record['data'] if e['role'] == 'A' and e['event'] == 'worker_started')
    rows = record['final']['rows']
    worker = next(r for r in rows if r['pid'] == w['a'])
    if name == 'census_missing_worker':
        rows.remove(worker)
    elif name == 'census_live_worker':
        worker['state'] = 'S'
    else:
        descendant = copy.deepcopy(next(r for r in record['baseline']['rows']
                                        if r['pgid'] == w['b'] and r['pid'] != w['a']))
        descendant['state'] = 'Z' if name.endswith('_zombie') else 'S'
        rows.append(descendant)
    # Synthetic copied census, never represented as a new ps observation.
    record['final']['raw'] = '\n'.join(
        f"{r['pid']} {r['ppid']} {r['pgid']} {r['state']} {r['start']} {r['executable']}"
        for r in rows)

def main():
    before = {str(p.relative_to(ROOT)): sha(p.read_bytes())
              for p in ORIGINAL.rglob('*') if p.is_file()}
    assert not subprocess.check_output(['git', 'diff', BASE, '--', str(ORIGINAL)], cwd=ROOT)
    old_source = subprocess.check_output(['git', 'show', BASE + ':' + SOURCE], cwd=ROOT)
    new_source = (ROOT / SOURCE).read_bytes()
    old, new = load_assessor(old_source), load_assessor(new_source)
    write(HERE / 'review-binding.json', {
        'base_commit': BASE,
        'review_session': '390fd0b1-99b1-43a9-b8be-01d6e95e0c2e',
        'review_sha256': sha((HERE / 'independent-review.json').read_bytes()),
        'old_assessor_sha256': sha(old_source), 'new_assessor_sha256': sha(new_source),
        'proof_driver_sha256': sha(Path(__file__).read_bytes()),
        'original_packet_files': before, 'process_fixture_runs': 0,
        'production_custody': 'HOLD'})
    canonical = []
    for case in CASES:
        record = context(case)
        result = score(new, record)
        receipt = json.loads((ORIGINAL / 'raw' / case / 'receipt.json').read_text())
        if case == 'host_kill':
            result['assertions']['actual_host_sigkill_exit'] = receipt['host_returncode'] == -9
            result['passed'] = all(result['assertions'].values())
        assert result['passed'], (case, result['failed'])
        canonical.append(dict(case=case, input_sha256=sha(encoded(record)),
                              source_sha256=sha(new_source), **result))
    write(HERE / 'canonical.json', canonical)
    specifications = [('nonretained_wait', 'host_eof'),
                      ('nonretained_wait_other_role', 'host_eof'),
                      ('census_missing_worker', 'census_error'),
                      ('census_live_worker', 'census_error'),
                      ('census_extra_member_live', 'census_error'),
                      ('census_extra_member_zombie', 'census_error')]
    copies = HERE / 'copies'
    copies.mkdir(exist_ok=True)
    controls = []
    for name, case in specifications:
        original = context(case)
        broken = copy.deepcopy(original)
        mutation(broken, name)
        restored = copy.deepcopy(original)
        results = {label: {'old': score(old, value), 'new': score(new, value)}
                   for label, value in [('positive', original), ('broken', broken), ('restored', restored)]}
        assert results['positive']['old']['passed'] and results['positive']['new']['passed']
        assert results['broken']['old']['passed'] and not results['broken']['new']['passed']
        assert results['restored']['old']['passed'] and results['restored']['new']['passed']
        assert encoded(original) == encoded(restored)
        # Store exactly the changed scorer arguments. Unchanged arguments are
        # bound to the immutable base context, avoiding huge duplicated traces.
        overrides = {k: v for k, v in broken.items() if v != original[k]}
        copy_record = dict(synthetic=True, base_case=case,
                           base_context_sha256=sha(encoded(original)), overrides=overrides,
                           materialized_copy_sha256=sha(encoded(broken)))
        copy_path = copies / (name + '.json')
        copy_path.write_bytes(encoded(copy_record) + b'\n')
        reconstructed = copy.deepcopy(original)
        reconstructed.update(json.loads(copy_path.read_text())['overrides'])
        assert encoded(reconstructed) == encoded(broken)
        controls.append(dict(name=name, case=case, copy_sha256=sha(copy_path.read_bytes()),
                             restored_context_sha256=sha(encoded(restored)), results=results))
    write(HERE / 'controls.json', controls)
    after = {str(p.relative_to(ROOT)): sha(p.read_bytes())
             for p in ORIGINAL.rglob('*') if p.is_file()}
    assert before == after
    write(HERE / 'immutability.json', dict(original_packet_unchanged=True,
                                         files_checked=len(before), file_sha256=after))
    print(json.dumps({'canonical_cases_pass': len(canonical),
                      'old_PASS_new_FAIL_restored_PASS_controls': len(controls),
                      'original_files_unchanged': len(before), 'process_fixture_runs': 0}))

if __name__ == '__main__':
    main()
