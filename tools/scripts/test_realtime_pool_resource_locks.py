#!/usr/bin/env python3
"""Every test suite that starts a graph worker pool must serialize on
`pulp_realtime_worker_pool`.

GraphRuntimeWorkerPool workers join the realtime-priority fallback on their
first batch and spin between batches. Two such suites running side by side
under parallel ctest put more realtime spinners on the machine than it has
cores, and realtime threads starve every normal-priority thread there: other
tests' workers, the ctest scheduler itself. The symptom is a timing assertion
failing in an unrelated suite on somebody else's pull request. This test keeps
a newly added pool-starting suite from silently skipping the lock.

Run:  python3 tools/scripts/test_realtime_pool_resource_locks.py
"""

from __future__ import annotations

import unittest
from pathlib import Path

import test_gpu_test_resource_locks as gpu_locks

REPO_ROOT = Path(__file__).resolve().parent.parent.parent
TEST_DIR = REPO_ROOT / "test"

# Source text that means the suite starts a worker pool: it drives the pool
# directly, or enables SignalGraph's parallel executor, which starts one sized
# to the core count.
POOL_MARKERS = (
    "GraphRuntimeWorkerPool",
    "set_parallel_routing_enabled(",
    "process_parallel",
)

LOCK = "RESOURCE_LOCK pulp_realtime_worker_pool"


def pool_sources() -> set[str]:
    found = set()
    for path in sorted(TEST_DIR.glob("test_*.cpp")):
        text = path.read_text(encoding="utf-8", errors="replace")
        if any(marker in text for marker in POOL_MARKERS):
            found.add(path.name)
    return found


def sources_of(target: str, body: str) -> str:
    """The declaration body, plus the implicit source a suite declared without
    SOURCES compiles (pulp-test-foo-bar builds test_foo_bar.cpp)."""
    if "SOURCES" in body or not target.startswith("pulp-test-"):
        return body
    return body + "\ntest_" + target[len("pulp-test-"):].replace("-", "_") + ".cpp"


def unlocked_pool_suites(regs, sources: set[str], lock_text) -> tuple[dict[str, list[str]], set[str]]:
    """(target -> pool sources, for every registration missing the lock;
    the pool sources some registration builds)."""
    missing: dict[str, list[str]] = {}
    covered: set[str] = set()
    for target, (body, manifest) in regs.items():
        used = {s for s in sources if s in sources_of(target, body)}
        if not used:
            continue
        covered |= used
        if LOCK not in lock_text(target, manifest):
            missing[target] = sorted(used)
    return missing, covered


class RealtimePoolResourceLockTests(unittest.TestCase):
    def test_pool_starting_suites_take_the_realtime_pool_lock(self) -> None:
        sources = pool_sources()
        missing, covered = unlocked_pool_suites(
            gpu_locks.registrations(), sources, gpu_locks.lock_text_for)
        self.assertEqual(
            missing, {},
            f"these suites start a realtime worker pool but are registered without "
            f"{LOCK!r}; under parallel ctest they can starve every other test on the machine")
        # Every detected source must belong to some registration, or a suite the
        # mapping cannot see escapes the policy.
        self.assertEqual(sources - covered, set(), "pool sources with no registration")
        self.assertGreater(len(covered), 0, "matched no pool suites; mapping broke")

    def test_known_pool_suites_are_detected(self) -> None:
        """Guard the detector itself: if these stop matching, the policy test
        above checks nothing."""
        sources = pool_sources()
        for name in (
            "test_graph_runtime_worker_pool.cpp",
            "test_signal_graph_executor_parity.cpp",
            "test_host_signal_graph.cpp",
            "test_timeline_graph_binding.cpp",
        ):
            with self.subTest(source=name):
                self.assertIn(name, sources)

    def test_a_suite_without_the_lock_is_reported(self) -> None:
        """Negative control: the policy flags a planted unlocked registration,
        including one that names its source only through the suite name."""
        sources = {"test_graph_runtime_worker_pool.cpp", "test_signal_graph_parallel_cost.cpp"}
        regs = {
            "pulp-test-planted": ("pulp-test-planted\n    SOURCES test_graph_runtime_worker_pool.cpp", None),
            "pulp-test-signal-graph-parallel-cost": ("pulp-test-signal-graph-parallel-cost GROUP g", None),
            "pulp-test-locked": ("pulp-test-locked\n    SOURCES test_graph_runtime_worker_pool.cpp", None),
            "pulp-test-unrelated": ("pulp-test-unrelated\n    SOURCES test_other.cpp", None),
        }
        texts = {"pulp-test-locked": LOCK}
        missing, covered = unlocked_pool_suites(regs, sources, lambda t, m: texts.get(t, ""))
        self.assertEqual(sorted(missing), ["pulp-test-planted", "pulp-test-signal-graph-parallel-cost"])
        self.assertEqual(covered, sources)

    def test_literal_locks_precede_labels(self) -> None:
        """Keep Catch's list-valued LABELS from swallowing a later lock."""
        for manifest in gpu_locks.manifests():
            text = manifest.read_text(encoding="utf-8")
            for target, body in gpu_locks._calls(text, ("catch_discover_tests", "pulp_add_test_suite")):
                lock_at = body.find(LOCK)
                if lock_at < 0:
                    continue
                labels_at = body.find("LABELS")
                with self.subTest(target=target, manifest=manifest.name):
                    self.assertTrue(labels_at < 0 or lock_at < labels_at,
                                    f"{target} places {LOCK!r} after LABELS")


if __name__ == "__main__":
    unittest.main(verbosity=2)
