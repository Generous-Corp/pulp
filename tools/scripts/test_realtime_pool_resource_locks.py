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

import os
import posixpath
import re
import tempfile
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


SOURCE_SUFFIXES = (".cpp", ".mm")
HEADER_SUFFIXES = (".hpp", ".h")
INCLUDE_RE = re.compile(r'^\s*#\s*(?:include|import)\s+"([^"]+)"', re.M)
# Data trees, not code; some are large.
SKIPPED_DIRS = {"fixtures"}


def _test_tree_files(test_dir: Path) -> dict[str, str]:
    """Every C/C++/Objective-C++ source and header under test/, by path
    relative to test/."""
    out: dict[str, str] = {}
    for dirpath, dirnames, filenames in os.walk(test_dir):
        dirnames[:] = sorted(d for d in dirnames if d not in SKIPPED_DIRS)
        for name in sorted(filenames):
            if not name.endswith(SOURCE_SUFFIXES + HEADER_SUFFIXES):
                continue
            path = Path(dirpath) / name
            out[path.relative_to(test_dir).as_posix()] = path.read_text(
                encoding="utf-8", errors="replace")
    return out


def pool_sources(test_dir: Path = TEST_DIR) -> set[str]:
    """Translation units under test/ that start a worker pool, relative to
    test/: those that name a marker themselves, and those that include,
    directly or through other test-tree headers, a file that does. A helper
    in test/support or a subdirectory starts the pool for every suite that
    includes it, so a scan of top-level test sources alone would miss it."""
    files = _test_tree_files(test_dir)
    marked = {rel for rel, text in files.items() if any(m in text for m in POOL_MARKERS)}
    changed = True
    while changed:
        changed = False
        for rel, text in files.items():
            if rel in marked:
                continue
            for inc in INCLUDE_RE.findall(text):
                # test/ is an include root, and a quoted include also resolves
                # beside the including file.
                beside = posixpath.normpath(posixpath.join(posixpath.dirname(rel), inc))
                if inc in marked or beside in marked:
                    marked.add(rel)
                    changed = True
                    break
    return {rel for rel in marked if rel.endswith(SOURCE_SUFFIXES)}


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

    def test_the_scan_reaches_objective_cpp_subdirectories_and_helpers(self) -> None:
        """A pool started from a .mm source, a subdirectory source, or a test
        helper header is found; a source that only mentions an unrelated
        header is not."""
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            files = {
                "test_direct.mm": "pool.start(GraphRuntimeWorkerPool{});\n",
                "suite/test_nested.cpp": "graph.set_parallel_routing_enabled(true);\n",
                "support/pool_helper.hpp": "inline void go() { GraphRuntimeWorkerPool p; }\n",
                "support/wrapper.hpp": '#include "pool_helper.hpp"\n',
                "test_via_helper.cpp": '#include "support/wrapper.hpp"\n',
                "support/plain.hpp": "inline int x = 1;\n",
                "test_plain.cpp": '#include "support/plain.hpp"\n',
                "fixtures/test_data.cpp": "GraphRuntimeWorkerPool in a fixture is data\n",
            }
            for rel, text in files.items():
                (root / rel).parent.mkdir(parents=True, exist_ok=True)
                (root / rel).write_text(text, encoding="utf-8")
            self.assertEqual(pool_sources(root),
                             {"test_direct.mm", "suite/test_nested.cpp", "test_via_helper.cpp"})

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
