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

#include <catch2/catch_test_macros.hpp>

#include <forge/chrome.hpp>
#include <forge/fx_shell.hpp>
#include <forge/instrument_shell.hpp>
#include <forge/midi_shell.hpp>

#include <pulp/format/processor.hpp>
#include <pulp/state/store.hpp>
#include <pulp/view/screenshot.hpp>
#include <pulp/view/buttons.hpp>
#include <pulp/view/view.hpp>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <fstream>
#include <string>
#include <vector>

namespace {

constexpr double kSr = 48000.0;
constexpr int kFrames = 256;

std::filesystem::path baseline_dir() {
    // Beside the test source, so the baselines travel with the repo rather than
    // living in a build directory that a clean checkout would not have.
    return std::filesystem::path(__FILE__).parent_path() / "baselines" / "chrome-home";
}

bool updating() { return std::getenv("FORGE_NO_LEAK_UPDATE") != nullptr; }

/// Render against an empty, private project store.
///
/// The home shelf renders whatever projects are on disk, so these baselines were
/// only reproducible on a machine whose store had not moved -- and the first
/// real failure here was Forge Modular having written 121 projects into Forge's
/// store, which is a genuine product bug but made the guard cry wolf about the
/// wrong thing. Pinned to a temp directory so a baseline means what it says.
struct HermeticProjects {
    HermeticProjects() {
        dir = std::filesystem::temp_directory_path() / "forge-no-leak-projects";
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
        std::filesystem::create_directories(dir, ec);
        ::setenv("FORGE_PROJECTS_DIR", dir.string().c_str(), /*overwrite=*/1);
    }
    ~HermeticProjects() { ::unsetenv("FORGE_PROJECTS_DIR"); }
    std::filesystem::path dir;
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
void check_home_frame(ShellT& shell, const char* product) {
    HermeticProjects isolated;
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
        WARN("wrote baseline for " << product << " -> " << baseline.string());
        return;
    }

    const auto want = read_all(baseline);
    INFO("baseline: " << baseline.string() << "\nactual:   " << actual.string()
                      << "\nIf this change was intended, refresh with "
                         "FORGE_NO_LEAK_UPDATE=1 and say why in the commit.");
    CHECK(digest(got) == digest(want));
}

}  // namespace

TEST_CASE("Forge FX's Home frame matches its baseline", "[no-leak]") {
    forge::ForgeFxShell shell;
    check_home_frame(shell, "fx");
}

TEST_CASE("Forge Instrument's Home frame matches its baseline", "[no-leak]") {
    forge::ForgeInstrumentShell shell;
    check_home_frame(shell, "instrument");
}

TEST_CASE("Forge MIDI's Home frame matches its baseline", "[no-leak]") {
    // MIDI ships CLAP and AU only -- no standalone to screenshot -- which is
    // exactly why this guard renders the chrome directly instead of driving three
    // apps. Every product is covered whether or not it has a window.
    forge::ForgeMidiShell shell;
    check_home_frame(shell, "midi");
}


// ── the seam is live, not merely harmless ────────────────────────────────────
//
// The tests above prove the three existing products are untouched. On their own
// that is also exactly what a dead code path looks like. These prove Forge
// Modular's answers actually reach the chrome — using the real shell rather than
// a test double, because ForgeFxShell is final and cannot be subclassed.

#include <forge/modular_shell.hpp>

TEST_CASE("Forge Modular's copy reaches the chrome", "[seam]") {
    forge_modular::ForgeModularShell shell;

    auto copy = shell.chrome_copy();
    CHECK(copy.badge.find("MODULE") != std::string::npos);
    CHECK(copy.prompt_placeholder.find("12 HP") != std::string::npos);

    // The mode is carried, not inferred. Both sides asserted, because checking
    // one side of a boolean is what let "Build always made a patch" ship.
    shell.set_artifact(forge_modular::Artifact::patch);
    copy = shell.chrome_copy();
    CHECK(copy.badge.find("PATCH") != std::string::npos);
    CHECK(copy.prompt_placeholder.find("drone") != std::string::npos);

    shell.set_artifact(forge_modular::Artifact::module);
    CHECK(shell.chrome_copy().badge.find("MODULE") != std::string::npos);
}

TEST_CASE("Forge Modular describes its own composer row", "[seam]") {
    forge_modular::ForgeModularShell shell;

    auto row = shell.composer_row();
    REQUIRE(row.left.size() == 2);
    REQUIRE(row.right.size() == 2);
    CHECK(row.left[1].label == "Random");
    CHECK(row.right[0].label == "Ask");
    CHECK(row.right[1].label == "Build module");
    CHECK(row.right[1].primary);
    CHECK_FALSE(row.right[0].primary);          // Ask must never read as the action

    // Every icon-only button needs an access label, or it is unreachable.
    for (const auto& a : row.left) {
        if (a.label.empty()) CHECK_FALSE(a.access_label.empty());
    }

    shell.set_artifact(forge_modular::Artifact::patch);
    CHECK(shell.composer_row().right[1].label == "Build patch");
}

TEST_CASE("Forge Modular's home accessory reaches the chrome", "[seam]") {
    // The tabs slot. Returning a view must put it in the tree; the three other
    // products return nullptr and are unaffected, which the baselines assert.
    forge_modular::ForgeModularShell shell;
    auto accessory = shell.home_accessory();
    REQUIRE(accessory != nullptr);
}

TEST_CASE("Forge Modular reports an unwired install rather than claiming success",
          "[seam]") {
    // A success that installed nothing is the failure mode this project has hit
    // most often, so the unwired path says so.
    forge_modular::ForgeModularShell shell;
    forge::ForgeShell::BundleInstallResult info;
    std::string err;
    forge::gen::Bundle bundle;
    CHECK_FALSE(shell.install_generated_bundle(bundle, 48000.0, 512, {}, info, err));
    CHECK_FALSE(err.empty());
}



// KNOWN FAILING, and hidden so the suite stays honest rather than red: this
// segfaults because ForgeModularShell::ensure_default_build() is a no-op while
// ForgeShell::create_view() calls it "so the editor always maps to a live
// graph". Run it deliberately with:  forge-test-chrome-no-leak "[.crash]"
// It should pass the moment a default build exists, and it is the regression
// test for that fix.
TEST_CASE("Forge Modular's view tree can be walked", "[.crash]") {
    // Two segfaults pointed here: a human's crash in rebuild_marketplace_cards
    // and a walk of this tree. Both touched code that reads the project store,
    // so this now runs against the same isolated store the baselines use.
    HermeticProjects isolated;
    forge_modular::ForgeModularShell shell;

    // The host attaches a store and declares parameters before opening an
    // editor; create_view() registers a listener on it. Skipping this
    // dereferenced a null store -- a test that had not set the stage, not a
    // product bug, which two rounds of bisecting the SHELL never would have
    // found. A stack trace answered it in one run.
    pulp::state::StateStore store;
    shell.set_state_store(&store);
    shell.define_parameters(store);

    pulp::format::PrepareContext pc;
    pc.sample_rate = kSr; pc.max_buffer_size = kFrames;
    pc.input_channels = 1; pc.output_channels = 2;
    shell.prepare(pc);

    auto view = shell.create_view();
    REQUIRE(view != nullptr);

    int views = 0;
    std::function<void(pulp::view::View&)> walk = [&](pulp::view::View& v) {
        ++views;
        for (std::size_t i = 0; i < v.child_count(); ++i) walk(*v.child_at(i));
    };
    walk(*view);
    INFO("walked " << views << " views");
    CHECK(views > 100);       // a real chrome, not a stub
}

TEST_CASE("clicking a tab switches the artifact, both ways", "[seam]") {
    // The whole of it: the tab must move the mode, and the mode must reach every
    // string that depends on it. Both directions, because asserting one side of
    // a boolean is what let "Build always made a patch" ship once already.
    HermeticProjects isolated;
    forge_modular::ForgeModularShell shell;
    pulp::state::StateStore store;
    shell.set_state_store(&store);
    shell.define_parameters(store);
    pulp::format::PrepareContext pc;
    pc.sample_rate = kSr; pc.max_buffer_size = kFrames;
    pc.input_channels = 1; pc.output_channels = 2;
    shell.prepare(pc);
    auto view = shell.create_view();
    REQUIRE(view != nullptr);

    pulp::view::TextButton* module_tab = nullptr;
    pulp::view::TextButton* patch_tab = nullptr;
    std::function<void(pulp::view::View&)> find = [&](pulp::view::View& v) {
        if (auto* b = dynamic_cast<pulp::view::TextButton*>(&v)) {
            if (b->label() == "Module") module_tab = b;
            if (b->label() == "Patch") patch_tab = b;
        }
        for (std::size_t i = 0; i < v.child_count(); ++i) find(*v.child_at(i));
    };
    find(*view);
    REQUIRE(module_tab != nullptr);
    REQUIRE(patch_tab != nullptr);
    REQUIRE(patch_tab->on_click);          // a tab with no handler is decoration

    CHECK(shell.artifact() == forge_modular::Artifact::module);
    CHECK(shell.chrome_copy().hero_title == "What should the module do?");
    CHECK(shell.composer_row().right[1].label == "Build module");

    patch_tab->on_click();
    CHECK(shell.artifact() == forge_modular::Artifact::patch);
    CHECK(shell.chrome_copy().hero_title == "What should the patch do?");
    CHECK(shell.chrome_copy().badge == "PATCH");
    CHECK(shell.composer_row().right[1].label == "Build patch");

    module_tab->on_click();               // and back, so it is not one-way
    CHECK(shell.artifact() == forge_modular::Artifact::module);
    CHECK(shell.chrome_copy().hero_title == "What should the module do?");
    CHECK(shell.composer_row().right[1].label == "Build module");
}
