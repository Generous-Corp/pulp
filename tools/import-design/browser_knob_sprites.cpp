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

bool write_bytes(const fs::path& path, const std::vector<std::uint8_t>& bytes) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) return false;
    out.write(reinterpret_cast<const char*>(bytes.data()),
              static_cast<std::streamsize>(bytes.size()));
    return static_cast<bool>(out);
}

}  // namespace

int apply_browser_capture_knob_sprites(
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
        if (node.audio_widget == pulp::view::AudioWidgetType::knob &&
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
        return fail("could not open the browser capture for knob sprites: " +
                    capture_png.string());
    const std::vector<std::uint8_t> bytes{
        std::istreambuf_iterator<char>(capture),
        std::istreambuf_iterator<char>()};
    const auto panel = decode_png_rgba(bytes.data(), bytes.size());
    if (!panel.valid())
        return fail("could not decode the browser capture for knob sprites: " +
                    capture_png.string());

    std::error_code ec;
    fs::create_directories(sprite_directory, ec);
    if (ec)
        return fail("could not create the knob sprite directory " +
                    sprite_directory.string() + ": " + ec.message());

    int skinned = 0;
    for (IRNode* node : pending) {
        const auto crop_rect =
            parse_pixel_rect(*attribute(*node, "browser_sprite_crop_px"));
        const auto pointer_rect =
            parse_pixel_rect(*attribute(*node, "browser_sprite_indicator_px"));
        if (!crop_rect || !pointer_rect)
            return fail("malformed knob sprite geometry on control " +
                        node->stable_anchor_id.value_or("<unnamed>"));

        auto sprite = crop(panel, *crop_rect);
        if (!sprite.valid())
            return fail("empty knob sprite crop on control " +
                        node->stable_anchor_id.value_or("<unnamed>"));
        erase_declared_pointer(
            sprite,
            PixelRect{pointer_rect->x - crop_rect->x,
                      pointer_rect->y - crop_rect->y,
                      pointer_rect->w, pointer_rect->h});

        const auto encoded = encode_png_rgba(sprite);
        if (encoded.empty())
            return fail("could not encode the knob sprite for control " +
                        node->stable_anchor_id.value_or("<unnamed>"));
        const auto digest =
            pulp::runtime::sha256_hex(encoded.data(), encoded.size());
        const auto path = sprite_directory / ("knob-" + digest + ".png");
        if (!fs::exists(path) && !write_bytes(path, encoded))
            return fail("could not write the knob sprite " + path.string());

        // generic_string() for the same reason every other asset path in this
        // pipeline uses it: the path is baked into generated JS and must use
        // '/' on every platform.
        node->attributes["asset_path"] = path.lexically_normal().generic_string();
        // The crop is the control's box at the capture's device scale, and
        // Knob::paint's no-core sprite branch renders a frame at its natural
        // size divided by a hardcoded 2. The two agree only because
        // validate_reference_geometry requires DPR 2; relax that without
        // teaching the renderer the scale and every imported disc resizes.
        node->attributes["png_natural_w"] = std::to_string(sprite.width);
        node->attributes["png_natural_h"] = std::to_string(sprite.height);
        // One static disc. A multi-frame strip encodes rotation in its frames
        // and gets no pointer overlay, which is the opposite of what a capture
        // needs.
        node->attributes["sprite_strip_frame_count"] = "1";
        node->attributes.erase("browser_sprite_crop_px");
        node->attributes.erase("browser_sprite_indicator_px");
        ++skinned;
    }
    return skinned;
}

}  // namespace pulp::import_design
