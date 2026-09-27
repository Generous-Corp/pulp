#!/usr/bin/env python3
"""Verify the bounded same-device screening receipt without promoting it to P4."""
import csv
import json
import math
from pathlib import Path
import sys


def verify(directory, log):
    summaries = []
    headers = []
    for line in log.splitlines():
        fields = dict(word.split('=', 1) for word in line.split() if '=' in word)
        if 'schema' in fields:
            headers.append(fields)
        if 'trial' in fields:
            summaries.append(fields)
    assert len(headers) == 1 and headers[0]['schema'] == 'same-device-storage-screening-v1'
    owner = int(headers[0]['owner'])
    assert owner > 0 and len(summaries) == 6
    assert 'paired_screening=passed performance_verdict=unassigned' in log
    outcomes = []
    for ordinal, summary in enumerate(summaries):
        mode = 'staged' if (ordinal % 2) ^ ((ordinal // 2) % 2) else 'shared'
        assert int(summary['trial']) == ordinal and summary['mode'] == mode
        assert int(summary['owner']) == owner
        with (directory / f'{ordinal}-{mode}.csv').open() as stream:
            rows = list(csv.DictReader(stream))
        assert len(rows) == 784
        start = int(rows[0]['scheduled_ns'])
        for sequence, row in enumerate(rows):
            assert int(row['sequence']) == sequence
            assert int(row['measured']) == int(32 <= sequence < 782)
            assert int(row['scheduled_ns']) == start + sequence * 128 * 1_000_000_000 // 48000
            assert int(row['deadline_ns']) == start + (sequence + 1) * 128 * 1_000_000_000 // 48000
            assert int(row['end_ns']) >= int(row['start_ns'])
            assert int(row['delivery']) in range(5) and int(row['accepted']) in (0, 1)
            assert math.isfinite(float(row['max_error'])) and float(row['max_error']) < 1e-4
        measured = rows[32:782]
        gpu = sum(int(r['delivery']) == 0 for r in measured)
        missed = sum(int(r['end_ns']) > int(r['deadline_ns']) for r in measured)
        rejected = sum(int(r['accepted']) == 0 for r in measured)
        assert int(summary['gpu']) == gpu and int(summary['fallback_reference']) == 750 - gpu
        assert int(summary['callback_deadline_misses']) == missed and int(summary['rejected']) == rejected
        assert gpu > 0 and int(summary['failed']) == 0
        transfers = [int(summary[k]) for k in ('writes', 'copies', 'maps')]
        assert len(set(transfers)) == 1
        assert transfers[0] > 0 if mode == 'staged' else transfers[0] == 0
        cpu = float(summary['process_cpu_seconds'])
        assert math.isfinite(cpu) and cpu >= 0
        outcomes.append({'trial': ordinal, 'mode': mode, 'owner': owner,
                         'gpu': gpu, 'reference_fallback': 750-gpu, 'rejected': rejected,
                         'callback_deadline_misses': missed, 'process_cpu_seconds': cpu})
    return {'passed': True, 'performance_verdict': 'unassigned', 'trials': outcomes,
            'scope': 'ordinary-thread source-linked storage screening; not RT, CPU-shadow, or installed-SDK proof'}


if __name__ == '__main__':
    print(json.dumps(verify(Path(sys.argv[1]), Path(sys.argv[2]).read_text()), indent=2))
