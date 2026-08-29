#!/usr/bin/env python3
"""Run the checked-in Pulp Standalone A3 role producer."""

import importlib.util
import os
from pathlib import Path

support = Path(os.environ.get(
    "PULP_A3_ROLE_PRODUCER_SUPPORT",
    Path(__file__).with_name("gpu_first_visible_a3_role_producer.py"),
))
spec = importlib.util.spec_from_file_location("pulp_a3_role_producer", support)
if spec is None or spec.loader is None:
    raise SystemExit("cannot load PULP_A3_ROLE_PRODUCER_SUPPORT")
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)
raise SystemExit(module.main_entry("standalone"))
