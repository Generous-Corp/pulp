// git_lock_health.hpp — stale git lock-file detection for `pulp doctor`.
//
// Git guards index and ref writes with a `*.lock` file created
// O_EXCL. If the process holding one dies without cleaning up, the
// lock survives and every subsequent index- or ref-writing command in
// that checkout fails, while reads (`git status`, `git diff`) keep
// working — so the checkout looks healthy and only writes are dead.
//
// The detection is deliberately conservative: a lock is stale only
// when it exists, no live process holds it, AND it is older than a
// threshold. A live lock is the normal state during a commit and must
// never be reported.
//
// Header-only on purpose: `cli_doctor_helpers.cpp` is compiled directly
// into a dozen separate test targets rather than linked as a library,
// so a companion .cpp would have to be added to each of their source
// lists, and every future target that picks up the doctor helpers would
// have to remember to mirror it or fail at link time.
#pragma once

#include <pulp/platform/child_process.hpp>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <string>
#include <system_error>
#include <vector>

namespace pulp::cli::git_lock {

namespace fs = std::filesystem;

// Whether a live process currently has a lock file open.
enum class Holder {
    held,      // A process has it open. The lock is live and normal.
    not_held,  // No process has it open.
    unknown,   // The question could not be answered on this host.
};

// Answers whether any process currently has `path` open.
using HolderProbe = std::function<Holder(const fs::path& path)>;

// A lock that exists, is older than the threshold, and that no live
// process holds.
struct StaleLock {
    fs::path path;
    std::chrono::seconds age{0};
};

// The outcome of a scan. `truncated` says the holder-probe budget ran
// out before every aged lock was checked, so `stale` is a lower bound
// rather than the whole story — a scan that quietly reported less than
// it found would read as a cleaner result than it earned.
struct Report {
    std::vector<StaleLock> stale;
    bool truncated = false;
};

// The most holder probes one scan will run. Each probe spawns a
// process, so an unbounded count would let a fleet of abandoned
// worktrees turn a diagnostic into a minutes-long stall.
inline constexpr int max_holder_probes = 64;

namespace detail {

// Bounds the recursive `refs/` walk so a pathological checkout cannot
// turn a diagnostic into a directory crawl.
inline constexpr int kMaxRefEntriesVisited = 20000;

// Bounds the sibling-worktree sweep. A long-lived agent checkout
// accumulates hundreds of linked worktrees.
inline constexpr int kMaxSiblingWorktrees = 4000;

// Locks that live directly in a git dir rather than under `refs/`.
inline constexpr const char* kTopLevelLockNames[] = {
    "index.lock", "HEAD.lock", "config.lock", "packed-refs.lock", "shallow.lock",
};

// A linked worktree's own git dir holds a private index and HEAD; its
// config, packed-refs and most of its refs live in the shared common
// dir and are covered there instead.
inline constexpr const char* kWorktreeLockNames[] = {"index.lock", "HEAD.lock"};

inline std::string trim(std::string text) {
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back())))
        text.pop_back();
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front())))
        text.erase(text.begin());
    return text;
}

// Resolve `<root>/.git`. A plain checkout has a directory there; a
// linked worktree has a file holding `gitdir: <path>`.
inline fs::path resolve_git_dir(const fs::path& repo_root) {
    if (repo_root.empty()) return {};
    std::error_code ec;
    auto dot_git = repo_root / ".git";

    if (fs::is_directory(dot_git, ec)) return dot_git;
    if (!fs::is_regular_file(dot_git, ec)) return {};

    std::ifstream stream(dot_git);
    std::string line;
    if (!std::getline(stream, line)) return {};
    constexpr std::string_view prefix = "gitdir:";
    if (line.rfind(prefix, 0) != 0) return {};

    fs::path pointed = trim(line.substr(prefix.size()));
    if (pointed.empty()) return {};
    if (pointed.is_relative()) pointed = repo_root / pointed;
    auto normalized = fs::weakly_canonical(pointed, ec);
    if (ec) return pointed.lexically_normal();
    return normalized;
}

// The shared git dir behind a linked worktree, named by `commondir`.
// For a plain checkout this is the git dir itself.
inline fs::path resolve_common_dir(const fs::path& git_dir) {
    if (git_dir.empty()) return {};
    std::error_code ec;
    auto marker = git_dir / "commondir";
    if (!fs::is_regular_file(marker, ec)) return git_dir;

    std::ifstream stream(marker);
    std::string line;
    if (!std::getline(stream, line)) return git_dir;
    fs::path pointed = trim(std::move(line));
    if (pointed.empty()) return git_dir;
    if (pointed.is_relative()) pointed = git_dir / pointed;
    auto normalized = fs::weakly_canonical(pointed, ec);
    if (ec) return pointed.lexically_normal();
    return normalized;
}

// `deep` walks `refs/` as well as the top-level lock names. Sibling
// worktrees are swept shallowly: there can be hundreds of them, and
// the refs they could lock are the shared ones already covered by the
// common dir.
inline void append_locks_in_git_dir(const fs::path& git_dir, std::vector<fs::path>& out, bool deep) {
    std::error_code ec;
    if (git_dir.empty() || !fs::is_directory(git_dir, ec)) return;

    if (deep) {
        for (const char* name : kTopLevelLockNames) out.push_back(git_dir / name);
    } else {
        for (const char* name : kWorktreeLockNames) out.push_back(git_dir / name);
        return;
    }

    auto refs = git_dir / "refs";
    if (!fs::is_directory(refs, ec)) return;

    // Stepped with the error_code overload rather than a range-for:
    // range-for increments through the throwing form, and a sibling
    // agent removing a directory mid-walk would then escape this
    // diagnostic as a filesystem_error.
    int visited = 0;
    fs::recursive_directory_iterator it(refs, fs::directory_options::skip_permission_denied, ec);
    if (ec) return;
    const fs::recursive_directory_iterator done;
    while (it != done) {
        if (++visited > kMaxRefEntriesVisited) break;
        if (it->path().extension() == ".lock") out.push_back(it->path());
        it.increment(ec);
        if (ec) break;
    }
}

}  // namespace detail

using detail::kMaxRefEntriesVisited;
using detail::kMaxSiblingWorktrees;
using detail::append_locks_in_git_dir;
using detail::resolve_common_dir;
using detail::resolve_git_dir;
using detail::trim;

// The default probe. Shells out to `lsof`, and reports `unknown` when
// lsof is missing, times out, or reports an error — a host that cannot
// answer the question must never produce a stale verdict.
inline Holder probe_holder_with_lsof(const fs::path& path) {
#if defined(_WIN32)
    // No lsof, and the obvious Windows substitute — opening the file
    // with an exclusive share mode — would briefly deny access to the
    // git process the check exists to protect. Reporting `unknown`
    // means the check stays silent here rather than guessing.
    (void)path;
    return Holder::unknown;
#else
    static const fs::path lsof = [] {
        if (auto found = pulp::platform::find_on_path("lsof")) return *found;
        // lsof lives in a sbin directory that a trimmed non-interactive
        // PATH routinely omits.
        for (const char* candidate : {"/usr/sbin/lsof", "/usr/bin/lsof"}) {
            std::error_code ec;
            if (fs::is_regular_file(candidate, ec)) return fs::path(candidate);
        }
        return fs::path{};
    }();
    if (lsof.empty()) return Holder::unknown;

    // `-t` prints holder pids and nothing else; `--` keeps a path that
    // begins with `-` from being read as an option.
    auto result = pulp::platform::exec(lsof.string(), {"-t", "--", path.string()}, 15000);
    if (result.timed_out) return Holder::unknown;

    const bool no_pids = trim(result.stdout_output).empty();
    const bool quiet = trim(result.stderr_output).empty();

    // lsof exits 0 only when it matched an open file, and 1 both when
    // it matched nothing and when it failed. Those two are told apart
    // by stderr: a real failure explains itself there.
    if (result.exit_code == 0 && !no_pids) return Holder::held;
    if (result.exit_code == 1 && no_pids && quiet) return Holder::not_held;
    return Holder::unknown;
#endif
}

// Every git lock path worth examining for `repo_root`: the locks in
// this checkout's own git dir, in the shared common dir (the primary
// checkout, when `repo_root` is a linked worktree), and in each
// sibling worktree's git dir. Enumeration only — nothing is opened,
// nothing is judged. Returns empty when `repo_root` is not a git
// checkout.
inline std::vector<fs::path> lock_candidates(const fs::path& repo_root) {
    std::vector<fs::path> out;
    auto git_dir = resolve_git_dir(repo_root);
    if (git_dir.empty()) return out;
    auto common_dir = resolve_common_dir(git_dir);

    append_locks_in_git_dir(git_dir, out, true);
    if (common_dir != git_dir) append_locks_in_git_dir(common_dir, out, true);

    // Sibling worktrees share this common dir. A lock left in one is
    // invisible from every other checkout once nobody is working
    // there, which is how one goes unnoticed for days.
    std::error_code ec;
    auto worktrees = common_dir / "worktrees";
    if (fs::is_directory(worktrees, ec)) {
        int seen = 0;
        fs::directory_iterator it(worktrees, fs::directory_options::skip_permission_denied, ec);
        const fs::directory_iterator done;
        while (!ec && it != done) {
            if (++seen > kMaxSiblingWorktrees) break;
            if (it->path() != git_dir) append_locks_in_git_dir(it->path(), out, false);
            it.increment(ec);
        }
    }

    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

// How old a lock must be before it can be called stale. Reads
// PULP_DOCTOR_STALE_LOCK_MINUTES when set to a positive integer.
inline std::chrono::seconds default_stale_after() {
    // An hour is far past any legitimate hold. Git keeps a lock file's
    // descriptor open for as long as it owns the lock, so the holder
    // probe alone already clears every live case; the age is the guard
    // for the sliver between git closing that descriptor and renaming
    // the file into place, which lasts microseconds.
    constexpr std::chrono::seconds fallback{3600};
    const char* raw = std::getenv("PULP_DOCTOR_STALE_LOCK_MINUTES");
    if (raw == nullptr) return fallback;
    try {
        std::size_t consumed = 0;
        const long long minutes = std::stoll(raw, &consumed);
        if (consumed != std::string(raw).size() || minutes <= 0) return fallback;
        return std::chrono::seconds{minutes * 60};
    } catch (...) {
        return fallback;
    }
}

// The locks under `repo_root` that are stale. A lock a live process
// holds is never reported, and neither is one whose holder cannot be
// determined.
inline Report find_stale_locks(const fs::path& repo_root, std::chrono::seconds stale_after,
                        const HolderProbe& probe) {
    Report report;
    if (!probe) return report;

    const auto now = fs::file_time_type::clock::now();
    int probes = 0;
    for (const auto& path : lock_candidates(repo_root)) {
        std::error_code ec;
        if (!fs::is_regular_file(path, ec)) continue;

        const auto written = fs::last_write_time(path, ec);
        if (ec) continue;
        // A negative age means the mtime is in the future — clock skew
        // or a copied tree. Not evidence of staleness, so it falls
        // below any threshold and is skipped.
        const auto age = std::chrono::duration_cast<std::chrono::seconds>(now - written);
        // Age is checked before the holder probe: it is free, and it
        // keeps the common case of a healthy checkout from spawning a
        // process per lock.
        if (age < stale_after) continue;

        if (probes >= max_holder_probes) {
            report.truncated = true;
            break;
        }
        ++probes;
        if (probe(path) != Holder::not_held) continue;
        report.stale.push_back(StaleLock{path, age});
    }
    return report;
}

}  // namespace pulp::cli::git_lock
