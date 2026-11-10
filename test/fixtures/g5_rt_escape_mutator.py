#!/usr/bin/env python3
"""Plant a callback escape or bypass event reset for a compiled negative control."""
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
if len(sys.argv) != 3 or sys.argv[1] not in (*plants, 'reset'):
    raise SystemExit('usage: g5_rt_escape_mutator.py allocation|mutex|write|reset FILE')
if sys.argv[1] == 'reset':
    needle = '        if (transport && transport->should_reset_stream_history()) {'
    replacement = '        if (false && transport && transport->should_reset_stream_history()) {'
else:
    replacement = plants[sys.argv[1]]
source = Path(sys.argv[2])
text = source.read_text(encoding="utf-8")
if text.count(needle) != 1:
    raise SystemExit('expected exactly one mutation target')
source.write_text(text.replace(needle, replacement, 1), encoding="utf-8")
