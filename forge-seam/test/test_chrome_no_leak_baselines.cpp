// Private translation unit for the pixel-exact Chrome no-leak guard.

// The no-leak guard: a change made for one Forge product must not alter another.
//
// Forge varies ONE chrome across three products through ShellKind. That sharing
// is the point -- it is what keeps the products looking like one family -- but it
// also means a change intended for one can silently move another. Renders exist
// in test_chrome.cpp today; nothing compares them to anything, so a drift is
// invisible until somebody notices by eye.
//
// This renders each product's Home frame and compares it to a committed
// baseline. Adding a fourth product, or touching shared chrome for any reason,
// must leave these three byte-identical.
//
// Refresh a baseline deliberately, never casually:
//     FORGE_NO_LEAK_UPDATE=1 ./forge-test-chrome-no-leak
// and commit the changed PNGs with the reason in the message.

#include "forge/installation.hpp"
#include "forge/module_catalog.hpp"
#include "forge/module_summary.hpp"
#include "forge/patch_loader.hpp"
#include "forge/portmap.hpp"
#include "forge/rack_preview.hpp"
#include <ImageIO/ImageIO.h>
#include <catch2/catch_test_macros.hpp>

#include <forge/chrome.hpp>
#include <forge/design_tokens.hpp>
#include <forge/rack_layout.hpp>
#include <forge/patch_loader.hpp>
#include <forge/process_engine.hpp>
#include <forge/rack_preview.hpp>

#include <catch2/catch_approx.hpp>

using Catch::Approx;
#include <forge/fx_shell.hpp>
#include <forge/instrument_shell.hpp>
#include <forge/midi_shell.hpp>

#include <pulp/format/processor.hpp>
#include <pulp/state/store.hpp>
#include <pulp/view/frame_clock.hpp>
#include <pulp/view/screenshot.hpp>
#include <pulp/canvas/svg_dom_cache.hpp>
#include "forge/project_store.hpp"
#include <pulp/view/buttons.hpp>
#include <pulp/view/view.hpp>

#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <signal.h>
#include <unistd.h>
#include <filesystem>
#include <functional>
#include <fstream>
#include <algorithm>
#include <set>
#include <string>
#include <array>
#include <chrono>
#include <cmath>
#include <thread>
#include <vector>

namespace {

constexpr double kSr = 48000.0;
constexpr int kFrames = 256;

bool process_is_running(pid_t pid) {
    std::string state;
    if (pid <= 1 || forge_modular::ProcessEngine::run_tool(
                        "ps", {"-p", std::to_string(pid), "-o", "state="},
                        state) != 0)
        return false;
    const auto first = state.find_first_not_of(" \t\r\n");
    return first != std::string::npos && state[first] != 'Z';
}

std::filesystem::path baseline_dir() {
    // Beside the test source, so the baselines travel with the repo rather than
    // living in a build directory that a clean checkout would not have.
    return std::filesystem::path(__FILE__).parent_path() / "baselines" / "chrome-home";
}

bool updating() { return std::getenv("FORGE_NO_LEAK_UPDATE") != nullptr; }

#ifndef FORGE_PULP_SDK_VERSION
#define FORGE_PULP_SDK_VERSION "unknown"
#endif

/// Which SDK this binary renders with.
std::string rendering_sdk() { return FORGE_PULP_SDK_VERSION; }

/// Which SDK the committed baselines were recorded with, written beside them.
///
/// These are pixel-exact digests, and the renderer is not ours -- so a red
/// baseline has two completely different causes that a digest cannot tell
/// apart. Either the chrome changed, which is what the guard is FOR, or the
/// SDK's rasterisation changed under it, which makes the image stale. They are
/// repaired in opposite directions: one is a bug to fix, the other is a
/// picture to retake. Three of these sat red for days because nothing on the
/// failure said which had happened, and the standing risk of that is somebody
/// re-recording a real regression to make a test green.
std::string blessed_sdk() {
    std::ifstream f(baseline_dir() / "BLESSED-WITH.txt");
    std::string v;
    std::getline(f, v);
    while (!v.empty() && (v.back() == '\n' || v.back() == '\r' || v.back() == ' '))
        v.pop_back();
    return v;
}

/// What to tell somebody staring at a failed digest.
std::string provenance_note() {
    const auto was = blessed_sdk();
    const auto now = rendering_sdk();
    if (was.empty())
        return "The baselines do not say which SDK recorded them, so nothing "
               "here can tell a stale image from a real change.";
    if (was == now)
        return "Recorded and rendered on the same SDK (" + now +
               "), so this is a CHANGE IN THE CHROME, not toolchain drift. Fix "
               "the render.";
    return "Recorded on Pulp " + was + ", rendered on Pulp " + now +
           ". The renderer moved under these images: look at the diff and, if "
           "it is only the toolchain, re-record with FORGE_NO_LEAK_UPDATE=1 "
           "and update BLESSED-WITH.txt in the same commit.";
}

/// A patch the generator really produced, or the one that travels with these
/// tests.
///
/// Four tests hardcoded "/tmp/ambient-drone.vcv" and skipped when it was
/// missing — which it has been since macOS cleared /tmp, so all four have been
/// reporting as passes while running nothing. The generator writes into the
/// installed pack, and a recorded patch sits beside these tests, so there is
/// no need to depend on a temp file that outlived nothing.
std::string a_real_patch() {
    const char* home = std::getenv("HOME");
    const std::filesystem::path dir =
        std::string(home ? home : ".") +
        "/Library/Application Support/Forge Modular/examples/forge-modular/patches";
    std::error_code ec;
    std::filesystem::path newest;
    std::filesystem::file_time_type best{};
    if (std::filesystem::exists(dir, ec)) {
        for (const auto& e : std::filesystem::directory_iterator(dir, ec)) {
            if (e.path().extension() != ".vcv") continue;
            // `_`-prefixed files are not patches, the same convention the
            // module manifests use. A CARTOG scan sheet — 52 modules, no
            // cables, no audio path — was dropped in this directory and became
            // "the newest patch", and three tests that read the newest one
            // started failing for a patch nobody generated.
            if (!e.path().filename().empty() &&
                e.path().filename().string()[0] == '_') continue;
            const auto when = std::filesystem::last_write_time(e, ec);
            if (newest.empty() || when > best) { newest = e.path(); best = when; }
        }
    }
    if (!newest.empty()) return newest.string();
    const auto travelling = baseline_dir().parent_path() / "app-generated-patch.vcv";
    return std::filesystem::exists(travelling) ? travelling.string() : std::string{};
}


/// Render against an empty, private project store.
///
/// The home shelf renders whatever projects are on disk, so these baselines were
/// only reproducible on a machine whose store had not moved -- and the first
/// real failure here was Forge Modular having written 121 projects into Forge's
/// store, which is a genuine product bug but made the guard cry wolf about the
/// wrong thing. Pinned to a temp directory so a baseline means what it says.
/// An empty, private store for everything the Home screen reads.
///
/// Pinning only FORGE_PROJECTS_DIR was not enough: the shelf also renders
/// MARKETPLACE listings, whose titles come from a directory the fixture did not
/// control. The guard failed three times on cards drifting between "Untitled",
/// "Split Stereo Echo" and "Dual Time Delay" while Forge's chrome was untouched
/// -- and a guard that cries wolf on its own fixtures is one people stop
/// reading. Both roots are pinned, and both are wiped, so a Home frame is a
/// function of the code and nothing else.
struct HermeticProjects {
    HermeticProjects() {
        const auto base = std::filesystem::temp_directory_path() / "forge-no-leak";
        dir = base / "projects";
        market = base / "marketplace";
        std::error_code ec;
        std::filesystem::remove_all(base, ec);
        std::filesystem::create_directories(dir, ec);
        std::filesystem::create_directories(market, ec);
        ::setenv("FORGE_PROJECTS_DIR", dir.string().c_str(), /*overwrite=*/1);
        ::setenv("FORGE_MARKETPLACE_DIR", market.string().c_str(), /*overwrite=*/1);
    }
    ~HermeticProjects() {
        ::unsetenv("FORGE_PROJECTS_DIR");
        ::unsetenv("FORGE_MARKETPLACE_DIR");
    }
    std::filesystem::path dir;
    std::filesystem::path market;
};

std::vector<unsigned char> read_all(const std::filesystem::path& p) {
    std::ifstream f(p, std::ios::binary);
    return {std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
}

/// A short digest of a file, for the failure message.
///
/// Comparing the byte vectors directly is correct and unreadable: Catch2 prints
/// both on failure, so a one-pixel drift buried the real message under thousands
/// of characters of PNG. Two hashes and two paths is what a person can act on.
std::string digest(const std::vector<unsigned char>& bytes) {
    std::uint64_t h = 1469598103934665603ull;          // FNV-1a
    for (unsigned char c : bytes) {
        h ^= c;
        h *= 1099511628211ull;
    }
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%016llx", static_cast<unsigned long long>(h));
    return std::string(buf) + " (" + std::to_string(bytes.size()) + " bytes)";
}

/// Render a shell's Home frame and hold it against its baseline.
///
/// Byte comparison rather than a tolerance. A tolerance invites the question of
/// how much drift is acceptable, and for "did this change another product" the
/// answer is none. Renders are deterministic here -- same backend, same size,
/// same scale -- so byte-equality is achievable and anything else is a real
/// change worth looking at.
template <typename ShellT>
void check_home_frame(const char* product) {
    // The fixture FIRST, then the shell. Constructing the shell before pinning
    // the store let it capture the real paths in its constructor, so the frame
    // rendered whatever projects happened to exist on this machine -- which is
    // why the guard kept failing on card titles nobody had touched.
    HermeticProjects isolated;
    ShellT shell;
    pulp::format::PrepareContext pc;
    pc.sample_rate = kSr;
    pc.max_buffer_size = kFrames;
    pc.input_channels = 1;
    pc.output_channels = 2;
    shell.prepare(pc);

    auto view = shell.create_view();
    REQUIRE(view != nullptr);

    auto* chrome = shell.chrome();
    REQUIRE(chrome != nullptr);
    REQUIRE(chrome->mode() == forge::ForgeChrome::Mode::Home);

    std::error_code ec;
    std::filesystem::create_directories(baseline_dir(), ec);
    const auto baseline = baseline_dir() / (std::string(product) + "-home.png");
    const auto actual = std::filesystem::temp_directory_path() /
                        (std::string("no-leak-") + product + "-home.png");

    REQUIRE(pulp::view::render_to_file(
        *view, forge::ForgeChrome::kDesignWidth, forge::ForgeChrome::kDesignHeight,
        actual.string(), /*scale=*/1.0f, pulp::view::ScreenshotBackend::skia));

    // A blank frame is not a passing frame. Without this a render that produced
    // nothing would match an equally empty baseline and report success.
    const auto got = read_all(actual);
    INFO("product: " << product << "  bytes: " << got.size());
    REQUIRE(got.size() > 20000);

    if (updating() || !std::filesystem::exists(baseline)) {
        std::filesystem::copy_file(
            actual, baseline, std::filesystem::copy_options::overwrite_existing, ec);
        std::ofstream(baseline_dir() / "BLESSED-WITH.txt") << rendering_sdk() << "\n";
        WARN("wrote baseline for " << product << " -> " << baseline.string()
                                   << "  (Pulp " << rendering_sdk() << ")");
        return;
    }

    const auto want = read_all(baseline);
    INFO("baseline: " << baseline.string() << "\nactual:   " << actual.string()
                      << "\n" << provenance_note());
    CHECK(digest(got) == digest(want));
}

}  // namespace

TEST_CASE("Forge FX's Home frame matches its baseline", "[no-leak]") {
    check_home_frame<forge::ForgeFxShell>("fx");
}

TEST_CASE("Forge Instrument's Home frame matches its baseline", "[no-leak]") {
    check_home_frame<forge::ForgeInstrumentShell>("instrument");
}

TEST_CASE("Forge MIDI's Home frame matches its baseline", "[no-leak]") {
    // MIDI ships CLAP and AU only -- no standalone to screenshot -- which is
    // exactly why this guard renders the chrome directly instead of driving three
    // apps. Every product is covered whether or not it has a window.
    check_home_frame<forge::ForgeMidiShell>("midi");
}
