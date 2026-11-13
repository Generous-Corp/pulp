#!/usr/bin/env python3
"""Tests for thread_assert_check.py.

The load-bearing pair is `test_container_form_is_flagged` /
`test_container_form_safe_twin_is_clean`: the SAME eight-worker hammer test in
its unsafe and safe spellings, so the guard is proven to DISTINGUISH them
rather than merely proven to be quiet on a clean tree.

CONTAINER_UNSAFE reconstructs the shape that actually shipped — a
`std::vector<std::thread>` populated by `emplace_back` with a `REQUIRE` in the
lambda. The narrow scan this file's guard replaced exited 0 with no output on
that shape while correctly flagging the direct `std::thread t([]{ REQUIRE })`
form, so a clean run looked identical to a checked one. Every fixture below
therefore comes in pairs: something that must fire, and something that must
not.
"""

from __future__ import annotations

import importlib.util
import tempfile
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
_spec = importlib.util.spec_from_file_location(
    "thread_assert_check", HERE / "thread_assert_check.py")
guard = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(guard)


def scan_source(body: str, wide: bool = False) -> list[tuple[int, str, str]]:
    """Scan `body` as a whole test source; return the guard's hits."""
    with tempfile.TemporaryDirectory() as tmp:
        path = Path(tmp) / "test_fixture.cpp"
        path.write_text(body)
        return guard.scan_file(path, wide=wide)


# --- the shape that shipped -------------------------------------------------
# Eight workers, 2000 assertions each, against a static prediction of 16,047.
# Three runs of the real binary produced 9180 / 14026 / 11512 — the counter was
# racing. The `std::thread` token appears only in the DECLARATION; the line
# that installs the body says `emplace_back`, which is why a scan keyed on the
# spawn token alone cannot see it.
CONTAINER_UNSAFE = """
#include <thread>
#include <vector>
TEST_CASE("concurrent presentations publish without tearing") {
    PresentTimingStore store;
    constexpr int kThreads = 8;
    constexpr int kPerThread = 2000;
    std::vector<std::thread> threads;
    threads.reserve(kThreads);
    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&store] {
            for (int i = 0; i < kPerThread; ++i)
                REQUIRE(store.publish_presented_seconds(12.5));
        });
    }
    for (auto& thread : threads)
        thread.join();
}
"""

# The same test, written the way the guard's docstring prescribes: the workers
# only RECORD, and every assertion runs on the test thread after join().
CONTAINER_SAFE = """
#include <thread>
#include <vector>
TEST_CASE("concurrent presentations publish without tearing") {
    PresentTimingStore store;
    constexpr int kThreads = 8;
    constexpr int kPerThread = 2000;
    std::vector<std::vector<char>> published(kThreads, std::vector<char>(kPerThread, 0));
    std::vector<std::thread> threads;
    threads.reserve(kThreads);
    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&store, &results = published[t]] {
            for (int i = 0; i < kPerThread; ++i)
                results[i] = store.publish_presented_seconds(12.5) ? char{1} : char{0};
        });
    }
    for (auto& thread : threads)
        thread.join();
    for (const auto& worker_results : published)
        for (const char accepted : worker_results)
            REQUIRE(accepted == char{1});
}
"""

DIRECT_UNSAFE = """
#include <thread>
TEST_CASE("direct") {
    std::thread worker([&]{ REQUIRE(ready()); });
    worker.join();
}
"""

DIRECT_SAFE = """
#include <thread>
TEST_CASE("direct") {
    std::atomic<bool> bad{false};
    std::thread worker([&]{ if (!ready()) bad.store(true); });
    worker.join();
    REQUIRE_FALSE(bad.load());
}
"""

# A lambda bound to a name and spawned on a later line. The spawn statement
# carries no lambda at all, so the body has to be reached through the binding.
NAMED_LAMBDA_UNSAFE = """
#include <thread>
TEST_CASE("token allocation is race-free") {
    auto allocate = [&] {
        for (int i = 0; i < 2000; ++i)
            REQUIRE(writer.allocate_command_id().valid());
    };
    std::thread a(allocate);
    std::thread b(allocate);
    a.join();
    b.join();
}
"""

NAMED_LAMBDA_VIA_CONTAINER_UNSAFE = """
#include <thread>
#include <vector>
TEST_CASE("token allocation is race-free") {
    auto allocate = [&] { REQUIRE(writer.allocate_command_id().valid()); };
    std::vector<std::thread> workers;
    workers.emplace_back(allocate);
    workers.emplace_back(allocate);
    for (auto& w : workers) w.join();
}
"""

# The flagged body calls a SECOND bound lambda that holds the assertion.
TRANSITIVE_UNSAFE = """
#include <thread>
TEST_CASE("transitive") {
    auto verify = [&] { REQUIRE(store.valid()); };
    auto body = [&] { for (int i = 0; i < 10; ++i) verify(); };
    std::thread worker(body);
    worker.join();
}
"""

JTHREAD_ARRAY_UNSAFE = """
#include <thread>
#include <array>
TEST_CASE("jthread array") {
    std::array<std::jthread, 4> workers;
    workers[0] = std::jthread([&]{ CHECK(store.valid()); });
}
"""

ASYNC_CONTAINER_UNSAFE = """
#include <future>
#include <vector>
TEST_CASE("async futures") {
    std::vector<std::future<int>> futures;
    futures.emplace_back([&]{ REQUIRE(store.valid()); return 1; });
}
"""

SUPPRESSED = """
#include <thread>
#include <vector>
TEST_CASE("suppressed") {
    std::vector<std::thread> threads;
    threads.emplace_back([&]{ REQUIRE(ready()); });  // thread-assert:allow
}
"""

# No thread anywhere: a helper lambda asserting on the test thread is correct
# C++ and must never be flagged.
NO_THREAD_HELPER = """
TEST_CASE("plain helper lambda") {
    auto expect_ok = [&](int v) { REQUIRE(v > 0); };
    expect_ok(1);
    expect_ok(2);
}
"""

# A thread IS spawned, but the asserting helper lambda is only ever called on
# the test thread. Gating on "any lambda in a thread-spawning TU" would flag
# this; the guard must not.
THREAD_PRESENT_HELPER_ON_TEST_THREAD = """
#include <thread>
TEST_CASE("helper stays on the test thread") {
    std::atomic<int> seen{0};
    std::thread worker([&]{ seen.store(1); });
    worker.join();
    auto expect_ok = [&](int v) { REQUIRE(v > 0); };
    expect_ok(seen.load());
}
"""

# Lexical traps the blanking pass has to survive: a digit separator, an
# assertion macro named only inside a string, and a subscript that looks like a
# lambda introducer.
LEXICAL_TRAPS = """
#include <thread>
#include <vector>
TEST_CASE("traps") {
    std::vector<int> v(10'000, 0);
    const char* doc = "call REQUIRE(x) from the test thread";
    // REQUIRE(commented_out());
    std::vector<std::thread> threads;
    threads.emplace_back([&]{ v[0] = 1; });
    for (auto& t : threads) t.join();
    REQUIRE(v[0] == 1);
}
"""


class ThreadAssertCheck(unittest.TestCase):
    # --- the pair the whole file exists for ---
    def test_container_form_is_flagged(self):
        hits = scan_source(CONTAINER_UNSAFE)
        self.assertEqual(len(hits), 1, hits)
        self.assertIn("REQUIRE(store.publish_presented_seconds", hits[0][1])
        self.assertIn("container of threads", hits[0][2])

    def test_container_form_safe_twin_is_clean(self):
        self.assertEqual(scan_source(CONTAINER_SAFE), [])

    # --- the form the narrow scan already caught must keep working ---
    def test_direct_thread_lambda_is_flagged(self):
        hits = scan_source(DIRECT_UNSAFE)
        self.assertEqual(len(hits), 1, hits)

    def test_direct_thread_safe_twin_is_clean(self):
        self.assertEqual(scan_source(DIRECT_SAFE), [])

    # --- indirection through a bound lambda ---
    def test_named_lambda_spawned_by_thread_is_flagged(self):
        hits = scan_source(NAMED_LAMBDA_UNSAFE)
        self.assertEqual(len(hits), 1, hits)
        self.assertIn("named lambda `allocate`", hits[0][2])

    def test_named_lambda_spawned_via_container_is_flagged(self):
        hits = scan_source(NAMED_LAMBDA_VIA_CONTAINER_UNSAFE)
        self.assertEqual(len(hits), 1, hits)

    def test_transitive_named_lambda_is_flagged(self):
        hits = scan_source(TRANSITIVE_UNSAFE)
        self.assertEqual(len(hits), 1, hits)
        self.assertIn("verify", hits[0][2])

    # --- other container spellings ---
    def test_jthread_array_is_flagged(self):
        self.assertEqual(len(scan_source(JTHREAD_ARRAY_UNSAFE)), 1)

    def test_future_container_is_flagged(self):
        self.assertEqual(len(scan_source(ASYNC_CONTAINER_UNSAFE)), 1)

    # --- the guard must stay quiet on correct code ---
    def test_suppression_comment_is_honoured(self):
        self.assertEqual(scan_source(SUPPRESSED), [])

    def test_helper_lambda_without_threads_is_clean(self):
        self.assertEqual(scan_source(NO_THREAD_HELPER), [])

    def test_helper_lambda_on_test_thread_is_clean(self):
        self.assertEqual(scan_source(THREAD_PRESENT_HELPER_ON_TEST_THREAD), [])

    def test_lexical_traps_do_not_desync_the_scan(self):
        self.assertEqual(scan_source(LEXICAL_TRAPS), [])

    # --- the over-broad diagnostic pass is real, and is NOT the gate ---
    def test_wide_pass_sees_the_test_thread_helper_the_gate_ignores(self):
        wide = scan_source(THREAD_PRESENT_HELPER_ON_TEST_THREAD, wide=True)
        self.assertEqual(len(wide), 1, wide)
        self.assertEqual(scan_source(THREAD_PRESENT_HELPER_ON_TEST_THREAD), [])

    def test_wide_pass_is_silent_without_any_thread(self):
        self.assertEqual(scan_source(NO_THREAD_HELPER, wide=True), [])


class ChangedFromTests(unittest.TestCase):
    """--changed-from: the pre-push form. Only test sources the push changed, read from the commit."""

    def git(self, repo: Path, *args: str) -> None:
        import os
        import subprocess
        env = dict(os.environ, GIT_AUTHOR_NAME="t", GIT_AUTHOR_EMAIL="t@t", GIT_COMMITTER_NAME="t",
                   GIT_COMMITTER_EMAIL="t@t", GIT_CONFIG_NOSYSTEM="1", HOME=str(repo))
        for k in ("GIT_DIR", "GIT_WORK_TREE", "GIT_INDEX_FILE"):
            env.pop(k, None)
        subprocess.run(["git", "-C", str(repo), *args], check=True, capture_output=True, env=env)

    def repo_with(self, tmp: str, base: dict, change: dict) -> Path:
        repo = Path(tmp) / "repo"
        repo.mkdir()
        self.git(repo, "init", "-q", "-b", "main")
        for files, msg in ((base, "base"), (change, "change")):
            if msg == "change":
                self.git(repo, "checkout", "-qb", "feat")
            for rel, text in files.items():
                (repo / rel).parent.mkdir(parents=True, exist_ok=True)
                (repo / rel).write_text(text, encoding="utf-8")
                self.git(repo, "add", rel)
            self.git(repo, "commit", "-q", "--allow-empty", "-m", msg)
        return repo

    def test_a_changed_unsafe_test_fails(self):
        with tempfile.TemporaryDirectory() as tmp:
            repo = self.repo_with(tmp, {"test/keep.cpp": DIRECT_SAFE}, {"test/sub/new.cpp": DIRECT_UNSAFE})
            self.assertEqual(guard.run_changed("main", root=repo), 1)

    def test_an_unsafe_test_only_on_the_base_does_not_block(self):
        # Negative control: the same unsafe source, but on main and untouched by the push.
        with tempfile.TemporaryDirectory() as tmp:
            repo = self.repo_with(tmp, {"test/old.cpp": DIRECT_UNSAFE}, {"test/new.cpp": DIRECT_SAFE})
            self.assertEqual(guard.run_changed("main", root=repo), 0)

    def test_sources_outside_test_are_not_scanned(self):
        with tempfile.TemporaryDirectory() as tmp:
            repo = self.repo_with(tmp, {}, {"core/x.cpp": DIRECT_UNSAFE})
            self.assertEqual(guard.run_changed("main", root=repo), 0)
            self.assertEqual(guard.changed_test_sources(repo, "main"), [])

    def test_no_merge_base_is_not_a_pass(self):
        with tempfile.TemporaryDirectory() as tmp:
            repo = self.repo_with(tmp, {}, {"test/new.cpp": DIRECT_UNSAFE})
            self.assertEqual(guard.run_changed("no-such-ref", root=repo), 2)


if __name__ == "__main__":
    unittest.main()
