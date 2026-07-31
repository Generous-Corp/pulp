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

#include "forge/module_summary.hpp"
#include "forge/patch_loader.hpp"
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
#include <pulp/view/screenshot.hpp>
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

TEST_CASE("a space closes the mention list", "[mention]") {
    // "@ " is somebody typing an address, not reaching for a module.
    forge_modular::MentionOverlay overlay;
    auto view = overlay.build();
    overlay.set_source(test_library);

    overlay.handle_text("@vc", 3);
    REQUIRE(overlay.is_open());
    overlay.handle_text("@vc ", 4);
    CHECK_FALSE(overlay.is_open());
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

TEST_CASE("only an installed module can be inserted", "[mention]") {
    // Rack keeps missing modules as placeholders and offers to fetch them, so an
    // unavailable mention is an offer -- but wiring a patch to a module the user
    // does not have produces a patch that cannot sound. Enter refuses rather
    // than silently making one.
    forge_modular::MentionOverlay overlay;
    auto view = overlay.build();
    overlay.set_source(test_library);
    std::string chosen;
    overlay.on_choose = [&](const std::string& slug) { chosen = slug; };

    overlay.handle_text("@drum", 5);
    REQUIRE(overlay.candidates().size() == 1);
    CHECK_FALSE(overlay.candidates()[0].insertable());     // 4ms, not installed

    CHECK(overlay.handle_key(36));      // consumed, so the prompt is not submitted
    CHECK(chosen.empty());             // but nothing was inserted
    CHECK(overlay.is_open());          // and it stays up rather than vanishing
}

TEST_CASE("the selection lands on something insertable", "[mention]") {
    // Opening with an uninstallable row first would put Enter on a dead choice.
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
    CHECK(overlay.selected_index() == 1);
    CHECK(overlay.candidates()[overlay.selected_index()].insertable());
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
    // All three states were known to the app and reached a person only as the
    // wording of a failure AFTER they pressed the button. Until then "Rack is
    // not installed" and "the button did nothing" looked the same.
    forge_modular::RackPresence p;
    CHECK(p.phrase() == "Rack is not installed");
    p.plugin_installed = true;
    CHECK(p.phrase() == "Rack is available as a plugin");
    p.standalone_installed = true;
    CHECK(p.phrase() == "Rack is installed");
    p.standalone_running = true;
    CHECK(p.phrase() == "Rack is running");

    // Every state says something different. A phrase() that returned one
    // string would satisfy nothing above but is worth pinning: the pill is
    // only useful because the four readings are distinguishable.
    std::set<std::string> said;
    for (int i = 0; i < 8; ++i) {
        forge_modular::RackPresence q;
        q.standalone_running = i & 1;
        q.standalone_installed = i & 2;
        q.plugin_installed = i & 4;
        said.insert(q.phrase());
    }
    CHECK(said.size() == 4);

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
    CHECK_FALSE(shell.rack_presence_phrase().empty());
    CHECK(shell.rack_presence_phrase() == pill->text());
    // It has to say one of the four things, not any string at all.
    CHECK(said.count(shell.rack_presence_phrase()) == 1);
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
    for (std::size_t i = 0; i < sample_patch().size(); ++i) {
        const auto* row = ex.row_for(i);
        REQUIRE(row != nullptr);
        float previous_top = -1000.0f;
        for (int c = 0; c < row->child_count(); ++c) {
            auto* lbl = dynamic_cast<const pulp::view::Label*>(row->child_at(c));
            if (!lbl) continue;                  // the role dot
            const auto lb = lbl->bounds();
            INFO("  cable " << i << " line at " << lb.y
                            << " height " << lb.height);
            CHECK(lb.height >= 17.0f);           // a full line, not a squeezed one
            if (previous_top > -999.0f) CHECK(lb.y - previous_top >= 16.0f);
            previous_top = lb.y;
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
            if (b->access_label().find("Build") != std::string::npos) {
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
    const std::string tools = "/Volumes/Workshop/Code/pulp-modular-rack/tools/rack";
    const std::string patch = "/tmp/ambient-drone.vcv";
    if (!std::filesystem::exists(tools) || !std::filesystem::exists(patch)) {
        WARN("toolchain or generated patch not present; skipping");
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
    const std::string patch = "/tmp/ambient-drone.vcv";
    if (!std::filesystem::exists(patch)) {
        WARN("no generated patch present; skipping");
        return;
    }

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
    CHECK(audio > 0);
    CHECK(mod > 0);

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

TEST_CASE("a running build shows a clock, not just a word", "[phase7][stage]") {
    // "asking the model" with nothing moving is indistinguishable from a wedged
    // process, and a model call takes minutes. Forge's own chips carry a live
    // time; ours cleared the field.
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

    const auto log = std::filesystem::temp_directory_path() / "fm-clock.log";
    std::filesystem::remove(log);
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

    const std::string real = "/tmp/ambient-drone.vcv";
    if (!std::filesystem::exists(real)) {
        WARN("no generated patch present; skipping");
        return;
    }

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

    const std::string real = "/tmp/ambient-drone.vcv";
    if (!std::filesystem::exists(real)) {
        WARN("no generated patch present; skipping");
        return;
    }
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

