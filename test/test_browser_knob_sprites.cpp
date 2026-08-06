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
    CHECK(pixel_at(indicator, kFaderIndicatorWidth / 2,
                   kFaderIndicatorHeight / 2)[0] == 244);
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

    // Paint in the box the design gave it. Set explicitly rather than left to
    // whatever a layout pass happens to produce, so the geometry asserted below
    // is the pointer's, not the layout's.
    const float logical_size = static_cast<float>(kDialSize) * 0.5f;
    knob->set_bounds({0.0f, 0.0f, logical_size, logical_size});
    const float logical_half = logical_size * 0.5f;
    const float centre = logical_half;

    std::vector<Segment> frames;
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
        frames.push_back(segment);
    }
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
