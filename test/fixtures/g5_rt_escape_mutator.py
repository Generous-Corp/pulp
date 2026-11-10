#!/usr/bin/env python3
"""Plant one real callback escape for confirm_failure.sh; never run outside it."""
from pathlib import Path
import sys

needle = '    // RT_ESCAPE_INJECTION: confirm_failure plants an actual callback escape here.'
plants = {
    'allocation': '    auto* escaped = new volatile unsigned char[64]; escaped[0] = 7; delete[] escaped;',
    'mutex': '    static std::mutex escaped; pulp::test::rt_contract_probe_record_lock(); escaped.lock(); escaped.unlock();',
    'write': '''    pulp::test::rt_contract_probe_record_blocking();
#if defined(_WIN32)
    (void)::_write(1, "x", 1);
#else
    (void)::write(1, "x", 1);
#endif''',
}
if len(sys.argv) != 3 or sys.argv[1] not in plants:
    raise SystemExit('usage: g5_rt_escape_mutator.py allocation|mutex|write FILE')
source = Path(sys.argv[2])
text = source.read_text()
if text.count(needle) != 1:
    raise SystemExit('expected exactly one callback escape marker')
source.write_text(text.replace(needle, plants[sys.argv[1]], 1))
