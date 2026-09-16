// Shell-out coverage for the `pulp render` verb's tail option.
//
// The renderer's own tail behavior is proven in the offline-render suite against
// the library. What can only be proven against the built binary is the argument
// surface: that the option is spelled the way the usage text advertises, that a
// missing or unparsable value is refused with its own message on stderr rather
// than silently rendering a shorter file, and that the refusal exits non-zero.

#include "test_cli_shellout_helpers.hpp"

using namespace pulp::platform;
namespace fs = std::filesystem;
using namespace pulp_test_cli;

namespace {

bool contains(const std::string& haystack, const std::string& needle) {
    return haystack.find(needle) != std::string::npos;
}

} // namespace

TEST_CASE("pulp render advertises the tail option in its usage", "[cli][render][tail]") {
    if (!binary_exists()) { SUCCEED("skipped: pulp not built"); return; }

    auto r = run_pulp({"render", "--help"});

    REQUIRE_FALSE(r.timed_out);
    REQUIRE(r.exit_code == 0);
    REQUIRE(contains(r.stdout_output, "--tail-frames"));
}

TEST_CASE("pulp render rejects a tail option with no value", "[cli][render][tail]") {
    if (!binary_exists()) { SUCCEED("skipped: pulp not built"); return; }

    auto r = run_pulp({"render", "project.json", "--out", "out.wav", "--tail-frames"});

    REQUIRE_FALSE(r.timed_out);
    REQUIRE(r.exit_code == 2);
    REQUIRE(contains(r.stderr_output, "--tail-frames requires a value"));
}

TEST_CASE("pulp render rejects an unparsable tail rather than rendering without one",
          "[cli][render][tail]") {
    if (!binary_exists()) { SUCCEED("skipped: pulp not built"); return; }

    // Each of these would read as zero under a parser that stops at the first
    // bad character, silently producing the untailed file the caller did not ask
    // for. The refusal has to come from the parse, not from the render.
    for (const auto* value : {"abc", "12x", "-1", "1.5", "0x10"}) {
        auto r = run_pulp({"render", "project.json", "--out", "out.wav", "--tail-frames", value});

        INFO("tail value: " << value);
        REQUIRE_FALSE(r.timed_out);
        REQUIRE(r.exit_code == 2);
        REQUIRE(contains(r.stderr_output, "--tail-frames must be between 0 and 4294967295"));
    }
}

TEST_CASE("pulp render rejects a tail wider than the renderer accepts", "[cli][render][tail]") {
    if (!binary_exists()) { SUCCEED("skipped: pulp not built"); return; }

    // One past the option's width. Truncating to the low 32 bits would render a
    // tail of zero and report success, which is the failure this rejects.
    auto r = run_pulp({"render", "project.json", "--out", "out.wav", "--tail-frames",
                       "4294967296"});

    REQUIRE_FALSE(r.timed_out);
    REQUIRE(r.exit_code == 2);
    REQUIRE(contains(r.stderr_output, "--tail-frames must be between 0 and 4294967295"));
}

TEST_CASE("pulp render still refuses an unknown option next to a valid tail",
          "[cli][render][tail]") {
    if (!binary_exists()) { SUCCEED("skipped: pulp not built"); return; }

    auto r = run_pulp({"render", "project.json", "--out", "out.wav", "--tail-frames", "512",
                       "--tail"});

    REQUIRE_FALSE(r.timed_out);
    REQUIRE(r.exit_code == 2);
    REQUIRE(contains(r.stderr_output, "unknown option: --tail"));
}
