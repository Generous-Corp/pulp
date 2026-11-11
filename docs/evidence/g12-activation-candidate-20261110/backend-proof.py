#!/usr/bin/env python3
"""Rebuild a planted wrong dispatch, then restore and verify the ordinary CI test."""
import hashlib
import json
from pathlib import Path
import subprocess

root = Path(__file__).resolve().parents[3]
out = Path(__file__).resolve().parent
source = root / 'test/fixtures/g5_graph_event_rt_cases.inc'
binary = root / 'build/test/pulp-test-group-trigger-graph'
saved = source.read_bytes()
results = []

def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()

def run(label, build=False):
    if build:
        command = ['tools/ci/governed-build.sh', 'cmake', '--build', 'build',
                   '--target', 'pulp-test-group-trigger-graph', '--verbose']
        with (out / (label + '-build.log')).open('w', encoding='utf-8') as stream:
            result = subprocess.run(command, cwd=root, stdout=stream, stderr=subprocess.STDOUT, timeout=600)
        assert result.returncode == 0
    command = [str(binary), 'G12 prepared gain activation has one exact audio transition under paced callbacks']
    with (out / (label + '.log')).open('w', encoding='utf-8') as stream:
        result = subprocess.run(command, cwd=root, stdout=stream, stderr=subprocess.STDOUT, timeout=30)
    results.append(dict(label=label, command=command, exit=result.returncode,
                        source_sha256=digest(source), binary_sha256=digest(binary),
                        binary_bytes=binary.stat().st_size))
    return result.returncode

assert run('backend-positive') == 0
needle = b'const auto observed = run(routed);'
assert saved.count(needle) == 1
broken = saved.replace(needle, b'const auto observed = run(false);')
try:
    source.write_bytes(broken)
    patch = subprocess.run(['git', 'diff', '--', str(source)], cwd=root, capture_output=True, text=True, check=True)
    (out / 'wrong-backend.patch').write_text(patch.stdout, encoding='utf-8')
    assert run('backend-broken', build=True) != 0
    text = (out / 'backend-broken.log').read_text(encoding="utf-8")
    assert 'blocks_processed' in text and '0 == 128' in text
finally:
    source.write_bytes(saved)
    restored = run('backend-restored', build=True)
    (out / 'backend-results.json').write_text(json.dumps(results, indent=2) + '\n', encoding='utf-8')
assert restored == 0
assert source.read_bytes() == saved
print('backend_control=pass positive=0 broken=assertion_failure restored=0')
