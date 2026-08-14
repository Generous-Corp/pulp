// Per-knob sprite production for the browser-capture lane, and the movement it
// exists to make possible.
//
// The chain under test is producer → materializer → renderer:
//   * the pass crops a declared knob out of the panel capture and erases the
//     pointer frozen into that crop,
//   * `build_native_view_tree` forwards the stamped `knob_ind_*` attributes to
//     the Knob,
//   * `Knob::paint` sweeps the design's pointer along the value arc.
//
// The movement assertion is the point. A single-frame similarity score cannot
// tell a live indicator from a dead one — a frozen pointer scores exactly as
// well as a moving one on the frame where they agree — so the pointer is
// rendered at several values and asserted to have MOVED.

#include "tools/import-design/browser_knob_sprites.hpp"
#include "tools/import-design/import_png_codec.hpp"

#include <pulp/canvas/recording_canvas.hpp>
#include <pulp/view/design_codegen.hpp>
#include <pulp/view/design_import.hpp>
#include <pulp/view/view.hpp>
#include <pulp/view/widgets.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace {

namespace fs = std::filesystem;
using pulp::import_design::ImportPngImage;
using pulp::view::AudioWidgetType;
using pulp::view::DesignIR;
using pulp::view::IRNode;

struct TempDirectory {
    fs::path root;

    TempDirectory() {
        root = fs::temp_directory_path() /
               ("pulp-browser-knob-sprites-" +
                std::to_string(std::chrono::steady_clock::now()
                                   .time_since_epoch()
                                   .count()));
        fs::create_directories(root);
    }
    ~TempDirectory() {
        std::error_code ec;
        fs::remove_all(root, ec);
    }
};

constexpr int kPanelWidth = 240;
constexpr int kPanelHeight = 240;
constexpr int kDialLeft = 20;
constexpr int kDialTop = 20;
constexpr int kDialSize = 200;
// The dot as the design drew it: near the top of the dial, 16 device px square.
constexpr int kDotLeft = 108;
constexpr int kDotTop = 36;
constexpr int kDotSize = 16;

std::uint8_t* pixel_at(ImportPngImage& image, int x, int y) {
    return &image.rgba[(static_cast<std::size_t>(y) * image.width + x) * 4];
}
const std::uint8_t* pixel_at(const ImportPngImage& image, int x, int y) {
    return &image.rgba[(static_cast<std::size_t>(y) * image.width + x) * 4];
}

/// The red the design painted its dot in, and a face that cannot produce it.
constexpr int kDotRed = 250;

/// A panel whose dial face is a strictly rotationally symmetric radial ramp,
/// with one bright dot painted on it.
///
/// Rotational symmetry is what makes the erase provable: any leftover of the
/// dot is a value the face cannot produce at that radius. The ramp is monotone
/// rather than a repeating ring pattern so that a one-pixel sampling difference
/// costs one level instead of wrapping the pattern -- the assertion is about
/// the erase, not about where a repeat happens to land.
double face_radius(int x, int y) {
    const double dx = x + 0.5 - (kDialLeft + kDialSize * 0.5);
    const double dy = y + 0.5 - (kDialTop + kDialSize * 0.5);
    return std::sqrt(dx * dx + dy * dy);
}

int face_red(double radius) {
    return static_cast<int>(20.0 + radius * 0.75);
}

ImportPngImage synthetic_panel() {
    ImportPngImage panel;
    panel.width = kPanelWidth;
    panel.height = kPanelHeight;
    panel.rgba.assign(static_cast<std::size_t>(kPanelWidth) * kPanelHeight * 4, 0);
    for (int y = 0; y < kPanelHeight; ++y) {
        for (int x = 0; x < kPanelWidth; ++x) {
            const double radius = face_radius(x, y);
            std::uint8_t* p = pixel_at(panel, x, y);
            p[0] = static_cast<std::uint8_t>(face_red(radius));
            p[1] = static_cast<std::uint8_t>(40.0 + radius * 0.3);
            p[2] = static_cast<std::uint8_t>(60.0 + radius * 0.2);
            p[3] = 255;
        }
    }
    for (int y = kDotTop; y < kDotTop + kDotSize; ++y)
        for (int x = kDotLeft; x < kDotLeft + kDotSize; ++x) {
            std::uint8_t* p = pixel_at(panel, x, y);
            p[0] = kDotRed; p[1] = 30; p[2] = 30; p[3] = 255;
        }
    return panel;
}

void write_png(const fs::path& path, const ImportPngImage& image) {
    const auto bytes = pulp::import_design::encode_png_rgba(image);
    REQUIRE_FALSE(bytes.empty());
    std::ofstream out(path, std::ios::binary);
    out.write(reinterpret_cast<const char*>(bytes.data()),
              static_cast<std::streamsize>(bytes.size()));
}

ImportPngImage read_png(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    const std::vector<std::uint8_t> bytes{
        std::istreambuf_iterator<char>(in),
        std::istreambuf_iterator<char>()};
    return pulp::import_design::decode_png_rgba(bytes.data(), bytes.size());
}

IRNode declared_knob() {
    IRNode knob;
    knob.type = "frame";
    knob.stable_anchor_id = "capture:cutoff:0";
    knob.audio_widget = AudioWidgetType::knob;
    knob.style.position = "absolute";
    knob.style.left = static_cast<float>(kDialLeft) * 0.5f;
    knob.style.top = static_cast<float>(kDialTop) * 0.5f;
    knob.style.width = static_cast<float>(kDialSize) * 0.5f;
    knob.style.height = static_cast<float>(kDialSize) * 0.5f;
    knob.attributes["binding"] = "cutoff";
    knob.attributes["designed_body"] = "capture";
    knob.attributes["knob_ind_r_in"] = "0.677";
    knob.attributes["knob_ind_r_out"] = "0.845";
    knob.attributes["knob_ind_w"] = "0.168";
    knob.attributes["knob_ind_color"] = "#fa1e1e";
    knob.attributes["knob_ind_phase_rad"] = "0.4";
    knob.attributes["browser_sprite_crop_px"] =
        std::to_string(kDialLeft) + "," + std::to_string(kDialTop) + "," +
        std::to_string(kDialSize) + "," + std::to_string(kDialSize);
    knob.attributes["browser_sprite_indicator_px"] =
        std::to_string(kDotLeft) + "," + std::to_string(kDotTop) + "," +
        std::to_string(kDotSize) + "," + std::to_string(kDotSize);
    return knob;
}

constexpr int kFaderLeft = 80;
constexpr int kFaderTop = 20;
constexpr int kFaderWidth = 40;
constexpr int kFaderHeight = 200;
constexpr int kFaderIndicatorLeft = 78;
constexpr int kFaderIndicatorTop = 110;
constexpr int kFaderIndicatorWidth = 36;
constexpr int kFaderIndicatorHeight = 20;
constexpr int kFaderBodyLeft = kFaderIndicatorLeft;
constexpr int kFaderBodyWidth =
    kFaderLeft + kFaderWidth - kFaderBodyLeft;

ImportPngImage synthetic_fader_panel() {
    ImportPngImage panel;
    panel.width = kPanelWidth;
    panel.height = kPanelHeight;
    panel.rgba.assign(static_cast<std::size_t>(panel.width) * panel.height * 4, 0);
    for (int y = 0; y < panel.height; ++y) {
        for (int x = 0; x < panel.width; ++x) {
            auto* p = pixel_at(panel, x, y);
            p[0] = 14; p[1] = 18; p[2] = 24; p[3] = 255;
        }
    }
    // Authored track/fill changes continuously along the travel axis, so the
    // erase test can prove it reconstructed that axis rather than flooding the
    // thumb hole with one constant.
    for (int y = kFaderTop; y < kFaderTop + kFaderHeight; ++y) {
        for (int x = kFaderLeft; x < kFaderLeft + kFaderWidth; ++x) {
            auto* p = pixel_at(panel, x, y);
            p[0] = static_cast<std::uint8_t>(30 + (y - kFaderTop) / 2);
            p[1] = 52; p[2] = 70; p[3] = 255;
        }
    }
    // Value-dependent captured fill. The static-body pass must remove this
    // whole band, not merely the thumb at its one captured position.
    for (int y = kFaderIndicatorTop + kFaderIndicatorHeight;
         y < kFaderTop + kFaderHeight; ++y) {
        for (int x = kFaderLeft + kFaderWidth / 2 - 2;
             x < kFaderLeft + kFaderWidth / 2 + 2; ++x) {
            auto* p = pixel_at(panel, x, y);
            p[0] = 216; p[1] = 112; p[2] = 48; p[3] = 255;
        }
    }
    for (int y = kFaderIndicatorTop;
         y < kFaderIndicatorTop + kFaderIndicatorHeight; ++y) {
        for (int x = kFaderIndicatorLeft;
             x < kFaderIndicatorLeft + kFaderIndicatorWidth; ++x) {
            const int ix = x - kFaderIndicatorLeft;
            const int iy = y - kFaderIndicatorTop;
            const bool corner =
                (ix < 3 || ix >= kFaderIndicatorWidth - 3) &&
                (iy < 3 || iy >= kFaderIndicatorHeight - 3);
            if (corner) continue;
            auto* p = pixel_at(panel, x, y);
            p[0] = 244; p[1] = 231; p[2] = 180; p[3] = 255;
        }
    }
    return panel;
}

IRNode declared_fader() {
    IRNode fader;
    fader.type = "frame";
    fader.stable_anchor_id = "capture:drive:0";
    fader.audio_widget = AudioWidgetType::fader;
    fader.style.position = "absolute";
    fader.style.left = static_cast<float>(kFaderLeft) * 0.5f;
    fader.style.top = static_cast<float>(kFaderTop) * 0.5f;
    fader.style.width = static_cast<float>(kFaderWidth) * 0.5f;
    fader.style.height = static_cast<float>(kFaderHeight) * 0.5f;
    fader.attributes["binding"] = "drive";
    fader.attributes["designed_body"] = "capture";
    fader.attributes["design_track"] = "#253047";
    fader.attributes["design_accent"] = "#d4a44d";
    fader.attributes["design_indicator"] = "#f4e7b4";
    fader.attributes["browser_sprite_crop_px"] =
        std::to_string(kFaderLeft) + "," + std::to_string(kFaderTop) + "," +
        std::to_string(kFaderWidth) + "," + std::to_string(kFaderHeight);
    fader.attributes["browser_sprite_indicator_px"] =
        std::to_string(kFaderIndicatorLeft) + "," +
        std::to_string(kFaderIndicatorTop) + "," +
        std::to_string(kFaderIndicatorWidth) + "," +
        std::to_string(kFaderIndicatorHeight);
    return fader;
}

constexpr int kStaticFaderLeft = 20;
constexpr int kStaticFaderTop = 30;
constexpr int kStaticFaderWidth = 200;
constexpr int kStaticFaderHeight = 40;
constexpr int kStaticIndicatorLeft = 84;
constexpr int kStaticIndicatorTop = 35;
constexpr int kStaticIndicatorWidth = 18;
constexpr int kStaticIndicatorHeight = 30;

ImportPngImage static_horizontal_fader_panel() {
    ImportPngImage panel;
    panel.width = kPanelWidth;
    panel.height = 100;
    panel.rgba.assign(static_cast<std::size_t>(panel.width) * panel.height * 4, 255);
    auto black = [&](int x, int y) {
        auto* p = pixel_at(panel, x, y);
        p[0] = p[1] = p[2] = 0;
        p[3] = 255;
    };
    for (int x = kStaticFaderLeft; x < kStaticFaderLeft + kStaticFaderWidth; ++x) {
        black(x, kStaticFaderTop);
        black(x, kStaticFaderTop + kStaticFaderHeight - 1);
        if (x >= kStaticFaderLeft + 10 &&
            x < kStaticFaderLeft + kStaticFaderWidth - 10)
            black(x, kStaticFaderTop + kStaticFaderHeight / 2);
    }
    for (int y = kStaticFaderTop; y < kStaticFaderTop + kStaticFaderHeight; ++y) {
        black(kStaticFaderLeft, y);
        black(kStaticFaderLeft + kStaticFaderWidth - 1, y);
        // Wide authored end housings are static chrome, not localized fill.
        for (int inset = 1; inset < 5; ++inset) {
            black(kStaticFaderLeft + inset, y);
            black(kStaticFaderLeft + kStaticFaderWidth - 1 - inset, y);
        }
    }
    for (int tick : {kStaticFaderLeft + 20, kStaticFaderLeft + 100, kStaticFaderLeft + 180}) {
        for (int x = tick; x < tick + 4; ++x)
            for (int y = kStaticFaderTop + 8;
                 y < kStaticFaderTop + kStaticFaderHeight - 8; ++y)
                black(x, y);
    }
    for (int y = kStaticIndicatorTop; y < kStaticIndicatorTop + kStaticIndicatorHeight; ++y)
        for (int x = kStaticIndicatorLeft; x < kStaticIndicatorLeft + kStaticIndicatorWidth; ++x)
            black(x, y);
    // Fractional CSS transforms leave an anti-aliased fringe just outside the
    // integer DOM rectangle. It belongs to the old thumb too and must not
    // survive as a grey ghost in the reusable body sprite.
    for (int y = kStaticIndicatorTop; y < kStaticIndicatorTop + kStaticIndicatorHeight; ++y) {
        for (const int x :
             {kStaticIndicatorLeft - 1, kStaticIndicatorLeft + kStaticIndicatorWidth}) {
            auto* p = pixel_at(panel, x, y);
            p[0] = p[1] = p[2] = 128;
        }
    }
    return panel;
}

IRNode declared_static_horizontal_fader() {
    IRNode fader;
    fader.type = "frame";
    fader.stable_anchor_id = "capture:rate:0";
    fader.audio_widget = AudioWidgetType::fader;
    fader.style.position = "absolute";
    fader.style.left = static_cast<float>(kStaticFaderLeft) * 0.5f;
    fader.style.top = static_cast<float>(kStaticFaderTop) * 0.5f;
    fader.style.width = static_cast<float>(kStaticFaderWidth) * 0.5f;
    fader.style.height = static_cast<float>(kStaticFaderHeight) * 0.5f;
    fader.attributes["binding"] = "rate";
    fader.attributes["designed_body"] = "capture";
    fader.attributes["browser_fader_static_track_declared"] = "1";
    fader.attributes["browser_sprite_crop_px"] =
        std::to_string(kStaticFaderLeft) + "," + std::to_string(kStaticFaderTop) + "," +
        std::to_string(kStaticFaderWidth) + "," + std::to_string(kStaticFaderHeight);
    fader.attributes["browser_sprite_indicator_px"] =
        std::to_string(kStaticIndicatorLeft - 1) + "," +
        std::to_string(kStaticIndicatorTop - 1) + "," +
        std::to_string(kStaticIndicatorWidth + 2) + "," +
        std::to_string(kStaticIndicatorHeight + 2);
    return fader;
}

ImportPngImage off_centre_fill_fader_panel() {
    auto panel = static_horizontal_fader_panel();
    const int y = kStaticIndicatorTop + 2; // deliberately away from centre
    const int before = kStaticFaderLeft + (kStaticIndicatorLeft - kStaticFaderLeft) / 2;
    const int after =
        kStaticIndicatorLeft + kStaticIndicatorWidth +
        (kStaticFaderLeft + kStaticFaderWidth - (kStaticIndicatorLeft + kStaticIndicatorWidth)) / 2;
    auto* left = pixel_at(panel, before, y);
    left[0] = 85;
    left[1] = 85;
    left[2] = 85;
    left[3] = 255;
    auto* right = pixel_at(panel, after, y);
    right[0] = 119;
    right[1] = 119;
    right[2] = 119;
    right[3] = 255;
    return panel;
}

ImportPngImage centre_origin_fill_fader_panel() {
    auto panel = static_horizontal_fader_panel();
    // Remove the original thumb and fringe, restoring the one-pixel track.
    for (int y = kStaticIndicatorTop; y < kStaticIndicatorTop + kStaticIndicatorHeight; ++y)
        for (int x = kStaticIndicatorLeft - 1;
             x <= kStaticIndicatorLeft + kStaticIndicatorWidth; ++x) {
            auto* p = pixel_at(panel, x, y);
            p[0] = p[1] = p[2] = 255;
            p[3] = 255;
        }
    for (int x = kStaticIndicatorLeft - 1;
         x <= kStaticIndicatorLeft + kStaticIndicatorWidth; ++x) {
        auto* p = pixel_at(panel, x, kStaticFaderTop + kStaticFaderHeight / 2);
        p[0] = p[1] = p[2] = 0;
    }

    // A bipolar fill occupies only the centre-to-thumb interval. The old
    // classifier's representative points (well left and right of this run)
    // both see background and therefore cannot detect it.
    for (int y = kStaticFaderTop + kStaticFaderHeight / 2 - 2;
         y <= kStaticFaderTop + kStaticFaderHeight / 2 + 2; ++y)
        for (int x = kStaticFaderLeft + kStaticFaderWidth / 2;
             x < kStaticFaderLeft + 140; ++x) {
            auto* p = pixel_at(panel, x, y);
            p[0] = p[1] = p[2] = 96;
            p[3] = 255;
        }

    constexpr int thumb_left = kStaticFaderLeft + 140;
    for (int y = kStaticIndicatorTop; y < kStaticIndicatorTop + kStaticIndicatorHeight; ++y)
        for (int x = thumb_left; x < thumb_left + kStaticIndicatorWidth; ++x) {
            auto* p = pixel_at(panel, x, y);
            p[0] = p[1] = p[2] = 0;
            p[3] = 255;
        }
    return panel;
}

IRNode declared_centre_origin_fill_fader() {
    auto fader = declared_static_horizontal_fader();
    constexpr int thumb_left = kStaticFaderLeft + 140;
    fader.attributes["browser_sprite_indicator_px"] =
        std::to_string(thumb_left - 1) + "," +
        std::to_string(kStaticIndicatorTop - 1) + "," +
        std::to_string(kStaticIndicatorWidth + 2) + "," +
        std::to_string(kStaticIndicatorHeight + 2);
    return fader;
}

/// The bright pointer is the LAST stroke_line the knob emits: the captured
/// pointer draws a dark backing stroke and then the design's colour over it.
struct Segment {
    float x0 = 0.0f, y0 = 0.0f, x1 = 0.0f, y1 = 0.0f;
    bool found = false;
};

Segment pointer_segment(pulp::view::Knob& knob, float value) {
    knob.set_value(value);
    pulp::canvas::RecordingCanvas canvas;
    knob.paint(canvas);
    Segment segment;
    for (const auto& command : canvas.commands()) {
        if (command.type != pulp::canvas::DrawCommand::Type::stroke_line)
            continue;
        segment = {command.f[0], command.f[1], command.f[2], command.f[3], true};
    }
    return segment;
}

}  // namespace

TEST_CASE("a declared knob gets its own crop of the capture with the pointer erased",
          "[import-design][browser-capture][knob][indicator]") {
    TempDirectory temp;
    const auto panel = synthetic_panel();
    const auto capture = temp.root / "browser.png";
    write_png(capture, panel);

    DesignIR ir;
    ir.root.type = "frame";
    ir.root.children.push_back(declared_knob());

    std::string error;
    REQUIRE(pulp::import_design::apply_browser_capture_control_sprites(
                ir, capture, temp.root / "sprites", &error) == 1);
    CHECK(error.empty());

    const auto& knob = ir.root.children[0];
    REQUIRE(knob.attributes.count("asset_path") == 1);
    CHECK(knob.attributes.at("png_natural_w") == std::to_string(kDialSize));
    CHECK(knob.attributes.at("png_natural_h") == std::to_string(kDialSize));
    // A single static disc. A multi-frame strip would bake rotation into its
    // frames and suppress the pointer overlay entirely.
    CHECK(knob.attributes.at("sprite_strip_frame_count") == "1");
    // Hand-off state, consumed. Leaving it would publish producer scratch in
    // the portable IR.
    CHECK(knob.attributes.count("browser_sprite_crop_px") == 0);
    CHECK(knob.attributes.count("browser_sprite_indicator_px") == 0);

    const auto sprite = read_png(fs::path(knob.attributes.at("asset_path")));
    REQUIRE(sprite.valid());
    REQUIRE(sprite.width == kDialSize);
    REQUIRE(sprite.height == kDialSize);

    SECTION("non-declared pixels stay byte-identical to the capture") {
        // Everything Pulp was not told about is the design's, unaltered. A crop
        // that "looks the same" is not the bar: a filter, a resample or a
        // colour-space round trip would all pass a similarity score and all
        // change the design.
        int compared = 0;
        for (int y = 0; y < sprite.height; ++y) {
            for (int x = 0; x < sprite.width; ++x) {
                const int px = kDialLeft + x;
                const int py = kDialTop + y;
                const bool declared =
                    px >= kDotLeft && px < kDotLeft + kDotSize &&
                    py >= kDotTop && py < kDotTop + kDotSize;
                if (declared) continue;
                const std::uint8_t* got = pixel_at(sprite, x, y);
                const std::uint8_t* want = pixel_at(panel, px, py);
                REQUIRE(std::equal(want, want + 4, got));
                ++compared;
            }
        }
        REQUIRE(compared == kDialSize * kDialSize - kDotSize * kDotSize);
    }

    SECTION("the pointer frozen into the capture is gone") {
        // Left in place it is a second, stuck pointer beside the live one --
        // the exact defect the Figma lane erases at import.
        int dot_pixels = 0;
        for (int y = kDotTop; y < kDotTop + kDotSize; ++y)
            for (int x = kDotLeft; x < kDotLeft + kDotSize; ++x) {
                const std::uint8_t* p =
                    pixel_at(sprite, x - kDialLeft, y - kDialTop);
                if (p[0] > kDotRed - 60 && p[1] < 100 && p[2] < 100) ++dot_pixels;
            }
        CHECK(dot_pixels == 0);
    }

    SECTION("the erased hole is filled from the face the pointer sat on") {
        // Not merely "not red": a hole punched to transparent, or flooded with
        // one constant, would also pass the check above while reading as
        // damage. The face is a radial ramp, so the replacement at a given
        // radius must be that radius' own value -- which also pins that the
        // erase reads the face at the SAME radius rather than dragging a
        // neighbouring feature across the hole.
        for (int y = kDotTop; y < kDotTop + kDotSize; ++y)
            for (int x = kDotLeft; x < kDotLeft + kDotSize; ++x) {
                const std::uint8_t* got =
                    pixel_at(sprite, x - kDialLeft, y - kDialTop);
                CHECK(static_cast<int>(got[3]) == 255);
                CHECK(std::abs(static_cast<int>(got[0]) -
                               face_red(face_radius(x, y))) <= 2);
            }
    }
}

TEST_CASE("an undeclared control is left exactly as it was",
          "[import-design][browser-capture][knob][indicator]") {
    // Pulp adds nothing of its own. Without a declaration there is nothing in a
    // flat picture that says which pixels move, so a knob that declared no
    // pointer keeps precisely the behaviour it has today -- no sprite, no
    // indicator, no attribute churn.
    TempDirectory temp;
    const auto capture = temp.root / "browser.png";
    write_png(capture, synthetic_panel());

    IRNode silent;
    silent.type = "frame";
    silent.stable_anchor_id = "capture:resonance:1";
    silent.audio_widget = AudioWidgetType::knob;
    silent.attributes["binding"] = "resonance";
    silent.attributes["designed_body"] = "capture";
    const auto before = silent.attributes;

    DesignIR ir;
    ir.root.type = "frame";
    ir.root.children.push_back(silent);

    std::string error;
    CHECK(pulp::import_design::apply_browser_capture_control_sprites(
              ir, capture, temp.root / "sprites", &error) == 0);
    CHECK(error.empty());
    CHECK(ir.root.children[0].attributes == before);
    // Nor is a sprite directory conjured for a panel that needs none.
    CHECK_FALSE(fs::exists(temp.root / "sprites"));
}

TEST_CASE("a declared fader hoists its authored thumb and keeps live chrome",
          "[import-design][browser-capture][fader][indicator][movement]") {
    TempDirectory temp;
    const auto panel = synthetic_fader_panel();
    const auto capture = temp.root / "browser.png";
    write_png(capture, panel);

    DesignIR ir;
    ir.source = pulp::view::DesignSource::html;
    ir.root.type = "frame";
    ir.root.stable_anchor_id = "root";
    ir.root.style.width = static_cast<float>(kPanelWidth) * 0.5f;
    ir.root.style.height = static_cast<float>(kPanelHeight) * 0.5f;
    ir.root.children.push_back(declared_fader());

    std::string error;
    REQUIRE(pulp::import_design::apply_browser_capture_control_sprites(
                ir, capture, temp.root / "sprites", &error) == 1);
    CHECK(error.empty());

    const auto& node = ir.root.children.front();
    REQUIRE(node.attributes.count("fader_body_asset_path") == 1);
    REQUIRE(node.attributes.count("fader_indicator_asset_path") == 1);
    CHECK(node.attributes.at("fader_body_natural_w") ==
          std::to_string(kFaderBodyWidth));
    CHECK(node.attributes.at("fader_body_natural_h") ==
          std::to_string(kFaderHeight));
    CHECK(node.attributes.at("fader_body_origin_x") == "-2");
    CHECK(node.attributes.at("fader_body_origin_y") == "0");
    CHECK(node.attributes.at("fader_control_natural_w") ==
          std::to_string(kFaderWidth));
    CHECK(node.attributes.at("fader_control_natural_h") ==
          std::to_string(kFaderHeight));
    CHECK(node.attributes.at("fader_indicator_natural_w") ==
          std::to_string(kFaderIndicatorWidth));
    CHECK(node.attributes.at("fader_indicator_natural_h") ==
          std::to_string(kFaderIndicatorHeight));
    CHECK(std::stof(node.attributes.at("fader_indicator_cross")) ==
          Catch::Approx(0.4f));
    CHECK(node.attributes.count("browser_sprite_crop_px") == 0);
    CHECK(node.attributes.count("browser_sprite_indicator_px") == 0);

    const auto body = read_png(node.attributes.at("fader_body_asset_path"));
    const auto indicator =
        read_png(node.attributes.at("fader_indicator_asset_path"));
    REQUIRE(body.valid());
    REQUIRE(indicator.valid());
    REQUIRE(indicator.width == kFaderIndicatorWidth);
    REQUIRE(indicator.height == kFaderIndicatorHeight);
    // The hoisted asset is the designer's pixels, not a sampled solid-color
    // substitute.
    CHECK(pixel_at(indicator, indicator.width / 2,
                   indicator.height / 2)[0] == 244);
    // The authored rounded corners stay transparent instead of carrying a
    // rectangular patch of the captured track as the thumb moves.
    CHECK(pixel_at(indicator, 0, 0)[3] == 0);
    // The cleaned body no longer carries a frozen copy of the slab.
    for (int y = kFaderIndicatorTop;
         y < kFaderIndicatorTop + kFaderIndicatorHeight; ++y)
        for (int x = kFaderIndicatorLeft;
             x < kFaderIndicatorLeft + kFaderIndicatorWidth; ++x)
            CHECK(pixel_at(body, x - kFaderBodyLeft, y - kFaderTop)[0] < 180);
    CHECK(pixel_at(body, kFaderLeft + kFaderWidth / 2 - kFaderBodyLeft,
                   kFaderHeight - 10)[0] < 180);

    auto root = pulp::view::build_native_view_tree(ir, {}, {});
    REQUIRE(root != nullptr);
    auto* fader = dynamic_cast<pulp::view::Fader*>(root->child_at(0));
    REQUIRE(fader != nullptr);
    REQUIRE(fader->has_captured_indicator_art());
    CHECK(fader->has_skin_track_color());
    CHECK(fader->has_skin_fill_color());
    fader->set_bounds({0.0f, 0.0f,
                       static_cast<float>(kFaderWidth) * 0.5f,
                       static_cast<float>(kFaderHeight) * 0.5f});

    auto indicator_y = [&](float value) {
        fader->set_value(value);
        pulp::canvas::RecordingCanvas canvas;
        fader->paint(canvas);
        std::vector<pulp::canvas::DrawCommand> images;
        for (const auto& command : canvas.commands())
            if (command.type == pulp::canvas::DrawCommand::Type::draw_image)
                images.push_back(command);
        // Cleaned designer body + moving designer indicator.
        REQUIRE(images.size() == 2);
        CHECK(images.front().text == node.attributes.at("fader_body_asset_path"));
        CHECK(images.back().text ==
              node.attributes.at("fader_indicator_asset_path"));
        CHECK(images.back().f[2] ==
              Catch::Approx(kFaderIndicatorWidth * 0.5f));
        CHECK(images.back().f[3] ==
              Catch::Approx(kFaderIndicatorHeight * 0.5f));
        // Functional native track and fill stay present, while the stock white
        // slab is absent (it would be a third rounded rect).
        REQUIRE(canvas.count(
                    pulp::canvas::DrawCommand::Type::fill_rounded_rect) == 2);
        return images.back().f[1];
    };
    const float low = indicator_y(0.0f);
    const float mid = indicator_y(0.5f);
    const float high = indicator_y(1.0f);
    CHECK(low > mid);
    CHECK(mid > high);

    // Preview selects the minimal body renderer, but the authored indicator is
    // a final overlay and must not disappear with the stock thumb branch.
    fader->set_render_style(pulp::view::WidgetRenderStyle::minimal);
    pulp::canvas::RecordingCanvas minimal_canvas;
    fader->paint(minimal_canvas);
    CHECK(minimal_canvas.count(
              pulp::canvas::DrawCommand::Type::draw_image) == 2);
    CHECK(minimal_canvas.count(
              pulp::canvas::DrawCommand::Type::fill_rounded_rect) == 1);
    fader->set_render_style(pulp::view::WidgetRenderStyle::standard);

    // The renderer's other travel axis uses the same hoisted art contract.
    // This catches accidentally hard-coding the vertical Y calculation while
    // the capture pass itself accepts both fader orientations.
    fader->set_orientation(pulp::view::Fader::Orientation::horizontal);
    fader->set_bounds({0.0f, 0.0f, 100.0f, 20.0f});
    auto indicator_x = [&](float value) {
        fader->set_value(value);
        pulp::canvas::RecordingCanvas canvas;
        fader->paint(canvas);
        std::vector<pulp::canvas::DrawCommand> images;
        for (const auto& command : canvas.commands())
            if (command.type == pulp::canvas::DrawCommand::Type::draw_image)
                images.push_back(command);
        REQUIRE(images.size() == 2);
        CHECK(images.front().text == node.attributes.at("fader_body_asset_path"));
        CHECK(images.back().text ==
              node.attributes.at("fader_indicator_asset_path"));
        CHECK(images.back().f[2] == Catch::Approx(90.0f));
        CHECK(images.back().f[3] == Catch::Approx(2.0f));
        REQUIRE(canvas.count(
                    pulp::canvas::DrawCommand::Type::fill_rounded_rect) == 2);
        return images.back().f[0];
    };
    const float left = indicator_x(0.0f);
    const float centre = indicator_x(0.5f);
    const float right = indicator_x(1.0f);
    CHECK(left < centre);
    CHECK(centre < right);

    auto& generated_fader = ir.root.children.front();
    generated_fader.style.width = 100.0f;
    generated_fader.style.height = 20.0f;
    generated_fader.attributes["fader_indicator_cross"] =
        "0.5); maliciousCall(); //";

    pulp::view::CodeGenOptions options;
    options.mode = pulp::view::CodeGenMode::bridge_native_js;
    const auto js = pulp::view::generate_pulp_js(ir, options);
    CHECK(js.find("setFaderCapturedArt") != std::string::npos);
    CHECK(js.find("setFaderSkin") != std::string::npos);
    CHECK(js.find("'horizontal'") != std::string::npos);
    CHECK(js.find("maliciousCall") == std::string::npos);
    CHECK(js.find(node.attributes.at("fader_indicator_asset_path")) !=
          std::string::npos);

    options.mode = pulp::view::CodeGenMode::web_compat;
    const auto web_js = pulp::view::generate_pulp_js(ir, options);
    CHECK(web_js.find("setOrientation") != std::string::npos);
    CHECK(web_js.find("'horizontal'") != std::string::npos);
    CHECK(web_js.find("maliciousCall") == std::string::npos);
}

TEST_CASE("a static browser fader keeps authored track and ticks while its thumb moves",
          "[import-design][browser-capture][fader][indicator][static-track]") {
    TempDirectory temp;
    const auto panel = static_horizontal_fader_panel();
    const auto capture = temp.root / "browser.png";
    write_png(capture, panel);

    DesignIR ir;
    ir.source = pulp::view::DesignSource::html;
    ir.root.type = "frame";
    ir.root.style.width = static_cast<float>(panel.width) * 0.5f;
    ir.root.style.height = static_cast<float>(panel.height) * 0.5f;
    ir.root.children.push_back(declared_static_horizontal_fader());

    std::string error;
    REQUIRE(pulp::import_design::apply_browser_capture_control_sprites(
                ir, capture, temp.root / "sprites", &error) == 1);
    CHECK(error.empty());
    const auto& node = ir.root.children.front();
    REQUIRE(node.attributes.at("fader_body_includes_static_track") == "1");

    const auto body = read_png(node.attributes.at("fader_body_asset_path"));
    REQUIRE(body.valid());
    // The authored inset 1px track and a wide tick away from the old thumb survive the
    // body crop exactly; only the declared indicator rectangle is inpainted.
    CHECK(pixel_at(body, 10, kStaticFaderHeight / 2)[0] == 0);
    CHECK(pixel_at(body, 20, 10)[0] == 0);
    // The old thumb and its transform fringe are gone away from the track.
    CHECK(pixel_at(body, kStaticIndicatorLeft - kStaticFaderLeft - 1,
                   kStaticIndicatorTop - kStaticFaderTop + 2)[0] == 255);

    const auto indicator = read_png(node.attributes.at("fader_indicator_asset_path"));
    REQUIRE(indicator.valid());
    CHECK(indicator.width == kStaticIndicatorWidth + 2);
    CHECK(indicator.height == kStaticIndicatorHeight + 2);
    // The explicitly declared moving footprint includes the transform fringe,
    // so the same pixels removed from the body travel with the live thumb.
    CHECK(pixel_at(indicator, 0, 2)[0] == 128);
    CHECK(pixel_at(indicator, 0, 2)[3] == 255);
    CHECK(pixel_at(indicator, indicator.width - 1, 2)[0] == 128);
    CHECK(pixel_at(indicator, indicator.width - 1, 2)[3] == 255);

    auto root = pulp::view::build_native_view_tree(ir, {}, {});
    REQUIRE(root != nullptr);
    auto* fader = dynamic_cast<pulp::view::Fader*>(root->child_at(0));
    REQUIRE(fader != nullptr);
    REQUIRE(fader->captured_body_includes_static_track());
    fader->set_bounds({0.0f, 0.0f, static_cast<float>(kStaticFaderWidth) * 0.5f,
                       static_cast<float>(kStaticFaderHeight) * 0.5f});

    auto indicator_x = [&](float value) {
        fader->set_value(value);
        pulp::canvas::RecordingCanvas canvas;
        fader->paint(canvas);
        std::vector<pulp::canvas::DrawCommand> images;
        for (const auto& command : canvas.commands())
            if (command.type == pulp::canvas::DrawCommand::Type::draw_image)
                images.push_back(command);
        REQUIRE(images.size() == 2);
        // No generic track/fill is painted over the captured track and ticks.
        CHECK(canvas.count(pulp::canvas::DrawCommand::Type::fill_rounded_rect) == 0);
        CHECK(images.back().f[2] == Catch::Approx(10.0f));
        CHECK(images.back().f[3] == Catch::Approx(16.0f));
        return images.back().f[0];
    };
    const float left = indicator_x(0.0f);
    const float middle = indicator_x(0.5f);
    const float right = indicator_x(1.0f);
    CHECK(left < middle);
    CHECK(middle < right);
    CHECK(left == Catch::Approx(0.0f));
    CHECK(middle == Catch::Approx(45.0f));
    CHECK(right == Catch::Approx(90.0f));

    pulp::view::CodeGenOptions options;
    options.mode = pulp::view::CodeGenMode::bridge_native_js;
    const auto js = pulp::view::generate_pulp_js(ir, options);
    CHECK(js.find("setFaderCapturedArt") != std::string::npos);
    CHECK(js.find(", true);") != std::string::npos);

    // A stale/missing body asset must degrade to a working stock fader rather
    // than trusting the metadata flag and painting only a floating thumb.
    pulp::view::Fader missing_body;
    missing_body.set_orientation(pulp::view::Fader::Orientation::horizontal);
    missing_body.set_bounds({0.0f, 0.0f, 100.0f, 20.0f});
    missing_body.set_captured_art({}, {}, 0.5f, 0.0f, 0.0f, 100.0f, 20.0f, true);
    pulp::canvas::RecordingCanvas fallback_canvas;
    missing_body.paint(fallback_canvas);
    CHECK(fallback_canvas.count(pulp::canvas::DrawCommand::Type::fill_rounded_rect) > 0);

    // A loaded static body is still incomplete without its moving indicator.
    // Do not paint the authored track before falling back to stock chrome.
    auto partial_body = std::make_shared<pulp::view::SpriteStrip>();
    partial_body->load_from_file(
        node.attributes.at("fader_body_asset_path"), body.width, body.height, 1,
        pulp::view::SpriteStrip::Orientation::vertical);
    pulp::view::Fader missing_indicator;
    missing_indicator.set_orientation(pulp::view::Fader::Orientation::horizontal);
    missing_indicator.set_bounds({0.0f, 0.0f, 100.0f, 20.0f});
    missing_indicator.set_captured_art(
        std::move(partial_body), {}, 0.5f, 0.0f, 0.0f, 100.0f, 20.0f, true);
    pulp::canvas::RecordingCanvas partial_canvas;
    missing_indicator.paint(partial_canvas);
    CHECK(partial_canvas.count(pulp::canvas::DrawCommand::Type::draw_image) == 0);
    CHECK(partial_canvas.count(
              pulp::canvas::DrawCommand::Type::fill_rounded_rect) > 0);
}

TEST_CASE("an undeclared uniform fader keeps the live chrome path",
          "[import-design][browser-capture][fader][indicator][static-track]") {
    TempDirectory temp;
    const auto panel = static_horizontal_fader_panel();
    const auto capture = temp.root / "browser.png";
    write_png(capture, panel);

    DesignIR ir;
    ir.source = pulp::view::DesignSource::html;
    ir.root.type = "frame";
    ir.root.style.width = static_cast<float>(panel.width) * 0.5f;
    ir.root.style.height = static_cast<float>(panel.height) * 0.5f;
    auto fader = declared_static_horizontal_fader();
    fader.attributes.erase("browser_fader_static_track_declared");
    ir.root.children.push_back(std::move(fader));

    std::string error;
    REQUIRE(pulp::import_design::apply_browser_capture_control_sprites(
                ir, capture, temp.root / "sprites", &error) == 1);
    CHECK(error.empty());
    CHECK(ir.root.children.front().attributes.count("fader_body_includes_static_track") == 0);
}

TEST_CASE("a low-contrast off-centre fill keeps the live fader chrome path",
          "[import-design][browser-capture][fader][indicator][dynamic-track]") {
    TempDirectory temp;
    const auto panel = off_centre_fill_fader_panel();
    const auto capture = temp.root / "browser.png";
    write_png(capture, panel);

    DesignIR ir;
    ir.source = pulp::view::DesignSource::html;
    ir.root.type = "frame";
    ir.root.style.width = static_cast<float>(panel.width) * 0.5f;
    ir.root.style.height = static_cast<float>(panel.height) * 0.5f;
    auto fader = declared_static_horizontal_fader();
    fader.attributes.erase("browser_fader_static_track_declared");
    ir.root.children.push_back(std::move(fader));

    std::string error;
    REQUIRE(pulp::import_design::apply_browser_capture_control_sprites(
                ir, capture, temp.root / "sprites", &error) == 1);
    CHECK(error.empty());
    CHECK(ir.root.children.front().attributes.count("fader_body_includes_static_track") == 0);
}

TEST_CASE("a centre-origin fill keeps the live fader chrome path",
          "[import-design][browser-capture][fader][indicator][dynamic-track]") {
    TempDirectory temp;
    const auto panel = centre_origin_fill_fader_panel();
    const auto capture = temp.root / "browser.png";
    write_png(capture, panel);

    DesignIR ir;
    ir.source = pulp::view::DesignSource::html;
    ir.root.type = "frame";
    ir.root.style.width = static_cast<float>(panel.width) * 0.5f;
    ir.root.style.height = static_cast<float>(panel.height) * 0.5f;
    auto fader = declared_centre_origin_fill_fader();
    fader.attributes.erase("browser_fader_static_track_declared");
    ir.root.children.push_back(std::move(fader));

    std::string error;
    REQUIRE(pulp::import_design::apply_browser_capture_control_sprites(
                ir, capture, temp.root / "sprites", &error) == 1);
    CHECK(error.empty());
    CHECK(ir.root.children.front().attributes.count("fader_body_includes_static_track") == 0);
}

TEST_CASE("a declared indicator that cannot be honoured fails the import",
          "[import-design][browser-capture][knob][indicator]") {
    // The failure mode this forecloses is the quiet one: a declared pointer
    // that produces no sprite still renders a perfectly good static panel, and
    // every pixel gate stays green while the control is dead.
    TempDirectory temp;
    const auto missing = temp.root / "absent.png";

    DesignIR ir;
    ir.root.type = "frame";
    ir.root.children.push_back(declared_knob());

    std::string error;
    CHECK(pulp::import_design::apply_browser_capture_control_sprites(
              ir, missing, temp.root / "sprites", &error) == 0);
    CHECK_FALSE(error.empty());
    CHECK(ir.root.children[0].attributes.count("asset_path") == 0);
}

TEST_CASE("the imported knob's indicator moves with its parameter",
          "[import-design][browser-capture][knob][indicator][movement]") {
    // The assertion the whole slice exists for. Everything upstream can be
    // correct-looking and still ship a frozen pointer, and a one-frame
    // similarity score cannot see the difference: the static capture scores
    // just as well as the live control on the frame where they agree.
    //
    // So drive the bound value across its range and require the drawn geometry
    // to MOVE -- monotonically, along the arc, at a fixed radius.
    TempDirectory temp;
    const auto capture = temp.root / "browser.png";
    write_png(capture, synthetic_panel());

    DesignIR ir;
    ir.root.type = "frame";
    ir.root.stable_anchor_id = "root";
    ir.root.style.width = static_cast<float>(kPanelWidth) * 0.5f;
    ir.root.style.height = static_cast<float>(kPanelHeight) * 0.5f;
    ir.root.children.push_back(declared_knob());

    std::string error;
    REQUIRE(pulp::import_design::apply_browser_capture_control_sprites(
                ir, capture, temp.root / "sprites", &error) == 1);

    auto root = pulp::view::build_native_view_tree(ir, {}, {});
    REQUIRE(root != nullptr);
    REQUIRE(root->child_count() == 1);
    auto* knob = dynamic_cast<pulp::view::Knob*>(root->child_at(0));
    REQUIRE(knob != nullptr);
    // The producer's attribute names are the ones the materializer already
    // reads -- no renderer or materializer branch was added for this lane.
    REQUIRE(knob->has_captured_indicator());
    CHECK(knob->captured_indicator_r_out() == Catch::Approx(0.845f));
    CHECK(knob->captured_indicator_phase_rad() == Catch::Approx(0.4f));

    // Paint in the box the design gave it. Set explicitly rather than left to
    // whatever a layout pass happens to produce, so the geometry asserted below
    // is the pointer's, not the layout's.
    const float logical_size = static_cast<float>(kDialSize) * 0.5f;
    knob->set_bounds({0.0f, 0.0f, logical_size, logical_size});
    const float logical_half = logical_size * 0.5f;
    const float centre = logical_half;

    std::vector<Segment> frames;
    std::vector<float> angles;
    float previous_angle = 0.0f;
    for (const float value : {0.0f, 0.25f, 0.5f, 0.75f, 1.0f}) {
        const auto segment = pointer_segment(*knob, value);
        REQUIRE(segment.found);
        float unwrapped = std::atan2(segment.y1 - centre, segment.x1 - centre);
        // The sweep spans 270 degrees, so atan2's branch cut falls inside it.
        // Unwrap into one continuous run before comparing, or the wrap alone
        // reads as a step backwards.
        if (!frames.empty()) {
            while (unwrapped - previous_angle > 3.1415927f)
                unwrapped -= 6.2831853f;
            while (unwrapped - previous_angle < -3.1415927f)
                unwrapped += 6.2831853f;
        }
        if (!frames.empty()) {
            // MOVED, not merely "differs somewhere": the pointer's far end must
            // advance along the arc every step. A constant would satisfy any
            // single threshold, and a jitter would satisfy "not equal".
            CHECK(unwrapped > previous_angle + 0.5f);
        }
        // ...and it is a sweep, not a stretch: the far end stays on the radius
        // the design declared.
        const float radius = std::sqrt(
            (segment.x1 - centre) * (segment.x1 - centre) +
            (segment.y1 - centre) * (segment.y1 - centre));
        CHECK(radius == Catch::Approx(0.845f * logical_half).margin(0.01f));
        previous_angle = unwrapped;
        angles.push_back(unwrapped);
        frames.push_back(segment);
    }
    // Calibration translates the entire arc; it must not shorten or freeze
    // it. End-to-end movement remains exactly the standard 270 degrees.
    REQUIRE(angles.size() == 5);
    CHECK(angles.back() - angles.front() ==
          Catch::Approx(4.712389f).margin(0.001f));
    // Every frame is a distinct position — no two values share a pointer.
    for (std::size_t i = 0; i + 1 < frames.size(); ++i)
        for (std::size_t j = i + 1; j < frames.size(); ++j)
            CHECK(std::abs(frames[i].x1 - frames[j].x1) +
                      std::abs(frames[i].y1 - frames[j].y1) > 1.0f);
}

TEST_CASE("the producer's pointer reaches the scripted path too",
          "[import-design][browser-capture][knob][indicator][codegen]") {
    // The materializer is not the only consumer. `--emit js` — the CLI default,
    // and what a generated project ships — goes through design_codegen and the
    // WidgetBridge, which had no way to receive a captured pointer at all. A
    // knob that gained a disc sprite but no pointer call renders the generic
    // white notch: the SAME import would look right materialized and wrong
    // scripted, and only a screenshot of the scripted path would say so.
    TempDirectory temp;
    write_png(temp.root / "browser.png", synthetic_panel());

    DesignIR ir;
    ir.source = pulp::view::DesignSource::html;
    ir.root.type = "frame";
    ir.root.name = "Panel";
    ir.root.style.width = static_cast<float>(kPanelWidth) * 0.5f;
    auto knob = declared_knob();
    knob.name = "Cutoff";
    ir.root.children.push_back(std::move(knob));

    std::string error;
    REQUIRE(pulp::import_design::apply_browser_capture_control_sprites(
                ir, temp.root / "browser.png", temp.root / "sprites",
                &error) == 1);

    pulp::view::CodeGenOptions options;
    options.mode = pulp::view::CodeGenMode::bridge_native_js;
    options.use_silver_knobs = false;
    const auto js = pulp::view::generate_pulp_js(ir, options);
    REQUIRE(js.find("setKnobSpriteStrip('Cutoff") != std::string::npos);
    REQUIRE(js.find("setKnobCapturedIndicator('Cutoff") != std::string::npos);
    REQUIRE(js.find("0.845") != std::string::npos);
    REQUIRE(js.find("0.4") != std::string::npos);
    // Hex, because the bridge parses hex only and quietly substitutes
    // near-white for anything else -- a colour that survives the materializer
    // can still be dropped here.
    REQUIRE(js.find("'#fa1e1e'") != std::string::npos);
}

TEST_CASE("a knob with no recovered pointer emits no pointer call",
          "[import-design][browser-capture][knob][indicator][codegen]") {
    // The synthetic notch stays the fallback. Emitting a captured-pointer call
    // for a knob that has none would install a zero-length stroke and suppress
    // the notch, leaving an imported knob with no visible indicator at all.
    DesignIR ir;
    ir.source = pulp::view::DesignSource::html;
    ir.root.type = "frame";
    ir.root.name = "Panel";

    IRNode knob;
    knob.type = "knob";
    knob.name = "Plain";
    knob.audio_widget = AudioWidgetType::knob;
    knob.style.width = 64.0f;
    knob.style.height = 64.0f;
    knob.attributes["asset_path"] = "/tmp/synthetic-knob-body.png";
    knob.attributes["png_natural_w"] = "128";
    knob.attributes["png_natural_h"] = "128";
    knob.attributes["sprite_strip_frame_count"] = "1";
    ir.root.children.push_back(std::move(knob));

    pulp::view::CodeGenOptions options;
    options.mode = pulp::view::CodeGenMode::bridge_native_js;
    options.use_silver_knobs = false;
    const auto js = pulp::view::generate_pulp_js(ir, options);
    REQUIRE(js.find("setKnobSpriteStrip('Plain") != std::string::npos);
    REQUIRE(js.find("setKnobCapturedIndicator") == std::string::npos);
}

TEST_CASE("browser control overlays do not invent stale companion labels",
          "[import-design][browser-capture][fader][codegen]") {
    DesignIR ir;
    ir.source = pulp::view::DesignSource::html;
    ir.root.type = "frame";
    ir.root.name = "Panel";

    auto fader = declared_fader();
    fader.name = "01 RATE 0.34";
    fader.style.width = 240.0f;
    fader.style.height = 32.0f;
    fader.audio_label = "VALUE";
    fader.audio_min = 0.0f;
    fader.audio_max = 1.0f;
    fader.audio_default = 0.34f;
    fader.has_audio_range = true;
    fader.attributes["pulpRouteId"] = "capture:param_1:0";
    fader.attributes["binding"] = "param_1";
    fader.attributes["fader_body_asset_path"] = "assets/body.png";
    fader.attributes["fader_indicator_asset_path"] = "assets/thumb.png";
    ir.root.children.push_back(std::move(fader));

    pulp::view::CodeGenOptions options;
    options.mode = pulp::view::CodeGenMode::bridge_native_js;
    options.use_silver_knobs = true;
    const auto js = pulp::view::generate_pulp_js(ir, options);

    REQUIRE(js.find("createFader('_01_RATE_0_34") != std::string::npos);
    REQUIRE(js.find("'horizontal'") != std::string::npos);
    REQUIRE(js.find("'vertical'") == std::string::npos);
    REQUIRE(js.find("setFaderCapturedArt('_01_RATE_0_34") != std::string::npos);
    REQUIRE(js.find("bindWidgetToParam('_01_RATE_0_340', 'param_1')") !=
            std::string::npos);
    REQUIRE(js.find("_01_RATE_0_34_lbl") == std::string::npos);
    REQUIRE(js.find("_01_RATE_0_34_val") == std::string::npos);
    REQUIRE(js.find("_01_RATE_0_34_sub") == std::string::npos);
    REQUIRE(js.find("setFlex('root', 'height', 52") == std::string::npos);
    REQUIRE(js.find("setFlex('root', 'min_width', 240") == std::string::npos);

    options.mode = pulp::view::CodeGenMode::web_compat;
    const auto web_js = pulp::view::generate_pulp_js(ir, options);
    REQUIRE(web_js.find("setLabel(_01_RATE_0_340._id, ' ')") !=
            std::string::npos);
    REQUIRE(web_js.find("setLabel(_01_RATE_0_340._id, 'VALUE')") ==
            std::string::npos);
}
