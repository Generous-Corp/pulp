#!/usr/bin/env python3
"""Run the checked-in Forge Modular AUv2/Logic A3 role producer."""

import os
import stat
import types
from pathlib import Path

support = Path(os.environ.get("PULP_A3_ROLE_PRODUCER_SUPPORT", Path(__file__).with_name("gpu_first_visible_a3_role_producer.py")))
descriptor = os.open(support, os.O_RDONLY | getattr(os, "O_NOFOLLOW", 0))
try:
    metadata = os.fstat(descriptor)
    if not stat.S_ISREG(metadata.st_mode) or metadata.st_size > 2 * 1024 * 1024:
        raise SystemExit("PULP_A3_ROLE_PRODUCER_SUPPORT must be a bounded regular source file")
    source = os.read(descriptor, metadata.st_size + 1)
finally:
    os.close(descriptor)
if len(source) != metadata.st_size:
    raise SystemExit("PULP_A3_ROLE_PRODUCER_SUPPORT changed while loading")
module = types.ModuleType("pulp_a3_role_producer")
module.__file__ = str(support)
module.__package__ = ""
exec(compile(source, str(support), "exec", dont_inherit=True), module.__dict__)
raise SystemExit(module.main_entry("forge-modular-auv2-logic"))
