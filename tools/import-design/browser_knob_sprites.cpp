#include "browser_knob_sprites.hpp"

#include "import_png_codec.hpp"

#include <pulp/runtime/crypto.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <functional>
#include <iterator>
#include <optional>
#include <vector>

namespace pulp::import_design {

namespace fs = std::filesystem;

namespace {

using pulp::view::IRNode;

/// One of the "x,y,w,h" hand-off rectangles, in capture-PNG pixels.
struct PixelRect {
    int x = 0;
    int y = 0;
    int w = 0;
    int h = 0;
};

std::optional<PixelRect> parse_pixel_rect(const std::string& text) {
    PixelRect rect;
    int* fields[4] = {&rect.x, &rect.y, &rect.w, &rect.h};
    std::size_t cursor = 0;
    for (int i = 0; i < 4; ++i) {
        if (cursor > text.size()) return std::nullopt;
        std::size_t consumed = 0;
        int value = 0;
        try {
            value = std::stoi(text.substr(cursor), &consumed);
        } catch (const std::exception&) {
            return std::nullopt;
        }
        *fields[i] = value;
        cursor += consumed;
        if (i < 3) {
            if (cursor >= text.size() || text[cursor] != ',') return std::nullopt;
            ++cursor;
        }
    }
    if (cursor != text.size()) return std::nullopt;
    if (rect.w <= 0 || rect.h <= 0) return std::nullopt;
    return rect;
}

std::optional<std::string> attribute(const IRNode& node, const char* key) {
    const auto it = node.attributes.find(key);
    if (it == node.attributes.end() || it->second.empty()) return std::nullopt;
    return it->second;
}

ImportPngImage crop(const ImportPngImage& source, const PixelRect& rect) {
    ImportPngImage out;
    out.width = rect.w;
    out.height = rect.h;
    out.rgba.assign(static_cast<std::size_t>(rect.w) * rect.h * 4, 0);
    for (int y = 0; y < rect.h; ++y) {
        const int sy = rect.y + y;
        if (sy < 0 || sy >= source.height) continue;
        for (int x = 0; x < rect.w; ++x) {
            const int sx = rect.x + x;
            if (sx < 0 || sx >= source.width) continue;
            const std::uint8_t* src =
                &source.rgba[(static_cast<std::size_t>(sy) * source.width + sx) * 4];
            std::uint8_t* dst =
                &out.rgba[(static_cast<std::size_t>(y) * rect.w + x) * 4];
            std::copy(src, src + 4, dst);
        }
    }
    return out;
}

/// Erase the pointer the design BAKED into this crop, by rotation.
///
/// The capture froze the pointer at the value the design was authored with. We
/// are about to draw a live one, so leaving the baked pixels would show two
/// pointers — the same "stuck second line" the Figma lane erases with
/// `clean_baked_knob_indicator`, arrived at differently because the shape and
/// position here are declared rather than inferred from a hairline layer.
///
/// A dial face is rotationally symmetric about its centre EXCEPT for the
/// pointer, so each erased pixel is replaced with the median of the pixels at
/// the same radius under several rotations. That reproduces rings, radial
/// gradients and tick repeats exactly, and degrades to a plausible mid-tone
/// under directional lighting rather than smearing a neighbouring feature
/// across the hole. Sampling reads the UNMODIFIED crop, so an erase never feeds
/// on its own output.
///
/// Samples that land outside the crop, or back inside the pointer itself, are
/// dropped; a pixel with too few surviving samples is left alone rather than
/// guessed at from one reading.
void erase_declared_pointer(ImportPngImage& image, const PixelRect& pointer) {
    const ImportPngImage source = image;
    const double centre_x = image.width * 0.5;
    const double centre_y = image.height * 0.5;
    constexpr int kRotations = 8;
    // An odd divisor keeps every rotation off the pointer's own angle and off
    // the diametrically opposite point, where a two-ended pointer would sit.
    constexpr double kStep = 2.0 * 3.14159265358979323846 / (kRotations + 1);
    constexpr int kMinimumSamples = 3;

    const int x0 = std::max(0, pointer.x);
    const int y0 = std::max(0, pointer.y);
    const int x1 = std::min(image.width, pointer.x + pointer.w);
    const int y1 = std::min(image.height, pointer.y + pointer.h);

    const auto inside_pointer = [&](int x, int y) {
        return x >= x0 && x < x1 && y >= y0 && y < y1;
    };

    for (int y = y0; y < y1; ++y) {
        for (int x = x0; x < x1; ++x) {
            const double dx = x + 0.5 - centre_x;
            const double dy = y + 0.5 - centre_y;
            const double radius = std::sqrt(dx * dx + dy * dy);
            const double angle = std::atan2(dy, dx);
            std::array<std::vector<std::uint8_t>, 4> samples;
            for (int k = 1; k <= kRotations; ++k) {
                const double rotated = angle + k * kStep;
                const int sx = static_cast<int>(
                    std::floor(centre_x + radius * std::cos(rotated)));
                const int sy = static_cast<int>(
                    std::floor(centre_y + radius * std::sin(rotated)));
                if (sx < 0 || sx >= source.width || sy < 0 || sy >= source.height)
                    continue;
                if (inside_pointer(sx, sy)) continue;
                const std::uint8_t* p =
                    &source.rgba[(static_cast<std::size_t>(sy) * source.width + sx) * 4];
                for (int c = 0; c < 4; ++c) samples[c].push_back(p[c]);
            }
            if (static_cast<int>(samples[0].size()) < kMinimumSamples) continue;
            std::uint8_t* dst =
                &image.rgba[(static_cast<std::size_t>(y) * image.width + x) * 4];
            for (int c = 0; c < 4; ++c) {
                auto& channel = samples[c];
                const auto middle = channel.begin() + channel.size() / 2;
                std::nth_element(channel.begin(), middle, channel.end());
                dst[c] = *middle;
            }
        }
    }
}

/// Remove the value-dependent fader chrome from the static control crop.
///
/// The declared thumb identifies the cross-axis band occupied by the live
/// track/fill/thumb assembly. Reconstruct that band from pixels on either side
/// of it for the full travel length; otherwise the capture's fill remains
/// frozen beneath the native value-driven fill at its authored value.
void erase_declared_fader_chrome(ImportPngImage& image,
                                 const ImportPngImage& panel,
                                 const PixelRect& crop_rect,
                                 const PixelRect& indicator,
                                 bool horizontal) {
    const int x0 = std::max(0, indicator.x - crop_rect.x);
    const int y0 = std::max(0, indicator.y - crop_rect.y);
    const int x1 = std::min(image.width,
                            indicator.x + indicator.w - crop_rect.x);
    const int y1 = std::min(image.height,
                            indicator.y + indicator.h - crop_rect.y);
    if (x0 >= x1 || y0 >= y1) return;

    const int erase_x0 = horizontal ? 0 : x0;
    const int erase_y0 = horizontal ? y0 : 0;
    const int erase_x1 = horizontal ? image.width : x1;
    const int erase_y1 = horizontal ? y1 : image.height;
    for (int y = erase_y0; y < erase_y1; ++y) {
        for (int x = erase_x0; x < erase_x1; ++x) {
            const int panel_x = crop_rect.x + x;
            const int panel_y = crop_rect.y + y;
            const int before_x = horizontal ? panel_x : indicator.x - 1;
            const int before_y = horizontal ? indicator.y - 1 : panel_y;
            const int after_x = horizontal ? panel_x
                                           : indicator.x + indicator.w;
            const int after_y = horizontal ? indicator.y + indicator.h
                                           : panel_y;
            const bool have_before = before_x >= 0 && before_y >= 0 &&
                                     before_x < panel.width && before_y < panel.height;
            const bool have_after = after_x >= 0 && after_y >= 0 &&
                                    after_x < panel.width && after_y < panel.height;
            if (!have_before && !have_after) continue;

            const std::uint8_t* before = have_before
                ? &panel.rgba[(static_cast<std::size_t>(before_y) * panel.width + before_x) * 4]
                : nullptr;
            const std::uint8_t* after = have_after
                ? &panel.rgba[(static_cast<std::size_t>(after_y) * panel.width + after_x) * 4]
                : nullptr;
            const float t = horizontal
                ? static_cast<float>(y - y0 + 1) / static_cast<float>(y1 - y0 + 1)
                : static_cast<float>(x - x0 + 1) / static_cast<float>(x1 - x0 + 1);
            std::uint8_t* dst =
                &image.rgba[(static_cast<std::size_t>(y) * image.width + x) * 4];
            for (int c = 0; c < 4; ++c) {
                if (before && after) {
                    dst[c] = static_cast<std::uint8_t>(std::lround(
                        static_cast<float>(before[c]) * (1.0f - t) +
                        static_cast<float>(after[c]) * t));
                } else {
                    dst[c] = (before ? before : after)[c];
                }
            }
        }
    }
}

/// Remove the background surrounding a non-rectangular thumb from its crop.
/// Browser screenshots are opaque, so a plain rectangle crop would move a
/// little patch of the old track with the thumb. The cleaned body is our best
/// estimate of those background pixels: pixels that agree with it within a
/// small capture/rounding tolerance become transparent, while authored fill,
/// border, antialiasing and shadow remain intact.
void isolate_fader_indicator(ImportPngImage& indicator,
                             const ImportPngImage& panel,
                             const PixelRect& absolute_indicator,
                             bool horizontal) {
    constexpr int kBackgroundTolerance = 4;
    for (int y = 0; y < indicator.height; ++y) {
        for (int x = 0; x < indicator.width; ++x) {
            const int panel_x = absolute_indicator.x + x;
            const int panel_y = absolute_indicator.y + y;
            const int before_x = horizontal ? absolute_indicator.x - 1 : panel_x;
            const int before_y = horizontal ? panel_y : absolute_indicator.y - 1;
            const int after_x = horizontal
                ? absolute_indicator.x + absolute_indicator.w : panel_x;
            const int after_y = horizontal
                ? panel_y : absolute_indicator.y + absolute_indicator.h;
            const bool have_before = before_x >= 0 && before_y >= 0 &&
                                     before_x < panel.width && before_y < panel.height;
            const bool have_after = after_x >= 0 && after_y >= 0 &&
                                    after_x < panel.width && after_y < panel.height;
            if (!have_before && !have_after) continue;
            auto* pixel = &indicator.rgba[
                (static_cast<std::size_t>(y) * indicator.width + x) * 4];
            const auto* before = have_before ? &panel.rgba[
                (static_cast<std::size_t>(before_y) * panel.width + before_x) * 4] : nullptr;
            const auto* after = have_after ? &panel.rgba[
                (static_cast<std::size_t>(after_y) * panel.width + after_x) * 4] : nullptr;
            const float t = horizontal
                ? static_cast<float>(x + 1) / static_cast<float>(indicator.width + 1)
                : static_cast<float>(y + 1) / static_cast<float>(indicator.height + 1);
            int difference = 0;
            for (int c = 0; c < 3; ++c) {
                const int background = before && after
                    ? static_cast<int>(std::lround(before[c] * (1.0f - t) + after[c] * t))
                    : static_cast<int>((before ? before : after)[c]);
                difference = std::max(
                    difference, std::abs(static_cast<int>(pixel[c]) -
                                         background));
            }
            if (difference <= kBackgroundTolerance) pixel[3] = 0;
        }
    }
}

bool write_bytes(const fs::path& path, const std::vector<std::uint8_t>& bytes) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) return false;
    out.write(reinterpret_cast<const char*>(bytes.data()),
              static_cast<std::streamsize>(bytes.size()));
    return static_cast<bool>(out);
}

}  // namespace

int apply_browser_capture_control_sprites(
    pulp::view::DesignIR& ir,
    const fs::path& capture_png,
    const fs::path& sprite_directory,
    std::string* error) {
    const auto fail = [&](std::string message) {
        if (error != nullptr) *error = std::move(message);
        return 0;
    };

    // Collect first so an unreadable capture is only an error when a knob
    // actually declared a pointer. A panel with no declared indicators must
    // import exactly as it does today, capture decode or not.
    std::vector<IRNode*> pending;
    std::function<void(IRNode&)> collect = [&](IRNode& node) {
        if ((node.audio_widget == pulp::view::AudioWidgetType::knob ||
             node.audio_widget == pulp::view::AudioWidgetType::fader) &&
            attribute(node, "browser_sprite_crop_px") &&
            attribute(node, "browser_sprite_indicator_px") &&
            !attribute(node, "asset_path"))
            pending.push_back(&node);
        for (auto& child : node.children) collect(child);
    };
    collect(ir.root);
    if (pending.empty()) return 0;

    std::ifstream capture(capture_png, std::ios::binary);
    if (!capture)
        return fail("could not open the browser capture for control sprites: " +
                    capture_png.string());
    const std::vector<std::uint8_t> bytes{
        std::istreambuf_iterator<char>(capture),
        std::istreambuf_iterator<char>()};
    const auto panel = decode_png_rgba(bytes.data(), bytes.size());
    if (!panel.valid())
        return fail("could not decode the browser capture for control sprites: " +
                    capture_png.string());

    std::error_code ec;
    fs::create_directories(sprite_directory, ec);
    if (ec)
        return fail("could not create the control sprite directory " +
                    sprite_directory.string() + ": " + ec.message());

    int skinned = 0;
    for (IRNode* node : pending) {
        const auto crop_rect =
            parse_pixel_rect(*attribute(*node, "browser_sprite_crop_px"));
        const auto indicator_rect =
            parse_pixel_rect(*attribute(*node, "browser_sprite_indicator_px"));
        if (!crop_rect || !indicator_rect)
            return fail("malformed browser control sprite geometry on control " +
                        node->stable_anchor_id.value_or("<unnamed>"));

        const bool is_knob =
            node->audio_widget == pulp::view::AudioWidgetType::knob;
        const bool horizontal = !is_knob && crop_rect->w >= crop_rect->h;
        PixelRect body_rect = *crop_rect;
        if (!is_knob) {
            const int right = std::max(crop_rect->x + crop_rect->w,
                                       indicator_rect->x + indicator_rect->w);
            const int bottom = std::max(crop_rect->y + crop_rect->h,
                                        indicator_rect->y + indicator_rect->h);
            body_rect.x = std::min(crop_rect->x, indicator_rect->x);
            body_rect.y = std::min(crop_rect->y, indicator_rect->y);
            body_rect.w = right - body_rect.x;
            body_rect.h = bottom - body_rect.y;
        }
        auto body = crop(panel, body_rect);
        if (!body.valid())
            return fail("empty browser control sprite crop on control " +
                        node->stable_anchor_id.value_or("<unnamed>"));
        const PixelRect local_indicator{
            indicator_rect->x - body_rect.x,
            indicator_rect->y - body_rect.y,
            indicator_rect->w, indicator_rect->h};
        if (is_knob) {
            erase_declared_pointer(body, local_indicator);
        } else {
            erase_declared_fader_chrome(
                body, panel, body_rect, *indicator_rect, horizontal);
        }

        const auto encoded = encode_png_rgba(body);
        if (encoded.empty())
            return fail("could not encode the browser control sprite for control " +
                        node->stable_anchor_id.value_or("<unnamed>"));
        const auto digest =
            pulp::runtime::sha256_hex(encoded.data(), encoded.size());
        const auto path = sprite_directory /
            ((is_knob ? "knob-" : "fader-body-") + digest + ".png");
        if (!fs::exists(path) && !write_bytes(path, encoded))
            return fail("could not write the browser control sprite " + path.string());

        // generic_string() for the same reason every other asset path in this
        // pipeline uses it: the path is baked into generated JS and must use
        // '/' on every platform.
        if (is_knob)
            node->attributes["asset_path"] = path.lexically_normal().generic_string();
        else
            node->attributes["fader_body_asset_path"] =
                path.lexically_normal().generic_string();
        // The crop is the control's box at the capture's device scale, and
        // Knob::paint's no-core sprite branch renders a frame at its natural
        // size divided by a hardcoded 2. The two agree only because
        // validate_reference_geometry requires DPR 2; relax that without
        // teaching the renderer the scale and every imported disc resizes.
        node->attributes[is_knob ? "png_natural_w" : "fader_body_natural_w"] =
            std::to_string(body.width);
        node->attributes[is_knob ? "png_natural_h" : "fader_body_natural_h"] =
            std::to_string(body.height);
        // One static disc. A multi-frame strip encodes rotation in its frames
        // and gets no pointer overlay, which is the opposite of what a capture
        // needs.
        if (is_knob) {
            node->attributes["sprite_strip_frame_count"] = "1";
        } else {
            node->attributes["fader_body_origin_x"] =
                std::to_string(body_rect.x - crop_rect->x);
            node->attributes["fader_body_origin_y"] =
                std::to_string(body_rect.y - crop_rect->y);
            node->attributes["fader_control_natural_w"] =
                std::to_string(crop_rect->w);
            node->attributes["fader_control_natural_h"] =
                std::to_string(crop_rect->h);
            auto indicator = crop(panel, *indicator_rect);
            if (!indicator.valid())
                return fail("empty fader indicator crop on control " +
                            node->stable_anchor_id.value_or("<unnamed>"));
            isolate_fader_indicator(indicator, panel, *indicator_rect, horizontal);
            const auto indicator_encoded = encode_png_rgba(indicator);
            if (indicator_encoded.empty())
                return fail("could not encode the fader indicator for control " +
                            node->stable_anchor_id.value_or("<unnamed>"));
            const auto indicator_digest = pulp::runtime::sha256_hex(
                indicator_encoded.data(), indicator_encoded.size());
            const auto indicator_path = sprite_directory /
                ("fader-indicator-" + indicator_digest + ".png");
            if (!fs::exists(indicator_path) &&
                !write_bytes(indicator_path, indicator_encoded))
                return fail("could not write the fader indicator sprite " +
                            indicator_path.string());
            node->attributes["fader_indicator_asset_path"] =
                indicator_path.lexically_normal().generic_string();
            node->attributes["fader_indicator_natural_w"] =
                std::to_string(indicator.width);
            node->attributes["fader_indicator_natural_h"] =
                std::to_string(indicator.height);
            const float cross_center = horizontal
                ? static_cast<float>((indicator_rect->y - crop_rect->y) * 2 +
                                     indicator_rect->h) /
                      static_cast<float>(crop_rect->h * 2)
                : static_cast<float>((indicator_rect->x - crop_rect->x) * 2 +
                                     indicator_rect->w) /
                      static_cast<float>(crop_rect->w * 2);
            node->attributes["fader_indicator_cross"] =
                std::to_string(std::clamp(cross_center, 0.0f, 1.0f));
        }
        node->attributes.erase("browser_sprite_crop_px");
        node->attributes.erase("browser_sprite_indicator_px");
        ++skinned;
    }
    return skinned;
}

}  // namespace pulp::import_design
