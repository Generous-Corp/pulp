#!/usr/bin/env python3
"""Tests for unbounded_wait_lint.py.

The load-bearing pair is `test_violating_fixture_is_flagged` /
`test_bounded_twin_is_clean`: the SAME wait, unbounded and bounded, so the gate
is proven to distinguish them rather than merely proven to be quiet. A guard
that never fires and a guard that always fires both look green on a clean tree.
"""

from __future__ import annotations

import importlib.util
import tempfile
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
_spec = importlib.util.spec_from_file_location(
    "unbounded_wait_lint", HERE / "unbounded_wait_lint.py")
lint = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(lint)


def scan_source(body: str, rel: str = "test/test_fixture.cpp") -> list[str]:
    """Scan `body` as a whole test source; return the finding kinds."""
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        path = root / rel
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(body)
        return [f.kind for f in lint.scan_file(root, rel, None)]


# The fixture: a test thread waiting for a worker to signal that it started.
UNBOUNDED_SPIN = """
TEST_CASE("worker reaches the call under test") {
    std::atomic<bool> started{false};
    std::atomic<int> blocks{0};
    std::thread worker([&] {
        started.store(true, std::memory_order_release);
        while (!stop.load(std::memory_order_relaxed)) {
            blocks.fetch_add(1, std::memory_order_relaxed);
        }
    });
    while (!started.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }
    stop.store(true);
    worker.join();
    CHECK(blocks.load() > 0);
}
"""

BOUNDED_TWIN = """
TEST_CASE("worker reaches the call under test") {
    std::atomic<bool> started{false};
    std::atomic<int> blocks{0};
    std::thread worker([&] {
        started.store(true, std::memory_order_release);
        while (!stop.load(std::memory_order_relaxed)) {
            blocks.fetch_add(1, std::memory_order_relaxed);
        }
    });
    CHECK(pulp::test::wait_for_condition(
        [&] { return started.load(std::memory_order_acquire); }));
    stop.store(true);
    worker.join();
    CHECK(blocks.load() > 0);
}
"""


class TestTheGateDistinguishes(unittest.TestCase):
    def test_violating_fixture_is_flagged(self):
        kinds = scan_source(UNBOUNDED_SPIN)
        self.assertEqual(len(kinds), 1, f"expected exactly one finding, got {kinds}")
        self.assertIn("unbounded spin", kinds[0])

    def test_bounded_twin_is_clean(self):
        # Same test, same worker, same assertion — only the wait changed.
        self.assertEqual(scan_source(BOUNDED_TWIN), [])


class TestShapes(unittest.TestCase):
    def test_cv_wait_predicate_form_is_flagged(self):
        kinds = scan_source("    cv.wait(lock, [&] { return ready; });\n")
        self.assertEqual(len(kinds), 1)
        self.assertIn("cv.wait", kinds[0])

    def test_cv_wait_for_is_clean(self):
        self.assertEqual(
            scan_source("    cv.wait_for(lock, 10s, [&] { return ready; });\n"), [])

    def test_bare_future_wait_is_flagged(self):
        kinds = scan_source("    first_block.wait();\n")
        self.assertEqual(len(kinds), 1)
        self.assertIn("future.wait()", kinds[0])

    def test_future_wait_for_is_clean(self):
        self.assertEqual(
            scan_source(
                "    CHECK(f.wait_for(pulp::test::kProgressDeadline)\n"
                "          == std::future_status::ready);\n"), [])


class TestNoFalsePositives(unittest.TestCase):
    def test_worker_exit_loop_is_not_a_wait(self):
        # The worker's own `while (!stop)` body is its exit condition, not a
        # wait for another thread. Flagging it would fire on every such test.
        self.assertEqual(
            scan_source("        while (!stop.load(std::memory_order_relaxed)) {\n"), [])

    def test_deadline_in_the_condition_is_clean(self):
        self.assertEqual(
            scan_source(
                "    while (!ready.load() &&\n"
                "           std::chrono::steady_clock::now() < deadline) {\n"
                "        std::this_thread::yield();\n"
                "    }\n"), [])

    def test_process_wait_is_not_a_future(self):
        # ChildProcess::wait() is a process wait with its own timeout plumbing.
        self.assertEqual(scan_source("    auto r = cp.wait();\n"), [])

    def test_suppression_comment_silences_a_line(self):
        self.assertEqual(
            scan_source(
                "    while (!started.load()) {}  // unbounded-wait: allow "
                "the publisher is joined above\n"), [])

    def test_production_source_is_out_of_scope(self):
        self.assertEqual(
            scan_source("    while (!started.load()) {}\n",
                        rel="core/host/src/signal_graph.cpp"), [])


class TestAdjacencyDoesNotVouch(unittest.TestCase):
    """A bounded wait next door must not silence an unbounded one.

    The first cut of this lint checked for a bound anywhere in a window of
    neighbouring lines. That made a newly introduced unbounded wait invisible
    whenever it sat beside an already-bounded one — the most likely arrangement
    in a file someone is part-way through fixing. It reported "ok" on a real
    planted violation, which is the precise failure this whole gate exists to
    prevent, so it is pinned here.
    """

    def test_unbounded_wait_beside_a_bounded_one_is_still_flagged(self):
        body = (
            "    while (!started.load(std::memory_order_acquire)) {}\n"
            "    CHECK(pulp::test::wait_for_condition(\n"
            "        [&] { return other.load(std::memory_order_acquire); }));\n"
        )
        kinds = scan_source(body)
        self.assertEqual(len(kinds), 1, f"adjacent bound vouched for it: {kinds}")
        self.assertIn("unbounded spin", kinds[0])

    def test_bound_on_a_continuation_line_still_counts(self):
        # The wait's OWN statement may legitimately span lines.
        body = (
            "    while (!ready.load() &&\n"
            "           std::chrono::steady_clock::now() < deadline) {\n"
            "        std::this_thread::yield();\n"
            "    }\n"
        )
        self.assertEqual(scan_source(body), [])


class TestDiffScoping(unittest.TestCase):
    def test_only_changed_lines_are_reported(self):
        body = "    while (!a.load()) {}\n    while (!b.load()) {}\n"
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            rel = "test/test_x.cpp"
            (root / "test").mkdir(parents=True)
            (root / rel).write_text(body)
            # Pretend only line 2 was touched: the pre-existing line 1 must not
            # block the PR. This is what keeps the backlog from failing CI.
            found = lint.scan_file(root, rel, {2})
            self.assertEqual(len(found), 1)
            self.assertEqual(found[0].line_no, 2)


if __name__ == "__main__":
    unittest.main()
