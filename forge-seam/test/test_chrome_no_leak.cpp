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
#include <filesystem>
#include <functional>
#include <fstream>
#include <algorithm>
#include <set>
#include <string>
#include <array>
#include <cmath>
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


// ── the seam is live, not merely harmless ────────────────────────────────────
//
// The tests above prove the three existing products are untouched. On their own
// that is also exactly what a dead code path looks like. These prove Forge
// Modular's answers actually reach the chrome — using the real shell rather than
// a test double, because ForgeFxShell is final and cannot be subclassed.

#include <forge/build_monitor.hpp>
#include <forge/mention_overlay.hpp>
#include <forge/modular_shell.hpp>
#include <forge/module_catalog.hpp>

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
    CHECK(shell.composer_row().right[1].label == "Create patch");
}

TEST_CASE("Forge Modular's home accessory reaches the chrome", "[seam]") {
    // The tabs slot. Returning a view must put it in the tree; the three other
    // products return nullptr and are unaffected, which the baselines assert.
    forge_modular::ForgeModularShell shell;
    auto accessory = shell.home_accessory();
    REQUIRE(accessory != nullptr);
}

namespace {

/// A decoded frame, for comparing two renders pixel by pixel.
///
/// count_pixels_near answers "is this colour present", which cannot tell a mark
/// drawn in the right place from the same mark drawn over everything. Where a
/// change lands is the thing worth asserting, so this hands back the pixels.
struct Decoded {
    int width = 0, height = 0;
    std::vector<std::uint8_t> pixels;   ///< RGBA, row-major from the top
};

Decoded decode_rgba(const std::vector<std::uint8_t>& png) {
    Decoded out;
    if (png.empty()) return out;
    auto* data = CFDataCreate(nullptr, png.data(), static_cast<CFIndex>(png.size()));
    auto* src = CGImageSourceCreateWithData(data, nullptr);
    if (src && CGImageSourceGetCount(src) > 0) {
        auto* img = CGImageSourceCreateImageAtIndex(src, 0, nullptr);
        if (img) {
            const std::size_t w = CGImageGetWidth(img), h = CGImageGetHeight(img);
            out.width = static_cast<int>(w);
            out.height = static_cast<int>(h);
            out.pixels.assign(w * h * 4, 0);
            auto* cs = CGColorSpaceCreateDeviceRGB();
            auto* ctx = CGBitmapContextCreate(out.pixels.data(), w, h, 8, w * 4, cs,
                                              kCGImageAlphaPremultipliedLast);
            if (ctx) {
                CGContextDrawImage(ctx, CGRectMake(0, 0, (CGFloat)w, (CGFloat)h), img);
                CGContextRelease(ctx);
            } else {
                out = Decoded{};
            }
            CGColorSpaceRelease(cs);
            CGImageRelease(img);
        }
    }
    if (src) CFRelease(src);
    CFRelease(data);
    return out;
}

/// Count pixels close to a colour. CoreGraphics rather than a new dependency --
/// the harness already decodes this way.
std::size_t count_pixels_near(const std::vector<std::uint8_t>& png,
                              std::uint8_t r, std::uint8_t g, std::uint8_t b) {
    if (png.empty()) return 0;
    auto* data = CFDataCreate(nullptr, png.data(), static_cast<CFIndex>(png.size()));
    auto* src = CGImageSourceCreateWithData(data, nullptr);
    std::size_t hits = 0;
    if (src && CGImageSourceGetCount(src) > 0) {
        auto* img = CGImageSourceCreateImageAtIndex(src, 0, nullptr);
        if (img) {
            const std::size_t w = CGImageGetWidth(img), h = CGImageGetHeight(img);
            std::vector<std::uint8_t> rgba(w * h * 4, 0);
            auto* cs = CGColorSpaceCreateDeviceRGB();
            auto* ctx = CGBitmapContextCreate(rgba.data(), w, h, 8, w * 4, cs,
                                              kCGImageAlphaPremultipliedLast);
            if (ctx) {
                CGContextDrawImage(ctx, CGRectMake(0, 0, (CGFloat)w, (CGFloat)h), img);
                for (std::size_t i = 0; i + 3 < rgba.size(); i += 4) {
                    if (std::abs(int(rgba[i]) - int(r)) < 24 &&
                        std::abs(int(rgba[i + 1]) - int(g)) < 24 &&
                        std::abs(int(rgba[i + 2]) - int(b)) < 24) ++hits;
                }
                CGContextRelease(ctx);
            }
            CGColorSpaceRelease(cs);
            CGImageRelease(img);
        }
    }
    if (src) CFRelease(src);
    CFRelease(data);
    return hits;
}

/// How far the drawing actually reaches, as a fraction of the canvas.
///
/// Ink, not bounds: a view's bounds are whatever they were set to, so they
/// prove nothing about whether anything was drawn out there. Anything
/// noticeably different from the colour of the far corner counts.
std::pair<double, double> ink_extent(const std::vector<std::uint8_t>& png) {
    if (png.empty()) return {0.0, 0.0};
    auto* data = CFDataCreate(nullptr, png.data(), static_cast<CFIndex>(png.size()));
    auto* src = CGImageSourceCreateWithData(data, nullptr);
    double fx = 0, fy = 0;
    if (src && CGImageSourceGetCount(src) > 0) {
        auto* img = CGImageSourceCreateImageAtIndex(src, 0, nullptr);
        if (img) {
            const std::size_t w = CGImageGetWidth(img), h = CGImageGetHeight(img);
            std::vector<std::uint8_t> rgba(w * h * 4, 0);
            auto* cs = CGColorSpaceCreateDeviceRGB();
            auto* ctx = CGBitmapContextCreate(rgba.data(), w, h, 8, w * 4, cs,
                                              kCGImageAlphaPremultipliedLast);
            if (ctx) {
                CGContextDrawImage(ctx, CGRectMake(0, 0, (CGFloat)w, (CGFloat)h), img);
                auto at = [&](std::size_t x, std::size_t y) {
                    const std::size_t i = (y * w + x) * 4;
                    return std::array<int, 3>{rgba[i], rgba[i + 1], rgba[i + 2]};
                };
                const auto corner = at(w - 2, h - 2);
                std::size_t right = 0, bottom = 0;
                for (std::size_t y = 0; y < h; y += 3)
                    for (std::size_t x = 0; x < w; x += 3) {
                        const auto p = at(x, y);
                        if (std::abs(p[0] - corner[0]) + std::abs(p[1] - corner[1])
                            + std::abs(p[2] - corner[2]) > 24) {
                            right = std::max(right, x);
                            bottom = std::max(bottom, y);
                        }
                    }
                fx = double(right) / double(w);
                fy = double(bottom) / double(h);
                CGContextRelease(ctx);
            }
            CGColorSpaceRelease(cs);
            CGImageRelease(img);
        }
    }
    if (src) CFRelease(src);
    CFRelease(data);
    return {fx, fy};
}

/// Every word the explanation actually puts on screen.
///
/// Depth has to be measured here rather than on one cable's string: a role's
/// primer belongs to the role and is written once under its heading, so a
/// per-cable comparison sees no difference and would push the primer back onto
/// every cable to satisfy itself.
inline std::string rendered_text(const pulp::view::View* root) {
    std::string all;
    std::function<void(const pulp::view::View*)> walk =
        [&](const pulp::view::View* v) {
            if (auto* l = dynamic_cast<const pulp::view::Label*>(v))
                all += l->text() + "\n";
            for (int i = 0; i < v->child_count(); ++i) walk(v->child_at(i));
        };
    walk(root);
    return all;
}

/// The same words with every run of whitespace collapsed to one space.
///
/// Searching rendered text for a phrase has to allow for the wrap falling in
/// the middle of it -- otherwise the assertion tracks the pane's width rather
/// than what the explanation says.
inline std::string flatten(const std::string& text) {
    std::string out;
    bool space = false;
    for (const char c : text) {
        if (c == '\n' || c == ' ' || c == '\t') { space = true; continue; }
        if (space && !out.empty()) out += ' ';
        space = false;
        out += c;
    }
    return out;
}

std::size_t distinct_colors(const std::vector<std::uint8_t>& png) {
    if (png.empty()) return 0;
    auto* data = CFDataCreate(nullptr, png.data(), static_cast<CFIndex>(png.size()));
    auto* src = CGImageSourceCreateWithData(data, nullptr);
    std::set<std::uint32_t> seen;
    if (src && CGImageSourceGetCount(src) > 0) {
        auto* img = CGImageSourceCreateImageAtIndex(src, 0, nullptr);
        if (img) {
            const std::size_t w = CGImageGetWidth(img), h = CGImageGetHeight(img);
            std::vector<std::uint8_t> rgba(w * h * 4, 0);
            auto* cs = CGColorSpaceCreateDeviceRGB();
            auto* ctx = CGBitmapContextCreate(rgba.data(), w, h, 8, w * 4, cs,
                                              kCGImageAlphaPremultipliedLast);
            if (ctx) {
                CGContextDrawImage(ctx, CGRectMake(0, 0, (CGFloat)w, (CGFloat)h), img);
                for (std::size_t i = 0; i + 3 < rgba.size(); i += 4)
                    seen.insert((std::uint32_t(rgba[i] >> 3) << 10) |
                                (std::uint32_t(rgba[i + 1] >> 3) << 5) |
                                std::uint32_t(rgba[i + 2] >> 3));
                CGContextRelease(ctx);
            }
            CGColorSpaceRelease(cs);
            CGImageRelease(img);
        }
    }
    if (src) CFRelease(src);
    CFRelease(data);
    return seen.size();
}

std::vector<std::uint8_t> render_preview(const std::string& panel_dir,
                                         const std::string& brand = "ForgeModular") {
    forge_modular::RackModule mod;
    mod.id = "m1";
    mod.name = "TESTPANEL";
    mod.brand = brand;            // our artwork is only drawn on our modules
    mod.hp = 10;
    forge_modular::RackPreview preview;
    preview.set_rack({mod}, {});
    if (!panel_dir.empty()) preview.set_panel_directory(panel_dir);
    return pulp::view::render_to_png(preview, 420, 320, 1.0f,
                                     pulp::view::ScreenshotBackend::skia);
}

}  // namespace

TEST_CASE("a module's own panel artwork is what the stage draws", "[seam]") {
    // The defect this pins: a finished module showed an empty rectangle with
    // its name on it, while the emitter had already written its panel -- knobs,
    // labels and all -- to a directory the preview never read.
    //
    // The artwork here is a flat magenta field, a colour the chrome uses
    // nowhere, so its presence on the stage can only have come from the file.
    const auto dir = std::filesystem::temp_directory_path() /
                     "forge-modular-panel-test";
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    {
        std::ofstream svg(dir / "TESTPANEL-dark.svg");
        svg << R"(<svg xmlns="http://www.w3.org/2000/svg" width="50" height="380" )"
            << R"(viewBox="0 0 50 380"><rect width="50" height="380" fill="#ff00ff"/></svg>)";
    }

    const auto painted = count_pixels_near(render_preview(dir.string()), 255, 0, 255);
    // The negative control is the point of this test. Without it, a preview
    // that drew nothing at all would pass the first assertion by accident.
    const auto unpainted = count_pixels_near(render_preview(""), 255, 0, 255);

    // A module of the same NAME from another plugin must not get our face.
    // Model slugs are unique within a plugin, not across the library:
    // Fundamental ships a VCO, a VCF, a VCA and an LFO too, and drawing our
    // panel on one of theirs is a confident lie about what is in the rack --
    // wrong controls, wrong width, entirely plausible.
    const auto other_vendor =
        count_pixels_near(render_preview(dir.string(), "Fundamental"), 255, 0, 255);

    INFO("magenta ours: " << painted << ", no directory: " << unpainted
                          << ", another vendor's module: " << other_vendor);
    CHECK(unpainted == 0);
    CHECK(other_vendor == 0);
    CHECK(painted > 200);

    std::filesystem::remove_all(dir);
}

TEST_CASE("a built module says what it is", "[seam]") {
    // The prototype's left column carried a spec beneath the description. The
    // app showed a prompt and a verdict, so the only way to learn what you had
    // was to open Rack.
    //
    // Asserted against the manifest each row is DERIVED from, not against
    // written-down expectations: a spec that disagrees with the module is
    // worse than no spec, and a test carrying its own copy of the numbers
    // would agree with itself while the screen disagreed with the module.
    const auto dir = std::filesystem::temp_directory_path() / "forge-spec-test";
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    const auto manifest = dir / "thing.json";
    {
        std::ofstream f(manifest);
        f << R"({"modules":[{"slug":"THING","name":"Thing","hp":12,)"
          << R"("description":"A thing that does a thing.",)"
          << R"("params":[{"type":"knob"},{"type":"knob"},{"type":"slider"}],)"
          << R"("inputs":[{"role":"Audio"},{"role":"Cv"}],)"
          << R"("outputs":[{"role":"Audio"}],)"
          << R"("lights":[{"color":"green"}]}]})";
    }

    forge_modular::ModuleSummary spec;
    REQUIRE(spec.set_manifest(manifest.string()));

    std::map<std::string, std::string> rows;
    for (const auto& [k, v] : spec.rows()) rows[k] = v;

    // 12 HP is 60.96mm, which is arithmetic on the manifest rather than a
    // number anybody typed.
    CHECK(rows["WIDTH"].find("12 HP") != std::string::npos);
    CHECK(rows["WIDTH"].find("60.9") != std::string::npos);
    // Counted by kind: two knobs and a slider are not "3 controls".
    CHECK(rows["CONTROLS"].find("2 knobs") != std::string::npos);
    CHECK(rows["CONTROLS"].find("1 slider") != std::string::npos);
    // Split by role: "2 in" would not say what the module expects.
    CHECK(rows["I/O"].find("Cv in") != std::string::npos);
    CHECK(rows["I/O"].find("Audio out") != std::string::npos);
    CHECK(rows["LIGHTS"].find("1 light") != std::string::npos);
    CHECK(spec.description() == "A thing that does a thing.");

    // A row that cannot be derived is not shown. This manifest has no width,
    // so there must be no WIDTH row rather than a plausible default.
    {
        std::ofstream f(manifest);
        f << R"({"modules":[{"slug":"BARE","name":"Bare"}]})";
    }
    forge_modular::ModuleSummary bare;
    bare.set_manifest(manifest.string());
    for (const auto& [k, v] : bare.rows()) {
        INFO("unexpected row: " << k << " = " << v);
        CHECK(k != "WIDTH");
    }

    // And nothing at all from a file that is not a manifest.
    forge_modular::ModuleSummary junk;
    CHECK_FALSE(junk.set_manifest((dir / "nothing-here.json").string()));
    CHECK(junk.rows().empty());

    std::filesystem::remove_all(dir);
}

TEST_CASE("a module with no panel says so instead of looking empty", "[seam]") {
    // The distinction this draws: "we have no picture of this module" and
    // "this module is empty" used to be the same grey rectangle. A reader who
    // cannot tell them apart will read a working rack as broken, or -- worse
    // -- a broken one as working.
    //
    // Hatching is the tell, so the test counts ink rather than trusting a
    // flag: a face that is merely a different shade of grey would pass any
    // check written on the enum.
    const auto count_ink = [](const std::vector<std::uint8_t>& png) {
        return distinct_colors(png);
    };

    auto render = [&](const std::string& brand, const std::string& panel_dir) {
        forge_modular::RackModule mod;
        mod.id = "m1";
        mod.name = "NOSUCHPANEL";
        mod.brand = brand;
        mod.hp = 10;
        forge_modular::RackPreview preview;
        preview.set_rack({mod}, {});
        if (!panel_dir.empty()) preview.set_panel_directory(panel_dir);
        return pulp::view::render_to_png(preview, 420, 320, 1.0f,
                                         pulp::view::ScreenshotBackend::skia);
    };

    // Ours, with no artwork on disk: a fault, and it has to look like one.
    const auto ours = count_ink(render("ForgeModular", ""));
    // A vendor's module we never had a picture of: not a fault, drawn plainly.
    const auto theirs = count_ink(render("Fundamental", ""));

    INFO("tones on our unpainted module: " << ours
         << ", on a vendor's: " << theirs);
    // The hatched face carries strictly more ink than the plain one. Equal
    // counts would mean both render identically, which is the defect.
    CHECK(ours > theirs);
}

TEST_CASE("a real generated panel renders and not just a synthetic one", "[seam]") {
    // The synthetic test above proves the wiring. This proves the wiring
    // survives the artwork we actually ship: 23 KB of paths, gradients and
    // text per panel, which Skia's SVG module has to accept in full.
    const char* home = std::getenv("HOME");
    const std::filesystem::path res =
        std::string(home ? home : ".") +
        "/Library/Application Support/Forge Modular/examples/forge-modular/res";
    std::error_code ec;
    if (!std::filesystem::exists(res, ec)) {
        SKIP("no generated panels installed on this machine");
    }
    std::string slug;
    for (const auto& e : std::filesystem::directory_iterator(res, ec)) {
        const auto n = e.path().filename().string();
        if (n.size() > 9 && n.substr(n.size() - 9) == "-dark.svg") {
            slug = n.substr(0, n.size() - 9);
            break;
        }
    }
    if (slug.empty()) SKIP("no dark panel artwork found");

    forge_modular::RackModule mod;
    mod.id = "m1";
    mod.name = slug;
    mod.brand = "ForgeModular";
    mod.hp = 10;
    forge_modular::RackPreview preview;
    preview.set_rack({mod}, {});
    preview.set_panel_directory(res.string());
    const auto png = pulp::view::render_to_png(preview, 420, 320, 1.0f,
                                               pulp::view::ScreenshotBackend::skia);
    REQUIRE_FALSE(png.empty());

    // A panel that failed to parse falls back to the flat placeholder, so the
    // tell is variety: real artwork has knob rings, silkscreen and jacks in
    // many distinct tones. Counting them separates "drew the panel" from
    // "drew a rectangle", which asserting non-empty output cannot.
    INFO("panel slug: " << slug);
    CHECK(distinct_colors(png) > 40);
}

TEST_CASE("a loaded patch explains itself beyond the wiring", "[seam]") {
    // The defect this pins: the generator printed its reasoning to stdout and
    // wrote only the netlist, and the loader never looked for prose. Every
    // patch the app showed therefore had an empty `why` on every cable -- and
    // because Standard depth adds exactly that string, Standard rendered
    // byte-identical to Terse for every real patch. The three-depth promise
    // held only in tests, which built their connections by hand.
    //
    // So this test refuses to build a Connection: it writes a patch and its
    // sidecar to disk and reads them back the way the app does.
    const auto dir = std::filesystem::temp_directory_path() / "forge-modular-why-test";
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    const auto vcv = dir / "demo.vcv";
    {
        std::ofstream f(vcv);
        f << R"({"modules":[{"id":1,"plugin":"Fundamental","model":"VCO"},)"
          << R"({"id":2,"plugin":"Fundamental","model":"VCF"}],)"
          << R"("cables":[{"outputModuleId":1,"outputId":0,)"
          << R"("inputModuleId":2,"inputId":0,"color":"#00ff00"}]})";
    }
    {
        std::ofstream f(dir / "demo.why.json");
        f << R"({"cables":{"1:0>2:0":{"why":)"
          << R"("the saw is the raw material the filter shapes",)"
          << R"("from_port":"SAW","to_port":"IN"}},)"
          << R"("modules":{"1":"VCO 1","2":"VCF"}})";
    }

    const auto loaded = forge_modular::load_patch(vcv.string());
    REQUIRE(loaded.connections.size() == 1);
    CHECK(loaded.connections[0].why ==
          "the saw is the raw material the filter shapes");
    // A .vcv stores port INDICES. Without the sidecar the app can only say
    // "out0 → in1", which is true and teaches nothing. Asserted on the line
    // the reader actually sees rather than on the field behind it: the ID has
    // to stay on the connection because the jack geometry is keyed on it, so
    // checking the field would pass while the screen still said "out0".
    forge_modular::PatchExplanation shown;
    shown.set_bounds({0, 0, 820, 300});
    shown.set_connections(loaded.connections, loaded.modules);
    const auto line = shown.line_text(0);
    INFO("line: " << line);
    CHECK(line.find("SAW") != std::string::npos);
    CHECK(line.find("IN") != std::string::npos);
    CHECK(line.find("out0") == std::string::npos);
    REQUIRE(loaded.modules.size() == 2);
    CHECK(loaded.modules[0].display == "VCO 1");
    // The slug stays put: the panel artwork is filed under it.
    CHECK(loaded.modules[0].name == "VCO");

    // The negative control: the same patch with no sidecar must come back with
    // nothing to say, or the assertion above proves only that a string exists
    // somewhere.
    std::filesystem::remove(dir / "demo.why.json");
    const auto bare = forge_modular::load_patch(vcv.string());
    REQUIRE(bare.connections.size() == 1);
    CHECK(bare.connections[0].why.empty());
    // With no sidecar there is nothing to name the jacks with, and the index
    // is what remains -- honest, and the reason the wiring still reads.
    CHECK(bare.connections[0].from_port == "out0");
    CHECK(bare.modules[0].display.empty());

    std::filesystem::remove_all(dir);
}

TEST_CASE("a really generated patch arrives with its reasons attached", "[seam]") {
    // The synthetic round-trip above proves the mechanism. This proves the two
    // halves agree in the field: the generator's key format and the loader's
    // must match exactly, and they are written in different languages in
    // different repositories -- the project's most expensive recurring bug.
    const char* home = std::getenv("HOME");
    const std::filesystem::path dir =
        std::string(home ? home : ".") +
        "/Library/Application Support/Forge Modular/examples/forge-modular/patches";
    std::error_code ec;
    if (!std::filesystem::exists(dir, ec)) SKIP("no patches installed here");

    std::string found;
    for (const auto& e : std::filesystem::directory_iterator(dir, ec)) {
        const auto n = e.path().string();
        if (n.size() > 9 && n.substr(n.size() - 9) == ".why.json") {
            found = n.substr(0, n.size() - 9) + ".vcv";
            break;
        }
    }
    if (found.empty()) SKIP("no patch with a sidecar installed here");

    const auto loaded = forge_modular::load_patch(found);
    REQUIRE_FALSE(loaded.connections.empty());
    const auto explained = std::count_if(
        loaded.connections.begin(), loaded.connections.end(),
        [](const forge_modular::Connection& c) { return !c.why.empty(); });
    INFO("patch: " << found << " -- " << explained << " of "
                   << loaded.connections.size() << " cables carry a reason");
    // Not every cable: the contract tells the model to omit the obvious. But a
    // patch where NONE carries a reason is the defect this test exists for.
    CHECK(explained > 0);
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
// graph".
//
// It ran hidden ([.crash]) while it waited for a default build to exist. That
// has been true for a long time and it passes, so it runs with everything
// else: it is the regression test for a segfault, and a regression test that
// only runs when somebody remembers to ask for it is not guarding anything.
TEST_CASE("Forge Modular's view tree can be walked", "[seam][crash]") {
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
            // Match the access label: the visible text lives on a child Label
            // so its colour can be set per state, and the access label is what
            // a screen reader reads out anyway.
            if (b->access_label() == "Module") module_tab = b;
            if (b->access_label() == "Patch") patch_tab = b;
        }
        for (std::size_t i = 0; i < v.child_count(); ++i) find(*v.child_at(i));
    };
    find(*view);
    REQUIRE(module_tab != nullptr);
    REQUIRE(patch_tab != nullptr);
    REQUIRE(patch_tab->on_click);          // a tab with no handler is decoration

    // The ACTIVE tab must be the one that looks active. This shipped
    // backwards -- Module was selected while Patch wore the raised pill --
    // and the old assertion never noticed because it only checked which
    // artifact was set, never which tab looked chosen.
    auto looks_active = [](pulp::view::TextButton* b) {
        REQUIRE(b->child_count() >= 1);
        auto* lbl = dynamic_cast<pulp::view::Label*>(b->child_at(0));
        REQUIRE(lbl != nullptr);
        return b->style() == pulp::view::TextButton::Style::secondary &&
               lbl->text_color() == forge::design::color::text;
    };

    CHECK(shell.artifact() == forge_modular::Artifact::module);
    CHECK(looks_active(module_tab));
    CHECK_FALSE(looks_active(patch_tab));
    // And the quiet one must not wear the accent, which would read as chosen.
    CHECK(dynamic_cast<pulp::view::Label*>(patch_tab->child_at(0))->text_color() !=
          forge::design::color::accent);
    CHECK(shell.chrome_copy().hero_title == "What should the module do?");
    CHECK(shell.composer_row().right[1].label == "Build module");

    patch_tab->on_click();
    CHECK(looks_active(patch_tab));
    CHECK_FALSE(looks_active(module_tab));
    CHECK(shell.artifact() == forge_modular::Artifact::patch);
    CHECK(shell.chrome_copy().hero_title == "What should the patch do?");
    CHECK(shell.chrome_copy().badge == "PATCH");
    CHECK(shell.composer_row().right[1].label == "Create patch");

    module_tab->on_click();               // and back, so it is not one-way
    CHECK(shell.artifact() == forge_modular::Artifact::module);
    CHECK(looks_active(module_tab));
    CHECK_FALSE(looks_active(patch_tab));
    // And the quiet one must not wear the accent, which would read as chosen.
    CHECK(shell.chrome_copy().hero_title == "What should the module do?");
    CHECK(shell.composer_row().right[1].label == "Build module");
}

// ── the @ mention overlay ────────────────────────────────────────────────────

namespace {

/// A small, known library so the assertions are about behaviour rather than
/// whatever 4,705 real modules happen to contain.
std::vector<forge_modular::MentionCandidate> test_library(const std::string& q) {
    using A = forge_modular::MentionCandidate::Availability;
    std::vector<forge_modular::MentionCandidate> all = {
        {"Fundamental", "VCO", "Fundamental/VCO", A::ready},
        {"Fundamental", "VCF", "Fundamental/VCF", A::ready},
        {"4ms", "DrumBus", "4ms-ProducerPack/DrumBus", A::available},
        {"Vult", "Freak", "Vult/Freak", A::paid},
    };
    if (q.empty()) return all;
    std::vector<forge_modular::MentionCandidate> hit;
    for (const auto& c : all) {
        auto lower = [](std::string s) {
            for (auto& ch : s) ch = static_cast<char>(std::tolower(ch));
            return s;
        };
        if (lower(c.name).find(lower(q)) != std::string::npos) hit.push_back(c);
    }
    return hit;
}

}  // namespace

TEST_CASE("typing @ opens the mention list and typing filters it", "[mention]") {
    forge_modular::MentionOverlay overlay;
    auto view = overlay.build();
    REQUIRE(view != nullptr);
    overlay.set_source(test_library);

    CHECK_FALSE(overlay.is_open());

    overlay.handle_text("a patch with @", 14);
    CHECK(overlay.is_open());
    CHECK(overlay.candidates().size() == 4);

    overlay.handle_text("a patch with @vc", 16);
    CHECK(overlay.candidates().size() == 2);      // VCO and VCF

    overlay.handle_text("a patch with @vco", 17);
    CHECK(overlay.candidates().size() == 1);
    CHECK(overlay.candidates()[0].name == "VCO");
}

TEST_CASE("a space that matches nothing closes the mention list", "[mention]") {
    // "@ " is somebody typing an address, not reaching for a module, and the
    // words after it are prose. That has to stay true — see the test below,
    // which is the other half of the same rule.
    forge_modular::MentionOverlay overlay;
    auto view = overlay.build();
    overlay.set_source(test_library);

    overlay.handle_text("@vc", 3);
    REQUIRE(overlay.is_open());
    overlay.handle_text("@vc ", 4);
    CHECK_FALSE(overlay.is_open());

    // And prose after an address stays prose, rather than the list hanging
    // around hoping a module is coming.
    overlay.handle_text("write to @me and", 16);
    CHECK_FALSE(overlay.is_open());
}

namespace {

/// A maker with several modules, a maker whose name has a space in it, and a
/// module whose common name is not its display name. Small enough to assert
/// about, and shaped like the three cases that were broken.
const std::vector<forge_modular::ModuleEntry>& space_library() {
    static const std::vector<forge_modular::ModuleEntry> lib = {
        // Deliberately FIRST, so insertion order alone would put this maker's
        // modules above the module actually named "Sphinx".
        {"Sphinx Modular", "Orbit", "SphinxModular/Orbit", false},
        {"Sphinx Modular", "Perigee", "SphinxModular/Perigee", false},
        {"CV funk", "Steps", "CVfunk/Steps", false},
        {"CV funk", "Sphinx", "CVfunk/Sphinx", false},
        {"CV funk", "Ouros", "CVfunk/Ouros", false},
        {"Audible Instruments", "Macro Oscillator", "AudibleInstruments/Braids", false},
        {"Audible Instruments", "Bernoulli Gate", "AudibleInstruments/Branches", false},
        {"Fundamental", "VCO", "Fundamental/VCO", true},
        // Longer name FIRST, so only the exact tier can put "Dunes" on top.
        {"Grande", "Dunestomper", "Grande/Dunestomper", false},
        {"Grande", "Dunes", "Grande/Dunes", false},
    };
    return lib;
}

std::vector<forge_modular::MentionCandidate> space_source(const std::string& q) {
    return forge_modular::search_entries(space_library(), q);
}

std::vector<std::string> names_of(
    const std::vector<forge_modular::MentionCandidate>& cs) {
    std::vector<std::string> out;
    for (const auto& c : cs) out.push_back(c.name);
    return out;
}

}  // namespace

TEST_CASE("a maker's name finds that maker's modules", "[mention][catalog]") {
    // Matching module NAMES only meant typing a manufacturer returned whichever
    // handful of their modules happened to have those letters in them: "CV funk"
    // found 2 of 50, and the other 48 could not be reached by any spelling.
    using forge_modular::search_entries;

    // A maker writes their own name three ways, and somebody types whichever
    // they remember. All three have to land on the same three modules.
    for (const auto& spelling : {"CV funk", "cvfunk", "CV Funk", "cv-funk"}) {
        const auto hits = search_entries(space_library(), spelling);
        INFO("spelling: " << spelling);
        CHECK(hits.size() == 3);
    }

    // A DIRECT HIT IS NEVER BURIED BY A MAKER. There is a module called Sphinx
    // and a maker called Sphinx Modular, and the maker's two modules are first
    // in the list — so only the ranking can put the direct hit on top.
    const auto sphinx = search_entries(space_library(), "sphinx");
    REQUIRE(sphinx.size() == 3);
    CHECK(sphinx[0].name == "Sphinx");

    // Exact above prefix: a whole name that loses to a longer one containing
    // it is the query being ignored.
    const auto dunes = search_entries(space_library(), "Dunes");
    REQUIRE(dunes.size() == 2);
    CHECK(dunes[0].name == "Dunes");

    // And the alias ranking survives all of it: "br" reaches Braids through
    // the slug, above two brand matches and a name that merely contains it.
    const auto br = search_entries(space_library(), "br");
    REQUIRE_FALSE(br.empty());
    CHECK(br[0].name == "Macro Oscillator");
    CHECK(br[0].alias == "Braids");
}

TEST_CASE("a mention survives a space while it still matches", "[mention]") {
    // "@CV funk" could not be typed at all: a space ended the mention
    // unconditionally, which took Audible Instruments, Count Modula, Frozen
    // Wasteland, Impromptu Modular and every module whose own name has a space
    // in it with it.
    //
    // The rule that replaced it is a guess that resolves itself. A space
    // extends the query speculatively, and the list stays open only while the
    // longer query still matches something.
    forge_modular::MentionOverlay overlay;
    auto view = overlay.build();
    overlay.set_source(space_source);

    overlay.handle_text("@CV", 3);
    REQUIRE(overlay.is_open());
    overlay.handle_text("@CV ", 4);
    CHECK(overlay.is_open());                      // the guess is still alive
    overlay.handle_text("@CV funk", 8);
    CHECK(overlay.is_open());
    CHECK(overlay.candidates().size() == 3);

    // A two-word module name, which is the other half of the same problem.
    overlay.handle_text("@Bernoulli Gate", 15);
    CHECK(overlay.is_open());
    REQUIRE(overlay.candidates().size() == 1);
    CHECK(overlay.candidates()[0].name == "Bernoulli Gate");

    // And the guess collapses the moment it is wrong: this is prose, not a
    // maker, and it has to become ordinary text again.
    overlay.handle_text("@VCO and then", 13);
    CHECK_FALSE(overlay.is_open());

    // Bounded, so a mention cannot swallow a sentence: past the cap the words
    // are prose whatever they say.
    overlay.handle_text("@CV funk and more", 17);
    CHECK_FALSE(overlay.is_open());
}

namespace {

/// A HOME nothing else has written to, restored on the way out.
///
/// The library index is a per-user file, so a test that asserts anything about
/// it against the developer's own HOME is asserting about that developer's
/// machine.
struct ScopedHome {
    explicit ScopedHome(const std::string& name) {
        if (const char* h = std::getenv("HOME")) previous = h;
        dir = std::filesystem::temp_directory_path() / name;
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
        std::filesystem::create_directories(dir, ec);
        ::setenv("HOME", dir.string().c_str(), 1);
    }
    ~ScopedHome() {
        if (previous.empty()) ::unsetenv("HOME");
        else ::setenv("HOME", previous.c_str(), 1);
    }
    std::filesystem::path dir;
    std::string previous;
};

/// An index document of a given shape: `plugins` plugins, `per` modules each.
/// The shape is what the plausibility floor judges, so a test needs to be able
/// to write a truncated one and a full one on demand.
std::string fake_index(int plugins, int per) {
    std::string out = "{";
    for (int p = 0; p < plugins; ++p) {
        if (p) out += ",";
        const auto slug = "Maker" + std::to_string(p);
        out += "\"" + slug + "\":{\"brand\":\"" + slug + "\",\"modules\":[";
        for (int m = 0; m < per; ++m) {
            if (m) out += ",";
            out += "{\"slug\":\"" + slug + "/M" + std::to_string(m) +
                   "\",\"name\":\"M" + std::to_string(m) + "\"}";
        }
        out += "]}";
    }
    return out + "}";
}

}  // namespace

TEST_CASE("the library index is asked for when nothing has written one",
          "[mention][catalog]") {
    // library_catalog.py could build the index, module_catalog.cpp could read
    // it, and no code path joined them. The list therefore offered only what
    // was already installed — which makes the whole download capability
    // unreachable, because you cannot mention what you do not already have.
    ScopedHome home("forge-library-index");
    const auto path = forge_modular::library_index_path();
    // The clock is real because the files written below are: their mtime is
    // now, so a fabricated "now" from years ago makes every one of them look
    // like it was written in the future.
    const std::time_t now = std::time(nullptr);

    // Missing is the first-run case.
    CHECK(forge_modular::library_index_needs_build(path, now));

    std::filesystem::create_directories(std::filesystem::path(path).parent_path());
    std::ofstream(path) << fake_index(500, 12);
    CHECK_FALSE(forge_modular::library_index_needs_build(path, now));

    // An index built long enough ago is wrong: VCV publishes continuously.
    CHECK(forge_modular::library_index_needs_build(path, now + 30 * 24 * 3600));

    // An empty file is worse than none — it looks current and offers nothing.
    std::ofstream(path, std::ios::trunc);
    CHECK(forge_modular::library_index_needs_build(path, now));

    // AND SO IS A SMALL ONE. This is the assertion this test used to get
    // wrong: it wrote a one-plugin index and required that no rebuild was
    // needed, which is exactly the rule that let a 200-plugin file with no CV
    // funk in it sit on a machine for four days looking current. The published
    // library is ~420 plugins and ~4,300 modules; anything near 200 is a
    // truncated fetch whatever its mtime says.
    std::ofstream(path, std::ios::trunc) << fake_index(200, 12);
    CHECK(forge_modular::library_index_needs_build(path, now));
    std::ofstream(path, std::ios::trunc) << R"({"CVfunk":{"modules":[]}})";
    CHECK(forge_modular::library_index_needs_build(path, now));
    // Counted, not guessed from the file size: an index of 260 plugins that
    // somehow lists no modules is broken too.
    std::ofstream(path, std::ios::trunc) << fake_index(260, 0);
    CHECK(forge_modular::library_index_needs_build(path, now));
    std::ofstream(path, std::ios::trunc) << fake_index(500, 12);
    CHECK_FALSE(forge_modular::library_index_needs_build(path, now));

    // And the command names the script, runs in the toolchain directory, and
    // keeps its output. A background job that fails in silence is how this
    // file came to be read by something nothing ever wrote.
    std::string issued;
    std::filesystem::remove(path);   // asking again needs there to be a reason
    const auto tools = std::string("/Applications/Forge Modular.app/Contents/"
                                   "Resources/tools/rack");
    const auto command = forge_modular::ensure_library_index(
        tools, [&](const std::string& c) { issued = c; }, now);
    INFO(command);
    CHECK(command == issued);
    CHECK(command.find("library_catalog.py index") != std::string::npos);
    // The path has a space in it, so an unquoted `cd` reaches "Forge" and
    // stops. Asserted through a real shell rather than by substring: the
    // stub prints where it landed.
    const auto probe = std::filesystem::temp_directory_path() /
                       "forge-index-cmd";
    std::filesystem::remove_all(probe);
    std::filesystem::create_directories(probe / "Forge Modular.app" / "Contents" /
                                        "Resources" / "tools" / "rack");
    const auto stub = probe / "Forge Modular.app" / "Contents" / "Resources" /
                      "tools" / "rack" / "library_catalog.py";
    { std::ofstream f(stub); f << "import os, sys\n"
                                 "open(os.environ['PROBE'],'w').write("
                                 "os.getcwd() + '\\n' + ' '.join(sys.argv[1:]))\n"; }
    const auto out = probe / "landed.txt";
    const auto real = forge_modular::library_index_command(
        (probe / "Forge Modular.app" / "Contents" / "Resources" / "tools" /
         "rack").string());
    // `export`, not a `VAR=x cmd` prefix: that form sets the variable for the
    // FIRST command only, and the command under test is a chain.
    std::system(("export PROBE=" + out.string() + "; " + real).c_str());
    std::ifstream landed(out);
    std::string where, args;
    std::getline(landed, where);
    std::getline(landed, args);
    // Canonical on both sides: macOS's temp directory is a symlink, and the
    // shell reports where it really landed.
    CHECK(std::filesystem::weakly_canonical(where) ==
          std::filesystem::weakly_canonical(
              probe / "Forge Modular.app" / "Contents" / "Resources" /
              "tools" / "rack"));
    CHECK(args == "index");

    // Nothing is asked for when the index is current, so opening the editor
    // does not fire two HTTP requests a day for no reason.
    std::ofstream(path, std::ios::trunc) << fake_index(500, 12);
    std::string second;
    CHECK(forge_modular::ensure_library_index(
              tools, [&](const std::string& c) { second = c; }, now).empty());
    CHECK(second.empty());
}

TEST_CASE("what the @ list finds in the real library", "[.library-probe]") {
    // Not run by default (the leading dot): it reads whatever library index
    // this machine has, so it asserts nothing and reports. Run it by name to
    // see the shipped search against the actual 4,000-odd published modules
    // rather than against eight invented ones:
    //
    //     ./forge-test-chrome-no-leak "[.library-probe]" -s
    const auto counts = forge_modular::catalog_counts();
    WARN("installed " << counts.installed << ", catalogued " << counts.catalogued
         << " (index: " << forge_modular::library_index_path() << ")");
    for (const auto& q : {"CV funk", "cvfunk", "Audible Instruments", "br",
                          "Frozen Wasteland", "Dunes"}) {
        const auto hits = forge_modular::search_modules(q, 4);
        std::string line = std::string(q) + " -> " +
                           std::to_string(hits.size()) + ": ";
        for (const auto& h : hits) line += h.brand + "/" + h.name + "  ";
        WARN(line);
    }
}

TEST_CASE("opening the editor asks for the library index", "[seam][catalog]") {
    // The unit test above proves the decision. This proves it is REACHED:
    // every defect in this list so far has been a finished feature that
    // nothing called.
    ScopedHome home("forge-library-index-shell");
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

    bool asked = false;
    for (const auto& command : shell.launched())
        if (command.find("library_catalog.py index") != std::string::npos)
            asked = true;
    INFO("launched " << shell.launched().size() << " command(s)");
    CHECK(asked);
}

namespace {

/// Open an editor under the current HOME and say whether it asked for an
/// index. The whole point is to go through the app's own path -- create_view()
/// -> overlay_accessory() -> ensure_library_index() -- rather than to call the
/// decision function directly, because every defect in this list so far has
/// been a finished feature that nothing called.
bool editor_asks_for_index() {
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
    if (!view) return false;
    bool asked = false;
    for (const auto& command : shell.launched())
        if (command.find("library_catalog.py index") != std::string::npos)
            asked = true;
    shell.on_view_closed(*view);
    return asked;
}

}  // namespace

TEST_CASE("the editor rebuilds a missing or truncated index and leaves a good "
          "one alone", "[mention][catalog][seam]") {
    // THE THREE STARTING STATES, each through the real editor path.
    //
    // Measured on a beta machine: the index file was deleted, the app
    // relaunched, and the directory stayed empty. And a four-day-old
    // 200-plugin file -- a truncated fetch with no CV funk in it -- passed the
    // freshness check for four days, because the check only looked at the
    // clock.
    ScopedHome home("forge-index-three-states");
    const auto path = forge_modular::library_index_path();
    std::filesystem::create_directories(std::filesystem::path(path).parent_path());

    // 1. No file at all: the first-run case, and the one that stayed empty.
    std::error_code ec;
    std::filesystem::remove(path, ec);
    CHECK(editor_asks_for_index());

    // 2. A truncated file, freshly written so no age rule can catch it.
    std::ofstream(path, std::ios::trunc) << fake_index(200, 12);
    CHECK(editor_asks_for_index());

    // 3. A full one: nothing is asked for, so opening an editor does not fire
    //    two HTTP requests every time somebody looks at the app.
    std::ofstream(path, std::ios::trunc) << fake_index(500, 12);
    CHECK_FALSE(editor_asks_for_index());
}

TEST_CASE("a library index build that fails says so", "[catalog][seam]") {
    // The failure this whole path exists to end. The command ran, a toolchain
    // too old to understand `index` printed its usage and exited 2, and the
    // only trace was a log nobody opens. Nothing on the machine could be asked
    // whether the last refresh had worked.
    ScopedHome home("forge-index-status");
    const auto tools = home.dir / "tools";
    std::filesystem::create_directories(tools);

    // A stand-in for the stale generator: it rejects `index` exactly as the
    // shipped one did, printing its usage and exiting 2.
    {
        std::ofstream f(tools / "library_catalog.py");
        f << "import sys\n"
             "print('usage: fetch | report')\n"
             "sys.exit(2)\n";
    }
    CHECK_FALSE(forge_modular::library_index_last_status().has_value());
    std::system(forge_modular::library_index_command(tools.string()).c_str());
    auto status = forge_modular::library_index_last_status();
    REQUIRE(status.has_value());
    CHECK(*status == 2);

    // And a generator that CAN build one reports zero, so the two outcomes are
    // distinguishable rather than both being silence.
    {
        std::ofstream f(tools / "library_catalog.py");
        f << "print('indexed')\n";
    }
    std::system(forge_modular::library_index_command(tools.string()).c_str());
    status = forge_modular::library_index_last_status();
    REQUIRE(status.has_value());
    CHECK(*status == 0);
}

TEST_CASE("the library index can be refreshed from Settings, and it reports "
          "what happened", "[catalog][prefs][seam]") {
    // Asked for directly. There was no way to say "do it now", so the only
    // remedy for a wrong index was an SSH session and a python command -- and
    // a refresh that says nothing is indistinguishable from one that did
    // nothing, which is how an hour went.
    ScopedHome home("forge-index-refresh");
    HermeticProjects isolated;
    const auto path = forge_modular::library_index_path();
    std::filesystem::create_directories(std::filesystem::path(path).parent_path());
    std::ofstream(path, std::ios::trunc) << fake_index(300, 10);

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
    auto* chrome = shell.chrome();
    REQUIRE(chrome != nullptr);

    // The control is REACHABLE, on the same rows as the two preferences.
    auto* refresh = chrome->settings_product_action_button(
        forge_modular::ForgeModularShell::kLibraryIndexRow);
    REQUIRE(refresh != nullptr);
    REQUIRE(refresh->on_click);

    // It reports the index WHEN IDLE, before anything is pressed.
    const auto idle = chrome->product_settings_status(
        forge_modular::ForgeModularShell::kLibraryIndexRow);
    INFO(idle);
    CHECK(idle.find("300 plugins") != std::string::npos);
    CHECK(idle.find("3000 modules") != std::string::npos);

    // Pressing it asks for a build.
    const auto before = shell.launched().size();
    refresh->on_click();
    REQUIRE(shell.launched().size() == before + 1);
    CHECK(shell.launched().back().find("library_catalog.py index") !=
          std::string::npos);

    // And the row now carries a before, and says a refresh is running. The
    // bare shell launched nothing, so no status file exists yet -- which is
    // exactly the in-flight state.
    shell.poll_surfaces();
    const auto running = chrome->product_settings_status(
        forge_modular::ForgeModularShell::kLibraryIndexRow);
    INFO(running);
    CHECK(running.find("300 plugins, 3000 modules →") != std::string::npos);
    CHECK(running.find("Refreshing") != std::string::npos);

    // A failed build is REPORTED, with its exit code and where to look.
    std::filesystem::create_directories(
        std::filesystem::path(forge_modular::library_index_status_path())
            .parent_path());
    { std::ofstream f(forge_modular::library_index_status_path()); f << "2"; }
    shell.poll_surfaces();
    const auto failed = chrome->product_settings_status(
        forge_modular::ForgeModularShell::kLibraryIndexRow);
    INFO(failed);
    CHECK(failed.find("failed (exit 2)") != std::string::npos);
    CHECK(failed.find("library.log") != std::string::npos);

    // And a successful one is reported as an after, not as silence.
    refresh->on_click();
    std::ofstream(path, std::ios::trunc) << fake_index(500, 12);
    { std::ofstream f(forge_modular::library_index_status_path()); f << "0"; }
    shell.poll_surfaces();
    const auto done = chrome->product_settings_status(
        forge_modular::ForgeModularShell::kLibraryIndexRow);
    INFO(done);
    CHECK(done.find("→ 500 plugins, 6000 modules") != std::string::npos);
    CHECK(done.find("Refreshed.") != std::string::npos);
    shell.on_view_closed(*view);
}

TEST_CASE("a newer release outranks an older installed toolchain",
          "[toolchain][installation]") {
    // The precedence that made an installer unable to update what it
    // installs. Application Support was preferred unconditionally, so a
    // toolchain written by 0.11 shadowed every fix 0.12.7 shipped -- and the
    // shadowed copy was too old to understand `library_catalog.py index`, so
    // the library index silently never rebuilt.
    //
    // mtime cannot decide this: every path here is a copy, and a copy rewrites
    // mtimes. The stamp is written at package time and travels with the files.
    using forge_modular::ToolchainCandidate;
    const ToolchainCandidate none{"/nowhere", false, {}};
    const ToolchainCandidate checkout{"/checkout", true, {}};

    // Older installed copy loses to the release inside the bundle.
    auto pick = forge_modular::choose_toolchain(
        none, {"/installed", true, "0.11.0"}, {"/bundle", true, "0.12.8"},
        checkout);
    CHECK(pick.path == "/bundle");
    INFO(pick.reason);
    CHECK(pick.reason.find("0.12.8") != std::string::npos);

    // An UNSTAMPED installed copy cannot outrank a release either -- that is
    // what every machine carrying a pre-stamp toolchain looks like.
    CHECK(forge_modular::choose_toolchain(
              none, {"/installed", true, ""}, {"/bundle", true, "0.12.8"},
              checkout).path == "/bundle");

    // Equal stamps leave the installed copy in charge, which is what keeps
    // hand-editing it working: editing a file does not change the stamp.
    CHECK(forge_modular::choose_toolchain(
              none, {"/installed", true, "0.12.8"}, {"/bundle", true, "0.12.8"},
              checkout).path == "/installed");
    // And a NEWER installed copy stays in charge, so a user who updates the
    // generator without reinstalling the app is not overruled.
    CHECK(forge_modular::choose_toolchain(
              none, {"/installed", true, "0.13.0"}, {"/bundle", true, "0.12.8"},
              checkout).path == "/installed");

    // An explicit override beats all of it. A developer who names a directory
    // means it.
    CHECK(forge_modular::choose_toolchain(
              {"/env", true, {}}, {"/installed", true, "9.9.9"},
              {"/bundle", true, "0.12.8"}, checkout).path == "/env");

    // Fallbacks: the bundle when nothing is installed, the checkout when there
    // is no bundle either.
    CHECK(forge_modular::choose_toolchain(none, none, {"/bundle", true, "0.12.8"},
                                          checkout).path == "/bundle");
    CHECK(forge_modular::choose_toolchain(none, none, none, checkout).path ==
          "/checkout");

    // The ordering itself, including the shapes a version string arrives in.
    CHECK(forge_modular::compare_stamps("0.12.8", "0.12.7") > 0);
    CHECK(forge_modular::compare_stamps("0.12.7", "0.12.10") < 0);
    CHECK(forge_modular::compare_stamps("1.0", "1.0.0") == 0);
    CHECK(forge_modular::compare_stamps("", "0.0.1") < 0);
    CHECK(forge_modular::compare_stamps("", "") == 0);
    // A release candidate must not sort above the release it precedes.
    CHECK(forge_modular::compare_stamps("0.13.0-rc1", "0.13.0") == 0);

    // The stamp file itself: a version, and when it was packaged.
    const auto parsed = forge_modular::parse_stamp("0.12.8\n2026-08-04T10:00:00Z\n");
    CHECK(parsed.first == "0.12.8");
    CHECK(parsed.second == "2026-08-04T10:00:00Z");
    CHECK(forge_modular::parse_stamp("0.12.8").second.empty());
}

TEST_CASE("the app can say what it is and what it is running",
          "[installation][seam]") {
    // An installed 0.12.7 reported 0.11.0 and ran a generator laid down three
    // days earlier, and nothing on the machine could be asked either
    // question -- which is how the shadowed toolchain stayed hidden.
    forge_modular::AppDetails d;
    d.app_version = "0.12.8";
    d.packaged_at = "2026-08-04T10:00:00Z";
    d.toolchain_path = "/Applications/Forge Modular.app/Contents/Resources/tools/rack";
    d.toolchain_stamp = "0.12.8";
    d.toolchain_reason = "this release (0.12.8) is newer than the installed copy (0.11.0)";
    d.index_plugins = 417;
    d.index_modules = 4299;
    d.index_written = 1000;
    d.sdk_path = "/Users/x/Library/Application Support/Forge Modular/Rack-SDK";
    d.signed_in = true;

    const auto rows = forge_modular::details_rows(d, 1000 + 4 * 24 * 3600);
    std::string joined;
    for (const auto& [label, value] : rows) joined += label + "=" + value + "\n";
    INFO(joined);
    // THE LIVE GENERATOR PATH. The field that matters, and the one that would
    // have made a day of shadowed fixes obvious in seconds.
    CHECK(joined.find("Generator in use=/Applications/Forge Modular.app") !=
          std::string::npos);
    CHECK(joined.find("Version=0.12.8") != std::string::npos);
    CHECK(joined.find("417 plugins, 4299 modules, 4 days ago") !=
          std::string::npos);
    CHECK(joined.find("Rack SDK=/Users/x/Library") != std::string::npos);
    CHECK(joined.find("VCV library sign-in=found") != std::string::npos);
    // Never the token itself, only that one was found.
    CHECK(joined.find("token") == std::string::npos);

    // The unknowns read as unknown rather than as blanks.
    forge_modular::AppDetails empty;
    empty.toolchain_path = "/x";
    const auto plain = forge_modular::details_text(empty, 0);
    INFO(plain);
    CHECK(plain.find("Version: unknown") != std::string::npos);
    CHECK(plain.find("Library index: never built") != std::string::npos);
    CHECK(plain.find("Rack SDK: not installed") != std::string::npos);
    CHECK(plain.find("unstamped") != std::string::npos);

    CHECK(forge_modular::describe_age(0, 100) == "never built");
    CHECK(forge_modular::describe_age(100, 130) == "just now");
    CHECK(forge_modular::describe_age(0 + 1, 1 + 4 * 24 * 3600) == "4 days ago");
    CHECK(forge_modular::describe_age(1, 1 + 26 * 3600) == "26 hours ago");
    CHECK(forge_modular::describe_age(1, 1 + 40 * 3600) == "1 day ago");
}

TEST_CASE("a module is named once, not twice", "[mention][copy]") {
    // The notice read "CV funk CV funk Blank 8HP". The label was brand + " " +
    // name, and VCV module names very often already lead with the maker.
    CHECK(forge_modular::module_label("CV funk", "CV funk Blank 8HP") ==
          "CV funk Blank 8HP");
    // Spelled differently in the slug and on the panel, and still one name.
    CHECK(forge_modular::module_label("CV funk", "CVfunk Blank 8HP") ==
          "CVfunk Blank 8HP");
    // A name that does NOT lead with the maker still gets one.
    CHECK(forge_modular::module_label("Audible Instruments", "Macro Oscillator") ==
          "Audible Instruments Macro Oscillator");
    // A brand that merely appears later in the name is not a prefix.
    CHECK(forge_modular::module_label("Valley", "Plateau Valley") ==
          "Valley Plateau Valley");
    CHECK(forge_modular::module_label("", "VCO") == "VCO");
    CHECK(forge_modular::module_label("Valley", "") == "Valley");
}

TEST_CASE("the mention notice hangs under the composer, and is amber only for "
          "a real block", "[mention][seam]") {
    // It sat at the dropdown's narrower inset while the composer was centred,
    // so it read as a detached box; and it was amber, which says warning when
    // a download starting is ordinary progress.
    ScopedHome home("forge-notice-frame");
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
    auto* chrome = shell.chrome();
    REQUIRE(chrome != nullptr);

    // Lay the tree out, or nothing has bounds to compare.
    REQUIRE(pulp::view::render_to_file(
        *view, forge::ForgeChrome::kDesignWidth,
        forge::ForgeChrome::kDesignHeight,
        (std::filesystem::temp_directory_path() / "modular-notice-frame.png").string(),
        1.0f, pulp::view::ScreenshotBackend::skia));

    auto* card = chrome->composer_card();
    REQUIRE(card != nullptr);
    REQUIRE(card->bounds().width > 0);
    shell.poll_surfaces();

    shell.mentions().show_notice("Downloading CV funk Blank 8HP…",
                                 forge_modular::MentionOverlay::Tone::progress);
    CHECK(shell.mentions().notice_tone() ==
          forge_modular::MentionOverlay::Tone::progress);
    // THE COLOUR ACTUALLY APPLIED, not the tone that was asked for. Asserting
    // only the stored enum cannot see a notice that is amber whatever it is
    // told, which is precisely what it was.
    CHECK(shell.mentions().notice_color().to_argb32() ==
          forge::design::color::text_muted.to_argb32());
    auto* panel = shell.mentions().panel();
    REQUIRE(panel != nullptr);
    // SAME INSET, SAME WIDTH as the card it is a message about.
    CHECK(panel->flex().preferred_width == card->bounds().width);
    float card_left = 0;
    for (const pulp::view::View* v = card; v; v = v->parent())
        card_left += v->bounds().x;
    CHECK(panel->flex().margin_left == card_left);

    // A genuine block earns amber; ordinary progress does not.
    shell.mentions().show_notice("needs a sign-in",
                                 forge_modular::MentionOverlay::Tone::blocked);
    CHECK(shell.mentions().notice_tone() ==
          forge_modular::MentionOverlay::Tone::blocked);
    CHECK(shell.mentions().notice_color().to_argb32() ==
          forge::design::color::amber.to_argb32());

    // The ROWS keep the dropdown's own geometry: a 680-wide list of six rows
    // is mostly empty space.
    shell.mentions().handle_text("@", 1);
    CHECK(panel->flex().preferred_width < card->bounds().width);
    shell.on_view_closed(*view);
}

TEST_CASE("a download says how it ended, not only that it started",
          "[mention][seam]") {
    // "fetching…" was the last word said on the subject, so a finished
    // download and a stuck one looked identical.
    ScopedHome home("forge-download-outcome");
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

    // Signed in, downloads on: a free module is fetched rather than refused.
    {
        std::filesystem::create_directories(
            home.dir / "Library" / "Application Support" / "Rack2");
        std::ofstream f(home.dir / "Library" / "Application Support" / "Rack2" /
                        "settings.json");
        f << R"({"token": "not-a-real-token"})";
    }
    forge_modular::MentionCandidate what;
    what.brand = "CV funk";
    what.name = "CV funk Blank 8HP";
    what.slug = "CVfunk/Blank8HP";
    what.state = forge_modular::MentionCandidate::Availability::available;
    REQUIRE(shell.mentions().on_refused);
    shell.mentions().on_refused(what);
    INFO(shell.mentions().notice());
    // Named ONCE, and as progress rather than as a warning.
    CHECK(shell.mentions().notice() == "Downloading CV funk Blank 8HP…");

    // Outcome first, remedy second.
    const auto status = home.dir / "Library" / "Application Support" /
                        "Forge Modular" / "runs" / "install-status";
    std::filesystem::create_directories(status.parent_path());
    { std::ofstream f(status); f << "0"; }
    shell.poll_surfaces();
    CHECK(shell.mentions().notice() ==
          "Downloaded CV funk Blank 8HP. Restart Rack to use it.");
    CHECK(shell.mentions().notice_tone() ==
          forge_modular::MentionOverlay::Tone::progress);

    // And a failure is a failure, in amber, naming where to look.
    shell.mentions().on_refused(what);
    { std::ofstream f(status); f << "1"; }
    shell.poll_surfaces();
    INFO(shell.mentions().notice());
    CHECK(shell.mentions().notice().find("Could not download CV funk Blank 8HP")
          == 0);
    CHECK(shell.mentions().notice().find("install.log") != std::string::npos);
    CHECK(shell.mentions().notice_tone() ==
          forge_modular::MentionOverlay::Tone::blocked);
    shell.on_view_closed(*view);
}

TEST_CASE("the mention list is keyboard-first", "[mention]") {
    forge_modular::MentionOverlay overlay;
    auto view = overlay.build();
    overlay.set_source(test_library);
    std::string chosen;
    overlay.on_choose = [&](const std::string& slug) { chosen = slug; };

    overlay.handle_text("@", 1);
    REQUIRE(overlay.is_open());
    CHECK(overlay.selected_index() == 0);

    CHECK(overlay.handle_key(125));               // down
    CHECK(overlay.selected_index() == 1);
    CHECK(overlay.handle_key(126));               // up
    CHECK(overlay.selected_index() == 0);

    CHECK(overlay.handle_key(36));                // enter
    CHECK(chosen == "Fundamental/VCO");
    CHECK_FALSE(overlay.is_open());               // and it closes

    // Escape dismisses without choosing.
    chosen.clear();
    overlay.handle_text("@", 1);
    CHECK(overlay.handle_key(53));
    CHECK_FALSE(overlay.is_open());
    CHECK(chosen.empty());
}

TEST_CASE("a module you do not have can still be named", "[mention]") {
    // This used to refuse: wiring a patch to a module nobody has produces a
    // patch that cannot sound, so Enter did nothing on those rows.
    //
    // It was the wrong place for that check. Somebody types @, sees six rows,
    // and can select none of them — reported twice as the list being broken,
    // which is what it is from the outside. The generator already refuses an
    // uninstalled module at build time, in words it already shows.
    //
    // So the name goes in, and the row says what still has to happen.
    forge_modular::MentionOverlay overlay;
    auto view = overlay.build();
    overlay.set_source(test_library);
    std::string chosen;
    overlay.on_choose = [&](const std::string& slug) { chosen = slug; };
    forge_modular::MentionCandidate announced;
    overlay.on_refused = [&](const forge_modular::MentionCandidate& c) {
        announced = c;
    };

    overlay.handle_text("@drum", 5);
    REQUIRE(overlay.candidates().size() == 1);
    CHECK_FALSE(overlay.candidates()[0].insertable());     // 4ms, not installed

    CHECK(overlay.handle_key(36));                 // Enter
    CHECK(chosen == "4ms-ProducerPack/DrumBus");   // it went in
    CHECK_FALSE(overlay.is_open());                // and the list closed
    // And it was announced rather than inserted in silence.
    CHECK(announced.name == "DrumBus");
}

TEST_CASE("the selection starts on the first row", "[mention]") {
    // It used to skip to the first INSTALLED row, so the highlight landed
    // somewhere other than where the eye does. That made sense while the other
    // rows could not be chosen; now that every row can, skipping past what
    // somebody is looking at is the same confusion in a smaller form.
    forge_modular::MentionOverlay overlay;
    auto view = overlay.build();
    overlay.set_source([](const std::string&) {
        using A = forge_modular::MentionCandidate::Availability;
        return std::vector<forge_modular::MentionCandidate>{
            {"Vult", "Freak", "Vult/Freak", A::paid},
            {"Fundamental", "VCA", "Fundamental/VCA", A::ready},
        };
    });
    overlay.handle_text("@", 1);
    CHECK(overlay.selected_index() == 0);
    // And the first row is choosable, which is the point.
    std::string chosen;
    overlay.on_choose = [&](const std::string& slug) { chosen = slug; };
    overlay.handle_key(36);
    CHECK(chosen == "Vult/Freak");
}

// ── the generator log ────────────────────────────────────────────────────────

namespace {

/// A refusal captured verbatim from a real run on a second machine, where it
/// reached a log file and never a person. Kept exactly as the generator wrote
/// it -- a hand-tidied paraphrase would agree with the classifier by
/// construction and prove nothing.
constexpr const char* kRealRefusal =
    "forge-modular: patch\n"
    "tools: /Users/x/Applications/Forge Modular.app/Contents/Resources/tools\n"
    "prompt: a kick and hat pattern where the hats swing against the kick\n"
    "---\n"
    "  hold on \xE2\x80\x94 this asks for something you don't have installed:\n"
    "\n"
    "  no drum module is installed. These would do it:\n"
    "      free     4ms-ProducerPack/Decay            Decay  (4ms)\n"
    "      free     4ms-ProducerPack/DrumBus          DrumBus  (4ms)\n"
    "\n"
    "  install one in Rack's Library, then ask again \xE2\x80\x94\n"
    "  or pass --anyway: Rack keeps missing modules as\n"
    "  placeholders and offers to fetch them when you open it.\n";

/// A gate rejection and a retry, also captured from a real run.
constexpr const char* kRealGateRetry =
    "forge-modular: module\n"
    "prompt: a 4 HP clock divider with reset and four divisions\n"
    "---\n"
    "      DIVIDELY: param DIV4_PARAM defaults to the TOP of its range "
    "(7.0 in 0.0..7.0); the knob has nowhere left to turn\n"
    "  asking the model (retry 2)\xE2\x80\xA6\n"
    "  uses Pulp DSP: trigger.hpp\n"
    "  installed \xE2\x86\x92 /Users/x/Library/Application Support/Rack2/plugins-mac-arm64/"
    "ForgeModular-2.0.0-mac-arm64.vcvplugin\n";

std::string write_log(const char* body, const char* name) {
    const auto p = std::filesystem::temp_directory_path() / name;
    std::ofstream(p) << body;
    return p.string();
}

}  // namespace

TEST_CASE("a real refusal is recognised as a refusal", "[buildlog]") {
    forge_modular::BuildMonitor mon;
    mon.watch(write_log(kRealRefusal, "fm-refusal.log"));
    const auto added = mon.poll();
    REQUIRE_FALSE(added.empty());

    CHECK(mon.outcome() == forge_modular::BuildOutcome::refused);

    // And the headline is the reason, not the last line of narration. Burying
    // it under "step 7 of 9" is how this failed on a real machine.
    INFO("headline: " << mon.headline());
    CHECK(mon.headline().find("hold on") != std::string::npos);
}

TEST_CASE("a gate rejection is not a failure, and a success is", "[buildlog]") {
    forge_modular::BuildMonitor mon;
    mon.watch(write_log(kRealGateRetry, "fm-gate.log"));
    mon.poll();

    // The run rejected an attempt, retried, and landed an artifact. That is the
    // pipeline working, and calling it "failed" would teach the user to ignore
    // the status line.
    CHECK(mon.outcome() == forge_modular::BuildOutcome::done);

    int gates = 0, retries = 0, successes = 0;
    for (const auto& l : mon.lines()) {
        if (l.kind == forge_modular::BuildLine::Kind::gate) ++gates;
        if (l.kind == forge_modular::BuildLine::Kind::retry) ++retries;
        if (l.kind == forge_modular::BuildLine::Kind::success) ++successes;
    }
    CHECK(gates >= 1);
    CHECK(retries >= 1);
    CHECK(successes >= 1);
}

TEST_CASE("unfamiliar output is progress, never failure", "[buildlog]") {
    // The generator's wording will drift. A classifier that invented failures
    // from lines it did not recognise would cry wolf on every change.
    CHECK(forge_modular::BuildMonitor::classify("  writing panel SVG") ==
          forge_modular::BuildLine::Kind::progress);
    CHECK(forge_modular::BuildMonitor::classify("something entirely new") ==
          forge_modular::BuildLine::Kind::progress);
    CHECK(forge_modular::BuildMonitor::classify("Traceback (most recent call last):") ==
          forge_modular::BuildLine::Kind::error);
}

TEST_CASE("the monitor streams rather than re-reading", "[buildlog]") {
    const auto path = std::filesystem::temp_directory_path() / "fm-stream.log";
    { std::ofstream f(path); f << "forge-modular: module\n"; }

    forge_modular::BuildMonitor mon;
    mon.watch(path.string());
    CHECK(mon.poll().size() == 1);
    CHECK(mon.poll().empty());                 // nothing new, nothing re-reported

    { std::ofstream f(path, std::ios::app); f << "  asking the model (retry 2)\n"; }
    const auto more = mon.poll();
    REQUIRE(more.size() == 1);
    CHECK(more[0].kind == forge_modular::BuildLine::Kind::retry);

    // A new run truncates the log. Without noticing, the reader would seek past
    // the end and go silent for the rest of the session.
    { std::ofstream f(path, std::ios::trunc); f << "forge-modular: patch\n"; }
    CHECK(mon.poll().size() == 1);
    CHECK(mon.lines().size() == 1);
}

TEST_CASE("Random fills the composer, and never repeats itself", "[seam]") {
    // Reported twice by a user: first as "the same suggestion every time", then
    // as a button that did nothing at all. Both are asserted here.
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

    auto row = shell.composer_row();
    REQUIRE(row.left.size() >= 2);
    const auto& random = row.left[1];
    REQUIRE(random.label == "Random");
    REQUIRE(random.on_click);            // a button with no handler is decoration

    auto* input = shell.chrome()->prompt_input();
    REQUIRE(input != nullptr);

    std::set<std::string> seen;
    std::string previous;
    for (int i = 0; i < 6; ++i) {
        random.on_click();
        const auto text = input->text();
        INFO("press " << i << ": " << text);
        REQUIRE_FALSE(text.empty());     // every press, not every other one
        CHECK(text != previous);         // never the same twice running
        previous = text;
        seen.insert(text);
    }
    CHECK(seen.size() > 1);              // not one value forever
}

TEST_CASE("a run reports itself on the status card, not in the chat",
          "[buildlog][seam]") {
    // Forge narrates a build on the status card and keeps the transcript for
    // things worth reading afterwards. Pushing every generator line into the
    // chat produced dozens of low-value bubbles per build -- gate notes,
    // retries, compiler paths -- and buried the one message that mattered.
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
    auto* chrome = shell.chrome();

    const auto log = std::filesystem::temp_directory_path() /
                     "forge-modular-narration.log";
    std::filesystem::remove(log);
    shell.watch_build_log(log.string());
    const int before = chrome->chat_line_count();

    // Ordinary progress and gate rejections: the card moves, the chat does not.
    {
        std::ofstream f(log);
        f << "  asking the model\n"
          << "  generated FOLDR (6HP, 5 params, 2 in, 1 out)\n"
          << "  FOLDR: param MIX_PARAM defaults to the TOP of its range\n"
          << "  asking the model (retry 1)\n";
    }
    CHECK(shell.pump_build_log() == 4);
    CHECK(chrome->chat_line_count() == before);        // nothing dumped
    CHECK_FALSE(chrome->status_detail_text().empty()); // but the card says so
    CHECK(chrome->status_detail_text().find("MIX_PARAM") != std::string::npos);

    // A refusal DOES earn the transcript: the run stopped, and the reason
    // names something the user can act on.
    {
        std::ofstream f(log, std::ios::app);
        f << "  Hold on -- this asks for something you don't have installed\n";
    }
    CHECK(shell.pump_build_log() == 1);
    CHECK(chrome->chat_line_count() == before + 1);
    CHECK(shell.build_outcome() == forge_modular::BuildOutcome::refused);

    // An idle tick adds nothing anywhere.
    CHECK(shell.pump_build_log() == 0);
    CHECK(chrome->chat_line_count() == before + 1);

    // The card's note is trimmed: a 200-character compiler path would push
    // everything else out of a two-line card.
    {
        std::ofstream f(log, std::ios::app);
        f << "      " << std::string(300, 'x') << "\n";
    }
    shell.pump_build_log();
    CHECK(chrome->status_detail_text().size() <= 130);

    std::filesystem::remove(log);
}

namespace {

/// Point HOME at a scratch directory for one test, so a preference the test
/// writes or clicks can never land in the person-at-the-machine's real
/// settings file, and their real file can never leak into an assertion.
struct ScratchHome {
    ScratchHome() {
        const char* real = std::getenv("HOME");
        saved = real ? real : "";
        dir = std::filesystem::temp_directory_path() / "forge-modular-prefs";
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
        std::filesystem::create_directories(
            dir / "Library" / "Application Support" / "Forge Modular", ec);
        ::setenv("HOME", dir.string().c_str(), /*overwrite=*/1);
    }
    ~ScratchHome() {
        if (saved.empty()) ::unsetenv("HOME");
        else ::setenv("HOME", saved.c_str(), /*overwrite=*/1);
    }
    std::filesystem::path settings() const {
        return dir / "Library" / "Application Support" / "Forge Modular" /
               "settings.json";
    }
    void write(const std::string& json) const {
        std::ofstream out(settings());
        out << json;
    }
    std::filesystem::path dir;
    std::string saved;
};

}  // namespace

TEST_CASE("preferences are read from the file patch.py owns", "[prefs][seam]") {
    ScratchHome home;

    // No file at all: the fallback is the answer, exactly as patch.py fills
    // its defaults in.
    CHECK(forge_modular::modular_setting("module_source", "prefer_existing") ==
          "prefer_existing");
    CHECK(forge_modular::auto_download_pref() == "entitled");

    home.write("{\"module_source\": \"balanced\", \"auto_download\": \"none\"}");
    CHECK(forge_modular::modular_setting("module_source", "prefer_existing") ==
          "balanced");
    CHECK(forge_modular::auto_download_pref() == "none");

    // A key the file does not carry falls back rather than misreading a
    // neighbour's value.
    CHECK(forge_modular::modular_setting("made_up", "fallback") == "fallback");
}

namespace {

/// How many options a product choice renders, walked through the chrome's
/// accessor so the count comes from the LIVE sheet rather than the shell.
int options_of(const forge::ForgeChrome& chrome, std::size_t choice) {
    int n = 0;
    while (chrome.settings_product_choice_button(choice, n)) ++n;
    return n;
}

/// Which option of a choice is painted as chosen. Exactly one must wear the
/// primary style -- the sheet's own selected look -- or the row reads as
/// two answers at once. Returns {count, index}.
std::pair<int, int> chosen_of(const forge::ForgeChrome& chrome,
                              std::size_t choice) {
    int n = 0, at = -1;
    for (int i = 0; auto* b = chrome.settings_product_choice_button(choice, i);
         ++i) {
        if (b->style() == pulp::view::TextButton::Style::primary) {
            ++n;
            at = i;
        }
    }
    return {n, at};
}

}  // namespace

TEST_CASE("the generation preferences live in Settings, on Permissions",
          "[prefs][seam]") {
    // The user asked for module_source and auto_download to be theirs to
    // change, in the real gear-menu settings sheet -- a first draft put them
    // on the Home screen and was rightly rejected as bolted on. The sheet
    // renders the rows itself (ForgeShell::settings_choices), so they share
    // the built-in rows' idiom by construction.
    ScratchHome home;
    home.write("{\"module_source\": \"prefer_generated\", "
               "\"auto_download\": \"none\"}");

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
    auto* chrome = shell.chrome();
    REQUIRE(chrome != nullptr);

    // Both controls are in the live sheet, every option present and handled.
    // Four rows now: the two choices, the library-index action and the
    // build-details report, all through the same settings_choices() hook.
    REQUIRE(chrome->settings_product_choice_count() == 4);
    REQUIRE(options_of(*chrome, 0) == 3);
    REQUIRE(options_of(*chrome, 1) == 2);
    for (std::size_t c = 0; c < 2; ++c)
        for (int i = 0; auto* b = chrome->settings_product_choice_button(c, i);
             ++i)
            REQUIRE(b->on_click);

    // And OFF the Home screen: the accessory is the Module|Patch tabs and
    // nothing else.
    auto accessory = shell.home_accessory();
    REQUIRE(accessory != nullptr);
    int home_buttons = 0;
    for (std::size_t i = 0; i < accessory->child_count(); ++i)
        if (dynamic_cast<pulp::view::TextButton*>(accessory->child_at(i)))
            ++home_buttons;
    CHECK(home_buttons == 2);

    // The sheet shows the FILE's values, not the defaults: a value written
    // by hand last week must not be undone by the control's first paint.
    CHECK(shell.module_source_shown() == "prefer_generated");
    CHECK(shell.auto_download_shown() == "none");
    CHECK(chosen_of(*chrome, 0) == std::pair{1, 2});   // Build my own
    CHECK(chosen_of(*chrome, 1) == std::pair{1, 1});   // Off

    // Clicking a different option adopts it, restyles exclusively, and hands
    // the write to patch.py -- the one validated writer -- rather than
    // writing JSON from C++. The bare shell records the command and launches
    // nothing, so this asserts the decision without touching the machine.
    const auto before = shell.launched().size();
    chrome->settings_product_choice_button(0, 1)->on_click();
    CHECK(shell.module_source_shown() == "balanced");
    CHECK(chosen_of(*chrome, 0) == std::pair{1, 1});
    REQUIRE(shell.launched().size() == before + 1);
    CHECK(shell.launched().back().find("patch.py setting module_source balanced")
          != std::string::npos);

    chrome->settings_product_choice_button(1, 0)->on_click();
    CHECK(shell.auto_download_shown() == "entitled");
    CHECK(chosen_of(*chrome, 1) == std::pair{1, 0});
    REQUIRE(shell.launched().size() == before + 2);
    CHECK(shell.launched().back().find("patch.py setting auto_download entitled")
          != std::string::npos);

    // Re-clicking the already-chosen option is a no-op: no second process to
    // write what is already written.
    chrome->settings_product_choice_button(0, 1)->on_click();
    chrome->settings_product_choice_button(1, 0)->on_click();
    CHECK(shell.launched().size() == before + 2);

    // The two ACTION rows are reachable too: refresh the index, and copy the
    // build report. A row that exists and cannot be pressed is the shape of
    // half the defects in this product's history.
    REQUIRE(chrome->settings_product_action_button(2) != nullptr);
    REQUIRE(chrome->settings_product_action_button(2)->on_click);
    REQUIRE(chrome->settings_product_action_button(3) != nullptr);
    REQUIRE(chrome->settings_product_action_button(3)->on_click);

    // The visual proof: the sheet, open on Permissions, rendered headlessly.
    // Kept in temp for a human to look at; the render succeeding is also the
    // layout pass that would catch a row that cannot fit its pane.
    chrome->open_permissions_settings();
    pulp::view::FrameClock clock;
    view->set_frame_clock(&clock);
    for (int frame = 0; frame < 8; ++frame) clock.pump_activity(0.040f);
    const auto shot = std::filesystem::temp_directory_path() /
                      "modular-settings-permissions.png";
    REQUIRE(pulp::view::render_to_file(
        *view, forge::ForgeChrome::kDesignWidth,
        forge::ForgeChrome::kDesignHeight, shot.string(), 1.0f,
        pulp::view::ScreenshotBackend::skia));
    view->set_frame_clock(nullptr);
    shell.on_view_closed(*view);
}

TEST_CASE("products that contribute no settings rows keep their sheet as-is",
          "[prefs][seam]") {
    // The hook must cost the three original products nothing: no heading,
    // no empty section, zero controls. Their Home frames are byte-pinned by
    // the no-leak baselines; this pins the sheet side of the same promise.
    HermeticProjects isolated;
    forge::ForgeFxShell shell;
    pulp::format::PrepareContext pc;
    pc.sample_rate = kSr; pc.max_buffer_size = kFrames;
    pc.input_channels = 1; pc.output_channels = 2;
    shell.prepare(pc);
    auto view = shell.create_view();
    REQUIRE(view != nullptr);
    REQUIRE(shell.chrome() != nullptr);
    CHECK(shell.chrome()->settings_product_choice_count() == 0);
    CHECK(shell.chrome()->settings_product_choice_button(0, 0) == nullptr);
    shell.on_view_closed(*view);
}

TEST_CASE("a preference chosen in one editor survives into the next",
          "[prefs][seam]") {
    // End to end: the click runs the REAL patch.py (a synchronous launcher
    // under a scratch HOME), the write lands in the real file format, and a
    // fresh editor's sheet reads it back. This is the loop a person actually
    // lives: change a setting, close the plugin window, open it again.
    ScratchHome home;
    HermeticProjects isolated;
    forge_modular::ForgeModularShell shell;
    shell.set_launcher([](const std::string& command) {
        std::string out;
        forge_modular::ProcessEngine::run(command, out);
    });
    pulp::state::StateStore store;
    shell.set_state_store(&store);
    shell.define_parameters(store);
    pulp::format::PrepareContext pc;
    pc.sample_rate = kSr; pc.max_buffer_size = kFrames;
    pc.input_channels = 1; pc.output_channels = 2;
    shell.prepare(pc);

    auto view = shell.create_view();
    REQUIRE(view != nullptr);
    REQUIRE(shell.chrome() != nullptr);
    CHECK(chosen_of(*shell.chrome(), 0) == std::pair{1, 0});  // the default
    shell.chrome()->settings_product_choice_button(0, 2)->on_click();
    CHECK(shell.module_source_shown() == "prefer_generated");
    // The write went through patch.py into the scratch HOME's file.
    REQUIRE(std::filesystem::exists(home.settings()));

    shell.on_view_closed(*view);
    view.reset();

    auto reopened = shell.create_view();
    REQUIRE(reopened != nullptr);
    REQUIRE(shell.chrome() != nullptr);
    REQUIRE(shell.chrome()->settings_product_choice_count() == 4);
    // The fresh sheet paints the persisted choice, not the default.
    CHECK(chosen_of(*shell.chrome(), 0) == std::pair{1, 2});
    shell.on_view_closed(*reopened);
}

TEST_CASE("the explanation-depth tabs are patch-only and switch", "[depth][seam]") {
    // From the prototype: three depths in the Build title bar, wrapped in the
    // `isPatch` guard. A module build has one artifact and nothing to narrate
    // at three depths, so the control must not appear for it.
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

    using Shell = forge_modular::ForgeModularShell;

    // A module build has the control mounted but hidden: the title bar is
    // built once when the editor opens, so a control that is absent for a
    // module can never appear when the user switches to Patch.
    shell.set_artifact(forge_modular::Artifact::module);
    // The group stays mounted and visible -- Open in Rack lives in it too --
    // but the depth TABS are hidden, because a module has nothing to narrate
    // at three depths.
    auto hidden = shell.build_accessory();
    REQUIRE(hidden != nullptr);
    CHECK_FALSE(hidden->child_at(0)->visible());

    shell.set_artifact(forge_modular::Artifact::patch);
    auto tabs = shell.build_accessory();
    REQUIRE(tabs != nullptr);
    // Three depths, Open in Rack, and the pill saying whether Rack is there
    // to open it in.
    REQUIRE(tabs->child_count() == 5);

    // Standard is the default: the middle setting, not an extreme.
    CHECK(shell.depth() == Shell::Depth::standard);
    CHECK(shell.shows_reasoning());
    CHECK_FALSE(shell.shows_asides());

    // Every tab is clickable and actually changes the depth -- the Random
    // button shipped once with no handler at all, so a handler-less control
    // is a defect this suite now names.
    for (int i = 0; i < 3; ++i) {
        auto* b = dynamic_cast<pulp::view::TextButton*>(tabs->child_at(i));
        REQUIRE(b != nullptr);
        REQUIRE(b->on_click);
    }

    dynamic_cast<pulp::view::TextButton*>(tabs->child_at(0))->on_click();
    CHECK(shell.depth() == Shell::Depth::terse);
    CHECK_FALSE(shell.shows_reasoning());   // Terse drops the "why"
    CHECK_FALSE(shell.shows_asides());

    dynamic_cast<pulp::view::TextButton*>(tabs->child_at(2))->on_click();
    CHECK(shell.depth() == Shell::Depth::learning);
    CHECK(shell.shows_reasoning());         // Learning adds to it, never replaces
    CHECK(shell.shows_asides());

    // Exactly one tab reads as selected, whichever is chosen -- asserted on the
    // colour actually painted, not the style enum. The first version of this
    // control passed a style-only check while rendering two tabs in accent and
    // the real selection in plain white: to a reader, two looked chosen. That
    // is the same "both tabs highlighted at once" defect reported on Home, and
    // only a colour assertion catches it.
    for (int chosen = 0; chosen < 3; ++chosen) {
        dynamic_cast<pulp::view::TextButton*>(tabs->child_at(chosen))->on_click();
        int raised = 0, bright = 0;
        for (int i = 0; i < 3; ++i) {
            auto* b = dynamic_cast<pulp::view::TextButton*>(tabs->child_at(i));
            REQUIRE(b->child_count() >= 1);
            auto* lbl = dynamic_cast<pulp::view::Label*>(b->child_at(0));
            REQUIRE(lbl != nullptr);
            if (b->style() == pulp::view::TextButton::Style::secondary) ++raised;
            if (lbl->text_color() == forge::design::color::text) ++bright;
            // No unselected tab may wear the accent: an accent sibling reads
            // as more chosen than the chosen one.
            if (i != chosen)
                CHECK(lbl->text_color() != forge::design::color::accent);
        }
        INFO("chose tab " << chosen);
        CHECK(raised == 1);
        CHECK(bright == 1);
    }
}

TEST_CASE("the patch composer renders its depth tabs", "[depth][render]") {
    // The visual proof: the tabs are in the live Build title bar, not just
    // reachable through build_accessory(). Renders the real screen.
    HermeticProjects isolated;
    forge_modular::ForgeModularShell shell;
    pulp::state::StateStore store;
    shell.set_state_store(&store);
    shell.define_parameters(store);
    pulp::format::PrepareContext pc;
    pc.sample_rate = kSr; pc.max_buffer_size = kFrames;
    pc.input_channels = 1; pc.output_channels = 2;
    shell.prepare(pc);
    shell.set_artifact(forge_modular::Artifact::patch);
    auto view = shell.create_view();
    REQUIRE(view != nullptr);

    auto* chrome = shell.chrome();
    REQUIRE(chrome != nullptr);
    chrome->enter_build();
    REQUIRE(chrome->mode() == forge::ForgeChrome::Mode::Build);

    const auto shot = std::filesystem::temp_directory_path() /
                      "modular-patch-composer.png";
    REQUIRE(pulp::view::render_to_file(
        *view, forge::ForgeChrome::kDesignWidth, forge::ForgeChrome::kDesignHeight,
        shot.string(), /*scale=*/1.0f, pulp::view::ScreenshotBackend::skia));
    // A blank frame is not a passing frame.
    CHECK(std::filesystem::file_size(shot) > 20000);
}

namespace {

/// A small rack: a 12 HP source into an 8 HP filter into a 6 HP output.
std::vector<forge_modular::RackModule> sample_rack() {
    using forge_modular::Port;
    using forge_modular::RackModule;
    RackModule vco{"VCO", "Fundamental", "VCO-1", 12,
                   {Port{"out", "OUT", 0.5f, 318.0f, false}}, true, true};
    RackModule vcf{"VCF", "Fundamental", "VCF", 8,
                   {Port{"in", "IN", 0.25f, 300.0f, true},
                    Port{"cv", "CV", 0.5f, 300.0f, true},
                    Port{"out", "OUT", 0.75f, 340.0f, false}}, true, true};
    RackModule out{"OUT", "Fundamental", "Audio", 6,
                   {Port{"l", "L", 0.3f, 320.0f, true},
                    Port{"r", "R", 0.7f, 320.0f, true}}, true, true};
    return {vco, vcf, out};
}

}  // namespace

TEST_CASE("panels butt together at their true widths", "[rack]") {
    // A preview that fudges a panel's width to tidy a row is lying about the
    // rack the user will get, so this asserts the arithmetic exactly.
    const auto mods = sample_rack();
    const auto L = forge_modular::layout_rack(mods, 1000.0f, 600.0f);

    REQUIRE(L.panels.size() == 3);
    CHECK(L.total_width == Approx((12 + 8 + 6) * forge_modular::kHorizontalPitch));

    // No gutters: each panel starts exactly where the previous one ended.
    for (std::size_t i = 1; i < L.panels.size(); ++i) {
        INFO("seam " << i);
        CHECK(L.panels[i].x == Approx(L.panels[i - 1].x + L.panels[i - 1].width));
    }
    // Widths stay in proportion to HP -- 12 HP is exactly twice 6 HP.
    CHECK(L.panels[0].width == Approx(L.panels[2].width * 2.0f));
    // And the strip is centred rather than pinned left.
    const float right = L.panels.back().x + L.panels.back().width;
    CHECK(L.panels.front().x == Approx(1000.0f - right).margin(0.01));
}

TEST_CASE("a small rack is not blown up past life size", "[rack]") {
    // Two modules stretched to fill a wide window stop looking like Eurorack.
    const std::vector<forge_modular::RackModule> two{sample_rack()[0], sample_rack()[2]};
    const auto L = forge_modular::layout_rack(two, 4000.0f, 3000.0f);
    CHECK(L.scale <= 1.05f);

    // A cramped viewport shrinks to fit rather than overflowing.
    const auto tight = forge_modular::layout_rack(sample_rack(), 300.0f, 600.0f);
    CHECK(tight.scale < 1.0f);
    CHECK(tight.total_width * tight.scale <= 300.0f);
}

TEST_CASE("a jack lands on its captured centre", "[rack]") {
    const auto mods = sample_rack();
    const auto L = forge_modular::layout_rack(mods, 1000.0f, 600.0f);

    const auto p = forge_modular::port_point(L, mods, "VCF", "cv", "VCO");
    const auto* box = L.panel("VCF");
    REQUIRE(box != nullptr);
    CHECK_FALSE(p.docked);                       // a placed module never docks
    CHECK(p.x == Approx(box->x + 0.5f * box->width));
    CHECK(p.y == Approx(box->y + 300.0f * L.scale));
    CHECK(p.name == "CV");
}

TEST_CASE("an unplaced module docks at the edge facing its partner", "[rack]") {
    // The honest degradation: no coordinates were ever captured, so ending at
    // the panel edge beats guessing a position that looks authoritative and is
    // wrong. It resolves the first time the module is placed.
    auto mods = sample_rack();
    mods[1].placed = false;                      // VCF, the middle panel
    const auto L = forge_modular::layout_rack(mods, 1000.0f, 600.0f);
    const auto* box = L.panel("VCF");
    REQUIRE(box != nullptr);

    // Partner on the left -> dock on the left edge; on the right -> right edge.
    const auto from_left = forge_modular::port_point(L, mods, "VCF", "in", "VCO");
    const auto from_right = forge_modular::port_point(L, mods, "VCF", "out", "OUT");
    CHECK(from_left.docked);
    CHECK(from_right.docked);
    CHECK(from_left.x == Approx(box->x + 3.0f));
    CHECK(from_right.x == Approx(box->x + box->width - 3.0f));

    // Several docked ends must not stack on one spot.
    const auto cv = forge_modular::port_point(L, mods, "VCF", "cv", "VCO");
    CHECK(from_left.y != Approx(cv.y));
    CHECK(std::abs(from_left.y - cv.y) >= 11.0f);

    // The name still comes through, so the explanation can still say "IN".
    CHECK(from_left.name == "IN");
}

TEST_CASE("a cable hangs, and hangs lower the further it spans", "[rack]") {
    forge_modular::JackPoint a{0.0f, 100.0f, "OUT", false};
    forge_modular::JackPoint near{100.0f, 100.0f, "IN", false};
    forge_modular::JackPoint far{800.0f, 100.0f, "IN", false};

    const auto short_cable = forge_modular::cable_curve(a, near);
    const auto long_cable = forge_modular::cable_curve(a, far);

    // Below both endpoints: a cable hangs, it does not arc over the rack.
    CHECK(short_cable.cy > 100.0f);
    CHECK(long_cable.cy > short_cable.cy);

    // Capped, or a very wide rack draws a coil on the floor.
    forge_modular::JackPoint miles{9000.0f, 100.0f, "IN", false};
    CHECK(forge_modular::cable_curve(a, miles).cy <= 100.0f + 160.0f);

    // Mid-build a cable has only reached partway across, and is not already at
    // full droop.
    const auto half = forge_modular::cable_curve(a, far, 0.5f);
    CHECK(half.x2 == Approx(400.0f));
    CHECK(half.cy < long_cable.cy);
    const auto unstarted = forge_modular::cable_curve(a, far, 0.0f);
    CHECK(unstarted.x2 == Approx(a.x));
}

TEST_CASE("cable colours are the ones Rack will show", "[rack]") {
    // Written into the patch's colour field at generation, so the preview is
    // not a private convention that diverges once the patch is opened.
    using forge_modular::SignalRole;
    CHECK(forge_modular::role_color(SignalRole::audio) == 0x00B56E);
    CHECK(forge_modular::role_color(SignalRole::pitch) == 0x3695EF);
    CHECK(forge_modular::role_color(SignalRole::clock) == 0xFFB437);
    CHECK(forge_modular::role_color(SignalRole::mod) == 0x8B4ADE);

    // All four distinct, or the roles stop being readable at a glance.
    std::set<std::uint32_t> seen;
    for (auto r : {SignalRole::audio, SignalRole::pitch,
                   SignalRole::clock, SignalRole::mod})
        seen.insert(forge_modular::role_color(r));
    CHECK(seen.size() == 4);
}

TEST_CASE("a degenerate viewport does not produce garbage", "[rack]") {
    // A window mid-resize can be zero-sized; the layout must stay finite.
    const auto L = forge_modular::layout_rack(sample_rack(), 0.0f, 0.0f);
    CHECK(L.scale > 0.0f);
    for (const auto& p : L.panels) {
        CHECK(std::isfinite(p.x));
        CHECK(std::isfinite(p.width));
        CHECK(p.width > 0.0f);
    }
    // And an empty rack lays out to nothing rather than dividing by zero.
    const auto empty = forge_modular::layout_rack({}, 800.0f, 600.0f);
    CHECK(empty.panels.empty());
    CHECK(std::isfinite(empty.scale));
}

namespace {

std::vector<forge_modular::Connection> sample_patch() {
    using forge_modular::Connection;
    using forge_modular::SignalRole;
    return {
        Connection{"VCO", "out", "VCF", "in", SignalRole::audio,
                   "The oscillator's output is the sound; everything else shapes it."},
        Connection{"VCF", "out", "OUT", "l", SignalRole::audio,
                   "Into the output, or there is silence."},
        Connection{"VCO", "out", "VCF", "cv", SignalRole::mod,
                   "A slow sweep of the cutoff, so the tone moves."},
    };
}

}  // namespace

TEST_CASE("hovering a line lights its cable and dims the rest", "[rack][hover]") {
    // The reason the preview is worth drawing at all: "the LFO is what makes it
    // move" means nothing until the cable it names is the one glowing.
    forge_modular::RackPreview preview;
    preview.set_rack(sample_rack(), sample_patch());

    // No hover is the normal state, and it must not look faded.
    for (std::size_t i = 0; i < 3; ++i) {
        INFO("cable " << i);
        CHECK(preview.cable_alpha(i) == Approx(1.0f));
    }

    preview.set_highlight(1);
    CHECK(preview.cable_alpha(1) == Approx(1.0f));
    CHECK(preview.cable_alpha(0) < 1.0f);
    CHECK(preview.cable_alpha(2) < 1.0f);

    // Letting go restores every cable, rather than leaving the rack dimmed.
    preview.set_highlight(std::nullopt);
    for (std::size_t i = 0; i < 3; ++i)
        CHECK(preview.cable_alpha(i) == Approx(1.0f));

    // A role hover lights every cable of that role -- two audio cables here.
    preview.highlight_role(forge_modular::SignalRole::audio);
    CHECK(preview.cable_alpha(0) == Approx(1.0f));
    CHECK(preview.cable_alpha(1) == Approx(1.0f));
    CHECK(preview.cable_alpha(2) < 1.0f);       // the modulation cable dims
    preview.highlight_role(std::nullopt);

    // An index past the end must not light nothing-at-all or read out of range.
    preview.set_highlight(99);
    CHECK_FALSE(preview.highlight().has_value());
    CHECK(preview.cable_alpha(0) == Approx(1.0f));
}

TEST_CASE("pointing at a cable in the rack finds that cable", "[rack][hover]") {
    // The half that was missing. Hovering a SENTENCE lit its cable, and only
    // that way round -- so the rack, which is the half a person actually looks
    // at, could not be interrogated at all. You could read the explanation to
    // find the cable, never point at the cable to find the explanation.
    forge_modular::RackPreview preview;
    preview.set_rack(sample_rack(), sample_patch());
    preview.set_bounds({0, 0, 900, 420});

    const auto L = preview.layout_for(900, 420);
    const auto& cables = preview.connections();
    REQUIRE(cables.size() == 3);

    // Halfway along each cable, which is the part of it that hangs in free
    // space and is the hardest place for a straight-line test to find.
    for (std::size_t i = 0; i < cables.size(); ++i) {
        const auto& c = cables[i];
        const auto a = forge_modular::port_point(L, preview.modules(),
                                                 c.from_module, c.from_port,
                                                 c.to_module);
        const auto b = forge_modular::port_point(L, preview.modules(),
                                                 c.to_module, c.to_port,
                                                 c.from_module);
        float x = 0, y = 0;
        forge_modular::cable_point(forge_modular::cable_curve(a, b), 0.5f, x, y);
        INFO("cable " << i << " midpoint " << x << "," << y);
        const auto found = preview.cable_at(x, y);
        REQUIRE(found.has_value());
        CHECK(*found == i);
    }

    // The negative control, and the one that matters: a point in the empty
    // air above the rack must find NOTHING. A hit test that answered "nearest
    // cable" would satisfy every assertion above while lighting a cable
    // whenever the pointer was anywhere on the stage at all.
    CHECK_FALSE(preview.cable_at(2.0f, 2.0f).has_value());
}

TEST_CASE("the pointer leaving the rack lets every cable go", "[rack][hover]") {
    forge_modular::RackPreview preview;
    preview.set_rack(sample_rack(), sample_patch());
    preview.set_bounds({0, 0, 900, 420});

    std::vector<std::optional<std::size_t>> heard;
    preview.on_cable_hover = [&](std::optional<std::size_t> i) { heard.push_back(i); };

    const auto L = preview.layout_for(900, 420);
    const auto& c = preview.connections()[1];
    const auto a = forge_modular::port_point(L, preview.modules(), c.from_module,
                                             c.from_port, c.to_module);
    const auto b = forge_modular::port_point(L, preview.modules(), c.to_module,
                                             c.to_port, c.from_module);
    float x = 0, y = 0;
    forge_modular::cable_point(forge_modular::cable_curve(a, b), 0.5f, x, y);

    preview.on_hover_move({x, y});
    REQUIRE(heard.size() == 1);
    REQUIRE(heard[0].has_value());
    CHECK(*heard[0] == 1);

    // Moving along the same cable must not chatter: a callback per hover
    // sample would rebuild the explanation's rows continuously.
    preview.on_hover_move({x + 0.5f, y});
    CHECK(heard.size() == 1);

    preview.on_mouse_leave();
    REQUIRE(heard.size() == 2);
    CHECK_FALSE(heard[1].has_value());
    for (std::size_t i = 0; i < 3; ++i)
        CHECK(preview.cable_alpha(i) == Approx(1.0f));
}

TEST_CASE("cables reach across as the patch is wired", "[rack][hover]") {
    forge_modular::RackPreview preview;
    preview.set_rack(sample_rack(), sample_patch());
    preview.set_bounds({0, 0, 900, 500});

    const auto L = preview.layout_for(900, 500);
    const auto mods = sample_rack();
    const auto from = forge_modular::port_point(L, mods, "VCO", "out", "VCF");
    const auto to = forge_modular::port_point(L, mods, "VCF", "in", "VCO");

    preview.set_progress(0.0f);
    CHECK(preview.progress() == Approx(0.0f));
    CHECK(forge_modular::cable_curve(from, to, 0.0f).x2 == Approx(from.x));

    preview.set_progress(1.0f);
    CHECK(forge_modular::cable_curve(from, to, 1.0f).x2 == Approx(to.x));

    // Out-of-range progress is clamped rather than drawing past the jack.
    preview.set_progress(3.0f);
    CHECK(preview.progress() == Approx(1.0f));
    preview.set_progress(-1.0f);
    CHECK(preview.progress() == Approx(0.0f));
}

TEST_CASE("the rack preview paints", "[rack][render]") {
    forge_modular::RackPreview preview;
    preview.set_rack(sample_rack(), sample_patch());
    preview.set_bounds({0, 0, 900, 500});
    preview.set_highlight(2);   // the modulation cable lit, the audio pair dimmed

    const auto shot = std::filesystem::temp_directory_path() / "modular-rack-preview.png";
    REQUIRE(pulp::view::render_to_file(preview, 900, 500, shot.string(),
                                       /*scale=*/1.0f,
                                       pulp::view::ScreenshotBackend::skia));
    CHECK(std::filesystem::file_size(shot) > 8000);   // a blank frame is not a pass
}

TEST_CASE("the preview names its panels", "[rack][render]") {
    // Assert the ink, not the box it sits in: a rack of anonymous rectangles
    // cannot be checked against what was asked for. Rendering the same rack
    // with and without names must produce different frames.
    auto render = [](const std::vector<forge_modular::RackModule>& mods,
                     const char* tag) {
        forge_modular::RackPreview preview;
        preview.set_rack(mods, sample_patch());
        preview.set_bounds({0, 0, 900, 500});
        const auto path = std::filesystem::temp_directory_path() /
                          (std::string("rack-names-") + tag + ".png");
        REQUIRE(pulp::view::render_to_file(preview, 900, 500, path.string(), 1.0f,
                                           pulp::view::ScreenshotBackend::skia));
        return read_all(path);
    };

    auto anonymous = sample_rack();
    for (auto& m : anonymous) { m.name.clear(); m.brand.clear(); }

    const auto named = render(sample_rack(), "named");
    const auto blank = render(anonymous, "blank");
    CHECK(named.size() > 8000);          // neither frame may be empty
    CHECK(blank.size() > 8000);
    CHECK(named != blank);               // the names actually reached the canvas
}

namespace {

/// A decoded frame, so a test can look at pixels rather than at file sizes.
struct Frame {
    std::size_t w = 0, h = 0;
    std::vector<std::uint8_t> rgba;
    bool ok() const { return w > 0 && h > 0; }
    std::array<int, 3> at(std::size_t x, std::size_t y) const {
        const std::size_t i = (y * w + x) * 4;
        return {rgba[i], rgba[i + 1], rgba[i + 2]};
    }
    /// Rec. 709 luma, which is what "lighter" and "darker" mean to an eye.
    double luma(std::size_t x, std::size_t y) const {
        const auto p = at(x, y);
        return 0.2126 * p[0] + 0.7152 * p[1] + 0.0722 * p[2];
    }
};

Frame decode(const std::vector<std::uint8_t>& png) {
    Frame f;
    if (png.empty()) return f;
    auto* data = CFDataCreate(nullptr, png.data(), static_cast<CFIndex>(png.size()));
    auto* src = CGImageSourceCreateWithData(data, nullptr);
    if (src && CGImageSourceGetCount(src) > 0) {
        auto* img = CGImageSourceCreateImageAtIndex(src, 0, nullptr);
        if (img) {
            const std::size_t w = CGImageGetWidth(img), h = CGImageGetHeight(img);
            std::vector<std::uint8_t> rgba(w * h * 4, 0);
            auto* cs = CGColorSpaceCreateDeviceRGB();
            auto* ctx = CGBitmapContextCreate(rgba.data(), w, h, 8, w * 4, cs,
                                              kCGImageAlphaPremultipliedLast);
            if (ctx) {
                CGContextDrawImage(ctx, CGRectMake(0, 0, (CGFloat)w, (CGFloat)h), img);
                f.w = w;
                f.h = h;
                f.rgba = std::move(rgba);
                CGContextRelease(ctx);
            }
            CGColorSpaceRelease(cs);
            CGImageRelease(img);
        }
    }
    if (src) CFRelease(src);
    CFRelease(data);
    return f;
}

/// A flat field of one colour, filed as a module's panel artwork.
///
/// Uniform on purpose: every assertion below is "this part of the panel is no
/// longer the colour the panel is", which only means anything when the panel
/// was one colour to begin with. Real artwork has knobs and silkscreen in it,
/// so a test written on real artwork would be measuring the artwork.
std::filesystem::path flat_panel_dir(const char* slug, const char* fill) {
    const auto dir = std::filesystem::temp_directory_path() /
                     (std::string("forge-flat-panel-") + slug);
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    std::ofstream svg(dir / (std::string(slug) + "-dark.svg"));
    svg << R"(<svg xmlns="http://www.w3.org/2000/svg" width="150" height="380" )"
        << R"(viewBox="0 0 150 380"><rect x="-4" y="-4" width="158" height="388" fill=")"
        << fill << R"("/></svg>)";
    return dir;
}

/// The mean luma of one column of the frame.
double column_luma(const Frame& f, std::size_t x) {
    if (!f.ok() || x >= f.w) return 0.0;
    double sum = 0;
    for (std::size_t y = 0; y < f.h; ++y) sum += f.luma(x, y);
    return sum / static_cast<double>(f.h);
}

/// How much of a disc is within `tol` of a colour, 0..1.
double disc_coverage(const Frame& f, double cx, double cy, double r,
                     std::array<int, 3> want, int tol) {
    if (!f.ok() || !(r > 0)) return 0.0;
    std::size_t hits = 0, total = 0;
    const int r_i = static_cast<int>(std::ceil(r));
    for (int dy = -r_i; dy <= r_i; ++dy)
        for (int dx = -r_i; dx <= r_i; ++dx) {
            if (dx * dx + dy * dy > r * r) continue;
            const double x = cx + dx, y = cy + dy;
            if (x < 0 || y < 0 || x >= f.w || y >= f.h) continue;
            ++total;
            const auto p = f.at(static_cast<std::size_t>(x), static_cast<std::size_t>(y));
            if (std::abs(p[0] - want[0]) <= tol && std::abs(p[1] - want[1]) <= tol &&
                std::abs(p[2] - want[2]) <= tol) ++hits;
        }
    return total ? double(hits) / double(total) : 0.0;
}

/// Pixels inside a disc that are both light and close to neutral grey.
///
/// The signature of bare metal, and specifically NOT of a cable: every cable
/// colour in the patch is saturated, so a light pixel with its three channels
/// within a few counts of each other can only have come from something painted
/// as hardware.
std::size_t count_light_neutral(const Frame& f, double cx, double cy, double r,
                                double min_luma = 100.0, int max_spread = 30) {
    std::size_t hits = 0;
    const int r_i = static_cast<int>(std::ceil(r));
    for (int dy = -r_i; dy <= r_i; ++dy)
        for (int dx = -r_i; dx <= r_i; ++dx) {
            if (dx * dx + dy * dy > r * r) continue;
            const double x = cx + dx, y = cy + dy;
            if (x < 0 || y < 0 || x >= f.w || y >= f.h) continue;
            const auto px = static_cast<std::size_t>(x);
            const auto py = static_cast<std::size_t>(y);
            if (f.luma(px, py) < min_luma) continue;
            const auto p = f.at(px, py);
            const int spread = *std::max_element(p.begin(), p.end()) -
                               *std::min_element(p.begin(), p.end());
            if (spread <= max_spread) ++hits;
        }
    return hits;
}

/// The darkest and lightest luma inside a disc.
std::pair<double, double> disc_luma_range(const Frame& f, double cx, double cy,
                                          double r) {
    double lo = 1e9, hi = -1e9;
    const int r_i = static_cast<int>(std::ceil(r));
    for (int dy = -r_i; dy <= r_i; ++dy)
        for (int dx = -r_i; dx <= r_i; ++dx) {
            if (dx * dx + dy * dy > r * r) continue;
            const double x = cx + dx, y = cy + dy;
            if (x < 0 || y < 0 || x >= f.w || y >= f.h) continue;
            const double l = f.luma(static_cast<std::size_t>(x),
                                    static_cast<std::size_t>(y));
            lo = std::min(lo, l);
            hi = std::max(hi, l);
        }
    if (lo > hi) return {0.0, 0.0};
    return {lo, hi};
}

}  // namespace

TEST_CASE("a rail screw goes where the artwork's hole is", "[rack]") {
    // The arithmetic behind the painted screw, asserted exactly. The generated
    // panels draw their screw HOLES at 7.62mm in from each side and 2.54mm
    // down from each rail; a painted screw a few points off one lands beside
    // it and reads as a printing error rather than as an improvement.
    forge_modular::PanelBox wide{"w", 100.0f, 40.0f, 12 * forge_modular::kHorizontalPitch,
                                 forge_modular::kPanelHeight};
    const auto pts = forge_modular::screw_points(wide, 1.0f);
    REQUIRE(pts.size() == 4);
    CHECK(pts[0].x == Approx(100.0f + 22.5f));
    CHECK(pts[1].x == Approx(100.0f + 180.0f - 22.5f));
    CHECK(pts[0].y == Approx(40.0f + 7.5f));
    CHECK(pts[2].y == Approx(40.0f + 380.0f - 7.5f));
    // Top pair first, then the bottom pair: the painting relies on nothing
    // else, but a silently reordered list would move every screw at once.
    CHECK(pts[0].y == Approx(pts[1].y));
    CHECK(pts[2].y == Approx(pts[3].y));

    // 22.5pt is 1.5 HP, so at 3 HP the two columns meet. One centred screw is
    // what the artwork draws there and what a real 3 HP module has -- a pair
    // 5mm apart would be neither.
    forge_modular::PanelBox narrow{"n", 0.0f, 0.0f, 3 * forge_modular::kHorizontalPitch,
                                   forge_modular::kPanelHeight};
    const auto few = forge_modular::screw_points(narrow, 1.0f);
    REQUIRE(few.size() == 2);
    CHECK(few[0].x == Approx(22.5f));
    CHECK(few[0].x == Approx(narrow.width / 2.0f));

    // Scaled with the rack, or the screws crawl inwards as the window shrinks.
    forge_modular::PanelBox half{"h", 0.0f, 0.0f,
                                 12 * forge_modular::kHorizontalPitch * 0.5f,
                                 forge_modular::kPanelHeight * 0.5f};
    const auto small = forge_modular::screw_points(half, 0.5f);
    REQUIRE(small.size() == 4);
    CHECK(small[0].x == Approx(11.25f));
    CHECK(small[0].y == Approx(3.75f));

    // A panel with no width lays out no screws rather than dividing by zero.
    CHECK(forge_modular::screw_points({}, 1.0f).empty());
}

TEST_CASE("the rack draws a screw rather than a hole", "[rack][render]") {
    // What the artwork gives us is a hollow ring a third of a point wide,
    // which at preview scale is a grey smudge. The tell that a screw was
    // painted over it is RANGE: a head lit from one side and a slot cut across
    // it put a light tone and a dark tone inside a few points of each other,
    // which a flat panel and a hairline ring cannot do.
    const auto dir = flat_panel_dir("SCREWTEST", "#808080");

    forge_modular::RackModule mod;
    mod.id = "m1";
    mod.name = "SCREWTEST";
    mod.brand = "ForgeModular";
    mod.hp = 12;
    forge_modular::RackPreview preview;
    preview.set_rack({mod}, {});
    preview.set_panel_directory(dir.string());
    preview.set_bounds({0, 0, 900, 900});

    const auto L = preview.layout_for(900, 900);
    REQUIRE(L.panels.size() == 1);
    const auto pts = forge_modular::screw_points(L.panels[0], L.scale);
    REQUIRE(pts.size() == 4);

    const auto f = decode(pulp::view::render_to_png(
        preview, 900, 900, 1.0f, pulp::view::ScreenshotBackend::skia));
    REQUIRE(f.ok());

    // Strictly inside the panel. A screw is seated ON the rail, so a disc of
    // its full radius reaches over the panel's top edge and picks up the dark
    // stage behind it and the lit bevel along it -- which between them supply
    // a light tone and a dark tone whether a screw was drawn or not. Measured
    // that way this test passes with the screws switched off entirely.
    const double r = forge_modular::kScrewRadius * L.scale * 0.7;
    const auto [lo, hi] = disc_luma_range(f, pts[0].x, pts[0].y, r);
    // The panel under it is a flat mid grey at luma 128. The head is lighter
    // than that and the slot is much darker; a hollow ring on a flat field
    // would put both ends within a few counts of 128.
    INFO("screw disc luma " << lo << " .. " << hi);
    CHECK(hi > 140.0);
    CHECK(lo < 60.0);

    // Two controls, both needed. The middle of the panel proves the panel is
    // flat where nothing was drawn.
    const auto [bare_lo, bare_hi] = disc_luma_range(
        f, L.panels[0].x + L.panels[0].width / 2.0f,
        L.panels[0].y + L.panels[0].height * 0.5f, r);
    INFO("bare panel disc luma " << bare_lo << " .. " << bare_hi);
    CHECK(bare_hi - bare_lo < 20.0);

    // And the same distance down from the same rail, midway between the two
    // screws: this is the one that matters, because it is the measurement
    // that would still see the top edge if the disc were reaching it.
    const auto [rail_lo, rail_hi] = disc_luma_range(
        f, (pts[0].x + pts[1].x) / 2.0f, pts[0].y, r);
    INFO("same rail, no screw: " << rail_lo << " .. " << rail_hi);
    CHECK(rail_hi - rail_lo < 20.0);

    std::filesystem::remove_all(dir);
}

TEST_CASE("butted panels still show where one ends and the next begins",
          "[rack][render]") {
    // Panels butt with no gutter, because that is what a rack does -- and with
    // nothing drawn at the join a row of modules reads as one undifferentiated
    // strip. A preview you cannot count the modules in cannot be checked
    // against what was asked for, which is most of what it is for.
    //
    // Three panels of the SAME flat artwork, so the only thing that can make
    // the join visible is the join being drawn. Real artwork would supply its
    // own edges and this test would pass without the seam existing.
    const auto dir = flat_panel_dir("SEAMTEST", "#7A7A7A");

    std::vector<forge_modular::RackModule> mods;
    for (int i = 0; i < 3; ++i) {
        forge_modular::RackModule m;
        m.id = "m" + std::to_string(i);
        m.name = "SEAMTEST";
        m.brand = "ForgeModular";
        m.hp = 10;
        mods.push_back(m);
    }
    forge_modular::RackPreview preview;
    preview.set_rack(mods, {});
    preview.set_panel_directory(dir.string());
    preview.set_bounds({0, 0, 700, 460});

    const auto L = preview.layout_for(700, 460);
    REQUIRE(L.panels.size() == 3);
    const auto f = decode(pulp::view::render_to_png(
        preview, 700, 460, 1.0f, pulp::view::ScreenshotBackend::skia));
    REQUIRE(f.ok());

    // The face of the middle panel, well away from either of its edges.
    const auto face = column_luma(
        f, static_cast<std::size_t>(L.panels[1].x + L.panels[1].width / 2.0f));
    // The join between the first and second. Sampled across three columns
    // because the seam is a hairline and the frame is anti-aliased; the
    // darkest of them is the seam wherever the rounding put it.
    const auto seam_x = static_cast<std::size_t>(L.panels[1].x + 0.5f);
    double seam = 1e9;
    for (std::size_t x = seam_x - 1; x <= seam_x + 1; ++x)
        seam = std::min(seam, column_luma(f, x));

    INFO("panel face luma " << face << ", seam luma " << seam);
    REQUIRE(face > 40.0);                 // the artwork actually painted
    // A quarter darker at the join. Without a seam the two are the same
    // column of the same grey and the ratio is 1.
    CHECK(seam < face * 0.75);

    // And the seam is a seam, not a wash: a column a tenth of a panel inboard
    // of it is back to the panel's own tone. A vignette wide enough to darken
    // the face would pass the check above while making the rack look filthy.
    const auto inboard = column_luma(
        f, static_cast<std::size_t>(L.panels[1].x + L.panels[1].width * 0.25f));
    INFO("inboard luma " << inboard);
    CHECK(inboard > face * 0.9);

    std::filesystem::remove_all(dir);
}

TEST_CASE("a cable lands on something on a panel with no artwork",
          "[rack][render]") {
    // The audio interface "missing connections": Core/AudioInterface2 is a
    // third-party module, so we have no picture of it and draw a plain face --
    // and every cable in the patch reached it and ended on a featureless slab.
    // Nothing there said a lead had arrived, so the module read as unwired
    // while being wired correctly.
    forge_modular::RackModule src{"SRC", "ForgeModular", "SRC", 8,
                                  {forge_modular::Port{"out", "OUT", 0.5f, 300.0f, false}}};
    forge_modular::RackModule dst{"DST", "Fundamental", "Whatever", 8,
                                  {forge_modular::Port{"in", "IN", 0.5f, 300.0f, true}}};
    forge_modular::RackPreview preview;
    preview.set_rack({src, dst},
                     {forge_modular::Connection{"SRC", "out", "DST", "in",
                                                forge_modular::SignalRole::audio, ""}});
    preview.set_bounds({0, 0, 700, 460});

    const auto L = preview.layout_for(700, 460);
    const auto jack = forge_modular::port_point(L, preview.modules(), "DST", "in", "SRC");
    REQUIRE_FALSE(jack.docked);

    const auto f = decode(pulp::view::render_to_png(
        preview, 700, 460, 1.0f, pulp::view::ScreenshotBackend::skia));
    REQUIRE(f.ok());

    const double r = forge_modular::kJackRadius * L.scale;
    // A metal nut: LIGHT and near-NEUTRAL. Both halves are needed. Luma alone
    // is no evidence, because the cable running into the jack is a mid green
    // that is lighter than the panel, and the black it casts under itself is
    // darker -- so a "light tone and a dark tone are present here" test passes
    // with the socket switched off, measuring the cable and calling it a jack.
    const auto metal = count_light_neutral(f, jack.x, jack.y, r);
    INFO("light neutral pixels at the jack: " << metal);
    CHECK(metal > 40);

    // Two controls, and the second is the one that matters.
    //
    // The bare face, a panel's width from any cable: nothing bright at all.
    const auto* box = L.panel("DST");
    REQUIRE(box != nullptr);
    const auto bare = count_light_neutral(
        f, box->x + box->width * 0.5f, box->y + box->height * 0.2f, r);
    INFO("light neutral pixels on the bare face: " << bare);
    CHECK(bare == 0);

    // And the middle of the same cable in the same frame -- a place where
    // there is definitely a cable and definitely no jack. If THIS showed metal
    // then the measurement above would be finding the cable.
    const auto from = forge_modular::port_point(L, preview.modules(), "SRC", "out", "DST");
    float mx = 0, my = 0;
    forge_modular::cable_point(forge_modular::cable_curve(from, jack), 0.5f, mx, my);
    const auto on_cable = count_light_neutral(f, mx, my, r);
    INFO("light neutral pixels mid-cable: " << on_cable);
    CHECK(on_cable == 0);
}

TEST_CASE("a docked cable end reads as a plug rather than a cable stopping",
          "[rack][render]") {
    // A module nobody ever placed has no jack coordinates, so its cables dock
    // at the panel edge rather than guess a position. That honesty cost the
    // drawing: the cable simply stopped, in mid-air, and a connection that is
    // there looked like one that is not.
    forge_modular::RackModule src{"SRC", "ForgeModular", "SRC", 8,
                                  {forge_modular::Port{"out", "OUT", 0.5f, 300.0f, false}}};
    forge_modular::RackModule dst{"DST", "Fundamental", "Whatever", 8,
                                  {forge_modular::Port{"in", "IN", 0.5f, 300.0f, true}}};
    dst.placed = false;                 // never placed: no coordinates exist

    forge_modular::RackPreview preview;
    preview.set_rack({src, dst},
                     {forge_modular::Connection{"SRC", "out", "DST", "in",
                                                forge_modular::SignalRole::audio, ""}});
    preview.set_bounds({0, 0, 700, 460});

    const auto L = preview.layout_for(700, 460);
    const auto dock = forge_modular::port_point(L, preview.modules(), "DST", "in", "SRC");
    REQUIRE(dock.docked);

    const auto f = decode(pulp::view::render_to_png(
        preview, 700, 460, 1.0f, pulp::view::ScreenshotBackend::skia));
    REQUIRE(f.ok());

    const auto rgb = forge_modular::role_color(forge_modular::SignalRole::audio);
    const std::array<int, 3> role{int((rgb >> 16) & 0xFF), int((rgb >> 8) & 0xFF),
                                  int(rgb & 0xFF)};

    // A disc a little wider than the cable, centred on the dock. A collar
    // fills it; a cable merely ending there covers about half of it, because
    // a stroke is a line and a plug is a body.
    const double probe = 3.0 * L.scale;
    const double at_dock = disc_coverage(f, dock.x, dock.y, probe, role, 26);

    // The control, on the same cable in the same frame: a point partway along
    // it, where there is a cable and nothing else. This is what "a cable
    // passes through here" looks like, and the dock has to look like more.
    float mx = 0, my = 0;
    const auto from = forge_modular::port_point(L, preview.modules(), "SRC", "out", "DST");
    forge_modular::cable_point(forge_modular::cable_curve(from, dock), 0.5f, mx, my);
    const double on_cable = disc_coverage(f, mx, my, probe, role, 26);

    INFO("role-coloured coverage at the dock " << at_dock << ", mid-cable "
         << on_cable);
    REQUIRE(on_cable > 0.2);          // the cable really is drawn there
    CHECK(at_dock > 0.85);
    CHECK(at_dock > on_cable * 1.4);
}

TEST_CASE("a panel's name stays on its own panel", "[rack][render]") {
    // "AudioInterface2" is fifteen characters on a 5 HP module. Centred and
    // unclipped it runs out over both neighbours and labels modules it does
    // not name -- which undoes the seam either side of it, and mislabels two
    // modules to fit one.
    forge_modular::RackModule narrow{"N", "Fundamental", "AudioInterface2", 5, {}};
    forge_modular::RackPreview preview;
    preview.set_rack({narrow}, {});
    preview.set_bounds({0, 0, 700, 460});

    const auto L = preview.layout_for(700, 460);
    REQUIRE(L.panels.size() == 1);
    const auto& box = L.panels[0];

    const auto f = decode(pulp::view::render_to_png(
        preview, 700, 460, 1.0f, pulp::view::ScreenshotBackend::skia));
    REQUIRE(f.ok());

    // Text is near-white; the panel is a dark blue-grey and the stage behind
    // it darker still. Anything bright outside the panel is spilled name.
    const auto title_band_top = static_cast<std::size_t>(box.y + 8.0f * L.scale);
    const auto title_band_bottom = static_cast<std::size_t>(box.y + 34.0f * L.scale);
    std::size_t inside = 0, outside = 0;
    for (std::size_t y = title_band_top; y < title_band_bottom && y < f.h; ++y)
        for (std::size_t x = 0; x < f.w; ++x) {
            if (f.luma(x, y) < 120.0) continue;
            if (x >= static_cast<std::size_t>(box.x) &&
                x <= static_cast<std::size_t>(box.x + box.width)) ++inside;
            else ++outside;
        }

    INFO("bright pixels in the title band: " << inside << " on the panel, "
         << outside << " off it");
    REQUIRE(inside > 60);        // the name was actually drawn
    CHECK(outside == 0);
}

TEST_CASE("a wired patch replaces the skeleton on the Build stage", "[rack][seam][render]") {
    HermeticProjects isolated;
    forge_modular::ForgeModularShell shell;
    pulp::state::StateStore store;
    shell.set_state_store(&store);
    shell.define_parameters(store);
    pulp::format::PrepareContext pc;
    pc.sample_rate = kSr; pc.max_buffer_size = kFrames;
    pc.input_channels = 1; pc.output_channels = 2;
    shell.prepare(pc);
    shell.set_artifact(forge_modular::Artifact::patch);
    auto view = shell.create_view();
    REQUIRE(view != nullptr);

    auto* chrome = shell.chrome();
    REQUIRE(chrome != nullptr);
    chrome->enter_build();

    // Assert what is actually on screen -- the mounted accessory -- not the
    // preview's own flag. A child reports itself visible while its parent is
    // hidden, so checking the child would pass on a rack nobody can see.
    auto* preview = shell.rack_preview();
    auto* mounted = chrome->stage_accessory();
    REQUIRE(preview != nullptr);
    REQUIRE(mounted != nullptr);
    CHECK_FALSE(mounted->visible());

    // An empty rack must NOT take the stage: a blank stage reads as a finished
    // build that produced nothing.
    shell.show_rack({}, {});
    CHECK_FALSE(mounted->visible());

    shell.show_rack(sample_rack(), sample_patch());
    CHECK(mounted->visible());
    CHECK(preview->modules().size() == 3);
    CHECK(preview->connections().size() == 3);

    const auto shot = std::filesystem::temp_directory_path() /
                      "modular-patch-wired.png";
    REQUIRE(pulp::view::render_to_file(
        *view, forge::ForgeChrome::kDesignWidth, forge::ForgeChrome::kDesignHeight,
        shot.string(), 1.0f, pulp::view::ScreenshotBackend::skia));
    CHECK(std::filesystem::file_size(shot) > 20000);
}

TEST_CASE("depth rewrites the explanation, and hover lights the cable",
          "[rack][depth][hover][seam]") {
    // The pairing that justifies drawing a rack at all, driven end to end
    // through the shell: press a depth tab, the words on screen change; point
    // at a sentence, the cable it names lights and the rest recede.
    HermeticProjects isolated;
    forge_modular::ForgeModularShell shell;
    pulp::state::StateStore store;
    shell.set_state_store(&store);
    shell.define_parameters(store);
    pulp::format::PrepareContext pc;
    pc.sample_rate = kSr; pc.max_buffer_size = kFrames;
    pc.input_channels = 1; pc.output_channels = 2;
    shell.prepare(pc);
    shell.set_artifact(forge_modular::Artifact::patch);
    auto view = shell.create_view();
    REQUIRE(view != nullptr);
    shell.chrome()->enter_build();
    shell.show_rack(sample_rack(), sample_patch());

    auto* ex = shell.explanation();
    auto* preview = shell.rack_preview();
    REQUIRE(ex != nullptr);
    REQUIRE(preview != nullptr);
    REQUIRE(ex->line_count() == 3);

    // Every depth still says what is plugged into what -- depth adds, it never
    // takes the wiring away.
    // Drive the tabs the chrome actually mounted. Building a second set would
    // repoint the shell at buttons nobody can see, so the on-screen control
    // would stop tracking the depth it is setting.
    auto* tabs = shell.chrome()->build_accessory();
    REQUIRE(tabs != nullptr);
    auto press = [&](int i) {
        dynamic_cast<pulp::view::TextButton*>(tabs->child_at(i))->on_click();
    };
    auto tab_is_selected = [&](int i) {
        return dynamic_cast<pulp::view::TextButton*>(tabs->child_at(i))->style() ==
               pulp::view::TextButton::Style::secondary;
    };

    // Measured on what is RENDERED, not on one cable's string. The old test
    // read line_text(0) at each depth, which rewarded exactly the wrong thing:
    // repeating a role's primer on every cable that shares that role made
    // "learning says more" true while teaching nothing. Said once under the
    // heading -- the right place -- the per-cable string is unchanged and this
    // check passed only because it was measuring the padding.
    press(0);   // Terse
    const auto terse = rendered_text(ex);
    const auto terse_line = ex->line_text(0);
    press(1);   // Standard
    const auto standard = rendered_text(ex);
    const auto standard_line = ex->line_text(0);
    press(2);   // Learning
    const auto learning = rendered_text(ex);
    const auto learning_line = ex->line_text(0);
    // The control on screen must agree with the depth the words are at.
    CHECK(tab_is_selected(2));
    CHECK_FALSE(tab_is_selected(1));

    for (const auto& t : {terse_line, standard_line, learning_line}) {
        INFO(t);
        CHECK(t.find("VCO-1 OUT") != std::string::npos);
        CHECK(t.find("VCF IN") != std::string::npos);
    }
    // Each depth genuinely says more than the one below it.
    CHECK(standard.size() > terse.size());
    CHECK(learning.size() > standard.size());
    CHECK(flatten(standard).find("everything else shapes it") != std::string::npos);
    CHECK(flatten(terse).find("everything else shapes it") == std::string::npos);
    // The concept a reader is here for appears once, not once per cable.
    const auto primer = std::string("What you actually hear");
    const auto flat_learning = flatten(learning);
    std::size_t occurrences = 0;
    for (std::size_t at = flat_learning.find(primer); at != std::string::npos;
         at = flat_learning.find(primer, at + 1)) ++occurrences;
    INFO("primer occurrences at learning depth: " << occurrences);
    CHECK(occurrences == 1);

    // Hovering a line lights its cable and dims the others.
    ex->hover_line(2);
    CHECK(preview->highlight().has_value());
    CHECK(*preview->highlight() == 2);
    CHECK(preview->cable_alpha(2) == Approx(1.0f));
    CHECK(preview->cable_alpha(0) < 1.0f);

    // Letting go restores the rack rather than leaving it dimmed.
    ex->hover_line(std::nullopt);
    CHECK_FALSE(preview->highlight().has_value());
    for (std::size_t i = 0; i < 3; ++i)
        CHECK(preview->cable_alpha(i) == Approx(1.0f));

    const auto shot = std::filesystem::temp_directory_path() /
                      "modular-patch-explained.png";
    press(2);
    ex->hover_line(2);
    REQUIRE(pulp::view::render_to_file(
        *view, forge::ForgeChrome::kDesignWidth, forge::ForgeChrome::kDesignHeight,
        shot.string(), 1.0f, pulp::view::ScreenshotBackend::skia));
    CHECK(std::filesystem::file_size(shot) > 20000);

    // And the same pairing from the rack, in the shell that ships rather than
    // in a re-creation of its wiring: point at a cable, the sentence that
    // explains it lights.
    {
        const auto L = preview->layout_for(preview->bounds().width,
                                           preview->bounds().height);
        const auto& c = preview->connections()[2];
        const auto a = forge_modular::port_point(L, preview->modules(),
                                                 c.from_module, c.from_port,
                                                 c.to_module);
        const auto b = forge_modular::port_point(L, preview->modules(),
                                                 c.to_module, c.to_port,
                                                 c.from_module);
        float x = 0, y = 0;
        forge_modular::cable_point(forge_modular::cable_curve(a, b), 0.5f, x, y);
        // After the render above, so the rack has been through the real
        // layout pass. Hovering before it, the preview's bounds are still
        // 0x0 and the hit test is asked about a rack with no size.
        REQUIRE(preview->bounds().width > 0);
        ex->hover_line(std::nullopt);
        preview->on_hover_move({x, y});
        REQUIRE(ex->hovered().has_value());
        CHECK(*ex->hovered() == 2);

        preview->on_mouse_leave();
        CHECK_FALSE(ex->hovered().has_value());
    }
}

TEST_CASE("Rack is handed the patch, not left to restore its autosave",
          "[rack][open]") {
    // The stray module. `~/Library/Application Support/Rack2/autosave/patch.json`
    // held one TURBID and no cables for days, and Rack restores that on every
    // launch -- so a freshly built patch handed over as a DOCUMENT arrived, if
    // at all, behind the previous session. It read as our patch failing to
    // load. It was Rack showing its last one first.
    //
    // Rack takes a patch path as a positional argument and loads THAT instead
    // of the autosave, and `open --args` is how a launch carries one.
    const std::string app = "/Applications/VCV Rack 2 Free.app";
    const std::string patch = "/tmp/some patch.vcv";

    const auto cold = forge_modular::rack_open_command(app, patch, false);
    INFO(cold);
    CHECK(cold.find("--args") != std::string::npos);
    CHECK(cold.find("some patch.vcv") != std::string::npos);

    // Already running is the other case, and --args would be ignored there --
    // it only reaches an app being launched. Handing a live Rack a second
    // launch instead would put two of them on one audio device.
    const auto warm = forge_modular::rack_open_command(app, patch, true);
    INFO(warm);
    CHECK(warm.find("--args") == std::string::npos);
    CHECK(warm.find("some patch.vcv") != std::string::npos);

    // Both ends are user-shaped text: Application Support has a space in it
    // and a patch is named after the prompt somebody typed. Asserted by
    // running the command through a real shell with a stub `open` that prints
    // its arguments -- substring-matching the command text would pass on a
    // path the shell then splits into three.
    const auto args_of = [](const std::string& command) {
        const auto dir = std::filesystem::temp_directory_path() /
                         "forge-rack-open-stub";
        std::filesystem::remove_all(dir);
        std::filesystem::create_directories(dir);
        const auto stub = dir / "open";
        {
            std::ofstream f(stub);
            f << "#!/bin/sh\nfor a in \"$@\"; do printf '%s\\n' \"$a\"; done\n";
        }
        std::filesystem::permissions(stub, std::filesystem::perms::owner_all);
        const auto out = dir / "argv.txt";
        const std::string sh = "PATH=" + dir.string() + ":$PATH " + command +
                               " > " + out.string();
        std::system(sh.c_str());
        std::vector<std::string> argv;
        std::ifstream in(out);
        for (std::string line; std::getline(in, line);) argv.push_back(line);
        return argv;
    };

    const auto awkward = args_of(forge_modular::rack_open_command(
        app, "/tmp/it's a drone.vcv", false));
    REQUIRE(awkward.size() == 4);
    CHECK(awkward[0] == "-a");
    CHECK(awkward[1] == app);           // the space in the app path survived
    CHECK(awkward[2] == "--args");
    CHECK(awkward[3] == "/tmp/it's a drone.vcv");   // and the apostrophe

    const auto plain = args_of(forge_modular::rack_open_command(
        app, "/tmp/some patch.vcv", true));
    REQUIRE(plain.size() == 3);
    CHECK(plain[2] == "/tmp/some patch.vcv");
}

TEST_CASE("whether Rack is there is shown, not left to be discovered",
          "[rack][open]") {
    // The states were known to the app and reached a person only as the
    // wording of a failure AFTER they pressed the button. Until then "Rack is
    // not installed" and "the button did nothing" looked the same.
    //
    // But only the ACTIONABLE states earn words. "Rack is installed", greyed
    // beside an Open in Rack button, is a label present exactly when it is not
    // needed, saying nothing the button does not already imply. Silence is the
    // right answer when there is nothing to do.
    forge_modular::RackPresence p;
    CHECK(p.phrase() == "Rack is not installed");
    p.plugin_installed = true;
    CHECK(p.phrase() == "Rack is available as a plugin");
    p.standalone_installed = true;
    CHECK(p.phrase().empty());
    p.standalone_running = true;
    CHECK(p.phrase().empty());

    // Every state that speaks says something DIFFERENT, and the states that
    // have nothing to act on stay quiet. A phrase() returning one string would
    // satisfy nothing above, but is worth pinning: the pill is only useful
    // because its readings are distinguishable.
    std::set<std::string> said;
    int silent = 0;
    for (int i = 0; i < 8; ++i) {
        forge_modular::RackPresence q;
        q.standalone_running = i & 1;
        q.standalone_installed = i & 2;
        q.plugin_installed = i & 4;
        const auto s = q.phrase();
        if (s.empty()) ++silent; else said.insert(s);
    }
    CHECK(said.size() == 2);   // not installed; plugin only
    CHECK(silent > 0);         // installed or running says nothing

    // And it reaches the screen. The pill is hidden until there is something
    // to open, then carries the words look_for_rack() found on this machine.
    forge_modular::ForgeModularShell shell;
    pulp::state::StateStore store;
    shell.set_state_store(&store);
    shell.define_parameters(store);
    shell.set_artifact(forge_modular::Artifact::patch);
    auto view = shell.create_view();
    REQUIRE(view != nullptr);
    auto* tabs = shell.chrome()->build_accessory();
    REQUIRE(tabs != nullptr);
    auto* pill = dynamic_cast<pulp::view::Label*>(
        tabs->child_at(tabs->child_count() - 1));
    REQUIRE(pill != nullptr);
    CHECK_FALSE(pill->visible());        // nothing built yet, nothing to open

    shell.refresh_rack_presence();
    INFO("pill says: " << shell.rack_presence_phrase());
    CHECK(shell.rack_presence_phrase() == pill->text());
    // Whatever it says must be one of the things phrase() can say -- or
    // nothing, which is the right answer on a machine where Rack is installed
    // and there is therefore nothing to tell the user. Asserting non-empty
    // here would pin the test to the DEVELOPER's machine having no Rack.
    const auto shown = shell.rack_presence_phrase();
    CHECK((shown.empty() || said.count(shown) == 1));

    // And it becomes VISIBLE once there is something to open.
    //
    // Everything above this line passes with the pill hidden forever: the
    // text is set on a label nobody can see, and the only visibility check
    // asserts it is hidden. That is how the depth tabs shipped invisible
    // while their tests were green -- the state was right and none of it
    // reached the screen.
    const auto patch = std::filesystem::temp_directory_path() /
                       "presence-pill-probe.vcv";
    { std::ofstream f(patch); f << "{}\n"; }
    shell.set_open_patch(patch.string());
    shell.on_poll();
    CHECK(pill->visible());
    CHECK(pill->text() == shell.rack_presence_phrase());

    // And goes away again when there is not.
    shell.set_open_patch("");
    shell.on_poll();
    CHECK_FALSE(pill->visible());

    std::error_code rm;
    std::filesystem::remove(patch, rm);
}

TEST_CASE("a patch nobody generated still says something", "[rack]") {
    // Imported and shipped patches have no per-cable reasoning and never will:
    // nobody wrote one. The only honest sentence is computed from the wiring,
    // and without it those patches are a bare netlist -- which is what every
    // shipped example was.
    auto bare = sample_patch();
    for (auto& c : bare) c.why.clear();

    forge_modular::PatchExplanation ex;
    ex.set_bounds({0, 0, 820, 400});
    ex.set_connections(bare, sample_rack());
    const auto text = flatten(rendered_text(&ex));
    INFO(text);
    CHECK(text.find("3 cables") != std::string::npos);
    CHECK(text.find("audio path") != std::string::npos);

    // And NOT shown when the patch explains itself: a computed summary above
    // the model's own reasoning would be the app talking over it.
    forge_modular::PatchExplanation withprose;
    withprose.set_bounds({0, 0, 820, 400});
    withprose.set_connections(sample_patch(), sample_rack());
    const auto explained = flatten(rendered_text(&withprose));
    INFO(explained);
    CHECK(explained.find("audio path") == std::string::npos);
}

TEST_CASE("the explanation is grouped by what each cable carries", "[rack]") {
    // A flat list of a dozen cables is a netlist. Grouped, the same dozen say
    // how the patch is organised before a line of it is read -- which is the
    // difference between showing someone a patch and teaching them one.
    forge_modular::PatchExplanation ex;
    ex.set_connections(sample_patch(), sample_rack());
    ex.set_bounds({0, 0, 820, 300});

    // Headings are the children that are not cable rows.
    std::set<const pulp::view::View*> cable_rows;
    for (std::size_t i = 0; i < sample_patch().size(); ++i)
        cable_rows.insert(ex.row_for(i));

    std::vector<std::string> headings;
    for (int i = 0; i < ex.child_count(); ++i) {
        const auto* child = ex.child_at(i);
        if (cable_rows.count(child)) continue;
        std::string text;
        for (int c = 0; c < child->child_count(); ++c)
            if (auto* l = dynamic_cast<const pulp::view::Label*>(child->child_at(c)))
                text += (text.empty() ? "" : " ") + l->text();
        headings.push_back(text);
    }

    // The sample is two audio cables and one modulation cable. The counts are
    // part of the heading because "3 CABLES" describes the patch's shape at a
    // glance; a heading without one would pass a weaker version of this test.
    REQUIRE(headings.size() == 2);
    CHECK(headings[0] == "AUDIO 2 CABLES");
    CHECK(headings[1] == "MODULATION 1 CABLE");

    // Signal order, not alphabetical and not the order the model happened to
    // emit: what you hear, then what picks the notes, then time, then motion.
    // And no heading for a role the patch does not use -- an empty PITCH &
    // GATE heading would claim the patch has something it has not.
    for (const auto& h : headings) CHECK(h.find("PITCH") == std::string::npos);
}

TEST_CASE("a cable's wiring and its reason are set differently", "[rack][render]") {
    // The wiring is a fact about jacks and the reason is an argument about
    // intent, and they were run together into one wrapped paragraph in one
    // face, one size and one grey. Nothing on screen said which half was the
    // patch, so the list could not be skimmed for the connection alone.
    //
    // Asserted on the labels that actually draw: the strings were always
    // right, which is why every existing test passed while the pane read
    // badly.
    forge_modular::PatchExplanation ex;
    ex.set_depth(forge_modular::ExplainDepth::standard);
    ex.set_connections(sample_patch(), sample_rack());
    ex.set_bounds({0, 0, 430, 600});

    const auto* row = ex.row_for(0);
    REQUIRE(row != nullptr);

    std::vector<const pulp::view::Label*> labels;
    std::function<void(const pulp::view::View*)> walk =
        [&](const pulp::view::View* v) {
            for (int c = 0; c < v->child_count(); ++c) {
                if (auto* l = dynamic_cast<const pulp::view::Label*>(v->child_at(c)))
                    labels.push_back(l);
                walk(v->child_at(c));
            }
        };
    walk(row);
    REQUIRE(labels.size() >= 2);

    // The first line is the wiring: monospaced, in the strong ink, and it
    // carries the arrow.
    const auto* wiring = labels.front();
    INFO("wiring: " << wiring->text());
    CHECK(wiring->font_family() == std::string(forge::design::type::mono));
    CHECK(wiring->text_color() == forge::design::color::text_strong);
    CHECK(wiring->text().find("\xE2\x86\x92") != std::string::npos);

    // And it is ONLY the wiring: the reason is not glued onto the end of it.
    CHECK(wiring->text().find("everything else shapes it") == std::string::npos);
    CHECK(wiring->text().find("\xE2\x80\x94") == std::string::npos);

    // The reason is beneath it, in the reading face and the quieter grey.
    std::string reason;
    const pulp::view::Label* first_reason = nullptr;
    for (std::size_t i = 1; i < labels.size(); ++i) {
        CHECK(labels[i]->font_family() == std::string(forge::design::type::display));
        CHECK(labels[i]->text_color() == forge::design::color::text_muted);
        if (!first_reason) first_reason = labels[i];
        reason += (reason.empty() ? "" : " ") + labels[i]->text();
    }
    REQUIRE(first_reason != nullptr);
    INFO("reason: " << reason);
    CHECK(flatten(reason).find("everything else shapes it") != std::string::npos);

    // Stacked, not side by side: the reason is indented under its cable, so it
    // starts to the right of the wiring and below it.
    const auto wb = wiring->bounds();
    const auto rb = first_reason->bounds();
    CHECK(rb.y + first_reason->parent()->bounds().y >= wb.y + wb.height);

    // Terse drops the reason and keeps the wiring, which is the promise depth
    // makes. Without this the split above could be satisfied by always
    // printing both.
    ex.set_depth(forge_modular::ExplainDepth::terse);
    const auto terse = flatten(rendered_text(ex.row_for(0)));
    INFO("terse row: " << terse);
    CHECK(terse.find("VCO-1 OUT") != std::string::npos);
    CHECK(terse.find("everything else shapes it") == std::string::npos);
}

TEST_CASE("pointing at a role's heading lights every cable it carries",
          "[rack][hover][render]") {
    // The second way in. A cable is the small unit; a role is the large one,
    // and "what is the audio path" is a question about the shape of the patch
    // rather than about any one wire. The preview could already draw a whole
    // role at once -- highlight_role() existed and was unit-tested -- but
    // nothing in the app ever called it, so the only way to be taught anything
    // was one cable at a time.
    //
    // Driven through the shell, at the pointer, because that is the path that
    // was dead: calling hover_role() directly would prove the setter works and
    // nothing about whether a mouse can reach it.
    HermeticProjects isolated;
    forge_modular::ForgeModularShell shell;
    pulp::state::StateStore store;
    shell.set_state_store(&store);
    shell.define_parameters(store);
    pulp::format::PrepareContext pc;
    pc.sample_rate = kSr; pc.max_buffer_size = kFrames;
    pc.input_channels = 1; pc.output_channels = 2;
    shell.prepare(pc);
    shell.set_artifact(forge_modular::Artifact::patch);
    auto view = shell.create_view();
    REQUIRE(view != nullptr);
    shell.chrome()->enter_build();
    shell.show_rack(sample_rack(), sample_patch());

    auto* ex = shell.explanation();
    auto* preview = shell.rack_preview();
    REQUIRE(ex != nullptr);
    REQUIRE(preview != nullptr);

    // Through a real layout pass, so the rectangles the pointer is tested
    // against are the ones that will be on screen. Then the app's own
    // re-wrap, because the explanation is built before it has ever been
    // measured and is laid out for the 20-column floor until that runs -- and
    // the row heights the pointer is tested against change when it does.
    const auto shot = std::filesystem::temp_directory_path() / "role-hover.png";
    REQUIRE(pulp::view::render_to_file(
        *view, forge::ForgeChrome::kDesignWidth, forge::ForgeChrome::kDesignHeight,
        shot.string(), 1.0f, pulp::view::ScreenshotBackend::skia));
    ex->apply_pending_rewrap();
    REQUIRE(pulp::view::render_to_file(
        *view, forge::ForgeChrome::kDesignWidth, forge::ForgeChrome::kDesignHeight,
        shot.string(), 1.0f, pulp::view::ScreenshotBackend::skia));

    // And the pane it ended up in is the pane it wrapped to. The wiring is
    // short enough to survive the floor, so this is asserted on the prose,
    // which is where a 20-column wrap is unmistakable.
    {
        std::vector<const pulp::view::Label*> prose;
        std::function<void(const pulp::view::View*)> walk =
            [&](const pulp::view::View* v) {
                for (int c = 0; c < v->child_count(); ++c) {
                    if (auto* l = dynamic_cast<const pulp::view::Label*>(v->child_at(c)))
                        if (l->font_family() == std::string(forge::design::type::display))
                            prose.push_back(l);
                    walk(v->child_at(c));
                }
            };
        walk(ex->row_for(0));
        REQUIRE_FALSE(prose.empty());
        std::size_t longest = 0;
        for (const auto* l : prose) longest = std::max(longest, l->text().size());
        INFO("longest prose line: " << longest);
        CHECK(longest > 24);
    }

    // The sample is two audio cables (0, 1) and one modulation cable (2).
    const auto* heading = ex->heading_for(forge_modular::SignalRole::audio);
    REQUIRE(heading != nullptr);
    const auto hb = heading->bounds();
    REQUIRE(hb.height > 0);
    ex->on_hover_move({hb.x + 4.0f, hb.y + hb.height / 2.0f});

    CHECK(ex->hovered_role().has_value());
    CHECK(*ex->hovered_role() == forge_modular::SignalRole::audio);
    CHECK(preview->cable_alpha(0) == Approx(1.0f));
    CHECK(preview->cable_alpha(1) == Approx(1.0f));
    CHECK(preview->cable_alpha(2) < 1.0f);
    // A role and a single cable are exclusive readings: holding both would
    // light one role plus one stray wire outside it.
    CHECK_FALSE(ex->hovered().has_value());

    // The small unit still works from the pointer, and takes the role reading
    // away when it does. This direction was unreachable by mouse too: nothing
    // ever called hover_line() from an event.
    const auto* row = ex->row_for(2);
    REQUIRE(row != nullptr);
    const auto rb = row->bounds();
    ex->on_hover_move({rb.x + 4.0f, rb.y + rb.height / 2.0f});
    CHECK(ex->hovered().has_value());
    CHECK(*ex->hovered() == 2);
    CHECK_FALSE(ex->hovered_role().has_value());
    CHECK(preview->cable_alpha(2) == Approx(1.0f));
    CHECK(preview->cable_alpha(0) < 1.0f);

    // And letting go restores the rack rather than leaving it dimmed.
    ex->on_mouse_leave();
    CHECK_FALSE(ex->hovered().has_value());
    CHECK_FALSE(ex->hovered_role().has_value());
    for (std::size_t i = 0; i < 3; ++i)
        CHECK(preview->cable_alpha(i) == Approx(1.0f));

    // Changing depth under the pointer rebuilds every row and heading, and the
    // highlight has to survive onto the new ones. It did not: the fresh
    // heading came back unlit while the rack stayed lit, and pointing at the
    // same heading again did nothing, because the setter saw a role it thought
    // it was already showing.
    ex->hover_role(forge_modular::SignalRole::audio);
    shell.set_depth(forge_modular::ForgeModularShell::Depth::learning);
    const auto* relit = ex->heading_for(forge_modular::SignalRole::audio);
    REQUIRE(relit != nullptr);
    CHECK(relit->background_color() == forge::design::color::surface_raised);
    const auto* other = ex->heading_for(forge_modular::SignalRole::mod);
    REQUIRE(other != nullptr);
    CHECK_FALSE(other->background_color() == forge::design::color::surface_raised);
    ex->on_mouse_leave();
}

TEST_CASE("render the explanation and rack for a look", "[.look]") {
    // Not part of the suite: a picture for a person to judge. The assertions
    // elsewhere say the geometry is sound; only an eye says it reads well.
    // Run deliberately:  forge-test-chrome-no-leak "[.look]"
    const char* home = std::getenv("HOME");
    const std::filesystem::path dir =
        std::string(home ? home : ".") +
        "/Library/Application Support/Forge Modular/examples/forge-modular/patches";
    std::error_code ec;
    std::string newest;
    std::filesystem::file_time_type best{};
    for (const auto& e : std::filesystem::directory_iterator(dir, ec)) {
        const auto n = e.path().string();
        if (n.size() > 9 && n.substr(n.size() - 9) == ".why.json") {
            const auto t = std::filesystem::last_write_time(e, ec);
            if (newest.empty() || t > best) { best = t; newest = n.substr(0, n.size() - 9) + ".vcv"; }
        }
    }
    if (newest.empty()) SKIP("no generated patch with a sidecar here");

    const auto loaded = forge_modular::load_patch(newest);
    REQUIRE_FALSE(loaded.connections.empty());

    for (const auto depth : {forge_modular::ExplainDepth::terse,
                             forge_modular::ExplainDepth::standard,
                             forge_modular::ExplainDepth::learning}) {
        forge_modular::PatchExplanation ex;
        ex.set_bounds({0, 0, 430, 900});
        ex.set_depth(depth);
        ex.set_connections(loaded.connections, loaded.modules);
        const char* names[] = {"terse", "standard", "learning"};
        const auto out = std::filesystem::temp_directory_path() /
            (std::string("forge-explanation-") +
             names[static_cast<int>(depth)] + ".png");
        REQUIRE(pulp::view::render_to_file(ex, 430, 900, out.string(), 2.0f,
                                           pulp::view::ScreenshotBackend::skia));
        WARN("wrote " << out.string());
    }

    forge_modular::RackPreview rack;
    rack.set_panel_directory(
        std::string(home ? home : ".") +
        "/Library/Application Support/Forge Modular/examples/forge-modular/res");
    rack.set_rack(loaded.modules, loaded.connections);
    rack.set_bounds({0, 0, 900, 620});
    const auto rack_png = std::filesystem::temp_directory_path() / "forge-rack.png";
    REQUIRE(pulp::view::render_to_file(rack, 900, 620, rack_png.string(), 2.0f,
                                       pulp::view::ScreenshotBackend::skia));
    WARN("wrote " << rack_png.string());
}

TEST_CASE("explanation lines do not overlap", "[rack][render]") {
    forge_modular::PatchExplanation ex;
    ex.set_connections(sample_patch(), sample_rack());
    ex.set_depth(forge_modular::ExplainDepth::learning);
    ex.set_bounds({0, 0, 820, 300});
    const auto shot = std::filesystem::temp_directory_path() / "explanation-only.png";
    REQUIRE(pulp::view::render_to_file(ex, 820, 300, shot.string(), 1.0f,
                                       pulp::view::ScreenshotBackend::skia));
    CHECK(std::filesystem::file_size(shot) > 3000);

    // The regression this closes: wrapped lines drew on top of each other, so
    // the explanation was unreadable in every render before this. Assert the
    // geometry, because the render only shows it to a human who looks.
    //
    // One child per cable, plus one heading per role present -- derived from
    // the fixture rather than written down, so adding a cable to the sample
    // does not silently turn this into a weaker test.
    // One child per cable, plus a heading per role, plus that role's primer
    // at this depth. The exact heading text is pinned by its own test; here
    // the point is that everything drawn has somewhere to sit.
    std::set<forge_modular::SignalRole> roles;
    for (const auto& c : sample_patch()) roles.insert(c.role);
    REQUIRE(ex.child_count() >=
            static_cast<int>(sample_patch().size() + roles.size()));
    float previous_bottom = -1.0f;
    for (int r = 0; r < ex.child_count(); ++r) {
        auto* row = ex.child_at(r);
        REQUIRE(row != nullptr);
        const auto rb = row->bounds();
        INFO("row " << r << " top " << rb.y << " height " << rb.height);
        CHECK(rb.y >= previous_bottom);          // rows never overlap
        previous_bottom = rb.y + rb.height;

        // Consecutive lines must advance by close to a full line. The bug was
        // an advance of about nine points against a 12.5pt font -- half a line
        // -- so each wrapped line sat on top of the one above it. A point of
        // rounding between the height and the advance is invisible; half a
        // line is not.
    }

    // Wrapped lines advance by close to a full line. Asserted over the CABLE
    // rows only: a role heading is two labels side by side, which is a
    // different shape on purpose and would fail an assertion written for a
    // stack of lines.
    //
    // Every label in the row, at any nesting, and in the row's own coordinates.
    // A cable's reason is set in a block of its own beneath the wiring, so a
    // scan of DIRECT children alone stops seeing the wrapped prose entirely --
    // which is exactly the text the overlap bug was about.
    std::function<void(const pulp::view::View*, float,
                       std::vector<std::pair<const pulp::view::Label*, float>>&)>
        collect = [&](const pulp::view::View* v, float offset,
                      std::vector<std::pair<const pulp::view::Label*, float>>& out) {
            for (int c = 0; c < v->child_count(); ++c) {
                const auto* child = v->child_at(c);
                const float top = offset + child->bounds().y;
                if (auto* lbl = dynamic_cast<const pulp::view::Label*>(child))
                    out.push_back({lbl, top});
                collect(child, top, out);
            }
        };

    for (std::size_t i = 0; i < sample_patch().size(); ++i) {
        const auto* row = ex.row_for(i);
        REQUIRE(row != nullptr);
        std::vector<std::pair<const pulp::view::Label*, float>> lines;
        collect(row, 0.0f, lines);
        // The wiring, plus one line per wrapped line of the reason.
        REQUIRE(lines.size() >= 2);
        float previous_top = -1000.0f;
        float previous_height = 0.0f;
        for (const auto& [lbl, top] : lines) {
            const auto lb = lbl->bounds();
            INFO("  cable " << i << " line at " << top << " height " << lb.height
                            << " size " << lbl->font_size());
            // A full line box for the size it is set at, not a squeezed one.
            // Derived from the label's own size rather than a constant, so the
            // check still means "one line" when the wiring and the prose are
            // set at different sizes.
            CHECK(lb.height >= lbl->font_size() * 1.3f);
            // And the next line clears the previous one's box. A point of
            // rounding between a line box and its advance is invisible; the
            // bug this closes was an advance of about nine points against a
            // 17-point box, which is half a line and put every wrapped line on
            // top of the one above it.
            if (previous_top > -999.0f)
                CHECK(top >= previous_top + previous_height - 2.0f);
            previous_top = top;
            previous_height = lb.height;
        }
    }

    // A deeper setting genuinely produces more to read. Counted over every
    // label drawn, at any nesting: this used to count grandchildren only, so a
    // line added directly to the explanation -- which is where a role's primer
    // belongs -- registered as nothing at all.
    auto line_count_at = [](forge_modular::ExplainDepth depth) {
        forge_modular::PatchExplanation s;
        s.set_connections(sample_patch(), sample_rack());
        s.set_depth(depth);
        s.set_bounds({0, 0, 820, 300});
        const auto text = rendered_text(&s);
        return static_cast<int>(std::count(text.begin(), text.end(), '\n'));
    };
    const int terse_lines = line_count_at(forge_modular::ExplainDepth::terse);
    const int standard_lines = line_count_at(forge_modular::ExplainDepth::standard);
    const int learning_lines = line_count_at(forge_modular::ExplainDepth::learning);
    INFO("terse " << terse_lines << ", standard " << standard_lines
                  << ", learning " << learning_lines);
    CHECK(standard_lines >= terse_lines);
    CHECK(learning_lines > standard_lines);
}

TEST_CASE("pressing Build through a real click does not tear itself down",
          "[phase6][controls][crash]") {
    // Forge Modular segfaulted on the M5 the moment Build was clicked:
    //
    //   Label::~Label -> View::~View -> ForgeChrome::clear_chat_rail
    //     -> begin_new_session -> start_build_with -> start_build
    //     -> composer_row()::$_3 -> deliver_mouse_down
    //
    // Starting a build resets the session, and resetting the session destroys
    // the Home subtree -- including the button whose click handler is still on
    // the stack, inside the very event delivery that is walking it. This
    // codebase already knows the shape: PatchExplanation::on_resized says in
    // as many words that replacing children from inside a pass that is walking
    // them segfaults, and defers to the next turn of the loop.
    //
    // Every existing test called on_click() DIRECTLY, which never enters
    // deliver_mouse_down and so never destroys anything under it. That is why
    // a suite of 600 assertions was green while the app crashed on its
    // primary button.
    // A generator that says yes. Without one, start_build_with returns "the
    // generator is not connected" before it ever resets the session -- which
    // is the second reason the suite never saw this: the crash lives past a
    // guard no test could get through.
    struct WillingEngine : forge_modular::EngineClient {
        bool submitted = false;
        bool available() const override { return true; }
        bool ensure_running() override { return true; }
        void submit(const std::string&, bool) override { submitted = true; }
    } engine;

    HermeticProjects isolated;
    forge_modular::ForgeModularShell shell;
    shell.set_engine(&engine);
    pulp::state::StateStore store;
    shell.set_state_store(&store);
    shell.define_parameters(store);
    pulp::format::PrepareContext pc;
    pc.sample_rate = kSr; pc.max_buffer_size = kFrames;
    pc.input_channels = 1; pc.output_channels = 2;
    shell.prepare(pc);
    auto view = shell.create_view();
    REQUIRE(view != nullptr);
    // Rendered once so the tree goes through the real layout pass; without
    // it every child is still at 0x0 and a click lands nowhere.
    const auto shot = std::filesystem::temp_directory_path() /
                      "modular-build-click.png";
    REQUIRE(pulp::view::render_to_file(
        *view, forge::ForgeChrome::kDesignWidth,
        forge::ForgeChrome::kDesignHeight, shot.string(), 1.0f,
        pulp::view::ScreenshotBackend::skia));

    auto* input = shell.chrome()->prompt_input();
    REQUIRE(input != nullptr);
    input->set_text("a classic subtractive voice with a filter envelope");

    // The Build button, found in the live tree rather than rebuilt here.
    pulp::view::View* build = nullptr;
    std::function<void(pulp::view::View*)> walk = [&](pulp::view::View* v) {
        if (!v || build) return;
        if (auto* b = dynamic_cast<pulp::view::TextButton*>(v)) {
            // The composer's primary, by its exact labels. A bare "Build"
            // substring matched the Home preference tabs the moment one of
            // them mentioned building, and this test clicked a preference.
            if (b->access_label() == "Build the patch" ||
                b->access_label() == "Build the module") {
                build = b;
                return;
            }
        }
        for (std::size_t i = 0; i < v->child_count(); ++i) walk(v->child_at(i));
    };
    walk(view.get());
    REQUIRE(build != nullptr);

    // Through the window server's path, not the handler's front door. This is
    // the whole point: the crash is in what happens to the view tree WHILE the
    // event is being delivered into it.
    const auto b = build->bounds();
    float rx = b.x + b.width / 2.0f, ry = b.y + b.height / 2.0f;
    for (auto* up = build->parent(); up; up = up->parent()) {
        rx += up->bounds().x;
        ry += up->bounds().y;
    }
    INFO("clicking Build at " << rx << "," << ry);
    view->simulate_click({rx, ry});

    // The defect, asserted deterministically rather than left to a
    // sanitiser: is the control that was clicked STILL IN THE TREE? A
    // use-after-free only faults when the memory happens to be reused, so a
    // plain run can pass over it all day -- but walking down from the live
    // root and failing to find the button is unambiguous, and it is exactly
    // what the crash report describes.
    pulp::view::View* found = nullptr;
    std::function<void(pulp::view::View*)> still_there = [&](pulp::view::View* v) {
        if (!v || found) return;
        if (v == build) { found = v; return; }
        for (std::size_t i = 0; i < v->child_count(); ++i) still_there(v->child_at(i));
    };
    still_there(view.get());
    CHECK(found != nullptr);

    // Surviving is most of the assertion. The rest says the press was not
    // simply swallowed to achieve that.
    CHECK(shell.chrome()->mode() == forge::ForgeChrome::Mode::Build);
    CHECK(engine.submitted);

    // And AGAIN, with a conversation behind it. The M5 crash was on the
    // SECOND build of a session -- the app launched at 22:55, built a patch,
    // and died at 22:59 when Build was pressed again. begin_new_session()
    // only resets the rail from Home, and on the first build that rail is
    // empty, so there is nothing to destroy and nothing goes wrong. The
    // second one tears down a rail full of the first build's bubbles while
    // the click that asked for it is still being delivered.
    shell.chrome()->narrate("Built. Open it in Rack to play it.");
    shell.chrome()->narrate("Eight modules, nine cables.");
    shell.chrome()->enter_home();
    REQUIRE(pulp::view::render_to_file(
        *view, forge::ForgeChrome::kDesignWidth,
        forge::ForgeChrome::kDesignHeight, shot.string(), 1.0f,
        pulp::view::ScreenshotBackend::skia));
    if (auto* again = shell.chrome()->prompt_input())
        again->set_text("a west coast voice through a low pass gate");
    view->simulate_click({rx, ry});
    CHECK(shell.chrome()->mode() == forge::ForgeChrome::Mode::Build);
}

TEST_CASE("a build can be asked for without driving the screen",
          "[phase6][controls][seam]") {
    // The claim no headless test can make is that a generation works when it
    // is spawned from INSIDE a host: the generator inherits the host's
    // environment, and a plugin whose editor draws perfectly can still never
    // reach it. Proving it meant synthetic clicks at screen coordinates,
    // which typed a prompt into somebody's terminal twice -- every guess
    // about what is on screen finds a new way to be wrong.
    //
    // So the host-side proof asks through a file, and this pins that seam:
    // it fires once, it consumes the request, and it is INERT unless the
    // environment names a file.
    struct WillingEngine : forge_modular::EngineClient {
        std::vector<std::string> prompts;
        bool available() const override { return true; }
        bool ensure_running() override { return true; }
        void submit(const std::string& p, bool) override { prompts.push_back(p); }
    } engine;

    HermeticProjects isolated;
    const auto trigger = std::filesystem::temp_directory_path() /
                         "forge-modular-test-prompt.txt";
    std::filesystem::remove(trigger);

    forge_modular::ForgeModularShell shell;
    shell.set_engine(&engine);
    pulp::state::StateStore store;
    shell.set_state_store(&store);
    shell.define_parameters(store);
    auto view = shell.create_view();
    REQUIRE(view != nullptr);

    // Inert while nothing names a file: a shipped plugin must not read this.
    unsetenv("FORGE_MODULAR_TEST_PROMPT");
    { std::ofstream f(trigger); f << "should never be read\n"; }
    shell.chrome()->poll();
    CHECK(engine.prompts.empty());
    CHECK(std::filesystem::exists(trigger));   // untouched
    std::filesystem::remove(trigger);

    setenv("FORGE_MODULAR_TEST_PROMPT", trigger.c_str(), 1);
    // And inert when the file is not there, which is every ordinary tick.
    shell.chrome()->poll();
    CHECK(engine.prompts.empty());

    { std::ofstream f(trigger); f << "a west coast voice through a low pass gate\n"; }
    shell.chrome()->poll();
    REQUIRE(engine.prompts.size() == 1);
    CHECK(engine.prompts[0] == "a west coast voice through a low pass gate");

    // The request says WHICH artifact. Without it the shell builds whichever
    // it was last on, so a request for a patch built a module and the run
    // failed for asking the wrong question rather than for anything broken.
    shell.set_artifact(forge_modular::Artifact::module);
    { std::ofstream f(trigger); f << "patch: two detuned oscillators\n"; }
    shell.chrome()->poll();
    REQUIRE(engine.prompts.size() == 2);
    CHECK(engine.prompts[1] == "two detuned oscillators");
    CHECK(shell.artifact() == forge_modular::Artifact::patch);

    { std::ofstream f(trigger); f << "module: a 6 HP sample and hold\n"; }
    shell.chrome()->poll();
    REQUIRE(engine.prompts.size() == 3);
    CHECK(engine.prompts[2] == "a 6 HP sample and hold");
    CHECK(shell.artifact() == forge_modular::Artifact::module);

    // Consumed, so a build that takes minutes is not started again every tick.
    CHECK_FALSE(std::filesystem::exists(trigger));
    const auto asked = engine.prompts.size();
    shell.chrome()->poll();
    shell.chrome()->poll();
    CHECK(engine.prompts.size() == asked);

    unsetenv("FORGE_MODULAR_TEST_PROMPT");
}

TEST_CASE("polling while the log is being written does not corrupt the heap",
          "[crash][race]") {
    // REAPER died with a heap abort the moment a build finished:
    //
    //   malloc_zone_error -> PatchExplanation::set_connections
    //     <- ForgeModularShell::show_rack <- open_patch_file <- on_poll
    //
    // with ProcessEngine::run still streaming the generator's output on
    // another thread. malloc_zone_error means the heap was ALREADY corrupt --
    // set_connections is the next allocation, the victim rather than the
    // cause. So this drives the same shape: polls that tail a log while that
    // log is being appended to underneath, and opens a real generated patch
    // over and over.
    //
    // Run it under Guard Malloc to mean anything:
    //   DYLD_INSERT_LIBRARIES=/usr/lib/libgmalloc.dylib ./forge-test-... "[race]"
    HermeticProjects isolated;
    const char* home = std::getenv("HOME");
    const std::filesystem::path patches =
        std::string(home ? home : ".") +
        "/Library/Application Support/Forge Modular/examples/forge-modular/patches";
    std::error_code ec;
    if (!std::filesystem::exists(patches, ec))
        SKIP("no generated patches on this machine");
    std::vector<std::string> real;
    for (const auto& e : std::filesystem::directory_iterator(patches, ec))
        if (e.path().extension() == ".vcv") real.push_back(e.path().string());
    if (real.size() < 2) SKIP("not enough generated patches to churn");

    const auto log = std::filesystem::temp_directory_path() /
                     "forge-race-last-run.log";
    std::filesystem::remove(log);
    { std::ofstream f(log); f << "starting\n"; }

    forge_modular::ForgeModularShell shell;
    pulp::state::StateStore store;
    shell.set_state_store(&store);
    shell.define_parameters(store);
    auto view = shell.create_view();
    REQUIRE(view != nullptr);
    shell.watch_build_log(log.string());

    // A writer that appends the way the generator does, including the
    // TRUNCATE a new build performs -- which is the case that makes a tailer
    // read from an offset past the end.
    std::atomic<bool> stop{false};
    std::thread writer([&] {
        for (int i = 0; !stop && i < 400; ++i) {
            if (i % 97 == 96) {
                std::ofstream f(log, std::ios::trunc);
                f << "  starting over\n";
            } else {
                std::ofstream f(log, std::ios::app);
                f << "  line " << i << " with some text on it\n";
            }
            std::this_thread::sleep_for(std::chrono::microseconds(200));
        }
    });

    for (int i = 0; i < 400; ++i) {
        shell.chrome()->poll();
        // The step that crashed: a real patch, parsed and pushed into the
        // views, while all of the above is going on.
        shell.open_patch_file(real[i % real.size()]);
    }
    stop = true;
    writer.join();
    std::filesystem::remove(log);

    // Surviving IS the assertion; the rest says it did real work.
    CHECK(shell.explanation() != nullptr);
    CHECK(shell.rack_preview() != nullptr);
}

TEST_CASE("the editor fills the window the host gave it", "[seam][sizing]") {
    // In REAPER the plugin drew its editor in the top-left of a much larger
    // window and left the rest empty grey -- reported as "the plugin has a
    // lot of white space in VCV Rack, is this intentional?". It is not.
    //
    // A host sizes the pane; the editor has to fill it. Measured as INK: the
    // rightmost and lowest painted pixel, against the canvas it was given.
    // Asserting the view's bounds would prove nothing, because the bounds are
    // whatever they were set to -- the question is what got drawn inside them.
    HermeticProjects isolated;
    forge_modular::ForgeModularShell shell;
    pulp::state::StateStore store;
    shell.set_state_store(&store);
    shell.define_parameters(store);
    auto view = shell.create_view();
    REQUIRE(view != nullptr);

    // Deliberately bigger than the 1280x800 design, which is what a host does
    // when it remembers a window somebody stretched.
    constexpr int kW = 1800, kH = 1100;
    const auto png = pulp::view::render_to_png(*view, kW, kH, 1.0f,
                                               pulp::view::ScreenshotBackend::skia);
    REQUIRE_FALSE(png.empty());
    const auto [across, down] = ink_extent(png);
    INFO("drawing reaches " << int(across * 100) << "% across and "
         << int(down * 100) << "% down a " << kW << "x" << kH << " window");
    // A rounded corner or a margin is fine. Half the window left blank is the
    // defect that was reported.
    CHECK(across > 0.9);
    CHECK(down > 0.9);
}

TEST_CASE("a module Rack cannot create is not drawn as if it will be there",
          "[rack][availability]") {
    // The preview reads OUR manifests; Rack can only create what its installed
    // plugin BINARY contains. On a machine running an older build those differ
    // by eight modules, so a patch renders beautifully here and opens over
    // there as a different rack with modules silently missing. Reported as
    // "the VCV Rack patch/models are DIFFERENT than what I see in Forge
    // Modular", and nothing in the project compared them.
    HermeticProjects isolated;
    const auto dir = std::filesystem::temp_directory_path() / "forge-avail-test";
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);

    auto write_patch = [&](const std::string& model) {
        const auto p = dir / (model + ".vcv");
        std::ofstream f(p);
        f << R"({"version":"2.6.6","modules":[)"
          << R"({"id":1,"plugin":"ForgeModular","model":")" << model
          << R"(","pos":[0,0]},)"
          << R"({"id":2,"plugin":"Core","model":"AudioInterface2","pos":[10,0]}],)"
          << R"("cables":[{"id":1,"outputModuleId":1,"outputId":0,)"
          << R"("inputModuleId":2,"inputId":0,"color":"#00b56e"}]})";
        return p.string();
    };

    // A model the installed plugin genuinely has, and one it certainly does
    // not. Both are drawn; only one of them will exist when Rack opens it.
    const char* home = std::getenv("HOME");
    const std::filesystem::path plugins =
        std::string(home ? home : ".") + "/Library/Application Support/Rack2";
    std::error_code ec;
    if (!std::filesystem::exists(plugins, ec)) SKIP("no Rack install here");

    const auto real = forge_modular::load_patch(write_patch("VCO"));
    const auto fake = forge_modular::load_patch(write_patch("NOSUCHMODULE"));
    REQUIRE(real.ok());
    REQUIRE(fake.ok());
    REQUIRE(real.modules.size() == 2);
    REQUIRE(fake.modules.size() == 2);

    // Core is compiled into Rack, so it is always there.
    CHECK(real.modules[1].available);
    CHECK(fake.modules[1].available);
    CHECK(real.modules[0].available);
    CHECK_FALSE(fake.modules[0].available);

    // And the preview SAYS it, rather than drawing a panel for something that
    // will not be in the rack you open.
    auto render = [&](const forge_modular::LoadedPatch& p) {
        forge_modular::RackPreview preview;
        preview.set_rack(p.modules, p.connections);
        return pulp::view::render_to_png(preview, 900, 420, 1.0f,
                                         pulp::view::ScreenshotBackend::skia);
    };
    // The refusal is drawn in the alarm red, which nothing else on the stage
    // uses. Counting it separates "said so" from "drew it anyway".
    const auto said = count_pixels_near(render(fake), 0xF3, 0x37, 0x4B);
    const auto quiet = count_pixels_near(render(real), 0xF3, 0x37, 0x4B);
    INFO("alarm pixels: missing module " << said << ", present module " << quiet);
    CHECK(said > quiet + 200);
    std::filesystem::remove_all(dir);
}

TEST_CASE("a resize re-wraps the explanation even with no loop to defer onto",
          "[rack][render][overlap]") {
    // on_resized may not rebuild in place -- that runs inside the layout pass
    // walking these very children, and replacing them there segfaults -- so it
    // defers. When there is no dispatcher to defer ONTO, it used to drop the
    // request entirely, on the reasoning that a headless render sets its
    // content after its bounds anyway.
    //
    // A hosted plugin is exactly that case and is not headless. It has no
    // dispatcher of its own, so the re-wrap never ran: the rows stayed laid
    // out for whatever width the view was FIRST built at, and after a resize
    // the text wrapped to more lines than the layout had allowed and ran over
    // what was below it.
    forge_modular::PatchExplanation ex;
    ex.set_connections(sample_patch(), sample_rack());
    ex.set_depth(forge_modular::ExplainDepth::learning);

    auto lay_out_at = [&](int w) {
        // Rendering is what actually runs a layout pass over the children;
        // set_bounds alone leaves them all at zero height.
        (void)pulp::view::render_to_png(ex, w, 400, 1.0f,
                                        pulp::view::ScreenshotBackend::skia);
    };
    auto content_height = [&] {
        float h = 0.0f;
        for (std::size_t i = 0; i < ex.child_count(); ++i)
            h += ex.child_at(i)->bounds().height;
        return h;
    };

    lay_out_at(900);
    const float wide = content_height();
    REQUIRE(wide > 0.0f);

    // Squeeze it. The resize itself may not rebuild -- that runs inside the
    // layout pass walking these children -- so this render still uses the
    // wrap computed for 900.
    lay_out_at(380);
    const float before_apply = content_height();

    // The poll is where the deferred work is allowed to happen.
    ex.apply_pending_rewrap();
    lay_out_at(380);
    const float after_apply = content_height();

    INFO("content height: 900 -> " << wide << ", 380 before the re-wrap -> "
         << before_apply << ", after -> " << after_apply);
    // The defect, exactly: laid out at 900 the content is one height, and
    // squeezed to 380 it is STILL that height -- the rows kept a layout
    // computed for a column more than twice as wide, which is what runs text
    // over whatever sits below it. The measurement is that the pending
    // re-wrap CHANGED something; which way it goes is the widget's business.
    CHECK(before_apply == Approx(wide));      // nothing re-wrapped on resize
    CHECK(after_apply != Approx(before_apply));  // the poll did the work
}

TEST_CASE("nothing in the rail draws on top of anything else",
          "[rack][render][overlap]") {
    // Reported from a screenshot: the left column had the transcript drawn
    // over the explanation -- "Built. Open it in Rack to play it." sitting on
    // top of a cable's reasoning, both readable, neither legible.
    //
    // The existing overlap test covers PatchExplanation ALONE, which is why it
    // never saw this: the two views are fine individually and collide when
    // they share a column. So this walks the LIVE tree and compares every
    // visible piece of text against every other, in root coordinates.
    HermeticProjects isolated;
    forge_modular::ForgeModularShell shell;
    pulp::state::StateStore store;
    shell.set_state_store(&store);
    shell.define_parameters(store);
    pulp::format::PrepareContext pc;
    pc.sample_rate = kSr; pc.max_buffer_size = kFrames;
    pc.input_channels = 1; pc.output_channels = 2;
    shell.prepare(pc);
    shell.set_artifact(forge_modular::Artifact::patch);
    auto view = shell.create_view();
    REQUIRE(view != nullptr);
    shell.chrome()->enter_build();
    shell.show_rack(sample_rack(), sample_patch());
    // A transcript, which is the half that landed on top.
    shell.chrome()->narrate("a classic subtractive voice with a filter envelope");
    shell.chrome()->narrate("Built. Open it in Rack to play it.");
    shell.chrome()->narrate("Eight modules, nine cables. The audio path is "
                            "three cables long.");
    // And the state the screenshot was actually in: a build in flight, so the
    // status card is up in the same column as the transcript and the
    // explanation. Three things sharing one rail is when they collided.
    shell.chrome()->set_active_stage(1);
    shell.chrome()->set_active_stage_elapsed("12s");
    shell.chrome()->set_status_activity("asking the model · 12s elapsed");
    shell.chrome()->set_status_note("FileNotFoundError: [Errno 2] No such file "
                                    "or directory: 'claude'");
    shell.chrome()->narrate("Traceback (most recent call last):", true);

    // At several widths, because the column that collided in the report was a
    // NARROW one: text wraps to more lines than a layout that measured it
    // unwrapped allowed for, and only then does it run into what is below.
    for (auto [kW, kH] : {std::pair<int, int>{forge::ForgeChrome::kDesignWidth,
                                              forge::ForgeChrome::kDesignHeight},
                          {1020, 760}, {860, 700}, {700, 640}}) {
    const auto shot = std::filesystem::temp_directory_path() /
                      ("rail-overlap-" + std::to_string(kW) + ".png");
    REQUIRE(pulp::view::render_to_file(*view, kW, kH, shot.string(), 1.0f,
                                       pulp::view::ScreenshotBackend::skia));

    struct Box { std::string text; float x, y, w, h; };
    std::vector<Box> texts;
    // CLIPPED to whatever scrolls it. A label inside a scroll view may sit
    // far below the viewport and be perfectly invisible; comparing raw bounds
    // would call that an overlap and send somebody hunting a defect that is
    // not on screen. Only what is actually painted counts.
    struct Clip { float x0, y0, x1, y1; };
    std::function<void(pulp::view::View*, float, float, Clip)> walk =
        [&](pulp::view::View* v, float ox, float oy, Clip clip) {
            if (!v || !v->visible()) return;
            const auto b = v->bounds();
            const float x = ox + b.x, y = oy + b.y;
            if (dynamic_cast<pulp::view::ScrollView*>(v)) {
                clip = {std::max(clip.x0, x), std::max(clip.y0, y),
                        std::min(clip.x1, x + b.width),
                        std::min(clip.y1, y + b.height)};
            }
            if (auto* l = dynamic_cast<pulp::view::Label*>(v)) {
                const auto t = l->text();
                const float vx0 = std::max(x, clip.x0), vy0 = std::max(y, clip.y0);
                const float vx1 = std::min(x + b.width, clip.x1);
                const float vy1 = std::min(y + b.height, clip.y1);
                if (!t.empty() && vx1 - vx0 > 1.0f && vy1 - vy0 > 1.0f)
                    texts.push_back({t, vx0, vy0, vx1 - vx0, vy1 - vy0});
            }
            for (std::size_t i = 0; i < v->child_count(); ++i)
                walk(v->child_at(i), x, y, clip);
        };
    walk(view.get(), 0.0f, 0.0f, {0.0f, 0.0f, float(kW), float(kH)});
    REQUIRE(texts.size() > 5);

    // Two pieces of text may touch, but they must not sit ON one another.
    // A generous overlap threshold, so a one-pixel kerning rectangle is not
    // called a defect while "one paragraph across another" certainly is.
    std::vector<std::string> collisions;
    for (std::size_t i = 0; i < texts.size(); ++i)
        for (std::size_t j = i + 1; j < texts.size(); ++j) {
            const auto& a = texts[i];
            const auto& b = texts[j];
            const float ox = std::min(a.x + a.w, b.x + b.w) - std::max(a.x, b.x);
            const float oy = std::min(a.y + a.h, b.y + b.h) - std::max(a.y, b.y);
            if (ox > 4.0f && oy > 4.0f)
                collisions.push_back("\"" + a.text.substr(0, 34) + "\" over \"" +
                                     b.text.substr(0, 34) + "\"");
        }
    // Gathered into ONE message: a scoped INFO per collision is destroyed at
    // the end of its loop iteration and never reaches the report, which is a
    // fine way to fail a test and learn nothing.
    std::string detail;
    for (const auto& c : collisions) detail += "\n      " + c;
    INFO("at " << kW << "x" << kH << ": " << texts.size() << " pieces of text, "
         << collisions.size() << " collisions:" << detail);
    CHECK(collisions.empty());
    }
}

TEST_CASE("no control on Home paints like a control and does nothing",
          "[phase6][controls]") {
    // A control that highlights and does nothing is indistinguishable from a
    // broken one. This sweeps the live tree rather than listing the buttons by
    // hand, so a control added later is covered the day it appears.
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

    std::vector<std::string> dead;
    int live = 0;
    std::function<void(pulp::view::View&, const std::string&)> sweep =
        [&](pulp::view::View& v, const std::string& path) {
        if (auto* b = dynamic_cast<pulp::view::TextButton*>(&v)) {
            if (b->visible() && b->enabled()) {
                if (b->on_click) {
                    ++live;
                } else {
                    auto name = b->access_label();
                    if (name.empty()) name = b->label();
                    // Any text inside it identifies it when it has no label.
                    if (name.empty()) {
                        for (std::size_t i = 0; i < b->child_count(); ++i)
                            if (auto* l = dynamic_cast<pulp::view::Label*>(b->child_at(i)))
                                name = l->text();
                    }
                    dead.push_back(path + " -> " + (name.empty() ? "<unnamed>" : name));
                }
            }
        }
        for (std::size_t i = 0; i < v.child_count(); ++i)
            sweep(*v.child_at(i), path + "/" + std::to_string(i));
    };
    sweep(*view, "");

    // Forge's own controls, inert before Forge Modular existed. Listed rather
    // than swept under the rug: fixing them here would be a change to Forge
    // made for Rack's benefit, which is exactly what must not leak. If one
    // gains a handler upstream, this list shrinks and the test says so.
    static const std::set<std::string> kForgeOwned = {
        "Share",
        "EXTRA HIGH",
        "Requires a verified merge strategy",
    };

    std::vector<std::string> ours;
    for (const auto& d : dead) {
        const auto name = d.substr(d.rfind("-> ") + 3);
        if (!kForgeOwned.count(name)) ours.push_back(d);
    }
    for (const auto& d : ours) WARN("no handler: " << d);
    INFO("live controls: " << live);
    CHECK(ours.empty());
    // The allowance must not silently grow: every inert Forge control is
    // accounted for, and no more than that.
    CHECK(dead.size() <= kForgeOwned.size());
    // And the sweep must actually be finding things -- an empty tree would
    // otherwise report a clean bill of health.
    CHECK(live >= 4);
}

namespace {

/// A generator that records what it was asked for and can be made to fail.
struct FakeEngine : forge_modular::EngineClient {
    bool installed = true;
    bool starts = true;
    std::string error;
    std::vector<std::pair<std::string, bool>> submissions;

    bool available() const override { return installed; }
    bool ensure_running() override { return starts; }
    void submit(const std::string& prompt, bool patch_mode) override {
        submissions.emplace_back(prompt, patch_mode);
    }
    std::string last_error() const override { return error; }
};

}  // namespace

TEST_CASE("every composer control changes something observable",
          "[phase6][controls]") {
    HermeticProjects isolated;
    forge_modular::ForgeModularShell shell;
    FakeEngine engine;
    shell.set_engine(&engine);
    pulp::state::StateStore store;
    shell.set_state_store(&store);
    shell.define_parameters(store);
    pulp::format::PrepareContext pc;
    pc.sample_rate = kSr; pc.max_buffer_size = kFrames;
    pc.input_channels = 1; pc.output_channels = 2;
    shell.prepare(pc);
    auto view = shell.create_view();
    REQUIRE(view != nullptr);
    auto* chrome = shell.chrome();
    auto* input = chrome->prompt_input();
    REQUIRE(input != nullptr);

    SECTION("Build with an empty prompt refuses with a reason") {
        input->set_text("   ");
        const auto why = shell.start_build();
        CHECK_FALSE(why.empty());              // never a silent nothing
        CHECK(engine.submissions.empty());
        CHECK(chrome->mode() == forge::ForgeChrome::Mode::Home);
    }

    SECTION("Build submits, moves to the Build screen and clears the prompt") {
        input->set_text("a 12 HP wavefolder");
        CHECK(shell.start_build().empty());
        REQUIRE(engine.submissions.size() == 1);
        CHECK(engine.submissions[0].first == "a 12 HP wavefolder");
        CHECK_FALSE(engine.submissions[0].second);      // module mode
        // Staying on Home after pressing Build is the reported failure: a user
        // cannot tell whether anything happened.
        CHECK(chrome->mode() == forge::ForgeChrome::Mode::Build);
        CHECK(input->text().empty());
        CHECK(chrome->chat_line_count() > 0);           // the prompt is in the transcript
    }

    SECTION("Build carries the artifact mode") {
        shell.set_artifact(forge_modular::Artifact::patch);
        input->set_text("an ambient drone patch");
        CHECK(shell.start_build().empty());
        REQUIRE(engine.submissions.size() == 1);
        CHECK(engine.submissions[0].second);            // patch mode
    }

    SECTION("a missing generator is reported, not swallowed") {
        engine.installed = false;
        input->set_text("anything");
        const auto why = shell.start_build();
        CHECK_FALSE(why.empty());
        CHECK(engine.submissions.empty());
    }

    SECTION("a generator that will not start says why") {
        engine.starts = false;
        engine.error = "the helper crashed on launch";
        input->set_text("anything");
        CHECK(shell.start_build() == "the helper crashed on launch");
        CHECK(engine.submissions.empty());
    }

    SECTION("Ask never reaches the generator") {
        input->set_text("what does symmetry do?");
        CHECK(shell.ask().empty());
        // The whole point of the distinction: an Ask that could rewrite the
        // artifact would destroy work on a misread intent.
        CHECK(engine.submissions.empty());
        CHECK(chrome->mode() == forge::ForgeChrome::Mode::Build);
        CHECK(chrome->chat_line_count() > 0);
    }

    SECTION("the mention button opens the list, as typing @ does") {
        input->set_text("wire a");
        shell.begin_mention();
        CHECK(input->text() == "wire a @");     // the @ is really in the prompt
        CHECK(shell.mentions().is_open());
    }

    SECTION("Random fills the composer without building") {
        auto row = shell.composer_row();
        REQUIRE(row.left.size() >= 2);
        REQUIRE(row.left[1].on_click);
        row.left[1].on_click();
        CHECK_FALSE(input->text().empty());
        CHECK(engine.submissions.empty());      // a suggestion, not a commitment
        CHECK(chrome->mode() == forge::ForgeChrome::Mode::Home);
    }
}

TEST_CASE("the shelf, the library link and settings all go somewhere",
          "[phase6][controls]") {
    // Phase 6's bar: one assertion per control that something observable
    // changed. Navigation counts as observable only if the mode actually moves.
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
    auto* chrome = shell.chrome();
    REQUIRE(chrome->mode() == forge::ForgeChrome::Mode::Home);

    SECTION("the library link opens the library") {
        chrome->enter_marketplace();
        CHECK(chrome->mode() == forge::ForgeChrome::Mode::Marketplace);
        // And it comes back, or the link is a trap door.
        chrome->enter_home();
        CHECK(chrome->mode() == forge::ForgeChrome::Mode::Home);
    }

    SECTION("opening a shelf item moves to the workspace") {
        chrome->open_project();
        CHECK(chrome->mode() != forge::ForgeChrome::Mode::Home);
    }

    SECTION("the shelf lists what the store holds, for both artifact kinds") {
        // The shelf must show patches as well as modules -- a patch that
        // cannot be reopened is a patch that was never really saved.
        for (auto kind : {forge_modular::Artifact::module,
                          forge_modular::Artifact::patch}) {
            shell.set_artifact(kind);
            const auto copy = shell.chrome_copy();
            INFO(copy.badge);
            CHECK_FALSE(copy.badge.empty());
            // The badge names the kind, so a card cannot be mistaken for the
            // other sort of thing.
            const bool patch = kind == forge_modular::Artifact::patch;
            // Starts with the kind, and may carry the size after it: the badge is
    // also the prototype's meta pill, so "MODULE · 12 HP" is correct and an
    // equality check here would forbid the feature rather than test it.
    INFO("badge: " << copy.badge);
    CHECK(copy.badge.rfind(patch ? "PATCH" : "MODULE", 0) == 0);
        }
    }
}

TEST_CASE("model roles are re-cut by artifact, not duplicated", "[phase6][settings]") {
    // Forge's settings stay the single source of the selections; this only
    // says which of them a Rack artifact consumes.
    using Shell = forge_modular::ForgeModularShell;
    const auto module_roles = Shell::roles_for(forge_modular::Artifact::module);
    const auto patch_roles = Shell::roles_for(forge_modular::Artifact::patch);

    // A module compiles DSP and draws a panel.
    CHECK(module_roles.dsp);
    CHECK(module_roles.ui);

    // A patch compiles nothing. Offering a DSP model for it would imply the
    // build does something it does not.
    CHECK_FALSE(patch_roles.dsp);
    CHECK(patch_roles.ui);

    // Every artifact uses at least one role, or its build has no model at all.
    for (auto a : {forge_modular::Artifact::module, forge_modular::Artifact::patch}) {
        const auto r = Shell::roles_for(a);
        CHECK((r.dsp || r.ui));
    }

    // And the live shell follows its own artifact rather than a stored copy.
    HermeticProjects isolated;
    Shell shell;
    pulp::state::StateStore store;
    shell.set_state_store(&store);
    shell.define_parameters(store);
    CHECK(shell.model_roles().dsp);                 // module by default
    shell.set_artifact(forge_modular::Artifact::patch);
    CHECK_FALSE(shell.model_roles().dsp);
}

TEST_CASE("Ask answers about a real patch, through the app's own path",
          "[phase7][app]") {
    // Drives the same engine the app uses against the real toolchain and a
    // real generated patch. Scripted proof that the CLI works says nothing
    // about whether the app reaches it -- the app had no engine connected at
    // all until this existed, so Build did nothing there while the command
    // line worked perfectly.
    // The INSTALLED toolchain, not a path on one machine's disk. Hardcoding a
    // checkout meant this skipped anywhere else — and skipping is reported as
    // a pass.
    const char* home = std::getenv("HOME");
    const std::string tools =
        std::string(home ? home : ".") +
        "/Library/Application Support/Forge Modular/tools/rack";
    const std::string patch = a_real_patch();
    if (!std::filesystem::exists(tools) || patch.empty()) {
        WARN("no installed toolchain or generated patch — skipped, NOT passed. "
             "Run tools/rack/install_toolchain.sh.");
        return;
    }

    forge_modular::ProcessEngine engine(
        tools, (std::filesystem::temp_directory_path() / "fm-ask.log").string());
    CHECK(engine.available());
    REQUIRE(engine.ensure_running());

    HermeticProjects isolated;
    forge_modular::ForgeModularShell shell;
    shell.set_engine(&engine);
    pulp::state::StateStore store;
    shell.set_state_store(&store);
    shell.define_parameters(store);
    pulp::format::PrepareContext pc;
    pc.sample_rate = kSr; pc.max_buffer_size = kFrames;
    pc.input_channels = 1; pc.output_channels = 2;
    shell.prepare(pc);
    shell.set_artifact(forge_modular::Artifact::patch);
    auto view = shell.create_view();
    REQUIRE(view != nullptr);
    auto* chrome = shell.chrome();

    // With no patch open, Ask says so rather than inventing an answer.
    chrome->prompt_input()->set_text("what makes the sound here?");
    CHECK(shell.ask().empty());
    const int after_empty = chrome->chat_line_count();
    CHECK(after_empty > 0);

    // With a patch open, the answer is derived from that file.
    shell.set_open_patch(patch);
    chrome->prompt_input()->set_text("what makes the sound here?");
    CHECK(shell.ask().empty());
    CHECK(chrome->chat_line_count() > after_empty + 2);   // a real explanation

    // And the explanation genuinely describes THIS patch: the audio path it
    // names must be one the file actually contains.
    const auto answer = engine.explain(patch);
    INFO(answer);
    CHECK(answer.find("AUDIO") != std::string::npos);
    CHECK(answer.find("Audio") != std::string::npos);     // it reaches an output
    // An explanation computed from the file cannot claim a cable that is not
    // in it -- so a module absent from the patch must not appear.
    CHECK(answer.find("Bogus Module That Is Not Here") == std::string::npos);

    // Asking twice costs nothing and gives the same answer, because no model
    // is involved.
    CHECK(engine.explain(patch) == answer);
}

TEST_CASE("a real generated patch drives the rack, explanation and tabs",
          "[phase7][app][render]") {
    // The last link: everything downstream was tested against hand-built
    // fixtures. This reads the .vcv the generator actually produced -- the
    // same file Rack opened -- so the preview cannot be right about a patch
    // that does not exist.
    const std::string patch = a_real_patch();
    REQUIRE_FALSE(patch.empty());

    const auto loaded = forge_modular::load_patch(patch);
    INFO(loaded.error);
    REQUIRE(loaded.ok());
    // Counted from the file itself rather than hardcoded. A fixed number
    // asserts which patch happened to be generated last, not that the loader
    // read the one in front of it.
    const auto raw = read_all(patch);
    const std::string text(raw.begin(), raw.end());
    auto occurrences = [&](const char* needle) {
        std::size_t n = 0, at = 0;
        while ((at = text.find(needle, at)) != std::string::npos) { ++n; at += 1; }
        return n;
    };
    CHECK(loaded.modules.size() == occurrences("\"plugin\""));
    CHECK(loaded.connections.size() == occurrences("\"outputModuleId\""));
    CHECK(loaded.modules.size() >= 3);        // a real patch, not a stub
    CHECK(loaded.connections.size() >= 3);

    // Roles come from the colour the file carries, which is the colour Rack
    // shows -- not re-derived here, where a guess could disagree with what the
    // user sees.
    int audio = 0, mod = 0;
    for (const auto& c : loaded.connections) {
        if (c.role == forge_modular::SignalRole::audio) ++audio;
        if (c.role == forge_modular::SignalRole::mod) ++mod;
    }
    // Audio is the one role every generated patch must carry: the gate
    // refuses a patch whose cables into the audio interface are all silent,
    // so a patch that exists reaches the interface.
    CHECK(audio > 0);
    // Modulation is NOT required. This asserted it because it ran against one
    // hardcoded patch that happened to have some; run against whatever the
    // generator last produced, a drone cluster with five cables and no
    // modulation fails it — and the patch is perfectly correct. A test that
    // only holds for one file is a test of that file.
    INFO("modulation cables: " << mod);

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
    auto* chrome = shell.chrome();
    chrome->enter_build();

    CHECK(shell.open_patch_file(patch).empty());
    // Opening a patch implies the patch view, so the depth tabs are present.
    CHECK(shell.artifact() == forge_modular::Artifact::patch);
    CHECK(chrome->stage_accessory() != nullptr);
    CHECK(chrome->stage_accessory()->visible());
    REQUIRE(shell.explanation() != nullptr);
    CHECK(shell.explanation()->line_count() == loaded.connections.size());
    REQUIRE(chrome->build_accessory() != nullptr);
    CHECK(chrome->build_accessory()->child_count() == 5);

    // The learning tabs change what this real patch says about itself.
    auto* tabs = chrome->build_accessory();
    auto press = [&](int i) {
        dynamic_cast<pulp::view::TextButton*>(tabs->child_at(i))->on_click();
    };
    press(0);
    const auto terse = rendered_text(shell.explanation());
    press(2);
    const auto learning = rendered_text(shell.explanation());
    INFO("terse " << terse.size() << " bytes, learning " << learning.size());
    CHECK(learning.size() > terse.size());

    // An unreadable file must not leave an empty rack on screen.
    CHECK_FALSE(shell.open_patch_file("/tmp/definitely-not-a-patch.vcv").empty());
    CHECK(shell.explanation()->line_count() == loaded.connections.size());   // the good patch survives

    const auto shot = std::filesystem::temp_directory_path() /
                      "modular-real-patch.png";
    REQUIRE(pulp::view::render_to_file(
        *view, forge::ForgeChrome::kDesignWidth, forge::ForgeChrome::kDesignHeight,
        shot.string(), 1.0f, pulp::view::ScreenshotBackend::skia));
    CHECK(std::filesystem::file_size(shot) > 20000);
}

TEST_CASE("the toolchain path is not on a volume macOS gates", "[phase8][tcc]") {
    // macOS gates removable-volume access behind a MODAL consent dialog.
    // Touching such a path from the UI thread parks the whole app behind that
    // modal, which is what read as a freeze on Build. Nothing under
    // Application Support is gated.
    forge_modular::ForgeModularShell shell;
    pulp::state::StateStore store;
    shell.set_state_store(&store);
    shell.define_parameters(store);
    auto processor = forge_modular::create_forge_modular();
    REQUIRE(processor != nullptr);

    const char* home = std::getenv("HOME");
    REQUIRE(home != nullptr);
    const auto installed = std::filesystem::path(home) /
        "Library/Application Support/Forge Modular/tools/rack/patch.py";
    if (!std::filesystem::exists(installed)) {
        WARN("no installed toolchain; skipping");
        return;
    }
    // The engine must resolve to the installed copy, never the checkout on an
    // external volume. There were two tools_dir() implementations once, and
    // the standalone kept using the one that preferred the gated path long
    // after the shared one was fixed.
    forge_modular::ProcessEngine engine(installed.parent_path().string(), "/tmp/x.log");
    CHECK(engine.available());
    CHECK(engine.log_path().find("/Volumes/") == std::string::npos);
}

TEST_CASE("no prompt route can reach Forge's plugin pipeline", "[phase7][routing]") {
    // A prompt for a Eurorack module once spent six model calls and two
    // minutes inside Forge's DSP+UI pipeline -- the model correctly protesting
    // that "a plugin has no CV jacks" -- before failing at an install step
    // that could never have applied. Every route must reach the Rack
    // generator instead.
    HermeticProjects isolated;
    forge_modular::ForgeModularShell shell;
    FakeEngine engine;
    shell.set_engine(&engine);
    pulp::state::StateStore store;
    shell.set_state_store(&store);
    shell.define_parameters(store);
    pulp::format::PrepareContext pc;
    pc.sample_rate = kSr; pc.max_buffer_size = kFrames;
    pc.input_channels = 1; pc.output_channels = 2;
    shell.prepare(pc);
    auto view = shell.create_view();
    REQUIRE(view != nullptr);
    auto* chrome = shell.chrome();

    CHECK(shell.owns_generation());

    // Route 1: the composer's Build button.
    chrome->prompt_input()->set_text("a 2 HP random module");
    auto row = shell.composer_row();
    REQUIRE(row.right.size() >= 2);
    REQUIRE(row.right[1].on_click);
    row.right[1].on_click();
    REQUIRE(engine.submissions.size() == 1);
    CHECK(engine.submissions[0].first == "a 2 HP random module");

    // Route 2: submit_prompt — what Enter in the prompt field calls, and the
    // route that actually reached Forge's pipeline in the field.
    chrome->submit_prompt("a 4 HP clock divider",
                          forge::ForgeChrome::PromptOrigin::home);
    REQUIRE(engine.submissions.size() == 2);
    CHECK(engine.submissions[1].first == "a 4 HP clock divider");

    // Route 3: a follow-up from inside the project.
    chrome->submit_prompt("make it 6 HP",
                          forge::ForgeChrome::PromptOrigin::project);
    REQUIRE(engine.submissions.size() == 3);
    CHECK(engine.submissions[2].first == "make it 6 HP");

    // Forge's own generation must never have started on any of them.
    CHECK_FALSE(chrome->generating());

    // A refusal is reported rather than silently dropping the prompt.
    engine.installed = false;
    chrome->submit_prompt("anything at all",
                          forge::ForgeChrome::PromptOrigin::home);
    CHECK(engine.submissions.size() == 3);      // nothing new submitted
    CHECK(chrome->chat_line_count() > 0);       // but the user was told
}

TEST_CASE("the title bar names what is being built", "[phase7][title]") {
    // It read "Untitled module" for a whole build because the setter refreshed
    // the copy but not the shell chrome, which is where the title label is
    // actually painted. Assert the painted text, not the stored string.
    HermeticProjects isolated;
    forge_modular::ForgeModularShell shell;
    FakeEngine engine;
    shell.set_engine(&engine);
    pulp::state::StateStore store;
    shell.set_state_store(&store);
    shell.define_parameters(store);
    pulp::format::PrepareContext pc;
    pc.sample_rate = kSr; pc.max_buffer_size = kFrames;
    pc.input_channels = 1; pc.output_channels = 2;
    shell.prepare(pc);
    auto view = shell.create_view();
    REQUIRE(view != nullptr);
    auto* chrome = shell.chrome();

    chrome->submit_prompt("a 4 HP sample and hold with an internal noise source",
                          forge::ForgeChrome::PromptOrigin::home);
    REQUIRE(engine.submissions.size() == 1);

    const auto title = chrome->project_title();
    INFO("title: " << title);
    CHECK_FALSE(title.empty());
    CHECK(title != "Untitled module");
    CHECK(title != "Wavefolder");          // never a leftover example name

    // Find the painted title, so a stored-but-unpainted value cannot pass.
    bool painted = false;
    std::function<void(pulp::view::View&)> walk = [&](pulp::view::View& v) {
        if (auto* l = dynamic_cast<pulp::view::Label*>(&v))
            if (l->text() == title) painted = true;
        for (std::size_t i = 0; i < v.child_count(); ++i) walk(*v.child_at(i));
    };
    walk(*view);
    CHECK(painted);

    // A second, different prompt renames it rather than keeping the first.
    chrome->submit_prompt("a 3 HP clock divider",
                          forge::ForgeChrome::PromptOrigin::home);
    CHECK(chrome->project_title() != title);
}

TEST_CASE("the stage stops saying 'materializing' when a build ends",
          "[phase7][stage]") {
    // Reported from a screenshot: the skeleton kept animating under a
    // transcript that had already printed "gave up after 3 attempts". A screen
    // contradicting itself is worse than a screen saying nothing.
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
    auto* chrome = shell.chrome();
    chrome->enter_build();

    const auto log = std::filesystem::temp_directory_path() / "fm-stage.log";
    std::filesystem::remove(log);
    shell.watch_build_log(log.string());

    auto caption = [&] { return chrome->skeleton_caption_text(); };

    // While it runs, the stage says what it always said.
    { std::ofstream f(log); f << "asking the model\n"; }
    shell.on_poll();
    CHECK(shell.build_outcome() == forge_modular::BuildOutcome::running);
    CHECK(caption().find("materializing") != std::string::npos);

    // A hard failure must be visible on the stage, not only in the transcript.
    { std::ofstream f(log, std::ios::app); f << "Traceback (most recent call last)\n"; }
    shell.on_poll();
    REQUIRE(shell.build_outcome() == forge_modular::BuildOutcome::failed);
    INFO("caption: " << caption());
    CHECK(caption().find("materializing") == std::string::npos);
    CHECK(caption().find("failed") != std::string::npos);

    // Starting a new build clears it rather than carrying the old verdict.
    chrome->begin_new_session();
    CHECK(caption().find("materializing") != std::string::npos);
}

TEST_CASE("the stage card follows the generator", "[phase7][stage]") {
    // Reported from a screenshot: the run was visible in the transcript while
    // the Thinking/Writing/Building/Verifying/Installing card beside it stayed
    // grey for the whole build. The chips are driven by Forge's own generation
    // loop, which never runs for a Rack prompt.
    using forge_modular::stage_of;
    CHECK(stage_of("  asking the model\xE2\x80\xA6") == 0);
    CHECK(stage_of("  generated CLKDIV (3HP, 2 params)") == 1);
    CHECK(stage_of("  manifest + panel validated") == 1);
    CHECK(stage_of("  compiled") == 2);
    CHECK(stage_of("  behaviour verified") == 3);
    CHECK(stage_of("  behavioural gate failed:") == 3);
    CHECK(stage_of("  installed \xE2\x86\x92 /Users/x/Rack2/plugins/y.vcvplugin") == 4);
    CHECK(stage_of("something the generator has never said") == -1);

    // Furthest, not latest: a retry re-runs earlier stages, and a card that
    // walks backwards reads as the build losing ground.
    forge_modular::BuildMonitor m;
    const auto log = std::filesystem::temp_directory_path() / "fm-stage-card.log";
    std::filesystem::remove(log);
    m.watch(log.string());
    { std::ofstream f(log);
      f << "asking the model\ngenerated CLKDIV\ncompiled\n"; }
    m.poll();
    CHECK(m.stage() == 2);
    { std::ofstream f(log, std::ios::app);
      f << "behavioural gate failed:\nasking the model (retry 1)\n"; }
    m.poll();
    CHECK(m.stage() == 3);          // not back to 0
    std::filesystem::remove(log);
}

TEST_CASE("the Home guard reads nothing outside its own fixture", "[no-leak]") {
    // The marketplace root defaults to the projects root's PARENT plus
    // "marketplace", so pinning only FORGE_PROJECTS_DIR pointed it at a shared
    // /tmp/marketplace that every product and every run wrote to. The guard
    // then failed three times on card titles drifting between "Untitled",
    // "Split Stereo Echo" and "Dual Time Delay" while Forge's chrome was
    // untouched.
    HermeticProjects isolated;

    const char* projects = std::getenv("FORGE_PROJECTS_DIR");
    const char* market = std::getenv("FORGE_MARKETPLACE_DIR");
    REQUIRE(projects != nullptr);
    REQUIRE(market != nullptr);

    // Both inside the fixture, and NOT one derived from the other's parent --
    // which is how the shared path was reached.
    const std::filesystem::path p(projects), m(market);
    CHECK(p.string().find("forge-no-leak") != std::string::npos);
    CHECK(m.string().find("forge-no-leak") != std::string::npos);
    const bool market_is_inside_the_fixture =
        m.string().find("forge-no-leak") != std::string::npos;
    CHECK(market_is_inside_the_fixture);

    // And both start empty, so a Home frame is a function of the code alone.
    CHECK(std::filesystem::is_directory(m));
    const auto listings = std::distance(std::filesystem::directory_iterator(m),
                                        std::filesystem::directory_iterator{});
    CHECK(listings == 0);
}

TEST_CASE("a finished patch lands in My projects and reopens", "[projects]") {
    // Before this, the shell wrote a .vcv to the patches directory and stopped.
    // A grep of the whole modular shell for ProjectStore returned ONE hit, and
    // it was a comment. So a generation the user watched succeed became
    // unreachable the moment they left the screen -- and the module cards that
    // WERE on the shelf came from Forge's own store, not ours.
    HermeticProjects isolated;
    const auto patch = a_real_patch();
    if (patch.empty()) {
        WARN("no real patch on this machine (skip, not a pass)");
        return;
    }
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

    forge::ProjectStore lib;
    const auto before = lib.list().size();

    shell.save_project_for_test(patch);

    const auto after = lib.list();
    INFO("entries: " << before << " -> " << after.size());
    REQUIRE(after.size() == before + 1);
    // A NAME, not a slug. The shelf is read by a person looking for the thing
    // they asked for.
    CHECK_FALSE(after.front().name.empty());
    CHECK(after.front().name.find('-') == std::string::npos);

    // And it reopens. This is the half that makes the entry worth having: the
    // patch travels WITH the entry, so opening is a lookup rather than a guess
    // at where the generator happened to write it.
    std::string err;
    INFO("open said: " << err);
    CHECK(shell.open_project_entry(after.front().id, err));
    CHECK(err.empty());
}

TEST_CASE("a rack's panels are parsed once, not once per frame", "[perf]") {
    // The defect this pins, measured on 2026-08-02: Forge Modular burned 62%
    // of an M5 Max DISPLAYING a static 39-module patch, with the GPU at 7%.
    // A `sample` of the running app put 993 of 1109 samples inside
    // RackPreview::paint in draw_svg -> SvgDomCache::get_or_build -> the
    // BUILDER closure, with the cache's own hash table showing up in `erase`.
    //
    // The cause was not the drawing. SvgDomCache defaults to 16 entries -- "a
    // handful of frames' worth of distinct docs" -- and a rack asks for one
    // document PER MODULE every frame. Working set 39, capacity 16: every
    // lookup evicted the entry it was about to need, so the hit rate was zero
    // and every panel was reparsed at up to 120Hz. That is the pathological
    // shape of an LRU -- one entry over capacity does not cost a little more,
    // it costs everything.
    //
    // So this asserts the property, not the number: repainting the same rack
    // must not keep rebuilding. If someone lowers the capacity, or a future
    // rack outgrows whatever RackPreview asks for, this fails loudly here
    // rather than quietly as a hot laptop on somebody else's desk.
    const auto patch = a_real_patch();
    if (patch.empty()) {
        WARN("no real patch on this machine to measure (skip, not a pass)");
        return;
    }
    const auto loaded = forge_modular::load_patch(patch);
    REQUIRE(loaded.ok());

    // The preview is driven DIRECTLY rather than through the shell. The first
    // draft rendered the whole editor and measured zero cache activity, because
    // the editor came up on the composer and the rack was never painted -- a
    // measurement of nothing, reported as a pass by a bound that zero satisfies.
    // Painting the preview itself is what the number is about.
    forge_modular::RackPreview preview;
    const char* home = std::getenv("HOME");
    preview.set_panel_directory(
        (std::filesystem::path(home ? home : ".") / "Library" /
         "Application Support" / "Forge Modular" / "examples" /
         "forge-modular" / "res").string());
    preview.set_rack(loaded.modules, loaded.connections);
    INFO("rack: " << loaded.modules.size() << " modules, "
                  << loaded.connections.size() << " cables");

    auto& cache = pulp::canvas::SvgDomCache::instance();
    const auto out = std::filesystem::temp_directory_path() / "fm-perf.png";

    // One frame to warm: the first paint legitimately parses every panel.
    REQUIRE(pulp::view::render_to_file(preview, 1280, 800, out.string(), 1.0f,
                                       pulp::view::ScreenshotBackend::skia));
    // The warm frame must have actually drawn panels, or everything below is
    // measuring an empty canvas.
    REQUIRE(cache.stats().builds > 0);

    // Then measure only the steady state.
    cache.reset_stats();
    constexpr int kFramesDrawn = 8;
    for (int i = 0; i < kFramesDrawn; ++i)
        REQUIRE(pulp::view::render_to_file(preview, 1280, 800, out.string(),
                                           1.0f,
                                           pulp::view::ScreenshotBackend::skia));

    const auto s = cache.stats();
    INFO("after " << kFramesDrawn << " warm frames: " << s.hits << " hits, "
                  << s.builds << " builds, " << s.size << " live entries");
    // A warm rack should rebuild essentially nothing. Before the fix this was
    // one build per panel per frame; the bound is deliberately generous so it
    // fails on thrash, not on a stray document.
    CHECK(s.builds < 16);
    CHECK(s.hits > s.builds);
}

TEST_CASE("a run that has printed nothing still shows a stage and a clock",
          "[stage]") {
    // The failure this exists to prevent, seen in full: a generation ran for
    // seven minutes with a healthy model call in flight and a 0-byte log,
    // and the card showed five identical grey rows the whole time. Python
    // block-buffers stdout when it is redirected to a file, so the first line
    // had not reached disk yet -- and the stage was driven ONLY by that file,
    // starting at -1 until something matched.
    //
    // On screen that is identical to a generator that died before its first
    // line, which is how it was read. The app launched the process and knows
    // when; that is enough to show a stage and count.
    HermeticProjects isolated;
    forge_modular::ForgeModularShell shell;
    FakeEngine engine;
    shell.set_engine(&engine);
    pulp::state::StateStore store;
    shell.set_state_store(&store);
    shell.define_parameters(store);
    pulp::format::PrepareContext pc;
    pc.sample_rate = kSr; pc.max_buffer_size = kFrames;
    pc.input_channels = 1; pc.output_channels = 2;
    shell.prepare(pc);
    auto view = shell.create_view();
    REQUIRE(view != nullptr);
    auto* chrome = shell.chrome();
    chrome->enter_build();

    const auto log = std::filesystem::temp_directory_path() / "fm-silent.log";
    std::filesystem::remove(log);
    { std::ofstream f(log); }                    // exists, and is EMPTY

    // Watching an empty log with NO build in flight must show nothing. The
    // first version of this fix skipped that condition, so the app logged a
    // phantom stage 0 at startup against a previous run's file -- and worse,
    // left reported_stage_ at 0, so the real build's transition never fired
    // and the card stayed grey for the run that mattered.
    shell.watch_build_log(log.string());
    shell.on_poll();
    CHECK(chrome->active_chip() != 0);

    // Now a real submission, which is what makes the run ours to report.
    CHECK(shell.submit_own("a clattering metallic texture").empty());
    CHECK(engine.submissions.size() == 1);
    shell.watch_build_log(log.string());
    shell.on_poll();

    CHECK(chrome->active_chip() == 0);
    CHECK_FALSE(chrome->active_stage_elapsed_text().empty());
    const auto activity = chrome->status_activity_text();
    INFO("activity: " << activity);
    CHECK(activity.find("elapsed") != std::string::npos);

    // And the log still WINS the moment it says anything: a silent run is
    // shown at stage 0, not pinned there. The line is a real marker
    // (stage_of maps "manifest" to Writing files) rather than an invented
    // one -- the first draft of this used "writing files", which matches
    // nothing, so it asserted that the log could not move the stage.
    { std::ofstream f(log, std::ios::app); f << "  manifest + panel ok\n"; }
    shell.on_poll();
    CHECK(chrome->active_chip() > 0);
}

TEST_CASE("a running build shows a clock, not just a word", "[phase7][stage]") {
    // "asking the model" with nothing moving is indistinguishable from a wedged
    // process, and a model call takes minutes. Forge's own chips carry a live
    // time; ours cleared the field.
    HermeticProjects isolated;
    forge_modular::ForgeModularShell shell;
    FakeEngine engine;
    shell.set_engine(&engine);
    pulp::state::StateStore store;
    shell.set_state_store(&store);
    shell.define_parameters(store);
    pulp::format::PrepareContext pc;
    pc.sample_rate = kSr; pc.max_buffer_size = kFrames;
    pc.input_channels = 1; pc.output_channels = 2;
    shell.prepare(pc);
    auto view = shell.create_view();
    REQUIRE(view != nullptr);
    auto* chrome = shell.chrome();
    chrome->enter_build();

    const auto log = std::filesystem::temp_directory_path() / "fm-clock.log";
    std::filesystem::remove(log);
    // A submission, so a run is genuinely in flight. The clock is deliberately
    // silent otherwise: rewriting the elapsed label every poll marks the view
    // dirty every poll, which had an IDLE app repainting the whole scene at
    // vsync forever for a number nobody was waiting on.
    CHECK(shell.submit_own("a clattering metallic texture").empty());
    shell.watch_build_log(log.string());

    { std::ofstream f(log); f << "  asking the model\n"; }
    shell.on_poll();
    CHECK(chrome->active_chip() == 0);
    // A time appears on the chip, and the activity line says what is being
    // waited on and for how long.
    CHECK_FALSE(chrome->active_stage_elapsed_text().empty());
    const auto activity = chrome->status_activity_text();
    INFO("activity: " << activity);
    CHECK(activity.find("asking the model") != std::string::npos);
    CHECK(activity.find("elapsed") != std::string::npos);

    // The clock belongs to the STAGE, so moving on restarts it rather than
    // carrying the previous stage's total.
    { std::ofstream f(log, std::ios::app); f << "  compiled\n"; }
    shell.on_poll();
    CHECK(chrome->active_chip() == 2);

    // A finished run stops claiming to be waiting on anything.
    { std::ofstream f(log, std::ios::app); f << "  installed -> /tmp/x.vcvplugin\n"; }
    shell.on_poll();
    CHECK(shell.build_outcome() == forge_modular::BuildOutcome::done);
    CHECK(chrome->status_activity_text().empty());

    std::filesystem::remove(log);
}

TEST_CASE("a finished build can actually open what it says it built",
          "[phase7][artifact]") {
    // The failure this closes: the app said "Built. Open it in Rack to play
    // it." and, in the same transcript, "the generator named a file that is
    // not there." Two parts of one screen contradicting each other, with a
    // green suite -- because artifact_path() was never tested against a REAL
    // generator log line, only around it.
    //
    // The line below is copied verbatim from a run. Both paths contain spaces,
    // which is exactly what the old parser could not survive: it scanned back
    // to the last separator and returned "/dualatt.vcv", the final segment
    // alone.
    HermeticProjects isolated;
    forge_modular::ForgeModularShell shell;
    pulp::state::StateStore store;
    shell.set_state_store(&store);
    shell.define_parameters(store);
    pulp::format::PrepareContext pc;
    pc.sample_rate = kSr; pc.max_buffer_size = kFrames;
    pc.input_channels = 1; pc.output_channels = 2;
    shell.prepare(pc);
    shell.set_standalone(true);
    auto view = shell.create_view();
    REQUIRE(view != nullptr);

    const auto dir = std::filesystem::temp_directory_path() /
                     "Forge Modular Test" / "examples" / "forge-modular" / "patches";
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    const auto artifact = dir / "dualatt.vcv";
    { std::ofstream f(artifact); f << "{\"version\":\"2\"}\n"; }

    const auto log = std::filesystem::temp_directory_path() / "fm-artifact.log";
    std::filesystem::remove(log);
    shell.watch_build_log(log.string());
    {
        std::ofstream f(log);
        f << "  installed \xE2\x86\x92 /Users/x/Rack2/plugins/ForgeModular.vcvplugin\n"
          << "  open it with:  \"/Applications/VCV Rack 2 Free.app/Contents/MacOS/Rack\" "
          << artifact.string() << "\n";
    }
    shell.on_poll();

    REQUIRE(shell.build_outcome() == forge_modular::BuildOutcome::done);

    // The whole path, spaces and all -- not its last segment.
    const auto found = shell.artifact_path();
    INFO("parsed: " << found);
    CHECK(found == artifact.string());

    // The invariant that was violated: if the build reports done, the thing it
    // named must be openable. Saying both at once is worse than saying neither.
    CHECK(std::filesystem::exists(found));
    CHECK(shell.open_in_rack().empty());

    // And when the file genuinely is missing, it says so rather than claiming
    // success -- the same check, with the opposite answer.
    std::filesystem::remove(artifact);
    CHECK_FALSE(shell.open_in_rack().empty());

    std::filesystem::remove(log);
    std::filesystem::remove_all(std::filesystem::temp_directory_path() /
                                "Forge Modular Test", ec);
}

TEST_CASE("Open in Rack never offers to open something unsafe",
          "[phase7][artifact]") {
    // Three ways this button could lie, all closed here: offering to open
    // nothing, offering a path the generator named but did not write, and --
    // the subtle one -- offering a patch while the next build is rewriting it,
    // which opens a half-written file or the previous module under the new
    // one's name.
    HermeticProjects isolated;
    forge_modular::ForgeModularShell shell;
    pulp::state::StateStore store;
    shell.set_state_store(&store);
    shell.define_parameters(store);
    pulp::format::PrepareContext pc;
    pc.sample_rate = kSr; pc.max_buffer_size = kFrames;
    pc.input_channels = 1; pc.output_channels = 2;
    shell.prepare(pc);
    shell.set_standalone(true);
    auto view = shell.create_view();
    REQUIRE(view != nullptr);
    shell.chrome()->enter_build();

    const auto log = std::filesystem::temp_directory_path() / "fm-openrules.log";
    std::filesystem::remove(log);
    shell.watch_build_log(log.string());

    auto* button = [&]() -> pulp::view::TextButton* {
        auto* acc = shell.chrome()->build_accessory();
        if (!acc) return nullptr;
        for (int i = 0; i < acc->child_count(); ++i) {
            auto* b = dynamic_cast<pulp::view::TextButton*>(acc->child_at(i));
            if (b && b->access_label() == "Open in VCV Rack") return b;
        }
        return nullptr;
    }();
    REQUIRE(button != nullptr);

    // Nothing built: hidden, and refuses if driven anyway.
    shell.on_poll();
    CHECK_FALSE(button->visible());
    CHECK_FALSE(shell.open_in_rack().empty());

    // Mid-build: hidden, and refuses for the RIGHT reason. The real flow
    // truncates the log on each run, so a path from a previous build does not
    // survive into the next one -- but the click can still land after a new
    // build starts, and opening a patch being rewritten gets a half-written
    // file or the previous module under the new one's name.
    { std::ofstream f(log); f << "  asking the model\n"; }
    shell.on_poll();
    REQUIRE(shell.build_outcome() == forge_modular::BuildOutcome::running);
    CHECK_FALSE(button->visible());
    const auto why = shell.open_in_rack();
    INFO("refusal: " << why);
    CHECK(why.find("build is running") != std::string::npos);

    // Finished, but the file was never written: refuses, and names the path so
    // the refusal is actionable rather than mysterious.
    {
        std::ofstream f(log, std::ios::app);
        f << "  open it with:  \"/Applications/VCV Rack 2 Free.app/Contents/MacOS/Rack\""
             " /tmp/forge-modular-never-written.vcv\n"
          << "  installed -> /tmp/x.vcvplugin\n";
    }
    shell.on_poll();
    REQUIRE(shell.build_outcome() == forge_modular::BuildOutcome::done);
    const auto missing = shell.open_in_rack();
    CHECK(missing.find("not there") != std::string::npos);
    CHECK(missing.find(".vcv") != std::string::npos);

    std::filesystem::remove(log);
}

TEST_CASE("a finished patch shows itself and can be opened", "[phase7][artifact]") {
    // Reported from a patch build: the screen said "Built. Open it in Rack to
    // play it", the button was absent, and the stage still showed the
    // materializing skeleton. Two generators write two formats -- generate.py
    // says  open it with: "<rack>" <patch>  and patch.py says
    // built 8 modules, 10 cables -> <patch>  -- and only the first was read.
    HermeticProjects isolated;
    forge_modular::ForgeModularShell shell;
    pulp::state::StateStore store;
    shell.set_state_store(&store);
    shell.define_parameters(store);
    pulp::format::PrepareContext pc;
    pc.sample_rate = kSr; pc.max_buffer_size = kFrames;
    pc.input_channels = 1; pc.output_channels = 2;
    shell.prepare(pc);
    shell.set_standalone(true);
    auto view = shell.create_view();
    REQUIRE(view != nullptr);
    auto* chrome = shell.chrome();
    chrome->enter_build();

    const std::string real = a_real_patch();
    REQUIRE_FALSE(real.empty());

    const auto log = std::filesystem::temp_directory_path() / "fm-patchdone.log";
    std::filesystem::remove(log);
    shell.watch_build_log(log.string());
    {
        // patch.py's format, verbatim: an arrow and a bare path, no quotes.
        std::ofstream f(log);
        f << "  built 8 modules, 10 cables \xE2\x86\x92 " << real << "\n";
    }
    shell.on_poll();

    REQUIRE(shell.build_outcome() == forge_modular::BuildOutcome::done);
    CHECK(shell.artifact_path() == real);      // the arrow format is read
    CHECK(shell.open_in_rack().empty());       // and it can actually be opened

    // The stage shows the patch rather than a skeleton that never resolves.
    REQUIRE(chrome->stage_accessory() != nullptr);
    CHECK(chrome->stage_accessory()->visible());
    REQUIRE(shell.explanation() != nullptr);
    CHECK(shell.explanation()->line_count() > 0);
    REQUIRE(shell.rack_preview() != nullptr);
    CHECK(shell.rack_preview()->modules().size() > 0);

    std::filesystem::remove(log);
}

TEST_CASE("every build starts its own clock and its own card",
          "[phase7][stage]") {
    // Reported from a screenshot: a new module started from Home showed
    // "asking the model · 5m 27s elapsed" beside a Thinking chip reading 25s
    // -- two numbers for the same thing disagreeing, because the run clock was
    // set once when the editor opened rather than when a build began.
    HermeticProjects isolated;
    forge_modular::ForgeModularShell shell;
    FakeEngine engine;
    shell.set_engine(&engine);
    pulp::state::StateStore store;
    shell.set_state_store(&store);
    shell.define_parameters(store);
    pulp::format::PrepareContext pc;
    pc.sample_rate = kSr; pc.max_buffer_size = kFrames;
    pc.input_channels = 1; pc.output_channels = 2;
    shell.prepare(pc);
    auto view = shell.create_view();
    REQUIRE(view != nullptr);
    auto* chrome = shell.chrome();

    const auto log = std::filesystem::temp_directory_path() / "fm-clock2.log";
    std::filesystem::remove(log);
    shell.watch_build_log(log.string());

    // First build finishes and leaves a verdict on the card.
    chrome->submit_prompt("a 4 HP attenuverter",
                          forge::ForgeChrome::PromptOrigin::home);
    { std::ofstream f(log); f << "  asking the model\n  installed -> /tmp/a.vcvplugin\n"; }
    shell.on_poll();
    REQUIRE(shell.build_outcome() == forge_modular::BuildOutcome::done);
    const int after_first = chrome->chat_line_count();
    CHECK(after_first > 0);

    // A second build from Home clears the transcript AND resets the card, so
    // the new run cannot inherit the old one's verdict or its elapsed time.
    // The log is truncated too, as a real run does.
    std::filesystem::remove(log);
    chrome->enter_home();
    chrome->submit_prompt("a 3 HP clock divider",
                          forge::ForgeChrome::PromptOrigin::home);
    CHECK(chrome->chat_line_count() < after_first);   // fresh transcript
    CHECK(chrome->status_note_text().empty());        // no carried-over note

    // A follow-up keeps the conversation but still restarts the card, because
    // it is a new run even though it is the same project.
    { std::ofstream f(log); f << "  asking the model\n  installed -> /tmp/b.vcvplugin\n"; }
    shell.on_poll();
    const int before_followup = chrome->chat_line_count();
    chrome->submit_prompt("make it 6 HP",
                          forge::ForgeChrome::PromptOrigin::project);
    CHECK(chrome->chat_line_count() >= before_followup);   // conversation kept
    CHECK(chrome->status_note_text().empty());             // card still reset

    std::filesystem::remove(log);
}

TEST_CASE("Open in Rack picks a sensible place to open", "[phase7][artifact]") {
    // The rules, in the order they are decided:
    //   Rack already running        -> hand it the patch, wherever we are
    //   hosted + Rack Pro installed -> the patch belongs in THIS session, and
    //                                  a second Rack would fight for the audio
    //                                  device; we cannot insert a plugin into
    //                                  a running host from outside, so say so
    //   Rack installed              -> open the desktop app
    //   nothing                     -> reveal the file rather than describe it
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
    shell.chrome()->enter_build();

    const std::string real = a_real_patch();
    REQUIRE_FALSE(real.empty());
    const auto log = std::filesystem::temp_directory_path() / "fm-where.log";
    std::filesystem::remove(log);
    shell.watch_build_log(log.string());
    { std::ofstream f(log);
      f << "  built 8 modules, 10 cables \xE2\x86\x92 " << real << "\n"; }
    shell.on_poll();
    REQUIRE(shell.build_outcome() == forge_modular::BuildOutcome::done);

    // Whatever this machine has, the answer is never the old "installed for
    // VCV Rack: <path>" refusal that launched nothing.
    shell.set_standalone(true);
    const auto standalone_said = shell.open_in_rack();
    INFO("standalone: " << standalone_said);
    CHECK(standalone_said.find("installed for") == std::string::npos);

    shell.set_standalone(false);
    const auto hosted_said = shell.open_in_rack();
    INFO("hosted: " << hosted_said);
    CHECK(hosted_said.find("installed for") == std::string::npos);
    // Hosted always says something -- either where to place Rack, or that a
    // second Rack is about to claim an audio device. Silence would leave a
    // DAW user wondering what just happened.
    CHECK_FALSE(hosted_said.empty());

    std::filesystem::remove(log);
}

// ─── The editor's views outlive nothing ──────────────────────────────────────
//
// This shell IS the processor. A host opens, closes and reopens a plugin
// window freely while the processor stays loaded, so every pointer the shell
// keeps into an editor's view tree is borrowed for exactly one editor. Holding
// one past a close is not a cosmetic mistake: the next build to finish walks
// it.
namespace {

/// Is `needle` still reachable by walking down from `root`?
bool in_tree(const pulp::view::View* root, const pulp::view::View* needle) {
    if (!root || !needle) return false;
    if (root == needle) return true;
    for (std::size_t i = 0; i < root->child_count(); ++i)
        if (in_tree(root->child_at(i), needle)) return true;
    return false;
}

/// A two-module, one-cable patch on disk, with the sidecar that carries its
/// reasoning -- the shape `open_patch_file` is handed at the end of a build.
std::string lifetime_patch(const std::filesystem::path& dir, const char* stem) {
    std::filesystem::create_directories(dir);
    const auto p = dir / (std::string(stem) + ".vcv");
    {
        std::ofstream f(p);
        f << R"({"version":"2.6.6","modules":[)"
          << R"({"id":1,"plugin":"ForgeModular","model":"VCO","pos":[0,0]},)"
          << R"({"id":2,"plugin":"Core","model":"AudioInterface2","pos":[10,0]}],)"
          << R"("cables":[{"id":1,"outputModuleId":1,"outputId":0,)"
          << R"("inputModuleId":2,"inputId":0,"color":"#00b56e"}]})";
    }
    {
        std::ofstream f(dir / (std::string(stem) + ".why.json"));
        f << R"({"cables":{"1:0>2:0":{"why":"the voice reaches the output",)"
          << R"("from_port":"OUT","to_port":"IN"}}})";
    }
    return p.string();
}

/// A generator that says yes. Without one, start_build_with refuses before it
/// ever resets the session, and none of the paths below are reached.
struct WillingLifetimeEngine : forge_modular::EngineClient {
    bool available() const override { return true; }
    bool ensure_running() override { return true; }
    void submit(const std::string&, bool) override {}
};

}  // namespace

TEST_CASE("a build that lands after the editor closed writes to nothing",
          "[crash][lifetime]") {
    // REAPER aborted the moment a build finished inside the hosted plugin:
    //
    //   ___BUG_IN_CLIENT_OF_LIBMALLOC_POINTER_BEING_FREED_WAS_NOT_ALLOCATED
    //     <- PatchExplanation::set_connections <- show_rack
    //     <- open_patch_file <- on_poll
    //
    // libmalloc is precise about which fault this is: the pointer being freed
    // was never allocated. `set_connections` move-assigns over `connections_`,
    // which frees the vector's old buffer -- and on a DESTROYED
    // PatchExplanation that buffer pointer is whatever the freed memory now
    // holds. Nothing had to corrupt the heap first; reaching a view the editor
    // already took away is the whole fault.
    //
    // Reproduced here by closing the editor and then letting the generator's
    // answer arrive, which is what a host does whenever somebody shuts the
    // plugin window while a build is still running.
    HermeticProjects isolated;
    const auto dir = std::filesystem::temp_directory_path() / "forge-lifetime-late";
    std::filesystem::remove_all(dir);
    const auto patch = lifetime_patch(dir, "late");

    WillingLifetimeEngine engine;
    forge_modular::ForgeModularShell shell;
    shell.set_engine(&engine);
    pulp::state::StateStore store;
    shell.set_state_store(&store);
    shell.define_parameters(store);

    auto view = shell.create_view();
    REQUIRE(view != nullptr);
    // A patch on screen first, so the explanation owns a real connections_
    // buffer -- an empty vector frees nothing and would abort nowhere.
    REQUIRE(shell.open_patch_file(patch).empty());
    REQUIRE(shell.explanation() != nullptr);
    REQUIRE(shell.explanation()->line_count() == 1);

    // The host's teardown order: the shell drops the chrome, then the bridge
    // destroys the root the chrome pointed into.
    shell.on_view_closed(*view);
    view.reset();

    // Asserted as pointers rather than left to a sanitiser. A use-after-free
    // only faults when the memory happens to have been reused, so a plain run
    // can pass over it all day; "the shell still names a view it no longer
    // has" is unambiguous, and it is exactly what the crash report describes.
    CHECK(shell.explanation() == nullptr);
    CHECK(shell.rack_preview() == nullptr);

    // And the step that aborted. It must be a no-op now, not a write.
    const auto loaded = forge_modular::load_patch(patch);
    REQUIRE(loaded.ok());
    shell.show_rack(loaded.modules, loaded.connections);
    shell.on_poll();
    std::filesystem::remove_all(dir);
}

TEST_CASE("reopening the editor never reads the closed one's views",
          "[crash][lifetime]") {
    // The same borrowed pointers are read on the way IN, before anything has
    // replaced them: ForgeChrome's constructor asks the shell for its copy,
    // and chrome_copy() reads the rack preview and the module summary to put
    // the patch's size in the badge. On the second editor of a session those
    // name the first editor's freed views.
    //
    // Each round below closes and reopens, and drives everything a person can
    // do to what is on screen in between, so the sanitiser has every one of
    // these pointers to walk.
    HermeticProjects isolated;
    const auto dir = std::filesystem::temp_directory_path() / "forge-lifetime-reopen";
    std::filesystem::remove_all(dir);
    const auto patch = lifetime_patch(dir, "reopen");

    WillingLifetimeEngine engine;
    forge_modular::ForgeModularShell shell;
    shell.set_engine(&engine);
    pulp::state::StateStore store;
    shell.set_state_store(&store);
    shell.define_parameters(store);
    pulp::format::PrepareContext pc;
    pc.sample_rate = kSr; pc.max_buffer_size = kFrames;
    pc.input_channels = 1; pc.output_channels = 2;
    shell.prepare(pc);

    const auto log = dir / "run.log";

    for (int session = 0; session < 3; ++session) {
        INFO("session " << session);
        auto view = shell.create_view();
        REQUIRE(view != nullptr);
        auto* chrome = shell.chrome();
        REQUIRE(chrome != nullptr);
        REQUIRE(shell.explanation() != nullptr);
        // Belonging to THIS editor, asserted by reachability rather than by
        // comparing against the last one's address: the previous explanation
        // was freed, so the allocator is free to hand its block straight back
        // and an address comparison would fail on a correct build.
        CHECK(in_tree(view.get(), shell.explanation()));
        CHECK(in_tree(view.get(), shell.rack_preview()));

        for (int round = 0; round < 3; ++round) {
            INFO("round " << round);
            chrome->enter_home();
            if (auto* input = chrome->prompt_input())
                input->set_text("an ambient drone that never repeats");
            shell.watch_build_log(log.string());
            // Through the shell's own front door, from Home -- which is what
            // resets the session and empties the chat rail.
            CHECK(shell.start_build().empty());
            { std::ofstream f(log, std::ios::trunc); f << "  asking the model\n"; }
            chrome->poll();
            { std::ofstream f(log, std::ios::app);
              f << "  built 2 modules, 1 cables \xE2\x86\x92 " << patch << "\n"; }
            chrome->poll();
            CHECK(shell.build_outcome() == forge_modular::BuildOutcome::done);

            shell.set_depth(forge_modular::ForgeModularShell::Depth::learning);
            shell.set_depth(forge_modular::ForgeModularShell::Depth::terse);
            if (auto* e = shell.explanation()) {
                e->hover_line(0);
                e->hover_line(std::nullopt);
                e->apply_pending_rewrap();
            }
            if (auto* p = shell.rack_preview()) {
                p->set_highlight(0);
                p->set_highlight(std::nullopt);
            }
            shell.set_artifact(forge_modular::Artifact::module);
            shell.set_artifact(forge_modular::Artifact::patch);
            shell.refresh_rack_presence();
            chrome->poll();
        }

        shell.on_view_closed(*view);
        view.reset();
        CHECK(shell.explanation() == nullptr);
        CHECK(shell.rack_preview() == nullptr);
    }
    std::filesystem::remove_all(dir);
}

TEST_CASE("a session reset keeps the product's own accessory in the rail",
          "[crash][chat-rail]") {
    // Starting a build from Home empties the chat rail, which is right: the
    // conversation belongs to the build being replaced. The product's chat
    // accessory is not part of that conversation -- it shows the ARTIFACT, and
    // for Forge Modular it is the patch explanation and the module spec.
    //
    // Sweeping it out with the bubbles cost two things. It could never come
    // back, because the accessory hook runs once, when the chrome mounts -- so
    // from the first build onward the explanation was built, filled and shown
    // to nobody. And it moved the views' OWNER from the tree to the chrome's
    // retired list, which ~ForgeChrome frees at the top of editor teardown,
    // while the shell is still live and still pointing at them.
    HermeticProjects isolated;
    forge_modular::ForgeModularShell shell;
    pulp::state::StateStore store;
    shell.set_state_store(&store);
    shell.define_parameters(store);
    auto view = shell.create_view();
    REQUIRE(view != nullptr);
    auto* chrome = shell.chrome();
    REQUIRE(chrome != nullptr);

    auto* explanation = shell.explanation();
    REQUIRE(explanation != nullptr);
    REQUIRE(in_tree(view.get(), explanation));

    chrome->begin_new_session();
    CHECK(shell.explanation() == explanation);   // not rebuilt
    CHECK(in_tree(view.get(), explanation));     // and not detached
}

TEST_CASE("a test cannot open an application on somebody's desktop",
          "[rack][open][launcher]") {
    // This suite really launched VCV Rack. `open_in_rack()` shelled out to
    // `open -a "VCV Rack 2 Free.app"` directly, eight cases here call it, and
    // two of them succeed -- so every full run opened Rack on the screen of
    // whoever was using the machine. Worse, the patch it was handed lives in
    // a temp fixture the suite deletes on the way out, so Rack came up and
    // put a "could not open archive" modal in front of them. It was reported
    // twice by the person sitting at the machine before anyone here noticed.
    //
    // Deciding WHICH command to run is the shell's job and worth testing.
    // Running it is not something a test may do, so the running is now
    // somebody else's job: production installs a launcher, and a bare shell
    // -- which is what every case in this file builds -- has none.
    HermeticProjects isolated;
    const auto patch = std::filesystem::temp_directory_path() /
                       "forge-launcher-test.vcv";
    { std::ofstream f(patch); f << R"({"version":"2.6.6","modules":[)"
        << R"({"id":1,"plugin":"Core","model":"AudioInterface2","pos":[0,0]}],)"
        << R"("cables":[]})"; }

    forge_modular::ForgeModularShell shell;
    pulp::state::StateStore store;
    shell.set_state_store(&store);
    shell.define_parameters(store);
    auto view = shell.create_view();
    REQUIRE(view != nullptr);
    // A finished build, so the shell is willing to open anything at all --
    // it refuses while one is in flight, because the patch is being rewritten.
    const auto log = std::filesystem::temp_directory_path() /
                     "forge-launcher-test.log";
    std::filesystem::remove(log);
    shell.watch_build_log(log.string());
    {
        std::ofstream f(log);
        f << "  installed \xE2\x86\x92 /Users/x/Rack2/plugins/ForgeModular.vcvplugin\n"
          << "  open it with:  \"/Applications/VCV Rack 2 Free.app/Contents/MacOS/Rack\" "
          << patch.string() << "\n";
    }
    shell.on_poll();
    REQUIRE(shell.build_outcome() == forge_modular::BuildOutcome::done);

    // No launcher: it still DECIDES, and records what it decided.
    const auto why = shell.open_in_rack();
    INFO("open_in_rack said: " << why);
    REQUIRE(shell.launched().size() == 1);
    CHECK(shell.launched()[0].find("VCV Rack") != std::string::npos);
    CHECK(shell.launched()[0].find(patch.string()) != std::string::npos);

    // With one installed, that is the ONLY route out. If any path still
    // shelled out directly, the recorded count and the launcher's count would
    // disagree -- which is exactly what a direct `ProcessEngine::run` looks
    // like from here.
    std::vector<std::string> ran;
    shell.set_launcher([&](const std::string& c) { ran.push_back(c); });
    shell.open_in_rack();
    CHECK(ran.size() == shell.launched().size() - 1);
    REQUIRE_FALSE(ran.empty());
    CHECK(ran.back() == shell.launched().back());

    std::filesystem::remove(patch);
    std::filesystem::remove(log);
}

TEST_CASE("asking about a module points at its cable", "[rack][ask]") {
    // The prototype's own note on Ask: it "never mutates the artifact -- it
    // appends an answer and points at the picture". We had the answer and not
    // the pointing, so "why did you wire the LFO there?" produced a paragraph
    // and left the reader to find the LFO's cable in a rack of ten modules.
    // The same words beside a glowing cable teach more.
    const auto rack = sample_rack();
    const auto cables = sample_patch();

    // Both ends named is unambiguous and wins outright.
    const auto both = forge_modular::cable_for_question(
        "why does the VCO go into the VCF?", cables, rack);
    REQUIRE(both.has_value());
    CHECK(cables[*both].from_module == "VCO");
    CHECK(cables[*both].to_module == "VCF");

    // One end named still points somewhere useful.
    const auto one = forge_modular::cable_for_question(
        "what is the VCF doing here", cables, rack);
    REQUIRE(one.has_value());
    CHECK((cables[*one].from_module == "VCF" || cables[*one].to_module == "VCF"));

    // A question naming nothing in this patch lights NOTHING. This is the
    // assertion that matters: a function that always returned cable 0 would
    // satisfy both checks above and would point confidently at the wrong
    // cable for every question anyone ever asked.
    CHECK_FALSE(forge_modular::cable_for_question(
        "how much CPU does this use?", cables, rack).has_value());
    CHECK_FALSE(forge_modular::cable_for_question("", cables, rack).has_value());

    // Whole words only. "ENV" sits inside "ENVELOPE", and a substring match
    // would light a cable for a word nobody typed.
    auto named = rack;
    named[0].name = "ENV";
    std::vector<forge_modular::Connection> one_cable{
        {named[0].id, "OUT", named[1].id, "IN", forge_modular::SignalRole::mod, ""}};
    CHECK(forge_modular::cable_for_question("what does ENV do", one_cable, named)
              .has_value());
    CHECK_FALSE(forge_modular::cable_for_question("describe the ENVELOPES",
                                                  one_cable, named).has_value());
}

TEST_CASE("Ask lights the cable it is answering about", "[rack][ask]") {
    // The pure `cable_for_question` being right proves nothing about the
    // screen: the shell can compute the answer and then paint nothing. This
    // drives Ask the way a reader does and looks at the picture afterwards.
    HermeticProjects isolated;
    const auto dir = std::filesystem::temp_directory_path() / "forge-ask-points";
    std::filesystem::remove_all(dir);
    const auto patch = lifetime_patch(dir, "points");   // VCO -> audio out

    WillingLifetimeEngine engine;
    forge_modular::ForgeModularShell shell;
    shell.set_engine(&engine);
    pulp::state::StateStore store;
    shell.set_state_store(&store);
    shell.define_parameters(store);
    auto view = shell.create_view();
    REQUIRE(view != nullptr);
    REQUIRE(shell.open_patch_file(patch).empty());
    auto* preview = shell.rack_preview();
    REQUIRE(preview != nullptr);
    REQUIRE(preview->connections().size() == 1);
    REQUIRE_FALSE(preview->highlight().has_value());   // nothing lit yet

    // Naming a module in the rack lights that module's cable.
    shell.chrome()->prompt_input()->set_text("why is the VCO wired like that?");
    CHECK(shell.ask().empty());
    REQUIRE(preview->highlight().has_value());
    CHECK(*preview->highlight() == 0u);

    // ...and the explanation's own line is lit with it, so the words and the
    // cable agree instead of two panes pointing at different things.
    CHECK(shell.explanation()->hovered() == preview->highlight());

    // The assertion that carries the weight: a question naming nothing in
    // this rack CLEARS the light. Leaving the last question's cable glowing
    // is worse than never lighting one -- the reader takes the stale glow for
    // an answer to the question they just asked.
    shell.chrome()->prompt_input()->set_text("how much CPU does this use?");
    CHECK(shell.ask().empty());
    CHECK_FALSE(preview->highlight().has_value());
    CHECK_FALSE(shell.explanation()->hovered().has_value());
}

/// A HOME with a Rack install we control.
///
/// Availability is answered by reading the plugin manifests under the real
/// $HOME, which would make this test say different things on different
/// machines -- and pass on the one machine where the module happens to be
/// installed. A fabricated home makes the answer a property of the code.
struct FakeRackHome {
    explicit FakeRackHome(const char* brand, std::vector<std::string> slugs) {
        const char* was = std::getenv("HOME");
        previous = was ? was : "";
        dir = std::filesystem::temp_directory_path() / "forge-fake-home";
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
        const auto plug = dir / "Library" / "Application Support" / "Rack2" /
                          "plugins-mac-arm64" / brand;
        std::filesystem::create_directories(plug, ec);
        std::ofstream f(plug / "plugin.json");
        f << R"({"slug":")" << brand << R"(","modules":[)";
        for (std::size_t i = 0; i < slugs.size(); ++i)
            f << (i ? "," : "") << R"({"slug":")" << slugs[i] << R"(","name":")"
              << slugs[i] << R"("})";
        f << "]}";
        f.close();
        ::setenv("HOME", dir.string().c_str(), 1);
    }
    ~FakeRackHome() {
        if (previous.empty()) ::unsetenv("HOME");
        else ::setenv("HOME", previous.c_str(), 1);
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
    }
    std::filesystem::path dir;
    std::string previous;
};

TEST_CASE("Open in Rack refuses a patch this Rack cannot create",
          "[rack][open]") {
    // Rack blocks on a modal error dialog -- even started headless -- when a
    // patch names a module its installed plugin does not have. Proven against
    // the real Rack 2.6.6: the stack sits in osdialog_message under
    // patch::Manager::load and never returns. What the user sees is Rack
    // opening with nothing in it, which is exactly the report that started
    // this.
    //
    // The preview already knew: it draws those modules struck out. Handing
    // Rack the patch anyway is having the information and using it for
    // nothing.
    HermeticProjects isolated;
    // A brand nothing else uses: availability is cached per plugin for the
    // life of the process, so a name shared with another case would answer
    // from whatever that case looked up first.
    FakeRackHome home("ForgeGateProbe", {"PRESENT"});

    const auto dir = std::filesystem::temp_directory_path() / "forge-open-refuse";
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    std::filesystem::create_directories(dir);

    auto write_patch = [&](const char* stem, const char* model) {
        const auto p = dir / (std::string(stem) + ".vcv");
        std::ofstream f(p);
        f << R"({"version":"2.6.6","modules":[{"id":1,"plugin":"ForgeGateProbe")"
          << R"(,"model":")" << model << R"(","pos":[0,0]}],"cables":[]})";
        return p;
    };
    const auto absent = write_patch("absent", "ABSENT");
    const auto present = write_patch("present", "PRESENT");

    auto point_at = [&](const std::filesystem::path& patch) {
        const auto log = dir / "build.log";
        {
            std::ofstream f(log);
            f << "  open it with:  \"/Applications/VCV Rack 2 Free.app/Contents"
                 "/MacOS/Rack\" " << patch.string() << "\n";
        }
        return log.string();
    };

    forge_modular::ForgeModularShell shell;
    pulp::state::StateStore store;
    shell.set_state_store(&store);
    shell.define_parameters(store);
    auto view = shell.create_view();
    REQUIRE(view != nullptr);

    shell.watch_build_log(point_at(absent));
    shell.on_poll();
    // Without this the build monitor is still "running" and open_in_rack
    // refuses for that reason instead -- a green test that never reached the
    // gate it was written for. It did, the first time this was written.
    REQUIRE(shell.build_outcome() != forge_modular::BuildOutcome::running);
    REQUIRE(shell.artifact_path() == absent.string());

    const auto why = shell.open_in_rack();
    INFO("message was: " << why);
    CHECK_FALSE(why.empty());                        // it says why
    CHECK(why.find("ABSENT") != std::string::npos);  // and names the module

    // The assertion that carries the weight. A refusal that still launches is
    // the bug with an apology attached: Rack is already sitting on its dialog
    // by the time the sentence is read. `launched()` records every command the
    // shell decided to run, so this is the decision itself, not a guess.
    //
    // Narrowed to the launches this case is about. Other things legitimately
    // reach that record -- an editor asks for a library index when it opens --
    // and counting every command would make this assertion depend on them.
    const auto rack_launches = [&] {
        std::vector<std::string> out;
        for (const auto& c : shell.launched())
            if (c.find("VCV Rack") != std::string::npos) out.push_back(c);
        return out;
    };
    CHECK(rack_launches().empty());

    // And the gate is about THIS patch, not about refusing everything: a patch
    // whose modules all exist is still handed over. Without this, deleting the
    // launch entirely would pass every check above.
    shell.watch_build_log(point_at(present));
    shell.on_poll();
    REQUIRE(shell.artifact_path() == present.string());
    const auto ok_why = shell.open_in_rack();
    INFO("second message was: " << ok_why);
    CHECK(rack_launches().size() == 1);

    std::filesystem::remove_all(dir, ec);
}

TEST_CASE("closing the editor drops the mention overlay's view pointers",
          "[crash][lifetime]") {
    // The overlay belongs to the SHELL, which outlives every editor, while the
    // views it points at belong to the editor's tree. Between one editor
    // closing and the next being built there is nothing here to point at, and
    // a kept pointer names freed memory.
    //
    // `forget_views()` existed for this and had no test that could fail
    // without it: removing it left the whole suite green, including under
    // Guard Malloc, because nothing touched the overlay after an editor
    // closed. An untested guard is indistinguishable from a comment.
    HermeticProjects isolated;
    forge_modular::ForgeModularShell shell;
    pulp::state::StateStore store;
    shell.set_state_store(&store);
    shell.define_parameters(store);
    auto view = shell.create_view();
    REQUIRE(view != nullptr);

    // Open it, so root_ and list_ genuinely point into this editor's tree. An
    // overlay that was never opened has nothing to forget.
    shell.begin_mention();
    REQUIRE(shell.mentions().is_open());

    shell.on_view_closed(*view);
    view.reset();

    // Deterministic half: an overlay still claiming to be open is claiming a
    // window that no longer exists, and the next thing to consult it acts on
    // that answer.
    CHECK_FALSE(shell.mentions().is_open());

    // Guard Malloc half: close() calls root_->set_visible(false) whenever
    // root_ is non-null. With the pointers forgotten this is a no-op; with
    // them kept it writes into the freed view tree.
    shell.mentions().close();
    CHECK_FALSE(shell.mentions().is_open());
}

TEST_CASE("the materializing placeholder is shaped like the artifact",
          "[rack][skeleton]") {
    // A placeholder is a promise about the result. One shape served both, so
    // a PATCH build drew a single module panel -- four knobs and a fader --
    // for the whole build, with the header saying PATCH beside it, and then
    // snapped to a rack of ten modules when it landed.
    HermeticProjects isolated;
    forge_modular::ForgeModularShell shell;
    pulp::state::StateStore store;
    shell.set_state_store(&store);
    shell.define_parameters(store);
    auto view = shell.create_view();
    REQUIRE(view != nullptr);
    auto* c = shell.chrome();
    REQUIRE(c != nullptr);

    // Both shapes exist, and exactly one is ever showing. A build cannot draw
    // a module panel and a rack at once, and must not draw neither.
    auto showing = [&] {
        return std::make_pair(c->skeleton_shape_visible(
                                  forge::ForgeChrome::SkeletonShape::module),
                              c->skeleton_shape_visible(
                                  forge::ForgeChrome::SkeletonShape::rack));
    };

    shell.set_artifact(forge_modular::Artifact::module);
    {
        auto [mod, rack] = showing();
        CHECK(mod);
        CHECK_FALSE(rack);
    }

    shell.set_artifact(forge_modular::Artifact::patch);
    {
        auto [mod, rack] = showing();
        CHECK(rack);
        CHECK_FALSE(mod);      // the assertion that carries the weight

        // And it actually draws panels. A visible but EMPTY container passes
        // every visibility check above while putting nothing on the stage --
        // which looks like a build that produced nothing, the exact reading
        // the skeleton exists to avoid.
        CHECK(c->skeleton_rack_panel_count() >= 3);
    }

    // And back, so this is a switch rather than a one-way door: a user who
    // tries Patch and returns to Module would otherwise be promised a rack
    // for the rest of the session.
    shell.set_artifact(forge_modular::Artifact::module);
    {
        auto [mod, rack] = showing();
        CHECK(mod);
        CHECK_FALSE(rack);
    }
}

TEST_CASE("the heading names the artifact, and never eats a real title",
          "[rack][skeleton]") {
    // The default heading names the artifact, and it was chosen once at
    // construction. Starting on Module and switching to Patch left "Untitled
    // module" beside a PATCH pill for the whole build -- the same disagreement
    // as a patch materializing into a module panel, in words instead of shapes.
    HermeticProjects isolated;
    forge_modular::ForgeModularShell shell;
    pulp::state::StateStore store;
    shell.set_state_store(&store);
    shell.define_parameters(store);
    auto view = shell.create_view();
    REQUIRE(view != nullptr);
    auto* c = shell.chrome();
    REQUIRE(c != nullptr);

    shell.set_artifact(forge_modular::Artifact::module);
    CHECK(c->project_title() == "Untitled module");

    shell.set_artifact(forge_modular::Artifact::patch);
    CHECK(c->project_title() == "Untitled patch");

    shell.set_artifact(forge_modular::Artifact::module);
    CHECK(c->project_title() == "Untitled module");

    // The assertion that carries the weight. Re-deriving on every switch is
    // the easy fix and the wrong one: a project the user has named is theirs,
    // and changing tabs must not take the name away.
    c->set_project_title_from_prompt("an ambient generative drone");
    const auto named = c->project_title();
    CHECK(named != "Untitled module");
    shell.set_artifact(forge_modular::Artifact::patch);
    CHECK(c->project_title() == named);
    shell.set_artifact(forge_modular::Artifact::module);
    CHECK(c->project_title() == named);
}

TEST_CASE("the depth tabs are actually on screen for a patch", "[rack][depth]") {
    // Terse/Standard/Learning was reachable in code, had nine tests covering
    // what set_depth does, and had never once appeared in the product. The
    // tabs are created hidden (the artifact defaults to module) and switching
    // to Patch re-showed the GROUP around them -- which was already showing,
    // because Open in Rack lives in it too. Every existing test asserted
    // behaviour through set_depth() and none asked whether a user could reach
    // it.
    HermeticProjects isolated;
    forge_modular::ForgeModularShell shell;
    pulp::state::StateStore store;
    shell.set_state_store(&store);
    shell.define_parameters(store);
    auto view = shell.create_view();
    REQUIRE(view != nullptr);

    auto visible_tabs = [&] {
        std::size_t n = 0;
        for (auto* b : shell.depth_tabs())
            if (b && b->visible()) ++n;
        return n;
    };

    REQUIRE(shell.depth_tabs().size() == 3);   // terse, standard, learning

    shell.set_artifact(forge_modular::Artifact::patch);
    CHECK(visible_tabs() == 3);                // the assertion that was missing

    // A module has one artifact and nothing to narrate at three depths, so the
    // tabs go away -- but the group must stay, because Open in Rack is in it.
    shell.set_artifact(forge_modular::Artifact::module);
    CHECK(visible_tabs() == 0);
    CHECK(shell.depth_group_visible());

    // And back, because the failure was specifically a one-way door: built
    // hidden, never revealed.
    shell.set_artifact(forge_modular::Artifact::patch);
    CHECK(visible_tabs() == 3);
}

TEST_CASE("a module we did not make is drawn from its own artwork",
          "[rack][artwork]") {
    // Only our own panels were ever looked up, so every module from another
    // plugin came out as a blank slab -- most visibly Core's audio interface,
    // which is in every patch that makes a sound. Its cables then docked at
    // the panel edge instead of at jacks, because a face with no artwork has
    // no jack positions; the odd wiring and the empty panel were one bug.
    forge_modular::RackPreview preview;
    preview.set_panel_directory(
        "/Volumes/Workshop/Code/pulp-modular-rack/examples/forge-modular/res");

    if (!std::filesystem::exists("/Applications/VCV Rack 2 Free.app")) {
        WARN("Rack is not installed — vendor artwork cannot be checked here. "
             "This is a skip, not a pass.");
        return;
    }

    // Core's files are named for what the module is, not for its slug:
    // AudioInterface2 lives in Audio2.svg. Looking up the slug alone finds
    // nothing, which is exactly how it stayed blank.
    CHECK(preview.has_artwork_for("Core", "AudioInterface2"));

    // Ours still resolve, from our own res/ rather than a vendor's.
    CHECK(preview.has_artwork_for("ForgeModular", "VCO"));

    // And a plugin nobody has resolves to nothing rather than to somebody
    // else's panel -- drawing the wrong module's face is worse than drawing
    // none, because it looks right.
    CHECK_FALSE(preview.has_artwork_for("NoSuchPluginAnywhere", "GHOST"));
}

TEST_CASE("our modules draw their knobs, at the sizes they declare",
          "[rack][artwork]") {
    // A panel SVG is a BACKGROUND. Rack composites every knob over it as a
    // separate widget, so drawing only the background showed labels with
    // nothing under them -- FREQ, FINE and PW floating over blank plate.
    // Vendor panels happen to have a knob well painted in, so ours looked
    // uniquely broken beside them.
    forge_modular::RackPreview preview;
    preview.set_panel_directory(
        "/Volumes/Workshop/Code/pulp-modular-rack/examples/forge-modular/res");

    // The VCO declares FREQ (KnobLarge), FINE, PW (Knob) and FM (Trimpot).
    const auto [count, widest] = preview.knob_summary("VCO");
    CHECK(count == 4);

    // The assertion that carries the weight: sizes are READ, not assumed.
    // Drawing every control at one diameter looks plausible and is wrong about
    // every trimpot on every panel -- and nothing about the picture says so.
    CHECK(widest == Catch::Approx(18.3f));     // KnobLarge, not the 12.2 default

    // A model we do not ship resolves to no knobs rather than to some other
    // module's.
    CHECK(preview.knob_summary("NoSuchModelAnywhere").first == 0);
}

TEST_CASE("the @ list actually lists modules", "[rack][mention]") {
    // The overlay opened onto an EMPTY list every time: nothing ever called
    // set_source, so the search function was null and returned nothing. The
    // feature was reachable, tested for openness, and had never named a single
    // module. Same shape as the depth tabs -- present in code, absent in use.
    HermeticProjects isolated;
    forge_modular::ForgeModularShell shell;
    pulp::state::StateStore store;
    shell.set_state_store(&store);
    shell.define_parameters(store);
    auto view = shell.create_view();
    REQUIRE(view != nullptr);

    const auto counts = forge_modular::catalog_counts();
    if (counts.installed == 0 && counts.catalogued == 0) {
        WARN("no Rack install and no cached library here — a skip, not a pass");
        return;
    }

    shell.begin_mention();
    REQUIRE(shell.mentions().is_open());
    CHECK_FALSE(shell.mentions().candidates().empty());   // the missing part

    // Searching narrows rather than returning everything regardless.
    const auto vco = forge_modular::search_modules("VCO");
    const auto nonsense = forge_modular::search_modules("zzzznosuchthing");
    CHECK_FALSE(vco.empty());
    CHECK(nonsense.empty());

    // Installed modules rank above catalogued ones, because only they can be
    // wired into a patch that will sound. A list that offered an uninstallable
    // module first would be ranking novelty over usefulness.
    if (counts.installed > 0) {
        const auto all = forge_modular::search_modules("", 200);
        REQUIRE_FALSE(all.empty());
        CHECK(all.front().insertable());
    } else {
        // Legitimate — a machine with no Rack plugins has nothing installed —
        // but it must SAY so. Silently taking the empty branch is a test
        // reporting success for the half it did not run.
        WARN("nothing installed here, so the installed-ranks-first half of "
             "this test did not run. This is a skip, not a pass.");
    }
}

TEST_CASE("the mention list behaves like a dropdown", "[rack][mention]") {
    // It behaved like a label. Reported as "typing @ mention does nothing",
    // "search puts a dropdown at the top of the app" and "i can't select
    // anything can't figure out wtf that is supposed to do" -- three symptoms
    // of a list that was drawn and never wired.
    HermeticProjects isolated;
    forge_modular::ForgeModularShell shell;
    pulp::state::StateStore store;
    shell.set_state_store(&store);
    shell.define_parameters(store);
    auto view = shell.create_view();
    REQUIRE(view != nullptr);
    auto& m = shell.mentions();

    // A source with more rows than fit, so the window and its affordances are
    // exercised rather than assumed.
    const int kMany = 20;
    m.set_source([&](const std::string& q) {
        std::vector<forge_modular::MentionCandidate> out;
        for (int i = 0; i < kMany; ++i) {
            forge_modular::MentionCandidate c;
            c.brand = "Test";
            c.name = "Mod" + std::to_string(i);
            c.slug = "SLUG" + std::to_string(i);
            c.state = forge_modular::MentionCandidate::Availability::ready;
            if (q.empty() || c.name.find(q) != std::string::npos)
                out.push_back(c);
        }
        return out;
    });

    // TYPING narrows. handle_text ran once, when the button was pressed, and
    // every keystroke after it went only to the text field -- which is why
    // "@braids" stayed on whatever the first call produced.
    m.handle_text("@", 1);
    REQUIRE(m.is_open());
    CHECK(m.candidates().size() == kMany);      // every match kept, not 6
    m.handle_text("@Mod1", 5);
    CHECK(m.candidates().size() < static_cast<std::size_t>(kMany));
    CHECK_FALSE(m.candidates().empty());

    m.handle_text("@", 1);
    REQUIRE(m.candidates().size() == kMany);

    // ARROWS move the selection and scroll it into view.
    const int rows = forge_modular::MentionOverlay::visible_rows();
    CHECK(m.selected_index() == 0);
    CHECK(m.scroll_top() == 0);
    for (int i = 0; i < rows; ++i) m.move_selection(1);
    CHECK(m.selected_index() == rows);
    CHECK(m.scroll_top() > 0);                  // the window followed

    // UP walks back, then WRAPS to the end -- what a menu does rather than
    // stopping dead at the top.
    for (int i = 0; i < rows; ++i) m.move_selection(-1);
    CHECK(m.selected_index() == 0);
    m.move_selection(-1);
    CHECK(m.selected_index() == kMany - 1);
    CHECK(m.scroll_top() == kMany - rows);      // scrolled to the bottom

    // CLICKING a row chooses it -- the same path the mouse takes.
    std::string chosen;
    m.on_choose = [&](const std::string& slug) { chosen = slug; };
    m.handle_text("@", 1);
    m.choose(3);
    CHECK(chosen == "SLUG3");
    CHECK_FALSE(m.is_open());                   // and the list goes away

    // ESCAPE dismisses without choosing.
    chosen.clear();
    m.handle_text("@", 1);
    REQUIRE(m.is_open());
    pulp::view::KeyEvent esc;
    esc.key = pulp::view::KeyCode::escape;
    CHECK(m.handle_key_event(esc));
    CHECK_FALSE(m.is_open());
    CHECK(chosen.empty());

    // ENTER chooses the selected row, and is CONSUMED so it does not also
    // submit the prompt -- one keystroke doing two things, one unasked for.
    m.handle_text("@", 1);
    pulp::view::KeyEvent down;
    down.key = pulp::view::KeyCode::down;
    CHECK(m.handle_key_event(down));
    pulp::view::KeyEvent ret;
    ret.key = pulp::view::KeyCode::enter;
    CHECK(m.handle_key_event(ret));
    CHECK(chosen == "SLUG1");

    // And with the list CLOSED the keys are left alone, or the prompt could
    // never be submitted with Enter again.
    CHECK_FALSE(m.is_open());
    CHECK_FALSE(m.handle_key_event(ret));
    CHECK_FALSE(m.handle_key_event(down));
}

TEST_CASE("hovering the mention list does not free the rows being walked",
          "[rack][mention][crash]") {
    // SIGSEGV in View::simulate_hover, on a plain mouse move. The hover is
    // dispatched WHILE the framework walks the child list, and the handler
    // called rebuild_rows() -- which destroys those very children. The walk
    // then continued over freed memory.
    //
    // Shipped and crashed twice within a minute of the user moving the mouse.
    // Nothing caught it because every test drove the model directly and none
    // moved a pointer over the view.
    HermeticProjects isolated;
    forge_modular::ForgeModularShell shell;
    pulp::state::StateStore store;
    shell.set_state_store(&store);
    shell.define_parameters(store);
    auto view = shell.create_view();
    REQUIRE(view != nullptr);
    auto& m = shell.mentions();

    m.set_source([](const std::string&) {
        std::vector<forge_modular::MentionCandidate> out;
        for (int i = 0; i < 20; ++i) {
            forge_modular::MentionCandidate c;
            c.brand = "Test";
            c.name = "Mod" + std::to_string(i);
            c.slug = "S" + std::to_string(i);
            c.state = forge_modular::MentionCandidate::Availability::ready;
            out.push_back(c);
        }
        return out;
    });
    m.handle_text("@", 1);
    REQUIRE(m.is_open());
    REQUIRE(m.candidates().size() == 20);

    // LAY THE TREE OUT FIRST. Without this the rows have no computed bounds,
    // so nothing is ever "under" the pointer, no hover handler fires, and the
    // sweep below proves nothing -- which is exactly why the first version of
    // this test passed against the broken code.
    (void)pulp::view::render_to_png(*view, 1280, 800, 1.0f,
                                    pulp::view::ScreenshotBackend::skia);
    REQUIRE(m.is_open());

    // Sweep a pointer across the whole window, which is what a mouse does and
    // what nothing here had ever done. Under Guard Malloc this reads freed
    // memory if a hover handler restructures the tree.
    for (int pass = 0; pass < 3; ++pass) {
        for (float y = 0; y < 800; y += 7) {
            for (float x = 250; x < 700; x += 23)
                view->simulate_hover({x, y});
        }
    }

    // NOTE: the fault this guards is only caught under Guard Malloc --
    // DYLD_INSERT_LIBRARIES=/usr/lib/libgmalloc.dylib. Plain, the freed rows
    // usually still read as plausible memory and the sweep completes. Verified
    // by restoring the rebuild-in-hover: passes plain, FAILS under gmalloc.
    //
    // Still coherent afterwards: the list survived being hovered, which is the
    // whole claim.
    CHECK(m.is_open());
    CHECK(m.candidates().size() == 20);
    CHECK(m.selected_index() >= 0);
    CHECK(m.selected_index() < 20);
}

TEST_CASE("a picked mention is marked, and deletes as one thing",
          "[rack][mention]") {
    // Two things at once, both reported. The inserted module was a bare word
    // -- indistinguishable from anything else typed -- and backspace ate it a
    // letter at a time, leaving "@VC", a half-name the generator would take
    // literally.
    //
    // The field is PLAIN TEXT: it cannot bold or highlight a range. So the
    // marker is the only thing that can say "you chose this", and it is also
    // the only delimiter that makes the token deletable as a unit.
    HermeticProjects isolated;
    forge_modular::ForgeModularShell shell;
    pulp::state::StateStore store;
    shell.set_state_store(&store);
    shell.define_parameters(store);
    auto view = shell.create_view();
    REQUIRE(view != nullptr);
    auto* input = shell.chrome()->prompt_input();
    REQUIRE(input != nullptr);

    shell.mentions().set_source([](const std::string&) {
        forge_modular::MentionCandidate c;
        c.brand = "VCV"; c.name = "VCO"; c.slug = "VCO";
        c.state = forge_modular::MentionCandidate::Availability::ready;
        return std::vector<forge_modular::MentionCandidate>{c};
    });

    input->set_text("wire a ");
    shell.begin_mention();
    shell.mentions().choose(0);
    // Marked, so a chosen module does not read as a typed word.
    CHECK(input->text() == "wire a @VCO ");

    pulp::view::KeyEvent bs;
    bs.key = pulp::view::KeyCode::backspace;

    // One press takes the whole mention, not one letter of it. The space
    // before it stays, so the next word can just be typed.
    CHECK(shell.handle_prompt_key(bs));
    CHECK(input->text() == "wire a ");

    // And ordinary text is left entirely alone -- the field still behaves like
    // a field everywhere else, so a second press is the editor's business.
    CHECK_FALSE(shell.handle_prompt_key(bs));
    CHECK(input->text() == "wire a ");

    // An '@' that is not the start of a token is not a mention.
    input->set_text("mail me at a@b");
    CHECK_FALSE(shell.handle_prompt_key(bs));
    CHECK(input->text() == "mail me at a@b");

    // Nor is one with text after it -- only the trailing token is a mention.
    input->set_text("@VCO into the filter");
    CHECK_FALSE(shell.handle_prompt_key(bs));
    CHECK(input->text() == "@VCO into the filter");
}

TEST_CASE("a rack laid out in rows is drawn in rows", "[rack][layout]") {
    // Every module went end to end on one line regardless of where the patch
    // put it, so a two-row rack drew as one very long strip and shrank to
    // nothing. Rack stores `pos` as [HP across, row down] -- and only that:
    // RACK_GRID_HEIGHT is a constant, every module in the library is exactly
    // 380 tall, and Eurorack's 1U utility rows are a physical-case idea that
    // Rack cannot represent at all.
    auto mod = [](const char* id, int hp, int x, int y) {
        forge_modular::RackModule m;
        m.id = id;
        m.brand = "Test";
        m.name = id;
        m.hp = hp;
        m.grid_x = x;
        m.grid_y = y;
        m.has_grid_pos = true;
        return m;
    };

    const std::vector<forge_modular::RackModule> two_rows{
        mod("a", 8, 0, 0), mod("b", 8, 8, 0),
        mod("c", 8, 0, 1), mod("d", 8, 8, 1)};
    const auto l = forge_modular::layout_rack(two_rows, 1200, 800);
    REQUIRE(l.panels.size() == 4);
    CHECK(l.rows == 2);

    // The second row is BELOW the first, by exactly one panel height.
    CHECK(l.panel("c")->y == Catch::Approx(l.panel("a")->y + l.panel("a")->height));
    CHECK(l.panel("a")->y == Catch::Approx(l.panel("b")->y));      // same row
    CHECK(l.panel("c")->y == Catch::Approx(l.panel("d")->y));

    // And the rack is only as wide as one row, not all four butted together.
    CHECK(l.total_width == Catch::Approx(16 * forge_modular::kHorizontalPitch));

    // Rack saves ABSOLUTE grid coordinates around an offset of its own, so a
    // real patch has positions in the thousands. Laid out literally that puts
    // the rack off the side of the world; normalised, it is the same picture.
    const std::vector<forge_modular::RackModule> far_away{
        mod("a", 8, 2000, 100), mod("b", 8, 2008, 100),
        mod("c", 8, 2000, 101), mod("d", 8, 2008, 101)};
    const auto f = forge_modular::layout_rack(far_away, 1200, 800);
    REQUIRE(f.panels.size() == 4);
    CHECK(f.rows == 2);
    CHECK(f.panel("a")->x == Catch::Approx(l.panel("a")->x));
    CHECK(f.panel("d")->y == Catch::Approx(l.panel("d")->y));

    // A patch that never said where anything goes keeps the old behaviour:
    // end to end on one row, in order. It has no arrangement to lose.
    std::vector<forge_modular::RackModule> unplaced{
        mod("a", 8, 0, 0), mod("b", 4, 0, 0)};
    for (auto& m : unplaced) m.has_grid_pos = false;
    const auto u = forge_modular::layout_rack(unplaced, 1200, 800);
    REQUIRE(u.panels.size() == 2);
    CHECK(u.rows == 1);
    CHECK(u.panel("b")->x == Catch::Approx(u.panel("a")->x + u.panel("a")->width));

    // Taller racks scale DOWN to fit rather than running off the viewport --
    // the whole point of noticing there is more than one row.
    std::vector<forge_modular::RackModule> tall;
    for (int r = 0; r < 6; ++r) tall.push_back(mod("r", 8, 0, r));
    const auto t = forge_modular::layout_rack(tall, 1200, 800);
    CHECK(t.rows == 6);
    CHECK(t.scale < l.scale);
    CHECK(t.panels.back().y + t.panels.back().height <= 800.0f);
}

// ── The port map: what a scan is allowed to claim ────────────────────────────
//
// A map is merged, never rewritten, so an entry outlives the scanner that made
// it. That went wrong in the real map on this machine: fourteen of nineteen
// modules were carried forward from a scanner that recorded jacks only, so
// Fundamental's LFO had four inputs, four outputs and none of its knobs -- and
// reported no gap at all, because the only thing compared was the plugin
// version, which matched exactly. It drew as a faceplate with no controls and
// nothing said it was incomplete.
//
// The fixture is that map's two vintages side by side.
TEST_CASE("an entry from an older scanner is not passed off as measured",
          "[portmap]") {
    const std::string map = R"({
  "modules": [
    {
      "plugin": "Fundamental",
      "model": "LFO",
      "pluginVersion": "2.6.4",
      "size": [135.0, 380.0],
      "inputs": [
        {"index": 0, "name": "Frequency modulation", "x": 20.0, "y": 286.0}
      ],
      "outputs": [
        {"index": 0, "name": "Sine", "x": 20.0, "y": 330.0}
      ]
    },
    {
      "plugin": "Fundamental",
      "model": "VCO",
      "pluginVersion": "2.6.4",
      "scan": 3,
      "size": [135.0, 380.0],
      "params": [
        {"index": 0, "name": "Frequency", "x": 67.0, "y": 100.0,
         "w": 45.0, "h": 45.0}
      ],
      "inputs": [
        {"index": 0, "name": "Frequency modulation", "x": 20.0, "y": 286.0}
      ],
      "outputs": []
    }
  ]
})";

    const auto pm = forge_modular::PortMap::parse(map);
    REQUIRE(pm.size() == 2);

    // Both are present and both name the installed version.
    const auto* lfo = pm.find("Fundamental", "LFO");
    const auto* vco = pm.find("Fundamental", "VCO");
    REQUIRE(lfo != nullptr);
    REQUIRE(vco != nullptr);
    CHECK(lfo->plugin_version == "2.6.4");
    CHECK(vco->plugin_version == "2.6.4");

    // The difference is what was RECORDED, and it is visible in the data
    // before any judgement is made about it.
    CHECK(lfo->params.empty());
    CHECK(vco->params.size() == 1);
    CHECK(lfo->scan_version == 1);          // no field: the oldest scanner
    CHECK(vco->scan_version == forge_modular::PortMap::kScanVersion);

    // And the judgement. This is the assertion the missing test would have
    // made: a matching plugin version is not enough to call an entry current.
    CHECK(pm.gap_for("Fundamental", "LFO", "2.6.4") ==
          forge_modular::PortMap::Gap::stale);
    CHECK(pm.gap_for("Fundamental", "VCO", "2.6.4") ==
          forge_modular::PortMap::Gap::none);

    // The two gaps it already distinguished still work: never scanned, and
    // scanned against a plugin that has since been updated.
    CHECK(pm.gap_for("Fundamental", "VCF", "2.6.4") ==
          forge_modular::PortMap::Gap::unmeasured);
    CHECK(pm.gap_for("Fundamental", "VCO", "2.7.0") ==
          forge_modular::PortMap::Gap::stale);

    // A jack keeps the name its author gave it, which is what the explanation
    // text is built from.
    REQUIRE(lfo->inputs.size() == 1);
    CHECK(lfo->inputs[0].name == "Frequency modulation");

    // An entry that predates the scan field but DOES carry controls is a
    // third case, and it is the common one: most of the map was written by a
    // scanner that recorded params and no lights. gap_for calls it stale,
    // correctly -- there is more to measure. What matters for drawing is that
    // its controls are known, so it must not be treated as faceless.
    //
    // Core's AudioInterface is that entry, and it wore the badge on a panel
    // with a visible knob until the badge stopped asking gap_for.
    const std::string with_params = R"({
  "modules": [
    {
      "plugin": "Core",
      "model": "AudioInterface2",
      "pluginVersion": "2.6.4",
      "size": [150.0, 380.0],
      "params": [
        {"index": 0, "name": "Level", "x": 75.0, "y": 120.0, "w": 30.0, "h": 30.0}
      ],
      "inputs": [],
      "outputs": []
    }
  ]
})";
    const auto older = forge_modular::PortMap::parse(with_params);
    const auto* ai = older.find("Core", "AudioInterface2");
    REQUIRE(ai != nullptr);
    CHECK(ai->params.size() == 1);
    CHECK(older.gap_for("Core", "AudioInterface2", "2.6.4") ==
          forge_modular::PortMap::Gap::stale);

    // And the fourth case, which "has no params" alone gets backwards: a
    // module that genuinely HAS no controls. Fundamental's Merge is sixteen
    // jacks and not one knob, and saying we could not measure its controls is
    // as wrong as drawing controls it does not have.
    //
    // The scan version is what separates them. Below the first scanner that
    // recorded controls, "no params" means nothing looked; at or above it,
    // "no params" is a measurement.
    const std::string knobless = R"({
  "modules": [
    {
      "plugin": "Fundamental",
      "model": "Merge",
      "pluginVersion": "2.6.4",
      "scan": 2,
      "size": [45.0, 380.0],
      "params": [],
      "inputs": [{"index": 0, "name": "Channel 1", "x": 22.0, "y": 100.0}],
      "outputs": [{"index": 0, "name": "Polyphonic", "x": 22.0, "y": 330.0}]
    }
  ]
})";
    const auto merged = forge_modular::PortMap::parse(knobless);
    const auto* mg = merged.find("Fundamental", "Merge");
    REQUIRE(mg != nullptr);
    CHECK(mg->params.empty());
    CHECK(mg->scan_version >= forge_modular::PortMap::kScanVersionWithParams);
}

// The UNMAPPED mark is drawn, and only where it belongs.
//
// Asserting on the layout flag alone would have passed while nothing reached
// the screen -- the flag existed, was threaded through, and no paint code read
// it. So this renders and compares pixels: the same two-module rack, once with
// both modules measured and once with one of them not, must differ, and the
// difference must sit in the lower part of the unmapped panel where the mark
// goes.
TEST_CASE("a module drawn without its controls is marked as unmapped",
          "[portmap][preview]") {
    auto rack_of = [](bool second_measured) {
        std::vector<forge_modular::RackModule> mods;
        for (int i = 0; i < 2; ++i) {
            forge_modular::RackModule m;
            m.id = i == 0 ? "a" : "b";
            m.brand = "Fundamental";
            m.name = i == 0 ? "VCO" : "LFO";
            m.hp = 9;
            m.placed = true;
            m.has_artwork = false;
            m.available = true;
            m.controls_measured = (i == 0) ? true : second_measured;
            mods.push_back(std::move(m));
        }
        return mods;
    };

    auto render = [&](bool second_measured, const char* tag) {
        auto preview = std::make_shared<forge_modular::RackPreview>();
        preview->set_bounds(pulp::view::Rect{0, 0, 640, 400});
        preview->set_rack(rack_of(second_measured), {});
        const auto path = std::filesystem::temp_directory_path() /
                          (std::string("unmapped-") + tag + ".png");
        REQUIRE(pulp::view::render_to_file(*preview, 640, 400, path.string(),
                                           1.0f,
                                           pulp::view::ScreenshotBackend::skia));
        auto bytes = read_all(path);
        // A blank frame would match another blank frame and prove nothing.
        REQUIRE(bytes.size() > 2000);
        return path;
    };

    const auto clean = render(true, "clean");
    const auto marked = render(false, "marked");

    const auto a = decode_rgba(read_all(clean));
    const auto b = decode_rgba(read_all(marked));
    REQUIRE(a.width == b.width);
    REQUIRE(a.height == b.height);
    REQUIRE(a.width > 0);

    // Where do they differ? Nowhere, if the flag never reached the paint.
    int differing = 0;
    int min_y = a.height, max_y = -1, min_x = a.width, max_x = -1;
    for (int y = 0; y < a.height; ++y) {
        for (int x = 0; x < a.width; ++x) {
            const std::size_t at = (static_cast<std::size_t>(y) * a.width + x) * 4;
            if (a.pixels[at] != b.pixels[at] ||
                a.pixels[at + 1] != b.pixels[at + 1] ||
                a.pixels[at + 2] != b.pixels[at + 2]) {
                ++differing;
                min_y = std::min(min_y, y); max_y = std::max(max_y, y);
                min_x = std::min(min_x, x); max_x = std::max(max_x, x);
            }
        }
    }
    INFO("differing pixels: " << differing
         << "  x " << min_x << ".." << max_x
         << "  y " << min_y << ".." << max_y
         << "  frame " << a.width << "x" << a.height);

    // Enough pixels to be a legible strip and a word, not a stray line.
    CHECK(differing > 200);

    // The mark belongs to the SECOND panel, in its lower portion. If the flag
    // were read for the wrong module, or the mark drawn over the whole rack,
    // this is what catches it.
    CHECK(min_x > a.width / 4);
    CHECK(min_y > a.height / 2);
}

// Our own modules are never "unmapped", whatever the port map holds.
//
// The port map exists for modules we did not make. Ours are drawn from the
// manifest their panel was emitted from, so they are always fully known and
// never scanned -- which means find() returns nothing for them, and treating
// "no entry" as "not measured" put the badge on every module we drew
// ourselves, with all its knobs visible underneath it.
//
// The fixture that missed it used one brand for both modules. This loads the
// real patches instead, so the question is asked of the data that ships.
TEST_CASE("a module with no knobs is not called unmapped", "[portmap][loader]") {
    // params.empty() alone said "we could not measure this module's controls"
    // for every module that HAS no controls -- Merge, Split, a mult. The badge
    // then appears on a panel whose emptiness is the truth about it.
    using forge_modular::PortMap;
    forge_modular::MappedModule measured;      // a scanner that looks for params
    measured.scan_version = PortMap::kScanVersionWithParams;
    forge_modular::MappedModule unmeasured;    // one that never did
    unmeasured.scan_version = 1;
    forge_modular::MappedModule knobbed;
    knobbed.scan_version = 1;
    knobbed.params.push_back({0, "Level", 10.0f, 20.0f, 5.0f, 5.0f});

    // Identical params -- both empty -- and opposite answers. Asked of the
    // rule itself, so removing it from the rule fails here rather than
    // leaving every assertion green while the badge goes back on Merge.
    CHECK(measured.params.empty());
    CHECK(unmeasured.params.empty());
    CHECK(PortMap::controls_known(measured));
    CHECK_FALSE(PortMap::controls_known(unmeasured));
    // And an old entry that DID record controls is still known.
    CHECK(PortMap::controls_known(knobbed));
}

TEST_CASE("a module we made is never marked unmapped", "[portmap][loader]") {
    // The patch that travels with the tests, so this runs in a rebuilt
    // worktree too -- the repo's examples/ directory is not there. The first
    // version of this looked for it and skipped when it was missing, which
    // Catch2 reports as a passing test case with no assertions: the shape
    // that reports success for having checked nothing.
    const auto patch_path = baseline_dir().parent_path() / "app-generated-patch.vcv";
    REQUIRE(std::filesystem::exists(patch_path));

    const auto patch = forge_modular::load_patch(patch_path.string());
    int ours = 0;
    for (const auto& m : patch.modules) {
        if (m.brand != "ForgeModular") continue;
        ++ours;
        INFO("module " << m.name);
        CHECK(m.controls_measured);
    }
    // A patch that loaded nothing would satisfy every CHECK above.
    REQUIRE(ours > 0);
}

// Arrows reach the mention list from the PROMPT, with nothing else clicked.
//
// Reported from the app: the list opens, and up/down do nothing until you
// click a row first — after which they work, because the row has focus and
// handles its own keys. So the global hook either is not installed or is not
// the one the window calls.
//
// This drives the hook the window drives: root->on_global_key.
TEST_CASE("arrows move the mention list without clicking into it",
          "[mention][keys]") {
    forge_modular::ForgeModularShell shell;
    pulp::state::StateStore store;
    shell.set_state_store(&store);
    shell.define_parameters(store);
    auto view = shell.create_view();
    REQUIRE(view != nullptr);

    // The hook has to be on the FIELD. The window host dispatches to its own
    // root, which for the standalone is an outer chrome the shell never sees,
    // so a hook on the shell's view is never reached.
    // Polled BEFORE the wrap, the way the app does it: the editor's tree
    // exists and is built, and the chrome inserts it into the window root
    // afterwards. A hook installed here lands on the inner root, and if it
    // latches it stays there — the window never calls it and the arrows do
    // nothing, which is what was reported after the first attempt at this.
    shell.on_poll();

    // WRAP the shell's view, the way the standalone does.
    //
    // Without this the shell's own view IS the root, a hook on it is reached,
    // and the test cannot tell the broken arrangement from the working one —
    // which is exactly why this shipped. The app puts an outer chrome around
    // the editor, the host dispatches to THAT, and a hook on the inner view
    // never fires.
    auto outer = std::make_unique<pulp::view::View>();
    auto* inner = view.get();
    outer->add_child(std::move(view));
    REQUIRE(inner->parent() == outer.get());

    // The hook goes on the root the WINDOW dispatches to, found by walking up
    // from the field.
    shell.on_poll();
    auto* input = shell.chrome() ? shell.chrome()->prompt_input() : nullptr;
    REQUIRE(input != nullptr);
    pulp::view::View* top = input;
    while (top->parent()) top = top->parent();
    REQUIRE(top->on_global_key != nullptr);

    shell.mentions().set_source([](const std::string& q) {
        return forge_modular::search_modules(q, 40);
    });
    shell.mentions().handle_text("@", 1);
    REQUIRE(shell.mentions().is_open());
    const int first = shell.mentions().selected_index();

    pulp::view::KeyEvent down;
    down.key = pulp::view::KeyCode::down;
    down.is_down = true;
    // Through the FIELD's own key path, which is what a focused field runs.
    CHECK(top->on_global_key(down));
    INFO("selected " << first << " -> " << shell.mentions().selected_index());
    CHECK(shell.mentions().selected_index() != first);

    pulp::view::KeyEvent up;
    up.key = pulp::view::KeyCode::up;
    up.is_down = true;
    CHECK(top->on_global_key(up));
    CHECK(shell.mentions().selected_index() == first);

    // Tab completes the highlighted row rather than moving focus away.
    pulp::view::KeyEvent tab;
    tab.key = pulp::view::KeyCode::tab;
    tab.is_down = true;
    CHECK(top->on_global_key(tab));
    CHECK_FALSE(shell.mentions().is_open());
}

// Picking a module you do not have says why, instead of doing nothing.
//
// Reported from the app: "i cannot select things with GET next to them,
// unclear if that's a premium money thing". It refused correctly — a patch
// wired to a module nobody has cannot sound — and refused in silence, which
// is indistinguishable from a list that does not work.
TEST_CASE("an uninsalled pick inserts AND explains itself",
          "[mention][refusal]") {
    forge_modular::MentionOverlay overlay;
    std::string said;
    forge_modular::MentionCandidate refused;
    overlay.on_refused = [&](const forge_modular::MentionCandidate& c) {
        refused = c;
        said = c.brand + " " + c.name;
    };
    bool inserted = false;
    overlay.on_choose = [&](const std::string&) { inserted = true; };

    overlay.set_source([](const std::string&) {
        std::vector<forge_modular::MentionCandidate> out;
        forge_modular::MentionCandidate free_one;
        free_one.brand = "Fehler Fabrik";
        free_one.name = "PSI OP";
        free_one.slug = "FehlerFabrik/PSIOP";
        free_one.state = forge_modular::MentionCandidate::Availability::available;
        out.push_back(free_one);
        return out;
    });
    overlay.handle_text("@psi", 4);
    REQUIRE(overlay.is_open());

    pulp::view::KeyEvent enter;
    enter.key = pulp::view::KeyCode::enter;
    enter.is_down = true;
    CHECK(overlay.handle_key_event(enter));

    // It goes in AND it is announced. Refusing the insert was the bug.
    CHECK(inserted);
    INFO("said: " << said);
    CHECK(said == "Fehler Fabrik PSI OP");
    CHECK(refused.state ==
          forge_modular::MentionCandidate::Availability::available);
}

// The audio interface gets its jacks, like any other vendor module.
//
// Reported from the app: cables into AUDIO end at the panel edge and the
// module shows no jacks, while every other module has them. The port map DOES
// carry it — Core/AudioInterface2, two inputs at y=286, two outputs at y=334 —
// so if the drawing docks at the edge, the loader did not find the entry.
TEST_CASE("the audio interface is placed like any other module",
          "[portmap][audio]") {
    const std::string map = R"({
  "modules": [
    {
      "plugin": "Core",
      "model": "AudioInterface2",
      "pluginVersion": "2.6.4",
      "scan": 3,
      "size": [75.0, 380.0],
      "params": [{"index": 0, "name": "Level", "x": 37.0, "y": 120.0,
                  "w": 30.0, "h": 30.0}],
      "inputs": [
        {"index": 0, "name": "To \"device output 1\"", "x": 21.5, "y": 286.0},
        {"index": 1, "name": "To \"device output 2\"", "x": 53.5, "y": 286.0}
      ],
      "outputs": [
        {"index": 0, "name": "From \"device input 1\"", "x": 21.5, "y": 334.0}
      ]
    }
  ]
})";
    const auto pm = forge_modular::PortMap::parse(map);
    const auto* ai = pm.find("Core", "AudioInterface2");
    REQUIRE(ai != nullptr);
    CHECK(ai->inputs.size() == 2);
    CHECK(ai->width == 75.0f);
    // Whatever else is true, an entry with jacks must not read as unmeasured.
    CHECK(forge_modular::PortMap::controls_known(*ai));

    // And through the real loader, on a real patch: the audio interface must
    // come back PLACED with jacks, or its cables dock at the panel edge and it
    // draws as a face with no holes — which is how it was reported.
    const auto patch_path = baseline_dir().parent_path() / "app-generated-patch.vcv";
    REQUIRE(std::filesystem::exists(patch_path));
    const auto loaded = forge_modular::load_patch(patch_path.string());
    const forge_modular::RackModule* audio = nullptr;
    for (const auto& m : loaded.modules)
        if (m.brand == "Core" && m.name.rfind("AudioInterface", 0) == 0)
            audio = &m;
    REQUIRE(audio != nullptr);
    INFO("placed=" << audio->placed << " ports=" << audio->ports.size()
                   << " hp=" << audio->hp);
    CHECK(audio->placed);
    CHECK(audio->ports.size() >= 2);
}

// Hovering a cable names both ends and outlines both modules.
//
// Dimming the other cables says WHICH wire you are on and nothing about where
// it goes — the ends are jacks among other jacks, and the panel labels are two
// points tall. The prototype named both ends and outlined the modules; this
// draws that, and proves it by rendering with and without a hover.
TEST_CASE("a hovered cable names what it joins", "[rack][hover][render]") {
    auto preview = std::make_shared<forge_modular::RackPreview>();
    preview->set_bounds(pulp::view::Rect{0, 0, 900, 460});

    std::vector<forge_modular::RackModule> mods;
    for (const char* n : {"LFO", "MULT", "VCA"}) {
        forge_modular::RackModule m;
        m.id = n; m.brand = "ForgeModular"; m.name = n; m.hp = 8;
        m.placed = true;
        m.ports.push_back({"out0", "SQR", 0.5f, 300.0f, false});
        m.ports.push_back({"in0", "IN", 0.5f, 120.0f, true});
        mods.push_back(std::move(m));
    }
    std::vector<forge_modular::Connection> cables{
        {"LFO", "out0", "MULT", "in0", forge_modular::SignalRole::clock, ""},
        {"MULT", "out0", "VCA", "in0", forge_modular::SignalRole::audio, ""},
    };
    preview->set_rack(mods, cables);

    auto shot = [&](const char* tag) {
        const auto path = std::filesystem::temp_directory_path() /
                          (std::string("hover-") + tag + ".png");
        REQUIRE(pulp::view::render_to_file(*preview, 900, 460, path.string(),
                                           1.0f,
                                           pulp::view::ScreenshotBackend::skia));
        auto bytes = read_all(path);
        REQUIRE(bytes.size() > 2000);
        return decode_rgba(bytes);
    };

    const auto plain = shot("plain");
    preview->set_highlight(0);
    const auto hovered = shot("hovered");
    REQUIRE(plain.width == hovered.width);

    int differing = 0;
    for (std::size_t at = 0; at + 3 < plain.pixels.size(); at += 4)
        if (plain.pixels[at] != hovered.pixels[at] ||
            plain.pixels[at + 1] != hovered.pixels[at + 1] ||
            plain.pixels[at + 2] != hovered.pixels[at + 2])
            ++differing;
    INFO("differing pixels: " << differing);
    // A dim pass alone changes a few hundred; a label and two outlines are
    // thousands. Enough to tell "something was drawn" from "the cables faded".
    CHECK(differing > 3000);
}

// The audio interface DRAWS its jacks, not just carries them.
//
// The map having coordinates and the panel showing holes are two claims. The
// second is the one that was reported wrong, and the first is what I checked
// when I said it was fixed.
TEST_CASE("the audio interface draws where its cables land", "[audio][render]") {
    auto preview = std::make_shared<forge_modular::RackPreview>();
    preview->set_bounds(pulp::view::Rect{0, 0, 700, 460});

    forge_modular::RackModule vca;
    vca.id = "vca"; vca.brand = "ForgeModular"; vca.name = "VCA"; vca.hp = 3;
    vca.placed = true;
    vca.ports.push_back({"out0", "OUT", 0.5f, 300.0f, false});

    // As patch_loader builds it from the port map: real jack coordinates.
    forge_modular::RackModule audio;
    audio.id = "audio"; audio.brand = "Core"; audio.name = "AudioInterface2";
    audio.hp = 5; audio.placed = true;
    audio.ports.push_back({"in0", "To \"device output 1\"", 21.5f / 75.0f, 286.0f, true});
    audio.ports.push_back({"in1", "To \"device output 2\"", 53.5f / 75.0f, 286.0f, true});

    std::vector<forge_modular::RackModule> mods{vca, audio};
    std::vector<forge_modular::Connection> cables{
        {"vca", "out0", "audio", "in0", forge_modular::SignalRole::audio, ""},
        {"vca", "out0", "audio", "in1", forge_modular::SignalRole::audio, ""},
    };
    preview->set_rack(mods, cables);

    const auto path = std::filesystem::temp_directory_path() / "audio-jacks.png";
    REQUIRE(pulp::view::render_to_file(*preview, 700, 460, path.string(), 1.0f,
                                       pulp::view::ScreenshotBackend::skia));

    // A docked cable draws a collar at the panel EDGE; a landed one draws a
    // ring on the face. So the question "did it land" is answerable in pixels:
    // the same rack with the jack coordinates removed must look different.
    const auto with_jacks = decode_rgba(read_all(path));
    REQUIRE(with_jacks.width > 0);

    auto stripped = mods;
    stripped[1].ports.clear();
    stripped[1].placed = false;          // what a machine with no port map has
    preview->set_rack(stripped, cables);
    const auto blind_path =
        std::filesystem::temp_directory_path() / "audio-nojacks.png";
    REQUIRE(pulp::view::render_to_file(*preview, 700, 460, blind_path.string(),
                                       1.0f,
                                       pulp::view::ScreenshotBackend::skia));
    const auto without = decode_rgba(read_all(blind_path));
    REQUIRE(without.width == with_jacks.width);

    int differing = 0;
    for (std::size_t at = 0; at + 3 < without.pixels.size(); at += 4)
        if (without.pixels[at] != with_jacks.pixels[at] ||
            without.pixels[at + 1] != with_jacks.pixels[at + 1] ||
            without.pixels[at + 2] != with_jacks.pixels[at + 2])
            ++differing;
    INFO("differing pixels: " << differing);
    // Two cables rerouted from the face to the rim is a large change. If this
    // is small, the map made no difference to the drawing and copying it to
    // another machine fixes nothing.
    CHECK(differing > 2000);
}

// What you typed ranks first, and a match you cannot see says what it was.
//
// Reported from a screenshot: "@br" returned Macro Oscillator, Bernoulli
// Gate, Breakout, Calibrator, PSI OP, Planck. Every one is a real match — the
// first two are Braids and Branches by slug, Calibrator has "br" in the middle
// — but the one anybody typing "br" wants is Breakout, and it was fourth.
TEST_CASE("the mention list ranks what you typed first", "[mention][rank]") {
    const auto hits = forge_modular::search_modules("br", 40);
    if (hits.empty()) {
        WARN("no library index on this machine — skipped, not passed");
        return;
    }

    // Among matches of the same installed-ness, a name STARTING with the
    // query comes before one that merely contains it somewhere.
    auto rank_of = [&](const std::string& name) {
        for (std::size_t i = 0; i < hits.size(); ++i)
            if (hits[i].name == name) return static_cast<int>(i);
        return -1;
    };
    const int breakout = rank_of("Breakout");
    const int calibrator = rank_of("Calibrator");
    // REQUIRED, not guarded. Written as `if (both >= 0)`, the comparison
    // silently stopped happening the moment either dropped out of the
    // results — a guarded assertion is one that reports success for having
    // skipped itself, which is the fault this whole test is about.
    REQUIRE(breakout >= 0);
    REQUIRE(calibrator >= 0);
    INFO("Breakout at " << breakout << ", Calibrator at " << calibrator);
    CHECK(breakout < calibrator);

    // A row matched by its slug says which name matched, or it reads as a
    // random result. Audible Instruments' Macro Oscillator is Braids.
    bool saw_alias_row = false;
    for (const auto& h : hits) {
        if (h.name != "Macro Oscillator") continue;
        saw_alias_row = true;
        INFO("alias: '" << h.alias << "'");
        CHECK_FALSE(h.alias.empty());
    }
    // And the row was actually there. Without this the loop asserts nothing
    // whenever Macro Oscillator falls out of the results, and passes for it.
    CHECK(saw_alias_row);

    // A long name is cut rather than allowed to run under its badge. Ohmer's
    // 'BRK ("Break") expander for RKD' is thirty characters and overlapped
    // GET · FREE; a Label sizes to its text and flex_shrink cannot shorten a
    // string, so the string is what has to give.
    {
        auto view = std::make_unique<forge_modular::MentionOverlay>();
        // The row builder is internal, so this checks the rule it applies.
        const std::string long_name = R"(BRK ("Break") expander for RKD)";
        CHECK(long_name.size() == 30);
        CHECK(forge_modular::elide_for_row(long_name).size() < long_name.size());
        CHECK(forge_modular::elide_for_row("Breakout") == "Breakout");
    }

    // And the ordering is stable: the same query twice gives the same list,
    // or the row under the pointer changes between keystrokes.
    const auto again = forge_modular::search_modules("br", 40);
    REQUIRE(again.size() == hits.size());
    for (std::size_t i = 0; i < hits.size(); ++i)
        CHECK(again[i].slug == hits[i].slug);
}

// A picture of the mention list, for a person to judge. Not part of the suite.
//   forge-test-chrome-no-leak "[.mention-look]"
TEST_CASE("render the mention list for a look", "[.mention-look]") {
    forge_modular::MentionOverlay overlay;
    overlay.set_source([](const std::string& q) {
        return forge_modular::search_modules(q, 8);
    });
    // build() FIRST: the rows are made into the view it returns, so filling
    // the list before there is anywhere to put them leaves an empty frame.
    auto view = overlay.build();
    REQUIRE(view != nullptr);
    overlay.handle_text("@br", 3);
    REQUIRE(overlay.is_open());
    // Wired the way the shell wires it, or the picture shows an empty panel
    // and proves only that the test forgot the callback.
    overlay.on_refused = [&](const forge_modular::MentionCandidate& c) {
        overlay.show_notice(
            c.brand + " " + c.name +
            " is not installed yet — get it free in Rack's Library, then "
            "rescan. The prompt can still name it.");
    };
    // Pick the top row, which is not installed, so the picture shows the
    // notice that has to survive the list closing.
    overlay.handle_key(36);
    REQUIRE_FALSE(overlay.notice().empty());
    // Tall enough to CONTAIN the list. It places itself under the composer,
    // ~448 points down, so a 300-point frame renders it off the bottom — and
    // a blank picture passes every assertion here just as well as a good one.
    // Inside a PARENT. The overlay places itself absolutely, which resolves
    // against a containing view — rendered as a root there is nothing to
    // place it against and the frame comes back empty.
    auto host = std::make_unique<pulp::view::View>();
    host->set_bounds(pulp::view::Rect{0, 0, 900, 760});
    host->add_child(std::move(view));
    host->layout_children();
    const auto out = std::filesystem::temp_directory_path() / "mention-list.png";
    REQUIRE(pulp::view::render_to_file(*host, 900, 760, out.string(), 2.0f,
                                       pulp::view::ScreenshotBackend::skia));
    // A blank frame is not a picture of anything.
    REQUIRE(read_all(out).size() > 8000);
    WARN("wrote " << out.string());
}

// Typing @, arrowing, and pressing Enter puts the module in the field.
//
// The whole point of the list, and the path it takes changed tonight: keys now
// arrive through the root the window dispatches to rather than a hook on the
// shell's own view. Every piece was tested separately — the keys move the
// selection, the choice fires on_choose, the composer inserts — and none of
// that says the three of them are joined up.
TEST_CASE("choosing from the list writes the module into the prompt",
          "[mention][insert]") {
    forge_modular::ForgeModularShell shell;
    pulp::state::StateStore store;
    shell.set_state_store(&store);
    shell.define_parameters(store);
    auto view = shell.create_view();
    REQUIRE(view != nullptr);

    // Wrapped, as the standalone wraps it.
    auto outer = std::make_unique<pulp::view::View>();
    auto* inner = view.get();
    outer->add_child(std::move(view));
    REQUIRE(inner->parent() == outer.get());
    shell.on_poll();

    auto* input = shell.chrome() ? shell.chrome()->prompt_input() : nullptr;
    REQUIRE(input != nullptr);
    pulp::view::View* top = input;
    while (top->parent()) top = top->parent();
    REQUIRE(top->on_global_key != nullptr);

    // Only installed modules can be inserted, so drive the list with one.
    shell.mentions().set_source([](const std::string&) {
        std::vector<forge_modular::MentionCandidate> out;
        forge_modular::MentionCandidate c;
        c.brand = "ForgeModular";
        c.name = "VCO";
        c.slug = "ForgeModular/VCO";
        c.state = forge_modular::MentionCandidate::Availability::ready;
        out.push_back(c);
        return out;
    });

    input->set_text("@vc");
    shell.mentions().handle_text("@vc", 3);
    REQUIRE(shell.mentions().is_open());

    pulp::view::KeyEvent enter;
    enter.key = pulp::view::KeyCode::enter;
    enter.is_down = true;
    CHECK(top->on_global_key(enter));

    // The list closes and the field carries the module.
    CHECK_FALSE(shell.mentions().is_open());
    INFO("field now: '" << input->text() << "'");
    CHECK(input->text().find("VCO") != std::string::npos);
    // And the half-typed token is gone rather than left in front of it.
    CHECK(input->text().find("@vc ") == std::string::npos);
}

// The parser reads what CARTOG actually WRITES.
//
// Every other port-map test here uses a hand-written fixture, and a fixture
// agrees with the parser by construction — I wrote both. The real join is
// CARTOG building JSON by string concatenation inside Rack, and PortMap
// parsing it with choc::json. A renamed field on either side breaks silently:
// the parse succeeds, the map comes back empty, and every vendor module draws
// without jacks, which looks exactly like never having scanned.
TEST_CASE("the port map parser reads a real scan", "[portmap][join]") {
    const char* home = std::getenv("HOME");
    const std::filesystem::path real =
        std::string(home ? home : ".") +
        "/Library/Application Support/Rack2/forge-portmap.json";
    if (!std::filesystem::exists(real)) {
        WARN("no port map on this machine — skipped, not passed. Press SCAN "
             "in CARTOG to make one.");
        return;
    }

    std::ifstream f(real);
    std::stringstream ss;
    ss << f.rdbuf();
    const auto text = ss.str();
    REQUIRE(text.size() > 200);

    const auto pm = forge_modular::PortMap::parse(text);
    INFO("file is " << text.size() << " bytes; parsed " << pm.size()
                    << " module(s)");
    // A file with modules in it must yield modules. Parsing to zero is the
    // silent failure this exists to catch.
    REQUIRE(pm.size() > 0);

    // And the fields the drawing depends on have to survive the round trip:
    // a module with jacks, and a jack with coordinates that are not the
    // origin. Counting modules alone would pass on entries full of zeroes.
    bool any_jack = false;
    for (const auto& name : {"AudioInterface2", "VCO", "LFO", "VCA"}) {
        for (const auto& plugin : {"Core", "Fundamental"}) {
            const auto* m = pm.find(plugin, name);
            if (!m) continue;
            for (const auto& in : m->inputs)
                if (in.x != 0.0f || in.y != 0.0f) any_jack = true;
            for (const auto& out : m->outputs)
                if (out.x != 0.0f || out.y != 0.0f) any_jack = true;
        }
    }
    CHECK(any_jack);
}

// The explanations the generator wrote reach the ones the app shows.
//
// patch.py writes a .why.json beside every patch; patch_loader reads it and
// hangs the text on each cable. That sidecar is the whole "a patch explains
// itself" promise, and the two halves are a Python writer and a C++ reader
// with nothing between them — a renamed key parses clean and yields a patch
// whose cables all have empty reasons, which reads as a model that said
// nothing rather than a reader that dropped it.
//
// So this uses a sidecar the generator actually produced, not a fixture.
TEST_CASE("a generated patch arrives with its reasons attached",
          "[portmap][join][why]") {
    const char* home = std::getenv("HOME");
    const std::filesystem::path dir =
        std::string(home ? home : ".") +
        "/Library/Application Support/Forge Modular/examples/forge-modular/patches";
    if (!std::filesystem::exists(dir)) {
        WARN("no generated patches on this machine — skipped, not passed");
        return;
    }

    // The newest patch that has a sidecar beside it.
    std::filesystem::path newest;
    std::filesystem::file_time_type best{};
    std::error_code ec;
    for (const auto& e : std::filesystem::directory_iterator(dir, ec)) {
        if (e.path().extension() != ".vcv") continue;
        const auto side = e.path().string().substr(
                              0, e.path().string().size() - 4) + ".why.json";
        if (!std::filesystem::exists(side)) continue;
        const auto when = std::filesystem::last_write_time(e, ec);
        if (newest.empty() || when > best) { newest = e.path(); best = when; }
    }
    if (newest.empty()) {
        WARN("no patch with a sidecar — skipped, not passed");
        return;
    }
    INFO("patch: " << newest.filename().string());

    const auto loaded = forge_modular::load_patch(newest.string());
    REQUIRE_FALSE(loaded.connections.empty());

    // At least one cable must carry a reason. Zero means the reader took
    // nothing from a file the writer filled — the silent half of this join.
    std::size_t with_why = 0;
    for (const auto& c : loaded.connections)
        if (!c.why.empty()) ++with_why;
    INFO(with_why << " of " << loaded.connections.size()
                  << " cables carry a reason");
    CHECK(with_why > 0);
}

// The monitor classifies REAL generator output, not just fixtures.
//
// BuildMonitor::classify decides whether a log line is progress, a refusal, an
// error or the success that ends a build, and every test for it here feeds it
// strings somebody typed. A generator that rewords a line leaves those passing
// while the app misreads the real thing — showing a finished build as still
// running, or a refusal as an error.
//
// prove_idioms.sh leaves real transcripts behind, so this uses those.
TEST_CASE("the build monitor reads a real generator log",
          "[buildlog][join]") {
    // A recorded run that TRAVELS with the tests. Reaching for the live logs
    // under tools/ resolved inside the Forge worktree, found nothing, and
    // skipped — reported by Catch2 as a passing test case.
    const auto log = baseline_dir().parent_path() / "real-generator-run.log";
    REQUIRE(std::filesystem::exists(log));

    std::size_t lines = 0, success = 0, errors = 0, refusals = 0;
    {
        std::ifstream f(log);
        std::string line;
        while (std::getline(f, line)) {
            ++lines;
            switch (forge_modular::BuildMonitor::classify(line)) {
                case forge_modular::BuildLine::Kind::success:  ++success;  break;
                case forge_modular::BuildLine::Kind::error:    ++errors;   break;
                case forge_modular::BuildLine::Kind::refusal:  ++refusals; break;
                default: break;
            }
        }
    }
    INFO(lines << " real log lines: " << success << " success, " << errors
               << " error, " << refusals << " refusal");
    REQUIRE(lines > 10);

    // A run of twelve prompts that all held ends in success lines. Zero means
    // the monitor cannot see the end of a build in the generator's own words,
    // and the app would show a finished run as still going.
    CHECK(success > 0);

    // And a log full of successful builds must not read as full of errors —
    // the app paints an error state and stops watching.
    CHECK(errors < lines / 4);
}

// A vendor slider is not drawn as a knob, and an unclassified one still draws.
//
// The manifest path already refuses to guess: it maps four knob kinds to
// diameters and skips the rest, because "a switch or a slider drawn as a knob
// is wrong in a way a reader cannot see". The port-map path did the opposite —
// every measured control became a knob sized min(w,h), so a fader came out a
// small dial. CARTOG measures the kind; nothing read it.
//
// The second half matters as much: every map written before CARTOG classified
// controls carries no kind at all, and refusing those would empty panels that
// draw correctly today.
TEST_CASE("a measured slider is not drawn as a knob", "[portmap][kind]") {
    const std::string map = R"({
  "modules": [
    {
      "plugin": "Vendor",
      "model": "Mixer",
      "pluginVersion": "1.0",
      "scan": 3,
      "size": [150.0, 380.0],
      "params": [
        {"index": 0, "name": "Level", "x": 30.0, "y": 120.0,
         "w": 18.0, "h": 18.0, "kind": "knob"},
        {"index": 1, "name": "Fader", "x": 70.0, "y": 200.0,
         "w": 10.0, "h": 90.0, "kind": "slider"},
        {"index": 2, "name": "Mute", "x": 110.0, "y": 300.0,
         "w": 14.0, "h": 14.0, "kind": "button"}
      ],
      "inputs": [], "outputs": []
    },
    {
      "plugin": "Vendor",
      "model": "Old",
      "pluginVersion": "1.0",
      "size": [150.0, 380.0],
      "params": [
        {"index": 0, "name": "Something", "x": 30.0, "y": 120.0,
         "w": 18.0, "h": 18.0}
      ],
      "inputs": [], "outputs": []
    }
  ]
})";
    const auto pm = forge_modular::PortMap::parse(map);
    const auto* mixer = pm.find("Vendor", "Mixer");
    REQUIRE(mixer != nullptr);
    REQUIRE(mixer->params.size() == 3);
    CHECK(mixer->params[0].kind == "knob");
    CHECK(mixer->params[1].kind == "slider");
    CHECK(mixer->params[2].kind == "button");

    // An entry from before classification says nothing rather than "knob".
    const auto* old_entry = pm.find("Vendor", "Old");
    REQUIRE(old_entry != nullptr);
    REQUIRE(old_entry->params.size() == 1);
    CHECK(old_entry->params[0].kind.empty());

    // And the DRAWING decision, which is the thing that was wrong. Asserting
    // the parsed kind alone would pass with every control still drawn as a
    // circle — the rule lives in the caller, and that is where it has to be
    // asked.
    using forge_modular::PortMap;
    CHECK(PortMap::draws_as_knob(mixer->params[0]));        // knob
    CHECK_FALSE(PortMap::draws_as_knob(mixer->params[1]));  // slider
    CHECK_FALSE(PortMap::draws_as_knob(mixer->params[2]));  // button
    // Unclassified still draws, or every pre-classification map empties.
    CHECK(PortMap::draws_as_knob(old_entry->params[0]));
}

// A map from the CURRENT scanner, which no real file is yet.
//
// Pressing SCAN will write a shape nothing has ever read: scan 3, controls
// carrying `kind`, and lights and displays that no consumer wants. The reader
// must take what it needs and ignore the rest — a parser that trips on an
// unknown field would turn a scan, which is meant to improve the drawing, into
// the thing that empties it.
//
// Written from CARTOG's own emitted shape: lights and displays are
// {x, y, w, h}, params carry kind, and the module carries scan 3.
TEST_CASE("the reader survives a map from the current scanner",
          "[portmap][scan3]") {
    const std::string fresh = R"({
  "modules": [
    {
      "plugin": "Fundamental",
      "model": "LFO",
      "pluginVersion": "2.6.4",
      "scan": 3,
      "size": [135.0, 380.0],
      "params": [
        {"index": 0, "name": "Frequency", "x": 30.0, "y": 90.0,
         "w": 20.0, "h": 20.0, "kind": "knob"},
        {"index": 1, "name": "Level", "x": 90.0, "y": 90.0,
         "w": 8.0, "h": 60.0, "kind": "slider"}
      ],
      "inputs": [
        {"index": 0, "name": "Frequency modulation", "x": 20.0, "y": 286.0}
      ],
      "outputs": [
        {"index": 0, "name": "Sine", "x": 20.0, "y": 330.0}
      ],
      "lights": [
        {"x": 60.0, "y": 40.0, "w": 6.0, "h": 6.0},
        {"x": 75.0, "y": 40.0, "w": 6.0, "h": 6.0}
      ],
      "displays": [
        {"x": 67.0, "y": 200.0, "w": 100.0, "h": 30.0, "type": "LedDisplay"}
      ]
    }
  ]
})";
    const auto pm = forge_modular::PortMap::parse(fresh);
    REQUIRE(pm.size() == 1);
    const auto* lfo = pm.find("Fundamental", "LFO");
    REQUIRE(lfo != nullptr);

    // What it needs survives the fields it does not want.
    CHECK(lfo->scan_version == 3);
    CHECK(lfo->width == 135.0f);
    REQUIRE(lfo->params.size() == 2);
    REQUIRE(lfo->inputs.size() == 1);
    CHECK(lfo->inputs[0].name == "Frequency modulation");

    // A scan this fresh is not stale, and its controls are known.
    CHECK(pm.gap_for("Fundamental", "LFO", "2.6.4") ==
          forge_modular::PortMap::Gap::none);
    CHECK(forge_modular::PortMap::controls_known(*lfo));

    // And the drawing rule reads the new field: the knob draws, the fader
    // does not get drawn as one.
    CHECK(forge_modular::PortMap::draws_as_knob(lfo->params[0]));
    CHECK_FALSE(forge_modular::PortMap::draws_as_knob(lfo->params[1]));
}

// A run that FAILED must read as failed.
//
// The success half of this is covered against a real log. The failure half was
// covered only by fixtures, and the monitor's own comment records what that
// costs: a build stuck at "running" forever because the generator worded its
// ending differently than the rule expected.
//
// A real failing run says "PATCH GATE FAILED: 1 failure(s)" and "gave up after
// 5 attempts". If neither is an error or a refusal, the app watches a dead
// build indefinitely — no verdict, no artifact, no way to tell it ended.
TEST_CASE("a run that failed does not read as still running",
          "[buildlog][join]") {
    const auto log = baseline_dir().parent_path() / "real-generator-failure.log";
    REQUIRE(std::filesystem::exists(log));

    std::vector<forge_modular::BuildLine> lines;
    {
        std::ifstream f(log);
        std::string text;
        while (std::getline(f, text))
            lines.push_back({forge_modular::BuildMonitor::classify(text), text});
    }
    REQUIRE(lines.size() > 10);

    const auto outcome = forge_modular::BuildMonitor::outcome_of(lines);
    INFO("outcome of a failed run: " << static_cast<int>(outcome));
    // Anything but `running`. A finished failure that reads as running is the
    // app waiting forever on a build that ended minutes ago.
    CHECK(outcome != forge_modular::BuildOutcome::running);
}

// The generator's own terminal messages must end the build.
//
// patch.py exits with "gave up after N attempts" when a build exhausts its
// retries. The monitor's error rules are Traceback, "fatal error" and "no such
// file" — none of which that is. So a build that gave up reads as STILL
// RUNNING: the app watches forever, the stage never resolves, and Open in Rack
// never appears, which is the exact failure the success rule above it was
// written to fix.
//
// The strings come from patch.py, not from memory.
TEST_CASE("a build that gives up is not still running", "[buildlog][join]") {
    using forge_modular::BuildMonitor;
    using forge_modular::BuildLine;
    using forge_modular::BuildOutcome;

    // Terminal: the generator has stopped and produced nothing.
    const std::string gave_up = "gave up after 5 attempts";
    CHECK(BuildMonitor::classify(gave_up) != BuildLine::Kind::progress);

    // NOT terminal: an attempt failed its gate and the generator retries.
    // Classifying this as an error would be worse than missing it, because
    // outcome_of ranks errored above succeeded — a recovered run would report
    // failed and the app would hide the patch it had just built.
    const std::string gate = "  PATCH GATE FAILED: 1 failure(s), 2 warning(s)";
    CHECK(BuildMonitor::classify(gate) == BuildLine::Kind::progress);

    // A run that gave up does not read as running.
    std::vector<BuildLine> abandoned{
        {BuildMonitor::classify("  asking the model"), "  asking the model"},
        {BuildMonitor::classify(gate), gate},
        {BuildMonitor::classify(gave_up), gave_up},
    };
    CHECK(BuildMonitor::outcome_of(abandoned) != BuildOutcome::running);

    // And a run that hit the gate and then succeeded is DONE.
    const std::string built = "  built 8 modules, 9 cables \u2192 /tmp/p.vcv";
    std::vector<BuildLine> recovered{
        {BuildMonitor::classify(gate), gate},
        {BuildMonitor::classify(built), built},
    };
    CHECK(BuildMonitor::outcome_of(recovered) == BuildOutcome::done);
}

// A run that recovered is DONE, not failed.
//
// outcome_of ranks refused > errored > succeeded, so any line classified as an
// error outranks a later success. Teaching the monitor that "PATCH GATE
// FAILED" ends a build therefore risks the opposite fault: a run that failed
// its first gate, retried, and produced a patch would report failed — and the
// app would hide the artifact it just built.
//
// The real log for prompt 4 is exactly that shape: two gate failures, then a
// patch.
TEST_CASE("a run that recovered reads as done", "[buildlog][join]") {
    const auto log = baseline_dir().parent_path() / "real-generator-failure.log";
    REQUIRE(std::filesystem::exists(log));

    std::vector<forge_modular::BuildLine> lines;
    std::size_t gate_failures = 0;
    {
        std::ifstream f(log);
        std::string text;
        while (std::getline(f, text)) {
            if (text.find("PATCH GATE FAILED") != std::string::npos)
                ++gate_failures;
            lines.push_back({forge_modular::BuildMonitor::classify(text), text});
        }
    }
    // The fixture has to BE that shape, or this asserts nothing.
    REQUIRE(gate_failures > 0);

    const auto outcome = forge_modular::BuildMonitor::outcome_of(lines);
    INFO("gate failures: " << gate_failures
                           << "  outcome: " << static_cast<int>(outcome));
    CHECK(outcome == forge_modular::BuildOutcome::done);
}

// Every generator ending classifies as terminal — and as the RIGHT terminal.
//
// A textual check that an ending matches "some rule" is not enough: matching a
// SUCCESS rule would be worse than matching nothing, because a failed run
// would report done and the app would offer an artifact that was never
// written. These are the exact strings both generators raise.
TEST_CASE("every generator ending is classified as a failure",
          "[buildlog][join]") {
    using forge_modular::BuildMonitor;
    using forge_modular::BuildLine;
    const std::vector<std::string> endings = {
        "gave up after 5 attempts",
        "model call failed (1): boom",
        "could not fetch the library catalog: timeout",
        "could not fetch the module index: timeout",
        "the patch contract is not sound: marker missing",
        "model reply did not contain both a json and a cpp block:",
        "duplicate addModel for ['VCO'] — Rack would abort at load",
        "Rack SDK not found at /nope. Set RACK_SDK_DIR, or download",
        "two manifests claim the model 'VCO': a.json and b.json.",
        "another generation is already running against this module pack.",
    };
    for (const auto& text : endings) {
        const auto kind = BuildMonitor::classify(text);
        INFO("ending: " << text << "  kind: " << static_cast<int>(kind));
        // Not progress — the app must stop waiting.
        CHECK(kind != BuildLine::Kind::progress);
        // And NOT success, which would offer an artifact that does not exist.
        CHECK(kind != BuildLine::Kind::success);
    }
}

// The UNMAPPED badge comes with a remedy, and only when it is earned.
//
// The badge names a state and no way out, and the way out is not guessable:
// the module has to be ON SCREEN in Rack when SCAN runs, so scanning once
// leaves every module that was not visible still badged. Somebody would
// reasonably read that as the scan having failed.
TEST_CASE("an unmapped module says how to fix it", "[portmap][note]") {
    forge_modular::ForgeModularShell shell;
    pulp::state::StateStore store;
    shell.set_state_store(&store);
    shell.define_parameters(store);
    auto view = shell.create_view();
    REQUIRE(view != nullptr);

    auto rack = [](bool measured) {
        std::vector<forge_modular::RackModule> mods;
        forge_modular::RackModule m;
        m.id = "lfo"; m.brand = "Fundamental"; m.name = "LFO"; m.hp = 9;
        m.placed = true; m.available = true;
        m.controls_measured = measured;
        mods.push_back(std::move(m));
        return mods;
    };

    // Measured: nothing to say.
    shell.show_rack(rack(true), {});
    CHECK(shell.unmapped_note().empty());

    // Unmeasured: names the module AND what to do about it.
    shell.show_rack(rack(false), {});
    const auto note = shell.unmapped_note();
    INFO("note: " << note);
    CHECK_FALSE(note.empty());
    CHECK(note.find("LFO") != std::string::npos);
    // The remedy, not just the diagnosis. Without it the badge is a dead end.
    CHECK(note.find("SCAN") != std::string::npos);
    CHECK(note.find("on screen") != std::string::npos);
}

TEST_CASE("a new build stops offering the patch that was open before it",
          "[artifact][stale]") {
    // Reported as "if I pick a prompt I have built before it shows me the
    // prebuilt one". A patch reopened from the shelf is remembered in
    // `open_patch_`, and artifact_path() falls back to it when this session's
    // log has no patch line yet -- which is deliberately right for a reopened
    // project with no build behind it, and wrong the moment a NEW build starts:
    // for the whole of that build "Open in Rack" hands over the PREVIOUS patch,
    // and if the build fails it never stops doing so.
    //
    // The clear existed, but only on the Home path. Pressing Build again from
    // the Build screen -- which is where a user already is after one build, and
    // where they land after opening a project -- skipped it.
    HermeticProjects isolated;
    std::error_code ec;
    const auto dir = std::filesystem::temp_directory_path() / "fm-stale-artifact";
    std::filesystem::remove_all(dir, ec);
    std::filesystem::create_directories(dir, ec);
    const auto earlier = dir / "built-before.vcv";
    {
        std::ofstream f(earlier);
        f << R"({"modules":[{"plugin":"ForgeModular","model":"VCO","id":1,
             "pos":[0,0]}],"cables":[]})";
    }

    forge_modular::ForgeModularShell shell;
    FakeEngine engine;
    shell.set_engine(&engine);
    pulp::state::StateStore store;
    shell.set_state_store(&store);
    shell.define_parameters(store);
    auto view = shell.create_view();
    REQUIRE(view != nullptr);
    auto* chrome = shell.chrome();
    REQUIRE(chrome != nullptr);
    shell.set_artifact(forge_modular::Artifact::patch);

    // A project reopened from the shelf: no build behind it, so offering its
    // file IS the right answer. This is the behaviour the fix must not break.
    shell.set_open_patch(earlier.string());
    REQUIRE(shell.artifact_path() == earlier.string());

    // Now ask for something else, from the Build screen, the way a second
    // prompt is actually typed.
    chrome->enter_build();
    REQUIRE(chrome->mode() == forge::ForgeChrome::Mode::Build);
    REQUIRE(shell.start_build_with("something completely different").empty());
    REQUIRE(engine.submissions.size() == 1);

    // The build is running and has produced nothing. There is no patch to
    // offer, and saying so is the only honest answer -- the earlier file is
    // not what was asked for.
    INFO("artifact_path() returned: " << shell.artifact_path());
    CHECK(shell.artifact_path().empty());
    CHECK(shell.open_patch().empty());

    std::filesystem::remove_all(dir, ec);
}

TEST_CASE("a new build stops drawing the rack that was on screen before it",
          "[artifact][stale]") {
    // The other half of the same report. Offering the wrong FILE is the half a
    // user only finds by pressing Open in Rack; the half they see is the rack
    // itself, which is drawn from the loaded patch and has no reason of its own
    // to go away when a different prompt is submitted.
    HermeticProjects isolated;
    std::error_code ec;
    const auto dir = std::filesystem::temp_directory_path() / "fm-stale-rack";
    std::filesystem::remove_all(dir, ec);
    std::filesystem::create_directories(dir, ec);
    const auto earlier = dir / "built-before.vcv";
    {
        std::ofstream f(earlier);
        f << R"({"modules":[{"plugin":"ForgeModular","model":"VCO","id":1,
             "pos":[0,0]},{"plugin":"ForgeModular","model":"VCA","id":2,
             "pos":[8,0]}],"cables":[]})";
    }

    forge_modular::ForgeModularShell shell;
    FakeEngine engine;
    shell.set_engine(&engine);
    pulp::state::StateStore store;
    shell.set_state_store(&store);
    shell.define_parameters(store);
    auto view = shell.create_view();
    REQUIRE(view != nullptr);
    auto* chrome = shell.chrome();
    REQUIRE(chrome != nullptr);
    shell.set_artifact(forge_modular::Artifact::patch);

    // Loaded, and genuinely on screen: without this REQUIRE the check below
    // passes over a preview that was empty the whole time.
    REQUIRE(shell.open_patch_file(earlier.string()).empty());
    auto* preview = shell.rack_preview();
    REQUIRE(preview != nullptr);
    REQUIRE(preview->modules().size() == 2);

    chrome->enter_build();
    REQUIRE(shell.start_build_with("something completely different").empty());

    // Nothing has been built yet, so there is no rack to show.
    CHECK(preview->modules().empty());

    std::filesystem::remove_all(dir, ec);
}

TEST_CASE("choosing a row does not wipe the notice that row just raised",
          "[mention][notice]") {
    // The notice existed, was correct, and was gone within the same frame.
    // `choose` rewrites the composer, the field reports a change, and the
    // change path clears the notice on the grounds that a new keystroke
    // supersedes the last pick -- but the pick is not a keystroke. Live, the
    // app logged show_notice("Geodesics Branes is not installed yet …")
    // immediately followed by show_notice(""), and the screen showed nothing.
    forge_modular::MentionOverlay overlay;
    auto root = overlay.build();
    REQUIRE(root != nullptr);
    overlay.set_source([](const std::string&) {
        std::vector<forge_modular::MentionCandidate> out;
        forge_modular::MentionCandidate c;
        c.name = "Branes";
        c.brand = "Geodesics";
        c.slug = "Geodesics/Branes";
        c.state = forge_modular::MentionCandidate::Availability::available;
        out.push_back(c);
        return out;
    });
    overlay.on_refused = [&](const forge_modular::MentionCandidate& c) {
        overlay.show_notice(c.name + " is not installed yet");
    };
    // The real wiring: choosing writes to the field, and the field's change
    // notification comes straight back in. Without this the test cannot fail,
    // because nothing re-enters handle_text at all.
    std::string field;
    overlay.on_choose = [&](const std::string& slug) {
        field = "@" + slug + " ";
        overlay.handle_text(field, field.size());
    };

    overlay.handle_text("@br", 3);
    REQUIRE(overlay.is_open());
    REQUIRE(overlay.handle_key(36));                 // Return picks row 0
    CHECK(field == "@Geodesics/Branes ");            // the pick landed
    CHECK(overlay.notice() == "Branes is not installed yet");

    // And a REAL keystroke still supersedes it, or the notice would never go
    // away -- deleting the guard's condition would pass everything above.
    overlay.handle_text("@Geodesics/Branes x", 19);
    CHECK(overlay.notice().empty());
}

TEST_CASE("the composer field offers arrow keys to the mention list first",
          "[mention][keys]") {
    // The bug the user reported twice, and the reason two previous fixes did
    // not fix it: the hook was on the window root, and AppKit offers every
    // key-down to performKeyEquivalent: BEFORE keyDown:, a path that asks the
    // FOCUSED view before the root's global handler. The composer is
    // multi-line, so it consumed Up and Down to move between lines and the
    // arrows never travelled any further while somebody was typing -- which is
    // the only time the list is open. Clicking a row moved focus off the field
    // and made the arrows start working, which is what made it look
    // intermittent.
    //
    // So this drives the FIELD, not the root: `prompt_input()->on_key_event`
    // is the virtual AppKit reaches first. A test that called the root hook
    // passed throughout the whole time the feature was broken.
    HermeticProjects isolated;
    forge_modular::ForgeModularShell shell;
    FakeEngine engine;
    shell.set_engine(&engine);
    pulp::state::StateStore store;
    shell.set_state_store(&store);
    shell.define_parameters(store);
    auto view = shell.create_view();
    REQUIRE(view != nullptr);
    auto* chrome = shell.chrome();
    REQUIRE(chrome != nullptr);
    auto* input = chrome->prompt_input();
    REQUIRE(input != nullptr);

    // The filter is installed from the poll, once the tree is attached.
    shell.on_poll();

    auto& mentions = shell.mentions();
    mentions.set_source([](const std::string&) {
        std::vector<forge_modular::MentionCandidate> out;
        for (const char* n : {"Alpha", "Beta", "Gamma"}) {
            forge_modular::MentionCandidate c;
            c.name = n;
            c.slug = std::string("Test/") + n;
            c.state = forge_modular::MentionCandidate::Availability::ready;
            out.push_back(c);
        }
        return out;
    });
    mentions.handle_text("@a", 2);
    REQUIRE(mentions.is_open());
    REQUIRE(mentions.selected_index() == 0);

    pulp::view::KeyEvent down;
    down.key = pulp::view::KeyCode::down;
    down.is_down = true;
    // Consumed BY THE FIELD, which is the whole point: if the field returns
    // false the key goes on to move the caret and the list never sees it.
    CHECK(input->on_key_event(down));
    CHECK(mentions.selected_index() == 1);
    CHECK(input->on_key_event(down));
    CHECK(mentions.selected_index() == 2);

    // And the field still works as a field: a key the list does not want is
    // NOT swallowed, or typing would stop working the moment the list opened.
    pulp::view::KeyEvent letter;
    letter.key = static_cast<pulp::view::KeyCode>('x');
    letter.is_down = true;
    CHECK_FALSE(mentions.handle_key_event(letter));
}

TEST_CASE("a second build is refused while one is still running",
          "[build][lock]") {
    // Reported from m5: "the first patch I tapped opened an existing one", and
    // a patch asked for as an acid line that opened under another name. Two
    // generations had finished six seconds apart — both launched, both
    // redirecting into the one `last-run.log`, each overwriting the other from
    // offset zero. The app reads the outcome, the stage AND the artifact path
    // out of that file, so the explanation on screen described one patch while
    // the filename named another.
    //
    // Nothing stopped the second Build. `busy()` existed and start_build_with
    // never asked it.
    HermeticProjects isolated;
    std::error_code ec;
    const auto dir = std::filesystem::temp_directory_path() / "fm-build-lock";
    std::filesystem::remove_all(dir, ec);
    std::filesystem::create_directories(dir, ec);
    const auto log = dir / "run.log";
    { std::ofstream f(log); f << "  asking the model\n"; }

    forge_modular::ForgeModularShell shell;
    FakeEngine engine;
    shell.set_engine(&engine);
    pulp::state::StateStore store;
    shell.set_state_store(&store);
    shell.define_parameters(store);
    auto view = shell.create_view();
    REQUIRE(view != nullptr);
    shell.watch_build_log(log.string());
    shell.on_poll();
    REQUIRE(shell.build_outcome() == forge_modular::BuildOutcome::running);

    // The first one goes.
    REQUIRE(shell.start_build_with("an acid line with accent and slide").empty());
    REQUIRE(engine.submissions.size() == 1);

    // The second does not, and says why rather than doing nothing.
    const auto why = shell.start_build_with("an ambient generative drone");
    INFO("second build said: " << why);
    CHECK_FALSE(why.empty());
    CHECK(why.find("already building") != std::string::npos);
    // The assertion that carries the weight: a refusal that still submits is
    // the bug with an apology attached.
    CHECK(engine.submissions.size() == 1);

    // And the lock LIFTS when the run ends, or the app would take one prompt
    // per launch. Without this, deleting the release would pass everything
    // above.
    { std::ofstream f(log, std::ios::app);
      f << "  built 10 modules, 12 cables \xE2\x86\x92 " << (dir / "a.vcv").string()
        << "\n"; }
    shell.on_poll();
    REQUIRE(shell.build_outcome() != forge_modular::BuildOutcome::running);
    CHECK(shell.start_build_with("an ambient generative drone").empty());
    CHECK(engine.submissions.size() == 2);

    std::filesystem::remove_all(dir, ec);
}

TEST_CASE("every run writes its own log", "[build][lock]") {
    // The lock stops a second build from THIS shell. It cannot stop a run left
    // over from a previous launch of the app, which survives by design — a
    // generation is started with nohup + setsid so closing the window does not
    // kill it. While every run redirected into one `last-run.log`, that
    // leftover wrote over the new run's transcript from offset zero, and the
    // app reads its outcome, its stage and its artifact path out of that file.
    //
    // So the collision is removed rather than merely made unlikely: a run's
    // log is its own file.
    std::error_code ec;
    const auto dir = std::filesystem::temp_directory_path() / "fm-per-run-log";
    std::filesystem::remove_all(dir, ec);
    std::filesystem::create_directories(dir, ec);
    const auto tools = dir / "tools";
    std::filesystem::create_directories(tools, ec);
    // `available()` wants both, and submit() must not actually run them.
    for (const char* t : {"generate.py", "patch.py"}) {
        std::ofstream f(tools / t);
        f << "import sys; sys.exit(0)\n";
    }

    forge_modular::ProcessEngine engine(tools.string(),
                                        (dir / "last-run.log").string());
    const auto first_default = engine.log_path();
    CHECK(first_default == (dir / "last-run.log").string());

    engine.submit("an acid line with accent and slide", /*patch_mode=*/true);
    const auto a = engine.log_path();
    engine.submit("an ambient generative drone", /*patch_mode=*/true);
    const auto b = engine.log_path();

    INFO("first run's log:  " << a);
    INFO("second run's log: " << b);
    CHECK(a != first_default);          // it moved off the shared file
    CHECK(b != first_default);
    CHECK(a != b);                      // and the two runs do not share one
    // Beside the log the shell starts on, not in a temp directory: a
    // generation costs minutes and its transcript is the only record of what
    // the model was asked and answered.
    CHECK(std::filesystem::path(a).parent_path() == dir / "runs");

    std::filesystem::remove_all(dir, ec);
}

TEST_CASE("a port map that will not parse says so, and is not silent",
          "[portmap][unreadable]") {
    // What the user saw: every module Rack owns drawn with hollow knobs and an
    // UNMAPPED badge, while our own modules were fine. The cause was one
    // widget writing `"w": inf` — Rack uses infinite bounds for widgets that
    // size themselves, `inf` is not JSON, and the WHOLE file stopped parsing.
    // Not that module's entry: every entry.
    //
    // The reader already failed safe, returning an empty map, which is right.
    // It failed SILENTLY, which is not: an unreadable map and a map nobody has
    // written look identical on screen and need opposite advice.
    const auto good = forge_modular::PortMap::parse(
        R"({"modules":[{"plugin":"Fundamental","model":"VCO","scan":3,
            "params":[{"index":0,"x":10,"y":20,"w":8,"h":8}]}]})");
    CHECK_FALSE(good.unreadable());
    CHECK(good.find("Fundamental", "VCO") != nullptr);

    // The exact shape that broke it.
    const auto bad = forge_modular::PortMap::parse(
        R"({"modules":[{"plugin":"Fundamental","model":"Delay","scan":3,
            "displays":[{"x": inf, "y": inf, "w": inf, "h": inf}]}]})");
    CHECK(bad.unreadable());
    CHECK(bad.find("Fundamental", "Delay") == nullptr);   // still fails safe

    // An absent map is NOT the unreadable state — that distinction is the
    // whole point, and a version that set the flag unconditionally would pass
    // every check above.
    CHECK_FALSE(forge_modular::PortMap::parse("").unreadable());
    CHECK_FALSE(forge_modular::PortMap::parse(R"({"modules":[]})").unreadable());
}

TEST_CASE("a refusal carries the options, not just the bad news",
          "[monitor][refusal]") {
    // What a user saw: "hold on — this asks for something you don't have
    // installed" followed immediately by "install one in Rack's Library, then
    // ask again". The three FREE modules that would have satisfied the request
    // were printed between those two lines and never reached the screen,
    // because only the first and last matched a rule. A refusal with no
    // options is a dead end; the options are the whole value of it.
    std::error_code ec;
    const auto dir = std::filesystem::temp_directory_path() / "fm-refusal-block";
    std::filesystem::remove_all(dir, ec);
    std::filesystem::create_directories(dir, ec);
    const auto log = dir / "run.log";
    {
        std::ofstream f(log);
        f << "  reading the installed library\n"
             "  hold on \xE2\x80\x94 this asks for something you don't have installed:\n"
             "\n"
             "  no arpeggiator module is installed. These would do it:\n"
             "      free     Ahornberg/Tracker       Tracker  (Ahornberg)\n"
             "      free     AmalgamatedHarmonics/Arp31  Arp 3.1 - Chord\n"
             "      PREMIUM  Vendor/FancyArp         Fancy Arp\n"
             "\n"
             "  install one in Rack's Library, then ask again \xE2\x80\x94\n";
    }

    forge_modular::BuildMonitor mon;
    mon.watch(log.string());
    mon.poll();

    int refusal_lines = 0;
    bool saw_free = false, saw_premium = false, saw_header = false;
    for (const auto& l : mon.lines()) {
        if (l.kind != forge_modular::BuildLine::Kind::refusal) continue;
        ++refusal_lines;
        if (l.text.find("Ahornberg/Tracker") != std::string::npos) saw_free = true;
        if (l.text.find("Vendor/FancyArp") != std::string::npos) saw_premium = true;
        if (l.text.find("hold on") != std::string::npos) saw_header = true;
    }
    INFO("refusal lines carried: " << refusal_lines);
    CHECK(saw_header);
    CHECK(saw_free);        // the thing the user can actually act on
    CHECK(saw_premium);
    CHECK(refusal_lines >= 5);

    // And the block ENDS: an ordinary line after it is not swallowed, or every
    // later line of the run would be painted as a refusal.
    {
        std::ofstream f(log, std::ios::app);
        f << "  reading the module index\n";
    }
    mon.poll();
    const auto& all = mon.lines();
    CHECK(all.back().kind != forge_modular::BuildLine::Kind::refusal);

    std::filesystem::remove_all(dir, ec);
}

TEST_CASE("the explanation opens with what was asked for", "[explain][request]") {
    // A patch explains what it DOES; on its own it never says what it was
    // meant to do. The prompt was echoed into the chat rail — a different
    // panel, usually scrolled away by the time the rack is drawn — so the
    // explanation opened straight into "AUDIO / VCO PLS -> VCF IN" with
    // nothing to compare it against, and the one question a reader has ("is
    // this what I asked for?") could not be answered from the screen.
    forge_modular::PatchExplanation ex;
    ex.set_bounds({0, 0, 420, 600});

    std::vector<forge_modular::RackModule> mods;
    forge_modular::RackModule a;
    a.id = "1"; a.brand = "ForgeModular"; a.name = "VCO"; a.hp = 8;
    forge_modular::RackModule b;
    b.id = "2"; b.brand = "ForgeModular"; b.name = "VCA"; b.hp = 3;
    mods.push_back(a);
    mods.push_back(b);
    std::vector<forge_modular::Connection> cables;
    forge_modular::Connection c;
    c.from_module = "1"; c.from_port = "OUT";
    c.to_module = "2"; c.to_port = "IN";
    c.role = forge_modular::SignalRole::audio;
    cables.push_back(c);

    ex.set_connections(cables, mods);
    const int without = ex.child_count();

    ex.set_request("a melodic arpeggiator quantized to the key of g");
    const int with = ex.child_count();
    INFO("children without the request: " << without << ", with: " << with);
    CHECK(with > without);            // it is actually added

    // And it is FIRST — after the cables it would be a footnote, and the
    // explanation already repeats the request at the end.
    auto* first = ex.child_at(0);
    REQUIRE(first != nullptr);
    bool found = false;
    std::function<void(pulp::view::View*)> walk = [&](pulp::view::View* v) {
        if (!v) return;
        if (auto* l = dynamic_cast<pulp::view::Label*>(v))
            if (l->text().find("arpeggiator") != std::string::npos) found = true;
        for (int i = 0; i < v->child_count(); ++i) walk(v->child_at(i));
    };
    walk(first);
    CHECK(found);

    // Clearing it takes the box away again, or a reopened patch would carry
    // the previous project's request over it.
    ex.set_request("");
    CHECK(ex.child_count() == without);
}

TEST_CASE("a scanned panel draws its screens and lamps", "[.screens-look]") {
    // Hidden by default: it writes a picture for a person to look at. Run with
    //   ./forge-test-chrome-no-leak "[.screens-look]"
    // A Quantizer carries 13 measured displays including its touch plate; an
    // audio interface carries 12 lamps, which are its meter ladder. Both were
    // measured by CARTOG and drawn by nobody until now, so this is the check
    // that the drawing is real rather than merely compiled.
    forge_modular::RackPreview preview;
    std::vector<forge_modular::RackModule> mods;
    forge_modular::RackModule q;
    q.id = "1"; q.brand = "Fundamental"; q.name = "Quantizer"; q.hp = 4;
    forge_modular::RackModule a;
    a.id = "2"; a.brand = "Core"; a.name = "AudioInterface2"; a.hp = 5;
    forge_modular::RackModule v;
    v.id = "3"; v.brand = "Fundamental"; v.name = "VCO"; v.hp = 10;
    mods = {q, a, v};
    preview.set_rack(mods, {});
    const auto png = pulp::view::render_to_png(preview, 420, 380, 2.0f,
                                               pulp::view::ScreenshotBackend::skia);
    REQUIRE_FALSE(png.empty());
    const auto out = std::filesystem::temp_directory_path() / "screens-look.png";
    std::ofstream f(out, std::ios::binary);
    f.write(reinterpret_cast<const char*>(png.data()),
            static_cast<std::streamsize>(png.size()));
    WARN("wrote " << out.string());
}

// --- @-mention fetch: refuse before promising --------------------------------

static bool says(const std::string& hay, const char* needle) {
    return hay.find(needle) != std::string::npos;
}

TEST_CASE("a mention fetch that cannot succeed is refused, not promised",
          "[mention][library]") {
    using A = forge_modular::MentionCandidate::Availability;

    // The whole point. A fetch spawned while signed out still printed
    // "fetching...", and the "not signed in" reply landed in a log nobody
    // opens -- a silent failure wearing the costume of progress.
    const auto out = forge_modular::plan_mention_fetch(A::available, false, "entitled");
    REQUIRE_FALSE(out.fetch);
    REQUIRE(says(out.why, "Log In"));

    // Off means off: the mention row honours the same preference the
    // generation path does, or the setting is a lie in one of two places.
    const auto off = forge_modular::plan_mention_fetch(A::available, true, "none");
    REQUIRE_FALSE(off.fetch);
    REQUIRE(says(off.why, "switched off"));
    // And it points at the REAL control. This copy said "in Settings" while
    // the preference lived only in a JSON file nobody could find; the
    // control now lives in Settings on the Permissions tab, and the words
    // must keep matching the place.
    REQUIRE(says(off.why, "Permissions"));

    // Paid and unowned is refused without ever suggesting a purchase.
    const auto paid = forge_modular::plan_mention_fetch(A::paid, true, "entitled");
    REQUIRE_FALSE(paid.fetch);
    REQUIRE(says(paid.why, "does not own"));
    REQUIRE_FALSE(says(paid.why, "buy"));

    // And the case that must actually work.
    REQUIRE(forge_modular::plan_mention_fetch(A::available, true, "entitled").fetch);
    // Already installed: saying "fetching" would be its own small lie.
    REQUIRE_FALSE(forge_modular::plan_mention_fetch(A::ready, true, "entitled").fetch);
}
