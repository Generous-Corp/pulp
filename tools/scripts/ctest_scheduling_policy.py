#!/usr/bin/env python3
"""The one statement of how the gate's heavy ctest registrations are scheduled.

Two checks enforce it from different sides, and both import this module so
they cannot disagree:

  * tools/scripts/test_ci_throughput_workflows.py reads the test manifests'
    source (workflow-lint, no build);
  * tools/scripts/test_ctest_scheduling_contract.py reads the registrations of
    a configured build (the `ctest-scheduling-contract` ctest on the gate).

A registration belongs to at most one scheduling class. The classes pull in
opposite directions -- a weighted suite must hold every slot, a long test must
start early while other tests run -- so a name in two classes is a
contradiction, and `consistency_errors()` rejects it before either check runs.
"""

from __future__ import annotations

# Suites that open a real audio device: RUN_SERIAL, and weighted PROCESSORS 8
# where they also carry the suite-wide scheduling cost.
WEIGHTED_CORE_AUDIO_SERIAL_SUITES = frozenset({
    "pulp-test-audio",
    "pulp-test-standalone-apply-config",
    "pulp-test-standalone-audio-inspector",
})
SPLIT_CORE_AUDIO_SERIAL_SUITES = frozenset({
    "pulp-test-audio",
    "pulp-test-standalone-audio-inspector",
})

# Standalone suites weighted PROCESSORS 8 without opening a device, so they are
# never RUN_SERIAL.
NON_DEVICE_WEIGHTED_SUITES = (
    "pulp-test-standalone-editor-chrome",
    "pulp-test-standalone-audio-capture-wav",
    "pulp-test-standalone-audio-capture-rolling-wav",
    "pulp-test-standalone-transport-midi",
    "pulp-test-screenshot-capture",
)

# Selftests whose sealed sub-builds or fan-out inflate co-scheduled tests past
# their budgets: PROCESSORS 8, never RUN_SERIAL.
QUALITY_WEIGHTED_SUITES = frozenset({
    "agent-capability-manifest-selftest",
    "agent-capability-rederive-selftest",
    "gpu-dpr-v2-evidence-selftest",
    "gpu-dpr-runner-selftest",
    "gpu-first-visible-external-adapter-selftest",
    "gpu-health-cpu-only-configure",
})

BROWSER_LOCK = "browser"
RESERVATION_ENV = "PULP_BROWSER_CAPTURE_RESERVED_CORES"
BROWSER_SUITE = "pulp-browser-capture-node-integration"
BROWSER_LIFECYCLE = "pulp-browser-capture-process-lifecycle"

# The smallest gate VM runs ctest at -j3. A long test reserving more than this
# many slots cannot start until most of the suite is idle on any host.
MAX_LONG_TEST_PROCESSORS = 4


def row(name, companion=None, optional=False):
    return {"name": name, "companion": companion, "optional": optional}


# Tests that launch a real Chrome (directly, or through the importer), or that
# collided with one: the `browser` RESOURCE_LOCK, never RUN_SERIAL.
BROWSER_TESTS = (
    row(BROWSER_SUITE, companion="pulp-browser-capture-node-unit"),
    row(BROWSER_LIFECYCLE, companion="pulp-browser-capture-node-unit"),
    row("generic browser HTML supplies the reference for --fail-below",
        companion="pulp-import-design reports help and argument diagnostics"),
    row("agent-panel-native-invariants"),
    row("agent-panel-clipped-is-rejected"),
)

# Tests over 40 s on the required gate's merge-group runs, longest first: a
# COST so ctest starts them first, and at most MAX_LONG_TEST_PROCESSORS slots.
# gpu-first-visible-role-producers-selftest does serial work (46 sealed-build
# invocations one after another; user+sys tracks wall), so two slots declare it;
# co-scheduled on the gate it runs 52-57 s against a 300 s budget.
LONG_TESTS = (
    row("sample-region-compat-baseline", optional=True),
    row(BROWSER_SUITE, companion="pulp-browser-capture-node-unit"),
    row("cmake-control-sdk-consumer", optional=True),
    row("gpu-trace-overhead-acceptance-selftest"),
    row("gpu-first-visible-role-producers-selftest"),
    row("combined-installer-selftest", optional=True),
)

def weighted_suites():
    """Every registration that must carry PROCESSORS 8."""
    return (WEIGHTED_CORE_AUDIO_SERIAL_SUITES | set(NON_DEVICE_WEIGHTED_SUITES)
            | QUALITY_WEIGHTED_SUITES)


def consistency_errors(weighted=None, browser=None, long=None):
    """Names that sit in two classes that cannot both hold."""
    weighted = weighted_suites() if weighted is None else set(weighted)
    browser = {r["name"] for r in BROWSER_TESTS} if browser is None else set(browser)
    long = {r["name"] for r in LONG_TESTS} if long is None else set(long)
    errors = []
    for name in sorted(weighted & long):
        errors.append(f"{name}: weighted PROCESSORS 8 and a long test that must "
                      f"start early with at most {MAX_LONG_TEST_PROCESSORS} slots")
    for name in sorted(weighted & browser):
        errors.append(f"{name}: weighted PROCESSORS 8 and a browser-locked test")
    return errors
