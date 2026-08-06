// What a failed run tells a person, and whether they can copy it.
//
// A real run failed five times and the user's report was "i didn't even get a
// way to copy the prompt output". Three things were wrong and all of them fail
// in the direction that reads as working: the generator's closing block was cut
// to one line, the run log existed at a path nothing ever named, and the text
// on screen lives in Labels a mouse cannot select.
//
// So these assert CONTENT, and the log-reading half is fed a real file on disk
// rather than a hand-built line vector -- the monitor's parsing is where a
// classification drifts, and a test that skips it proves the rules and not the
// pipeline.

#include "forge/build_monitor.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using forge_modular::BuildLine;
using forge_modular::BuildMonitor;
using forge_modular::BuildOutcome;
using forge_modular::RunFailure;

namespace {

// The tail of a real handover, as patch.py prints it.
constexpr const char* kHandover =
    "  asking the model (retry 4)\xE2\x80\xA6\n"
    "  not a sequenced-voice patch yet:\n"
    "    - the sequencer's gate has to fire an envelope, or every step runs "
    "together\n"
    "      this patch's sequencer CANNOT send it, however it is wired: "
    "CVfunk/PentaSequencer (its outputs are A, B, C, D, E)\n"
    "      installed jacks that can send it: CVfunk/StepWave out1 "
    "'Sequencer Gate'\n"
    "  built 6 modules, 7 cables \xE2\x86\x92 /patches/bass-unfinished.vcv\n"
    "\n"
    "  gave up after 5 attempt(s). Handing over the best one anyway.\n"
    "\n"
    "  you asked for: a sequenced bass line\n"
    "  this patch does not meet that: it is not a sequenced-voice patch.\n"
    "    - the sequencer's gate has to fire an envelope, or every step runs "
    "together\n"
    "  OPEN IT AND LISTEN. It lints clean and every module in it is one\n"
    "    the patch, attempt 3: /patches/bass-unfinished.vcv\n"
    "    every attempt behind it: /attempts/20260805-101500-4242\n"
    "  Copy this whole block if you are reporting it.\n";

/// A monitor that has read `text` from a real file, which is the path the app
/// takes. Returns the temp file's path so a caller can name it.
std::string feed(BuildMonitor& m, const std::string& text) {
    auto path = std::filesystem::temp_directory_path() /
                ("forge-failure-" + std::to_string(::rand()) + ".log");
    { std::ofstream out(path, std::ios::binary); out << text; }
    m.watch(path.string());
    m.poll();
    return path.string();
}

std::string joined(const std::vector<std::string>& lines) {
    std::string out;
    for (const auto& l : lines) out += l + "\n";
    return out;
}

}  // namespace

TEST_CASE("a handover still ends the run", "[modular][failure]") {
    BuildMonitor m;
    const auto path = feed(m, kHandover);

    // It handed a patch over AND it failed. Both, and in that order of
    // precedence: reporting `done` would present an unfinished patch as the
    // thing that was asked for, and leaving it `running` would leave the app
    // watching a dead build forever.
    REQUIRE(m.outcome() == BuildOutcome::failed);
    std::filesystem::remove(path);
}

TEST_CASE("the closing block is the whole block", "[modular][failure]") {
    BuildMonitor m;
    const auto path = feed(m, kHandover);
    const auto block = joined(m.closing_block());

    // The line that ended it, and every line after it. `headline()` returns
    // only the first of these, and that was the entire failure report.
    REQUIRE(block.find("gave up after 5 attempt") != std::string::npos);
    REQUIRE(block.find("a sequenced bass line") != std::string::npos);
    REQUIRE(block.find("not a sequenced-voice patch") != std::string::npos);
    REQUIRE(block.find("bass-unfinished.vcv") != std::string::npos);
    REQUIRE(block.find("/attempts/20260805-101500-4242") != std::string::npos);

    // And NOT the whole transcript. A block that starts at the first line is
    // not a closing block, it is the log -- and the retries above the ending
    // are what a person has to scroll past to reach the point.
    REQUIRE(block.find("asking the model") == std::string::npos);
    REQUIRE(block.find("retry 4") == std::string::npos);
    std::filesystem::remove(path);
}

TEST_CASE("a run that has not ended has no closing block",
          "[modular][failure]") {
    BuildMonitor m;
    const auto path = feed(m, "  asking the model\xE2\x80\xA6\n"
                              "  audio out: MacBook Pro Speakers\n");
    REQUIRE(m.outcome() == BuildOutcome::running);
    REQUIRE(m.closing_block().empty());
    std::filesystem::remove(path);
}

TEST_CASE("the closing block starts at the LAST ending", "[modular][failure]") {
    // A run can print an ending-shaped line and carry on -- the generator
    // retries a model call that failed. Starting at the first one hands back
    // most of the transcript as though it were the verdict.
    BuildMonitor m;
    const auto path = feed(m,
        std::string("  model call failed: connection reset\n"
                    "  asking the model (retry 1)\xE2\x80\xA6\n") + kHandover);
    const auto block = joined(m.closing_block());
    REQUIRE(block.find("gave up after") != std::string::npos);
    REQUIRE(block.find("connection reset") == std::string::npos);
    std::filesystem::remove(path);
}

TEST_CASE("the report carries everything needed to report it",
          "[modular][failure]") {
    RunFailure f;
    f.app_version = "0.14.2";
    f.request = "a sequenced bass line";
    f.headline = "gave up after 5 attempt(s).";
    f.artifact = "/patches/bass-unfinished.vcv";
    f.log_path = "/runs/20260805-101500.log";
    f.log_text = kHandover;

    const auto text = forge_modular::format_failure_report(f);
    REQUIRE(text.find("0.14.2") != std::string::npos);
    REQUIRE(text.find("a sequenced bass line") != std::string::npos);
    REQUIRE(text.find("gave up after 5 attempt") != std::string::npos);
    REQUIRE(text.find("/patches/bass-unfinished.vcv") != std::string::npos);
    REQUIRE(text.find("/runs/20260805-101500.log") != std::string::npos);
    // The transcript WHOLE, which is the part that was asked for by name.
    // A summary of a failure is what the user already had.
    REQUIRE(text.find("CVfunk/StepWave out1 'Sequencer Gate'") !=
            std::string::npos);

    // The patch before the log. It is the one thing here a person can act on
    // in ten seconds, and under a log path it gets missed.
    REQUIRE(text.find("/patches/bass-unfinished.vcv") <
            text.find("/runs/20260805-101500.log"));

    // Product copy: an em-dash reads as machine-written.
    REQUIRE(text.find("\xE2\x80\x94") == std::string::npos);
}

TEST_CASE("a report with nothing to say says nothing false",
          "[modular][failure]") {
    // A failure with no artifact and no log is a real state -- the generator
    // died before writing either. The report must not invent a path, and an
    // empty field must not become an empty line claiming a patch exists.
    RunFailure f;
    f.app_version = "0.14.2";
    f.headline = "the Rack SDK is not installed";
    const auto text = forge_modular::format_failure_report(f);
    REQUIRE(text.find("the Rack SDK is not installed") != std::string::npos);
    REQUIRE(text.find("Unfinished patch:") == std::string::npos);
    REQUIRE(text.find("Run log:") == std::string::npos);
    REQUIRE(text.find("----- the run -----") == std::string::npos);
}

TEST_CASE("a handed-over patch is still findable in the log",
          "[modular][failure]") {
    // The app locates the artifact by reading the generator's own finish line
    // out of the transcript. A handover prints that line and then fails, so
    // the parse has to survive a failed outcome -- otherwise the patch is on
    // disk with nothing on screen able to open it, which is the bug being
    // fixed showing up one layer higher.
    BuildMonitor m;
    const auto path = feed(m, kHandover);
    bool found = false;
    for (const auto& line : m.lines()) {
        if (line.text.find("bass-unfinished.vcv") != std::string::npos &&
            line.text.find("\xE2\x86\x92") != std::string::npos) {
            found = true;
            REQUIRE(line.kind == BuildLine::Kind::success);
        }
    }
    REQUIRE(found);
    std::filesystem::remove(path);
}
