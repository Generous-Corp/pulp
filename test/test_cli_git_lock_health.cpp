// Stale git lock detection used by `pulp doctor`.
//
// The load-bearing assertion here is the negative one: a lock a live
// process holds must NEVER be reported. A check that fires on a
// healthy checkout teaches people to ignore doctor output, so the
// held-lock control matters more than the stale-lock case.

#include <catch2/catch_test_macros.hpp>

#include "tools/cli/cli_doctor.hpp"
#include "tools/cli/git_lock_health.hpp"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#ifdef _WIN32
#include <process.h>
#else
#include <unistd.h>
#endif

namespace fs = std::filesystem;
namespace git_lock = pulp::cli::git_lock;

using namespace std::chrono_literals;

namespace {

int current_process_id() {
#ifdef _WIN32
    return static_cast<int>(_getpid());
#else
    return static_cast<int>(::getpid());
#endif
}

void set_env(const char* name, const char* value) {
#ifdef _WIN32
    _putenv_s(name, value);
#else
    ::setenv(name, value, 1);
#endif
}

void clear_env(const char* name) {
#ifdef _WIN32
    _putenv_s(name, "");
#else
    ::unsetenv(name);
#endif
}

// A temp directory removed when the test leaves scope, so a failing
// assertion cannot leave lock files behind in the build tree.
class ScopedTempDir {
public:
    explicit ScopedTempDir(const std::string& label) {
        path_ = fs::temp_directory_path() /
                ("pulp-git-lock-" + label + "-" + std::to_string(current_process_id()) +
                 "-" + std::to_string(counter_++));
        fs::remove_all(path_);
        fs::create_directories(path_);
    }
    ~ScopedTempDir() {
        std::error_code ec;
        fs::remove_all(path_, ec);
    }
    ScopedTempDir(const ScopedTempDir&) = delete;
    ScopedTempDir& operator=(const ScopedTempDir&) = delete;

    const fs::path& path() const { return path_; }

private:
    fs::path path_;
    static inline int counter_ = 0;
};

// Lay out a checkout with a real `.git` directory.
fs::path make_plain_checkout(const fs::path& root) {
    fs::create_directories(root / ".git" / "refs" / "remotes" / "origin");
    return root / ".git";
}

void write_file(const fs::path& path, const std::string& body = {}) {
    fs::create_directories(path.parent_path());
    std::ofstream out(path);
    out << body;
}

void age_file(const fs::path& path, std::chrono::seconds age) {
    fs::last_write_time(path, fs::file_time_type::clock::now() - age);
}

git_lock::HolderProbe constant_probe(git_lock::Holder answer) {
    return [answer](const fs::path&) { return answer; };
}

// Candidate paths come back canonicalized, and on macOS the temp
// directory is reached through a symlink (/var -> /private/var), so
// both sides are resolved before comparing.
bool contains_path(const std::vector<git_lock::StaleLock>& locks, const fs::path& path) {
    std::error_code ec;
    const auto wanted = fs::weakly_canonical(path, ec);
    for (const auto& lock : locks) {
        if (lock.path == path) return true;
        if (!ec && fs::weakly_canonical(lock.path, ec) == wanted) return true;
    }
    return false;
}

}  // namespace

TEST_CASE("an old unheld lock is stale", "[cli][doctor][git-lock]") {
    ScopedTempDir temp("stale");
    auto git_dir = make_plain_checkout(temp.path());
    auto lock = git_dir / "index.lock";
    write_file(lock);
    age_file(lock, 48h);

    auto report = git_lock::find_stale_locks(temp.path(), 1h,
                                             constant_probe(git_lock::Holder::not_held));
    REQUIRE(report.stale.size() == 1);
    CHECK_FALSE(report.truncated);
    CHECK(contains_path(report.stale, lock));
    CHECK(report.stale.front().age >= 47h);
}

TEST_CASE("a lock a process holds is never stale", "[cli][doctor][git-lock]") {
    ScopedTempDir temp("held");
    auto git_dir = make_plain_checkout(temp.path());
    auto lock = git_dir / "index.lock";
    write_file(lock);
    // Old enough that the age gate cannot be what suppresses it — only
    // the holder answer can.
    age_file(lock, 48h);

    CHECK(git_lock::find_stale_locks(temp.path(), 1h,
                                     constant_probe(git_lock::Holder::held))
              .stale.empty());

    // A host that cannot answer the question must also stay silent.
    CHECK(git_lock::find_stale_locks(temp.path(), 1h,
                                     constant_probe(git_lock::Holder::unknown))
              .stale.empty());

    // Control: the same lock IS reported once the holder answer flips,
    // which proves the two assertions above were suppressed by the
    // probe rather than by a lock this fixture failed to create.
    CHECK(git_lock::find_stale_locks(temp.path(), 1h,
                                     constant_probe(git_lock::Holder::not_held))
              .stale.size() == 1);
}

TEST_CASE("a fresh lock is never stale and is never probed", "[cli][doctor][git-lock]") {
    ScopedTempDir temp("fresh");
    auto git_dir = make_plain_checkout(temp.path());
    auto lock = git_dir / "index.lock";
    write_file(lock);  // mtime is now

    int probe_calls = 0;
    git_lock::HolderProbe counting = [&probe_calls](const fs::path&) {
        ++probe_calls;
        return git_lock::Holder::not_held;
    };

    CHECK(git_lock::find_stale_locks(temp.path(), 1h, counting).stale.empty());
    // The age gate runs first so a healthy checkout spawns no probes.
    CHECK(probe_calls == 0);

    // Control: the same probe does get called, and does report, once
    // the lock crosses the threshold.
    age_file(lock, 48h);
    CHECK(git_lock::find_stale_locks(temp.path(), 1h, counting).stale.size() == 1);
    CHECK(probe_calls == 1);
}

TEST_CASE("ref locks are found under refs/", "[cli][doctor][git-lock]") {
    ScopedTempDir temp("refs");
    auto git_dir = make_plain_checkout(temp.path());
    auto ref_lock = git_dir / "refs" / "remotes" / "origin" / "feature" / "topic.lock";
    write_file(ref_lock);
    age_file(ref_lock, 72h);
    // A live ref file next to it must not be mistaken for a lock.
    write_file(git_dir / "refs" / "remotes" / "origin" / "main", "deadbeef\n");
    age_file(git_dir / "refs" / "remotes" / "origin" / "main", 72h);

    auto stale = git_lock::find_stale_locks(temp.path(), 1h,
                                            constant_probe(git_lock::Holder::not_held))
                     .stale;
    REQUIRE(stale.size() == 1);
    CHECK(contains_path(stale, ref_lock));
}

TEST_CASE("a linked worktree sees the shared and sibling git dirs", "[cli][doctor][git-lock]") {
    ScopedTempDir temp("worktree");
    auto primary = temp.path() / "primary";
    auto common = make_plain_checkout(primary);

    // The layout `git worktree add` produces: the linked checkout's
    // `.git` is a file naming its private git dir, and that dir names
    // the shared common dir relatively.
    auto linked_git_dir = common / "worktrees" / "linked";
    auto sibling_git_dir = common / "worktrees" / "sibling";
    fs::create_directories(linked_git_dir);
    fs::create_directories(sibling_git_dir);
    write_file(linked_git_dir / "commondir", "../..\n");
    write_file(sibling_git_dir / "commondir", "../..\n");

    auto linked_root = temp.path() / "linked";
    fs::create_directories(linked_root);
    write_file(linked_root / ".git", "gitdir: " + linked_git_dir.string() + "\n");

    auto own_lock = linked_git_dir / "index.lock";
    auto shared_lock = common / "index.lock";
    auto sibling_lock = sibling_git_dir / "index.lock";
    for (const auto& lock : {own_lock, shared_lock, sibling_lock}) {
        write_file(lock);
        age_file(lock, 200h);
    }

    auto stale = git_lock::find_stale_locks(linked_root, 1h,
                                            constant_probe(git_lock::Holder::not_held))
                     .stale;
    // The shared entry is the one that matters: a lock in the primary
    // checkout is invisible from every linked worktree, which is how
    // one survives for days unnoticed.
    CHECK(contains_path(stale, shared_lock));
    CHECK(contains_path(stale, own_lock));
    CHECK(contains_path(stale, sibling_lock));
}

TEST_CASE("a scan that hits the probe budget says so", "[cli][doctor][git-lock]") {
    ScopedTempDir temp("budget");
    auto common = make_plain_checkout(temp.path());

    // Each holder probe spawns a process, so the scan is bounded. A
    // bound that under-reported silently would read as a cleaner
    // result than it earned, so the budget must surface.
    const int over_budget = git_lock::max_holder_probes + 5;
    for (int i = 0; i < over_budget; ++i) {
        auto lock = common / "worktrees" / ("wt" + std::to_string(i)) / "index.lock";
        write_file(lock);
        age_file(lock, 48h);
    }

    int probe_calls = 0;
    git_lock::HolderProbe counting = [&probe_calls](const fs::path&) {
        ++probe_calls;
        return git_lock::Holder::not_held;
    };

    auto report = git_lock::find_stale_locks(temp.path(), 1h, counting);
    CHECK(report.truncated);
    CHECK(probe_calls == git_lock::max_holder_probes);
    CHECK(report.stale.size() == static_cast<std::size_t>(git_lock::max_holder_probes));

    // Control: the same fixture under the budget reports every lock
    // and does NOT claim truncation, so the flag tracks the budget
    // rather than being stuck on.
    for (int i = git_lock::max_holder_probes - 1; i < over_budget; ++i)
        fs::remove(common / "worktrees" / ("wt" + std::to_string(i)) / "index.lock");
    probe_calls = 0;
    auto under = git_lock::find_stale_locks(temp.path(), 1h, counting);
    CHECK_FALSE(under.truncated);
    CHECK(under.stale.size() < static_cast<std::size_t>(git_lock::max_holder_probes));
}

TEST_CASE("a directory that is not a checkout yields no candidates", "[cli][doctor][git-lock]") {
    ScopedTempDir temp("bare");
    CHECK(git_lock::lock_candidates(temp.path()).empty());
    CHECK(git_lock::find_stale_locks(temp.path(), 1h,
                                     constant_probe(git_lock::Holder::not_held))
              .stale.empty());
}

TEST_CASE("the threshold is overridable", "[cli][doctor][git-lock]") {
    struct Restore {
        ~Restore() { clear_env("PULP_DOCTOR_STALE_LOCK_MINUTES"); }
    } restore;

    clear_env("PULP_DOCTOR_STALE_LOCK_MINUTES");
    CHECK(git_lock::default_stale_after() == 1h);

    set_env("PULP_DOCTOR_STALE_LOCK_MINUTES", "5");
    CHECK(git_lock::default_stale_after() == 5min);

    // Garbage and non-positive values fall back rather than disabling
    // the age gate, which would turn every live lock into a report.
    for (const char* bad : {"0", "-3", "abc", "5m", ""}) {
        set_env("PULP_DOCTOR_STALE_LOCK_MINUTES", bad);
        CHECK(git_lock::default_stale_after() == 1h);
    }
}

// ── Real lsof probe ─────────────────────────────────────────────────
//
// The tests above prove the decision logic against an injected answer.
// These prove the shipped probe can actually tell the two states
// apart — without them, an lsof invocation that always answered
// `not_held` would pass everything above and report every live lock.

TEST_CASE("the lsof probe distinguishes a held file from a free one", "[cli][doctor][git-lock]") {
    ScopedTempDir temp("probe");
    auto free_path = temp.path() / "free.lock";
    auto held_path = temp.path() / "held.lock";
    write_file(free_path);
    write_file(held_path);

    const auto free_answer = git_lock::probe_holder_with_lsof(free_path);
    if (free_answer == git_lock::Holder::unknown) {
        SKIP("lsof is unavailable on this host, so the holder probe cannot be exercised");
    }
    CHECK(free_answer == git_lock::Holder::not_held);

    // Hold the file open in this very process. lsof reports every
    // process including the caller, so this is a real holder.
    std::FILE* handle = std::fopen(held_path.string().c_str(), "r");
    REQUIRE(handle != nullptr);
    const auto held_answer = git_lock::probe_holder_with_lsof(held_path);
    std::fclose(handle);
    CHECK(held_answer == git_lock::Holder::held);

    // Once released, the same path reads as free — so the answer above
    // tracked the open descriptor, not something about the path.
    CHECK(git_lock::probe_holder_with_lsof(held_path) == git_lock::Holder::not_held);
}

TEST_CASE("a lock held open end to end is not reported", "[cli][doctor][git-lock]") {
    ScopedTempDir temp("e2e");
    auto git_dir = make_plain_checkout(temp.path());
    auto lock = git_dir / "index.lock";
    write_file(lock);
    age_file(lock, 48h);

    if (git_lock::probe_holder_with_lsof(lock) == git_lock::Holder::unknown) {
        SKIP("lsof is unavailable on this host, so the holder probe cannot be exercised");
    }

    // Unheld and old: the shipped probe reports it.
    CHECK(git_lock::find_stale_locks(temp.path(), 1h, git_lock::probe_holder_with_lsof)
              .stale.size() == 1);

    // Same lock, same age, now held open: the shipped probe stays silent.
    std::FILE* handle = std::fopen(lock.string().c_str(), "r");
    REQUIRE(handle != nullptr);
    const auto while_held =
        git_lock::find_stale_locks(temp.path(), 1h, git_lock::probe_holder_with_lsof);
    std::fclose(handle);
    CHECK(while_held.stale.empty());
}

// ── The doctor row, and what it must not gate ───────────────────────

TEST_CASE("the doctor row reports a stale lock without gating pulp create",
          "[cli][doctor][git-lock]") {
    ScopedTempDir temp("row");
    auto git_dir = make_plain_checkout(temp.path());
    auto lock = git_dir / "index.lock";
    write_file(lock);
    age_file(lock, 48h);

    auto checks = run_doctor_checks(temp.path(), /*standalone_mode=*/false, "git locks");
    REQUIRE(checks.size() == 1);
    const auto& row = checks.front();
    REQUIRE(row.name == "git locks");

    if (git_lock::probe_holder_with_lsof(lock) == git_lock::Holder::unknown) {
        SKIP("lsof is unavailable on this host, so the holder probe cannot be exercised");
    }

    CHECK_FALSE(row.passed);
    CHECK(row.detail.find(lock.filename().string()) != std::string::npos);

    // A stale lock in some checkout says nothing about whether a new
    // project can be scaffolded, so it must not stop `pulp create`.
    CHECK(row.checkout_health);
    CHECK_FALSE(doctor_check_gates_project_creation(row));

    // The remediation must not be executable: doctor must never delete
    // a lock a live process may be about to use.
    CHECK_FALSE(row.fix.empty());

    // Control: an ordinary failing row DOES gate creation, so the
    // assertion above reflects this row's classification rather than a
    // predicate that never gates anything.
    DoctorCheck ordinary{"some tool", false, "missing", "install it"};
    CHECK(doctor_check_gates_project_creation(ordinary));
    DoctorCheck passing{"some tool", true, "found", {}};
    CHECK_FALSE(doctor_check_gates_project_creation(passing));
}

TEST_CASE("a healthy checkout produces a passing doctor row", "[cli][doctor][git-lock]") {
    ScopedTempDir temp("row-clean");
    make_plain_checkout(temp.path());

    auto checks = run_doctor_checks(temp.path(), /*standalone_mode=*/false, "git locks");
    REQUIRE(checks.size() == 1);
    CHECK(checks.front().passed);
    CHECK(checks.front().fix.empty());
}
