// Release-discovery Slice 5 (#550 / parent #499) — unit tests for the
// auto/prompt/manual/off mode enforcement state machine.
//
// Covers:
//   - Mode parsing (tolerant of typos — defaults to Prompt)
//   - Snooze round-trip + expiration against an injected clock
//   - Pending-upgrade marker round-trip (JSON shape locked)
//   - Tombstone path composition + cleanup no-op on POSIX, file-removal
//     on any platform when the file actually exists
//   - Banner composition for manual + auto-staged + auto-completed
//     (locked verbatim, same policy as Slice 2's compose_banner)
//   - Decision helpers: decide_prompt_banner + should_stage_auto_download
//
// Filesystem is mocked via a per-test tmp directory — no real
// ~/.pulp is touched. Time enters via explicit epoch-seconds
// arguments so no test depends on wall-clock state.

#include <catch2/catch_test_macros.hpp>

#include "tools/cli/update_mode.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#ifdef _WIN32
#  include <process.h>
#  define pulp_getpid() _getpid()
#else
#  include <unistd.h>
#  define pulp_getpid() ::getpid()
#endif

namespace um = pulp::cli::update_mode;
namespace fs = std::filesystem;

namespace {

fs::path make_tmpdir(const std::string& tag) {
    auto base = fs::temp_directory_path() /
                ("pulp-test-update-mode-" + tag + "-" +
                 std::to_string(pulp_getpid()) + "-" +
                 std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::error_code ec;
    fs::create_directories(base, ec);
    return base;
}

}  // namespace

// ── Mode parse ──────────────────────────────────────────────────────────────

TEST_CASE("parse_mode recognizes the four documented values",
          "[cli][update-mode][issue-550]") {
    REQUIRE(um::parse_mode("auto")   == um::Mode::Auto);
    REQUIRE(um::parse_mode("prompt") == um::Mode::Prompt);
    REQUIRE(um::parse_mode("manual") == um::Mode::Manual);
    REQUIRE(um::parse_mode("off")    == um::Mode::Off);
}

TEST_CASE("parse_mode defaults to Prompt on unknown / empty input",
          "[cli][update-mode][issue-550]") {
    // Tolerant-on-typo is a hard requirement — a malformed config
    // should degrade to the safest behaviour, not fail the invocation.
    REQUIRE(um::parse_mode("")        == um::Mode::Prompt);
    REQUIRE(um::parse_mode("AUTO")    == um::Mode::Prompt);   // case-sensitive
    REQUIRE(um::parse_mode("default") == um::Mode::Prompt);
    REQUIRE(um::parse_mode("y")       == um::Mode::Prompt);
}

TEST_CASE("mode_name round-trips the enum back to strings",
          "[cli][update-mode][issue-550]") {
    REQUIRE(std::string(um::mode_name(um::Mode::Auto))   == "auto");
    REQUIRE(std::string(um::mode_name(um::Mode::Prompt)) == "prompt");
    REQUIRE(std::string(um::mode_name(um::Mode::Manual)) == "manual");
    REQUIRE(std::string(um::mode_name(um::Mode::Off))    == "off");
}

// ── Snooze ──────────────────────────────────────────────────────────────────

TEST_CASE("snooze round-trips through serialize/parse",
          "[cli][update-mode][issue-550]") {
    auto s = um::serialize_snooze(1'713'638'400);
    REQUIRE(um::parse_snooze(s) == 1'713'638'400);
}

TEST_CASE("parse_snooze tolerates whitespace and trailing content",
          "[cli][update-mode][issue-550]") {
    REQUIRE(um::parse_snooze("  1700000000  \n") == 1'700'000'000);
    REQUIRE(um::parse_snooze("1700000000 # comment") == 1'700'000'000);
    // Negative values — allowed (represents a snooze that expired in
    // the past; callers check against `now > expiry`).
    REQUIRE(um::parse_snooze("-5") == -5);
}

TEST_CASE("parse_snooze starts at the first numeric run",
          "[cli][update-mode][coverage][phase3]") {
    REQUIRE(um::parse_snooze("expires: 1700000000") == 1'700'000'000);
    REQUIRE(um::parse_snooze("abc -42 trailing 99") == -42);
    REQUIRE(um::parse_snooze("12 34") == 12);
}

TEST_CASE("parse_snooze returns 0 on malformed input (failure-open)",
          "[cli][update-mode][issue-550]") {
    REQUIRE(um::parse_snooze("") == 0);
    REQUIRE(um::parse_snooze("not a number") == 0);
    // "0" is valid — represents an always-expired snooze.
    REQUIRE(um::parse_snooze("0") == 0);
}

TEST_CASE("is_snooze_active honours the expiry boundary with a mocked clock",
          "[cli][update-mode][issue-550]") {
    auto dir = make_tmpdir("snooze-boundary");
    auto snooze = dir / "update-snooze";

    // Write a snooze that expires at now=1000 + 24h.
    REQUIRE(um::write_snooze(snooze, 1'000, 24));
    REQUIRE(fs::exists(snooze));

    // Mock clock: 12h in → still snoozed.
    REQUIRE(um::is_snooze_active(snooze, 1'000 + 12 * 3600));

    // Exactly at expiry → NOT active (strict-greater semantics).
    REQUIRE_FALSE(um::is_snooze_active(snooze, 1'000 + 24 * 3600));

    // Past expiry → not active.
    REQUIRE_FALSE(um::is_snooze_active(snooze, 1'000 + 48 * 3600));

    fs::remove_all(dir);
}

TEST_CASE("is_snooze_active returns false when file is missing",
          "[cli][update-mode][issue-550]") {
    auto dir = make_tmpdir("snooze-missing");
    REQUIRE_FALSE(um::is_snooze_active(dir / "nope", 1'000));
    fs::remove_all(dir);
}

TEST_CASE("clear_snooze removes the file and succeeds when absent",
          "[cli][update-mode][issue-550]") {
    auto dir = make_tmpdir("snooze-clear");
    auto snooze = dir / "update-snooze";

    // Missing → success.
    REQUIRE(um::clear_snooze(snooze));

    // Present → removed.
    REQUIRE(um::write_snooze(snooze, 1'000, 24));
    REQUIRE(fs::exists(snooze));
    REQUIRE(um::clear_snooze(snooze));
    REQUIRE_FALSE(fs::exists(snooze));

    fs::remove_all(dir);
}

TEST_CASE("write_snooze rejects non-positive hour values",
          "[cli][update-mode][issue-550]") {
    auto dir = make_tmpdir("snooze-negative");
    auto snooze = dir / "update-snooze";
    REQUIRE_FALSE(um::write_snooze(snooze, 1'000, 0));
    REQUIRE_FALSE(um::write_snooze(snooze, 1'000, -1));
    REQUIRE_FALSE(fs::exists(snooze));
    fs::remove_all(dir);
}

// ── Pending upgrade ─────────────────────────────────────────────────────────

TEST_CASE("pending-upgrade JSON round-trips through serialize/parse",
          "[cli][update-mode][issue-550]") {
    um::PendingUpgrade p;
    p.version = "0.31.0";
    p.staged_at_epoch_sec = 1'713'638'400;
    p.staged_binary_path = "/tmp/pulp-upgrade-0.31.0/pulp";

    auto s = um::serialize_pending_upgrade(p);
    auto parsed = um::parse_pending_upgrade(s);
    REQUIRE(parsed.has_value());
    REQUIRE(parsed->version == "0.31.0");
    REQUIRE(parsed->staged_at_epoch_sec == 1'713'638'400);
    REQUIRE(parsed->staged_binary_path == "/tmp/pulp-upgrade-0.31.0/pulp");
}

TEST_CASE("parse_pending_upgrade rejects markers without a version",
          "[cli][update-mode][issue-550]") {
    // Version-less markers are junk — if we accepted them we'd loop
    // forever trying to complete a "staged" upgrade to nothing.
    REQUIRE_FALSE(um::parse_pending_upgrade("{}").has_value());
    REQUIRE_FALSE(um::parse_pending_upgrade("").has_value());
    REQUIRE_FALSE(
        um::parse_pending_upgrade("{\"staged_at\":1}").has_value());
}

TEST_CASE("pending-upgrade file round-trips through write/read/clear",
          "[cli][update-mode][issue-550]") {
    auto dir = make_tmpdir("pending-upgrade");
    auto marker = dir / "pending-upgrade";

    REQUIRE_FALSE(um::read_pending_upgrade(marker).has_value());

    um::PendingUpgrade p;
    p.version = "0.99.0";
    p.staged_at_epoch_sec = 42;
    p.staged_binary_path = "/elsewhere/pulp";
    REQUIRE(um::write_pending_upgrade(marker, p));

    auto back = um::read_pending_upgrade(marker);
    REQUIRE(back.has_value());
    REQUIRE(back->version == "0.99.0");
    REQUIRE(back->staged_at_epoch_sec == 42);

    REQUIRE(um::clear_pending_upgrade(marker));
    REQUIRE_FALSE(fs::exists(marker));

    // clear-when-absent is a no-op success.
    REQUIRE(um::clear_pending_upgrade(marker));

    fs::remove_all(dir);
}

// #590 Codex P2 / wave-4 sweep: the auto-mode banner in pulp_cli.cpp
// must gate its "downloaded / will complete next invocation" notice
// on write_pending_upgrade succeeding. Exercise the underlying failure
// mode (non-existent file as the parent path) so a regression in the
// return-value contract surfaces immediately.
TEST_CASE("write_pending_upgrade returns false when the parent cannot be created",
          "[cli][update-mode][issue-590]") {
    auto dir = make_tmpdir("pending-upgrade-badparent");
    // Create a plain file at what would otherwise be a directory —
    // fs::create_directories refuses to stomp on a regular file, so
    // the atomic writer's ofstream then fails to open the .tmp file.
    auto blocker = dir / "blocker";
    { std::ofstream(blocker) << "not-a-dir"; }
    auto marker = blocker / "pending-upgrade";  // parent is a regular file

    um::PendingUpgrade p;
    p.version = "0.99.0";
    p.staged_at_epoch_sec = 42;

    REQUIRE_FALSE(um::write_pending_upgrade(marker, p));
    REQUIRE_FALSE(fs::exists(marker));

    fs::remove_all(dir);
}

// #590 Codex P2 / wave-4 sweep: the opportunistic tombstone sweep in
// maybe_complete_pending_upgrade() runs even when no pending marker
// exists (covers direct `pulp upgrade` flows on Windows). This test
// asserts the contract of the piece that sweep relies on:
// cleanup_tombstone must be safe to call unconditionally, regardless
// of marker state. The two cases already covered above (present /
// absent) are what the unconditional call in pulp_cli.cpp depends on.
TEST_CASE("cleanup_tombstone is safe to call after a no-marker fast path",
          "[cli][update-mode][issue-590]") {
    auto dir = make_tmpdir("tombstone-no-marker");
    auto exe = dir / "pulp";
    std::ofstream(exe) << "live";
    auto tomb = um::tombstone_path_for(exe);
    std::ofstream(tomb) << "old-bytes";

    // Simulate the "no pending marker" path: the caller must still
    // sweep the tombstone. Calling twice in succession (as would
    // happen if cleanup ran once for a completion event and once for
    // the unconditional sweep) must also be a no-op success.
    REQUIRE(um::cleanup_tombstone(exe));
    REQUIRE_FALSE(fs::exists(tomb));
    REQUIRE(um::cleanup_tombstone(exe));
    REQUIRE(fs::exists(exe));

    fs::remove_all(dir);
}

// ── Tombstone (Windows swap pattern) ────────────────────────────────────────

TEST_CASE("tombstone_path_for appends the .pulp.old suffix",
          "[cli][update-mode][issue-550]") {
    fs::path exe = "/usr/local/bin/pulp";
    REQUIRE(um::tombstone_path_for(exe) ==
            fs::path("/usr/local/bin/pulp.pulp.old"));

    // Windows-shape path with extension — suffix stacks after .exe.
    fs::path winexe = "C:/Users/dev/.pulp/bin/pulp.exe";
    REQUIRE(um::tombstone_path_for(winexe) ==
            fs::path("C:/Users/dev/.pulp/bin/pulp.exe.pulp.old"));
}

TEST_CASE("cleanup_tombstone is a no-op when no tombstone exists",
          "[cli][update-mode][issue-550]") {
    auto dir = make_tmpdir("tombstone-missing");
    auto exe = dir / "pulp";
    // Create only the "live" executable — no tombstone.
    std::ofstream(exe) << "fake";
    REQUIRE(um::cleanup_tombstone(exe));
    REQUIRE(fs::exists(exe));  // didn't touch the real file
    fs::remove_all(dir);
}

TEST_CASE("cleanup_tombstone deletes the .pulp.old file when present",
          "[cli][update-mode][issue-550]") {
    auto dir = make_tmpdir("tombstone-present");
    auto exe = dir / "pulp";
    std::ofstream(exe) << "live";
    auto tomb = um::tombstone_path_for(exe);
    std::ofstream(tomb) << "old-bytes";
    REQUIRE(fs::exists(tomb));

    REQUIRE(um::cleanup_tombstone(exe));
    REQUIRE_FALSE(fs::exists(tomb));
    REQUIRE(fs::exists(exe));  // live binary still in place

    fs::remove_all(dir);
}

// ── Banner composition (locked verbatim) ────────────────────────────────────

TEST_CASE("compose_manual_notice matches the locked Section A shape",
          "[cli][update-mode][issue-550]") {
    auto b = um::compose_manual_notice("0.30.0", "0.31.0");
    // Locked verbatim — any change to this string must update the
    // test in the same PR. Same policy as compose_banner (Slice 2).
    REQUIRE(b ==
            "Pulp v0.31.0 available (you have v0.30.0). "
            "Run `pulp upgrade` when you're ready.");
}

TEST_CASE("compose_auto_staged_notice matches the locked shape",
          "[cli][update-mode][issue-550]") {
    REQUIRE(um::compose_auto_staged_notice("0.31.0") ==
            "Pulp v0.31.0 downloaded. "
            "The upgrade will complete on your next `pulp` invocation.");
}

TEST_CASE("compose_auto_completed_notice matches the locked shape",
          "[cli][update-mode][issue-550]") {
    REQUIRE(um::compose_auto_completed_notice("0.31.0") ==
            "Pulp CLI upgraded to v0.31.0. "
            "Run `pulp upgrade --notes` to see what changed.");
}

// ── decide_prompt_banner ────────────────────────────────────────────────────

TEST_CASE("decide_prompt_banner stays silent outside prompt mode",
          "[cli][update-mode][issue-550]") {
    for (auto m : {um::Mode::Auto, um::Mode::Manual, um::Mode::Off}) {
        auto d = um::decide_prompt_banner(
            m, "0.30.0", "0.31.0", false, false);
        REQUIRE_FALSE(d.show_banner);
    }
}

TEST_CASE("decide_prompt_banner shows once per new version",
          "[cli][update-mode][issue-550]") {
    auto d = um::decide_prompt_banner(
        um::Mode::Prompt, "0.30.0", "0.31.0", false, false);
    REQUIRE(d.show_banner);

    // Second invocation with banner_already_shown_this_cycle=true →
    // silent. This preserves Slice 2's banner-suppression bookkeeping.
    auto d2 = um::decide_prompt_banner(
        um::Mode::Prompt, "0.30.0", "0.31.0", true, false);
    REQUIRE_FALSE(d2.show_banner);
}

TEST_CASE("decide_prompt_banner stays silent while snooze is active",
          "[cli][update-mode][issue-550]") {
    auto d = um::decide_prompt_banner(
        um::Mode::Prompt, "0.30.0", "0.31.0", false, true);
    REQUIRE_FALSE(d.show_banner);
}

TEST_CASE("decide_prompt_banner stays silent when already on latest",
          "[cli][update-mode][issue-550]") {
    auto d = um::decide_prompt_banner(
        um::Mode::Prompt, "0.31.0", "0.31.0", false, false);
    REQUIRE_FALSE(d.show_banner);

    // Caller passes an empty latest when no release known → no banner.
    auto d2 = um::decide_prompt_banner(
        um::Mode::Prompt, "0.31.0", "", false, false);
    REQUIRE_FALSE(d2.show_banner);
}

// ── should_stage_auto_download ──────────────────────────────────────────────

TEST_CASE("should_stage_auto_download fires only in auto mode",
          "[cli][update-mode][issue-550]") {
    for (auto m : {um::Mode::Prompt, um::Mode::Manual, um::Mode::Off}) {
        REQUIRE_FALSE(um::should_stage_auto_download(
            m, "0.30.0", "0.31.0", std::nullopt));
    }
    REQUIRE(um::should_stage_auto_download(
        um::Mode::Auto, "0.30.0", "0.31.0", std::nullopt));
}

TEST_CASE("should_stage_auto_download does not re-stage the same version",
          "[cli][update-mode][issue-550]") {
    um::PendingUpgrade existing;
    existing.version = "0.31.0";
    REQUIRE_FALSE(um::should_stage_auto_download(
        um::Mode::Auto, "0.30.0", "0.31.0", existing));

    // Same pulp, but a NEWER latest than the staged version → re-stage.
    REQUIRE(um::should_stage_auto_download(
        um::Mode::Auto, "0.30.0", "0.32.0", existing));
}

TEST_CASE("should_stage_auto_download respects up-to-date state",
          "[cli][update-mode][issue-550]") {
    REQUIRE_FALSE(um::should_stage_auto_download(
        um::Mode::Auto, "0.31.0", "0.31.0", std::nullopt));
    REQUIRE_FALSE(um::should_stage_auto_download(
        um::Mode::Auto, "0.31.0", "", std::nullopt));
}
