#include "sprite_skins.hpp"

#include <pulp/view/widget_skin_derive.hpp>
#include <miniz.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <iterator>
#include <sstream>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace fs = std::filesystem;
using namespace pulp::view;

namespace {

// ── Minimal PNG → RGBA8 decoder ─────────────────────────────────────────────
// AssetManager::decode_png only stores raw bytes + IHDR dims (the real decode
// happens in the Skia renderer, which isn't linked in the GPU-off importer
// build). For the fader/meter skin sampler we need actual pixels, so decode
// here using miniz (already linked) for the common 8-bit, non-interlaced case.
// Returns RGBA8 row-major; empty on any unsupported/failed path (caller then
// skips skin derivation). Supports color types 2 (RGB), 6 (RGBA), and 0/4
// (grey / grey+alpha) — covers design-tool PNG exports.
struct DecodedPng {
    std::vector<uint8_t> rgba;
    int width = 0;
    int height = 0;
    bool valid() const { return !rgba.empty() && width > 0 && height > 0; }
};

static uint32_t png_be32(const uint8_t* p) {
    return (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) |
           (static_cast<uint32_t>(p[2]) << 8) | static_cast<uint32_t>(p[3]);
}

// Read a PNG's pixel dimensions from its IHDR header (width @ byte 16, height
// @ byte 20) without decoding the pixel data. Returns {0,0} on any non-PNG or
// unreadable file. Used to recover the true source aspect ratio so imported
// images/sprites are never skewed.
static std::pair<int, int> read_png_dimensions(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f.good()) return {0, 0};
    uint8_t hdr[24];
    f.read(reinterpret_cast<char*>(hdr), sizeof(hdr));
    if (f.gcount() < static_cast<std::streamsize>(sizeof(hdr))) return {0, 0};
    static const uint8_t sig[8] = {0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A};
    if (std::memcmp(hdr, sig, 8) != 0) return {0, 0};
    int w = static_cast<int>(png_be32(hdr + 16));
    int h = static_cast<int>(png_be32(hdr + 20));
    if (w <= 0 || h <= 0) return {0, 0};
    return {w, h};
}

static DecodedPng decode_png_rgba(const uint8_t* data, size_t size) {
    DecodedPng out;
    static const uint8_t sig[8] = {0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A};
    if (size < 33 || std::memcmp(data, sig, 8) != 0) return out;

    int width = static_cast<int>(png_be32(data + 16));
    int height = static_cast<int>(png_be32(data + 20));
    int bit_depth = data[24];
    int color_type = data[25];
    int interlace = data[28];
    if (width <= 0 || height <= 0 || bit_depth != 8 || interlace != 0) return out;

    int channels;
    switch (color_type) {
        case 0: channels = 1; break;  // grey
        case 2: channels = 3; break;  // RGB
        case 4: channels = 2; break;  // grey + alpha
        case 6: channels = 4; break;  // RGBA
        default: return out;
    }

    // Concatenate IDAT chunk payloads.
    std::vector<uint8_t> idat;
    size_t pos = 8;
    while (pos + 8 <= size) {
        uint32_t clen = png_be32(data + pos);
        const uint8_t* ctype = data + pos + 4;
        size_t body = pos + 8;
        if (body + clen + 4 > size) break;
        if (std::memcmp(ctype, "IDAT", 4) == 0)
            idat.insert(idat.end(), data + body, data + body + clen);
        else if (std::memcmp(ctype, "IEND", 4) == 0)
            break;
        pos = body + clen + 4;  // skip CRC
    }
    if (idat.empty()) return out;

    // Inflate. Raw filtered size = height * (1 + width*channels).
    size_t stride = static_cast<size_t>(width) * channels;
    mz_ulong raw_len = static_cast<mz_ulong>(height) * (stride + 1);
    std::vector<uint8_t> raw(raw_len);
    if (mz_uncompress(raw.data(), &raw_len, idat.data(),
                      static_cast<mz_ulong>(idat.size())) != MZ_OK)
        return out;
    if (raw_len < static_cast<mz_ulong>(height) * (stride + 1)) return out;

    // Un-filter (PNG filter types 0-4) into a contiguous channel buffer.
    std::vector<uint8_t> img(static_cast<size_t>(height) * stride);
    auto paeth = [](int a, int b, int c) {
        int p = a + b - c;
        int pa = std::abs(p - a), pb = std::abs(p - b), pc = std::abs(p - c);
        if (pa <= pb && pa <= pc) return a;
        return pb <= pc ? b : c;
    };
    for (int y = 0; y < height; ++y) {
        const uint8_t* src = raw.data() + static_cast<size_t>(y) * (stride + 1);
        uint8_t filter = src[0];
        uint8_t* row = img.data() + static_cast<size_t>(y) * stride;
        const uint8_t* prev = (y > 0) ? img.data() + static_cast<size_t>(y - 1) * stride : nullptr;
        for (size_t x = 0; x < stride; ++x) {
            int a = (x >= static_cast<size_t>(channels)) ? row[x - channels] : 0;
            int b = prev ? prev[x] : 0;
            int c = (prev && x >= static_cast<size_t>(channels)) ? prev[x - channels] : 0;
            int v = src[1 + x];
            switch (filter) {
                case 0: break;
                case 1: v += a; break;
                case 2: v += b; break;
                case 3: v += (a + b) / 2; break;
                case 4: v += paeth(a, b, c); break;
                default: return out;
            }
            row[x] = static_cast<uint8_t>(v & 0xFF);
        }
    }

    // Expand to RGBA8.
    out.rgba.resize(static_cast<size_t>(width) * height * 4);
    for (int i = 0; i < width * height; ++i) {
        const uint8_t* s = img.data() + static_cast<size_t>(i) * channels;
        uint8_t* d = out.rgba.data() + static_cast<size_t>(i) * 4;
        if (channels == 4) { d[0] = s[0]; d[1] = s[1]; d[2] = s[2]; d[3] = s[3]; }
        else if (channels == 3) { d[0] = s[0]; d[1] = s[1]; d[2] = s[2]; d[3] = 255; }
        else if (channels == 2) { d[0] = d[1] = d[2] = s[0]; d[3] = s[1]; }
        else { d[0] = d[1] = d[2] = s[0]; d[3] = 255; }
    }
    out.width = width;
    out.height = height;
    return out;
}

// The bbox of an image's *solid* content — pixels whose alpha is at least
// `min_alpha` (default 0.5). For a captured sprite this isolates the drawn
// geometry (a knob's disc, a shape's body) from the soft drop-shadow / glow
// that bleeds far past it. Lets the importer scale a sprite so its solid core
// fills the node's logical box while the shadow is free to extend beyond —
// the generalizable answer to "size sprites correctly without skew", derived
// from the pixels themselves. Returns false if no qualifying pixels.
struct OpaqueCore { int x = 0, y = 0, w = 0, h = 0, png_w = 0, png_h = 0; };
static bool compute_opaque_core(const std::string& path, OpaqueCore& out,
                                float min_alpha = 0.5f) {
    std::ifstream f(path, std::ios::binary);
    if (!f.good()) return false;
    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(f)),
                               std::istreambuf_iterator<char>());
    auto img = decode_png_rgba(bytes.data(), bytes.size());
    if (!img.valid()) return false;
    const uint8_t thresh = static_cast<uint8_t>(
        std::clamp(min_alpha, 0.0f, 1.0f) * 255.0f + 0.5f);
    int minx = img.width, miny = img.height, maxx = -1, maxy = -1;
    for (int y = 0; y < img.height; ++y) {
        const uint8_t* row = img.rgba.data() + static_cast<size_t>(y) * img.width * 4;
        for (int x = 0; x < img.width; ++x) {
            if (row[x * 4 + 3] >= thresh) {
                if (x < minx) minx = x;
                if (x > maxx) maxx = x;
                if (y < miny) miny = y;
                if (y > maxy) maxy = y;
            }
        }
    }
    if (maxx < minx || maxy < miny) return false;
    out.x = minx; out.y = miny;
    out.w = maxx - minx + 1; out.h = maxy - miny + 1;
    out.png_w = img.width; out.png_h = img.height;
    return true;
}

}  // namespace

namespace pulp::import_design {

void resolve_sprite_skins(pulp::view::DesignIR& ir,
                          const std::string& input_file,
                          bool use_silver_knobs,
                          bool skin_faders,
                          bool skin_meters) {
    // Sprite knob style = pixel-exact Figma reproduction that still TURNS.
    // When a recognized knob ships a captured graphic child (an image with an
    // asset_ref — e.g. a silver-knob body group), HOIST that body art onto the
    // knob node so codegen emits a single-frame sprite skin (createKnob +
    // setKnobSpriteStrip) instead of a native vector body. The engine then
    // draws the captured disc as the static body and overlays the native
    // rotating indicator notch — so the imported sprite knob stays
    // interactive (the old path DEMOTED the knob to a plain image, which was
    // pixel-faithful but dead: it never rotated). The hoisted render_bounds
    // triggers the importer's opaque-core recovery below, so the disc fits the
    // knob box at the right size (shadow bleed extends beyond). Silver mode
    // keeps the native-vector widget. Generalizable: keyed on widget kind + an
    // asset-bearing image child, no layer-name match.
    if (!use_silver_knobs) {
        std::function<void(IRNode&)> convert = [&](IRNode& n) {
            if (n.audio_widget == pulp::view::AudioWidgetType::knob) {
                // Count the captured image layers (each an asset-backed image
                // child). The single-frame sprite skin can carry exactly one,
                // and the native knob codegen is a leaf that does not emit
                // image children — so the disposition depends on the count.
                int asset_images = 0;
                IRNode* body = nullptr;
                for (auto& c : n.children) {
                    if (c.type == "image" && c.attributes.count("asset_ref")) {
                        ++asset_images;
                        if (!body) body = &c;
                    }
                }
                if (asset_images == 1) {
                    // The common separate-pointer knob: ONE captured disc
                    // image + a stroked-vector pointer (which the native
                    // rotating notch replaces). HOIST the disc onto the knob
                    // so it stays interactive.
                    n.attributes["asset_ref"] = body->attributes.at("asset_ref");
                    // Carry the child's bleed extent up so opaque-core recovery
                    // fires for the knob node (the knob frame itself usually
                    // has no render_bounds).
                    if (body->style.render_bounds && !n.style.render_bounds)
                        n.style.render_bounds = body->style.render_bounds;
                    // Single static body; a designer-supplied multi-frame strip
                    // would set its own frame count (Approach A).
                    if (!n.attributes.count("sprite_strip_frame_count"))
                        n.attributes["sprite_strip_frame_count"] = "1";
                    for (auto it = n.children.begin(); it != n.children.end(); ++it)
                        if (&*it == body) { n.children.erase(it); break; }
                } else if (asset_images > 1) {
                    // Multiple captured image layers (e.g. body + highlight +
                    // logo). A single-frame sprite skin can only hold one and
                    // the leaf knob codegen would silently drop the rest, so
                    // DEMOTE to a plain container (the pre-interactive-sprite
                    // behavior): every layer renders as an image — faithful but
                    // not turnable. Compositing the layers into one rotational
                    // strip is Approach A (follow-up). No silent layer loss.
                    n.audio_widget = pulp::view::AudioWidgetType::none;
                }
                // asset_images == 0: no captured art — leave the knob
                // recognized; it falls through to the default knob paint.
            }
            for (auto& c : n.children) convert(c);
        };
        convert(ir.root);
    }

    // Resolve asset_ref → absolute file path. For envelopes that include an
    // asset_manifest with local_path entries (figma-plugin lane), walk the IR
    // tree and stamp each node's attributes["asset_path"] with the absolute
    // resolution of asset_manifest[asset_ref].local_path against the input
    // file's parent directory. Codegen consumes attributes["asset_path"] to
    // emit setImageSource calls; nodes without a resolvable asset_ref are
    // left untouched and codegen falls through to its normal frame branch.
    if (!input_file.empty() && !ir.asset_manifest.assets.empty()) {
        std::error_code rec;
        auto base_dir = fs::weakly_canonical(fs::path(input_file), rec).parent_path();
        if (rec) base_dir = fs::path(input_file).parent_path();
        std::function<void(IRNode&)> resolve_node = [&](IRNode& n) {
            auto it = n.attributes.find("asset_ref");
            if (it != n.attributes.end() && !it->second.empty()) {
                if (auto* ref = ir.asset_manifest.resolve(it->second)) {
                    if (ref->local_path && !ref->local_path->empty()) {
                        fs::path p(*ref->local_path);
                        if (p.is_relative()) p = base_dir / p;
                        // generic_string() (forward slashes) so the path baked
                        // into the generated JS is identical on every platform.
                        // fs::path::string() would emit native backslashes on
                        // Windows, breaking both downstream consumers that
                        // expect web-style separators and the import tests that
                        // assert on "assets/...". Windows file APIs accept '/'.
                        std::string abs = p.lexically_normal().generic_string();
                        n.attributes["asset_path"] = abs;

                        // Stamp the asset's TRUE pixel dimensions from the PNG
                        // header (the manifest ships null dims). Codegen uses
                        // this to preserve the source aspect ratio — sprites
                        // must never be skewed (e.g. a knob graphic whose
                        // render_bounds claim a 1.81 aspect while the PNG is
                        // 0.87: naively sizing to render_bounds stretches it
                        // ~2× wide). Generalizable: every imported image
                        // carries its real aspect.
                        if (auto d = read_png_dimensions(abs); d.first > 0 && d.second > 0) {
                            n.attributes["png_natural_w"] = std::to_string(d.first);
                            n.attributes["png_natural_h"] = std::to_string(d.second);
                        }
                        // For a sprite that bleeds past its layout box
                        // (render_bounds present), also recover the solid-core
                        // bbox so codegen can scale the art so its core fills
                        // the box while the soft shadow extends beyond — the
                        // correct, data-driven sprite size+placement (the knob
                        // disc sits in the PNG's top with a long shadow below,
                        // so render_bounds and naive centering both misplace
                        // it). Decode is gated on render_bounds to keep the
                        // common image path header-only cheap.
                        if (n.style.render_bounds) {
                            OpaqueCore core;
                            if (compute_opaque_core(abs, core)) {
                                n.attributes["art_core_x"] = std::to_string(core.x);
                                n.attributes["art_core_y"] = std::to_string(core.y);
                                n.attributes["art_core_w"] = std::to_string(core.w);
                                n.attributes["art_core_h"] = std::to_string(core.h);
                                n.attributes["png_natural_w"] = std::to_string(core.png_w);
                                n.attributes["png_natural_h"] = std::to_string(core.png_h);
                            }
                        }

                        // Derive a value-driven skin for recognized
                        // faders/meters by SAMPLING the captured PNG (not baking
                        // it). The widget then redraws the recovered colors /
                        // gradient procedurally so the thumb/level still move
                        // with their bound value. Generalizable importer rule:
                        // reads the exported pixels, hardcodes nothing.
                        const bool want_fader =
                            (n.audio_widget == pulp::view::AudioWidgetType::fader) && skin_faders;
                        const bool want_meter =
                            (n.audio_widget == pulp::view::AudioWidgetType::meter) && skin_meters;
                        if (want_fader || want_meter) {
                            std::ifstream af(abs, std::ios::binary);
                            if (af.good()) {
                                std::vector<uint8_t> bytes(
                                    (std::istreambuf_iterator<char>(af)),
                                    std::istreambuf_iterator<char>());
                                auto img = decode_png_rgba(bytes.data(), bytes.size());
                                if (img.valid()) {
                                    auto hex = [](pulp::canvas::Color c) {
                                        auto b = [](float v) {
                                            int i = static_cast<int>(v * 255.0f + 0.5f);
                                            return i < 0 ? 0 : (i > 255 ? 255 : i);
                                        };
                                        char buf[8];
                                        std::snprintf(buf, sizeof(buf), "#%02x%02x%02x",
                                                      b(c.r), b(c.g), b(c.b));
                                        return std::string(buf);
                                    };
                                    pulp::view::SkinImage si{img.rgba.data(),
                                                             img.width, img.height};
                                    // Asset scale = captured PNG width / the
                                    // node's logical box width. The figma-plugin
                                    // exports at 2× but we derive it from the
                                    // data rather than assume, so a re-scaled
                                    // export still maps art px → logical px.
                                    // Derive the asset scale from the captured width.
                                    float node_w = n.style.width.value_or(0.0f);
                                    float asset_scale =
                                        (node_w > 0.0f && img.width > 0)
                                            ? static_cast<float>(img.width) / node_w
                                            : 2.0f;
                                    if (asset_scale <= 0.0f) asset_scale = 2.0f;
                                    auto fmt_px = [](float v) {
                                        std::ostringstream os;
                                        os << v;
                                        return os.str();
                                    };
                                    if (want_fader) {
                                        auto fs_skin = pulp::view::derive_fader_skin(si);
                                        if (fs_skin.has_track)        n.attributes["skin_track_color"]        = hex(fs_skin.track_color);
                                        if (fs_skin.has_fill)         n.attributes["skin_fill_color"]         = hex(fs_skin.fill_color);
                                        if (fs_skin.has_thumb)        n.attributes["skin_thumb_color"]        = hex(fs_skin.thumb_color);
                                        if (fs_skin.has_thumb_border) n.attributes["skin_thumb_border_color"] = hex(fs_skin.thumb_border_color);
                                        if (fs_skin.has_track_border) n.attributes["skin_track_border_color"] = hex(fs_skin.track_border_color);
                                        // Widths: the widget/thumb box uses the
                                        // derived thumb-slab width; the track is
                                        // the narrow central column. Both in
                                        // logical px (asset px / scale).
                                        if (fs_skin.has_thumb_width)
                                            n.attributes["shape_width"] = fmt_px(fs_skin.thumb_width_px / asset_scale);
                                        if (fs_skin.has_track_width)
                                            n.attributes["skin_track_width"] = fmt_px(fs_skin.track_width_px / asset_scale);
                                        // Control housing height (logical px) —
                                        // the captured PNG bakes the value-stack
                                        // text below the control, so the node's
                                        // declared height spans control+labels;
                                        // use the real control extent so the
                                        // fader isn't stretched ~2× tall.
                                        if (fs_skin.has_housing_height)
                                            n.attributes["shape_height"] = fmt_px(fs_skin.housing_height_px / asset_scale);
                                        // Captured thumb position (0..1) → initial
                                        // value-position, so the imported fader
                                        // matches where the design drew the thumb.
                                        if (fs_skin.has_thumb_position)
                                            n.attributes["skin_thumb_position"] = fmt_px(fs_skin.thumb_position);
                                    } else {
                                        auto ms = pulp::view::derive_meter_skin(si);
                                        if (ms.valid()) {
                                            std::string stops;
                                            for (size_t k = 0; k < ms.gradient.size(); ++k) {
                                                if (k) stops += ',';
                                                stops += hex(ms.gradient[k]);
                                            }
                                            n.attributes["skin_meter_gradient"] = stops;
                                            if (ms.has_background)
                                                n.attributes["skin_meter_background"] = hex(ms.background);
                                        }
                                        // Bar width → the meter's widget width
                                        // (logical px); the column min_width keeps
                                        // the box spacing so the narrow bar centers.
                                        if (ms.has_bar_width)
                                            n.attributes["shape_width"] = fmt_px(ms.bar_width_px / asset_scale);
                                        // Control housing height (logical px) —
                                        // exclude the baked value-stack text so
                                        // the meter isn't stretched ~2× tall
                                        // (which also doubles the absolute fill).
                                        if (ms.has_housing_height)
                                            n.attributes["shape_height"] = fmt_px(ms.housing_height_px / asset_scale);
                                        // Colored-bar / housing width ratio →
                                        // the meter insets its gradient bar so a
                                        // narrow colored fill reads recessed in
                                        // the wider dark housing (the capture's
                                        // structure). Scale-invariant ratio.
                                        if (ms.has_bar_fill_ratio)
                                            n.attributes["skin_meter_bar_ratio"] = fmt_px(ms.bar_fill_ratio);
                                        // Captured fill level (0..1) → initial
                                        // meter level matching the design.
                                        if (ms.has_fill_level)
                                            n.attributes["skin_fill_level"] = fmt_px(ms.fill_level);
                                    }
                                }
                            }
                        }
                    }
                    // Asset-bleed detection (generalization of the Knob
                    // sprite-strip natural-size fix). The Figma plugin
                    // exports PNGs at 2× scale; with the bounding-box
                    // origin point as both bounds, layout_size = PNG_px / 2.
                    // When the PNG pixel dims exceed twice the layout dims
                    // by ≥1.5×, the asset has drop-shadow or stroke bleed
                    // that would visibly squish if fit-to-layout-box. We
                    // stamp asset_bleed=1 so the codegen emits an explicit
                    // object-fit:none for ImageView, which honours the
                    // native pixel size centered.
                    constexpr float kExportScale = 2.0f;
                    float layout_w = n.style.width.value_or(0.0f);
                    float layout_h = n.style.height.value_or(0.0f);
                    int rw_px = ref->width.value_or(0);
                    int rh_px = ref->height.value_or(0);
                    if (rw_px > 0 && rh_px > 0 &&
                        layout_w > 0.0f && layout_h > 0.0f) {
                        float natural_w = static_cast<float>(rw_px) / kExportScale;
                        float natural_h = static_cast<float>(rh_px) / kExportScale;
                        float rw = natural_w / layout_w;
                        float rh = natural_h / layout_h;
                        if (std::max(rw, rh) >= 1.5f) {
                            n.attributes["asset_bleed"] = "1";
                        }
                    }
                }
            }
            for (auto& c : n.children) resolve_node(c);
        };
        resolve_node(ir.root);

        // Resolve bundled-font asset_ids to absolute paths so codegen can
        // emit registerFont(family, path). Same base_dir + manifest resolution
        // as the node asset-path pass above.
        for (auto& fa : ir.font_family_assets) {
            if (fa.asset_id.empty()) continue;
            if (auto* ref = ir.asset_manifest.resolve(fa.asset_id)) {
                if (ref->local_path && !ref->local_path->empty()) {
                    fs::path p(*ref->local_path);
                    if (p.is_relative()) p = base_dir / p;
                    // generic_string() for the same cross-platform reason as the
                    // node asset-path pass above: the resolved font path is baked
                    // into the generated JS (registerFont) and must use '/'.
                    fa.resolved_path = p.lexically_normal().generic_string();
                }
            }
        }
    }
}

void localize_ir_assets(pulp::view::DesignIR& ir, const std::string& output_file) {
    if (output_file.empty()) return;

    fs::path out_dir = fs::path(output_file).parent_path();
    std::error_code ec;
    if (out_dir.empty()) out_dir = fs::current_path(ec);
    const fs::path assets_dir = out_dir / "assets";

    std::unordered_map<std::string, std::string> rel_by_source;   // abs source → assets/<file>
    std::unordered_map<std::string, std::string> source_by_name;  // <file> → abs source
    bool dir_ready = false;

    auto localize = [&](std::string& path_ref) {
        if (path_ref.empty()) return;
        const fs::path src(path_ref);
        // Already-relative references (an envelope that lives in the output
        // dir, or a re-run over localized IR) are portable as-is.
        if (src.is_relative()) return;
        if (auto hit = rel_by_source.find(path_ref); hit != rel_by_source.end()) {
            path_ref = hit->second;
            return;
        }
        std::error_code lec;
        if (!fs::is_regular_file(src, lec) || lec) return;  // unresolved ref — leave for diagnostics

        // Asset filenames are content hashes in the export lanes, so clashes
        // only happen for genuinely different files; suffix those.
        std::string name = src.filename().string();
        for (int n = 2;; ++n) {
            auto used = source_by_name.find(name);
            if (used == source_by_name.end() || used->second == path_ref) break;
            name = src.stem().string() + "-" + std::to_string(n)
                 + src.extension().string();
        }

        if (!dir_ready) {
            fs::create_directories(assets_dir, lec);
            if (lec) {
                std::cerr << "Warning: could not create " << assets_dir << ": "
                          << lec.message()
                          << " — keeping absolute asset path in output\n";
                return;
            }
            dir_ready = true;
        }
        const fs::path dst = assets_dir / name;
        const bool same_file = fs::exists(dst, lec) && !lec
                            && fs::equivalent(src, dst, lec) && !lec;
        lec.clear();
        if (!same_file) {
            fs::copy_file(src, dst, fs::copy_options::overwrite_existing, lec);
            if (lec) {
                std::cerr << "Warning: could not copy asset " << src << " → "
                          << dst << ": " << lec.message()
                          << " — keeping absolute asset path in output\n";
                return;
            }
        }
        // generic_string(): the relative path is baked into generated JS and
        // must use '/' on every platform (same reason as the resolve pass).
        std::string rel = (fs::path("assets") / name).generic_string();
        rel_by_source.emplace(path_ref, rel);
        source_by_name.emplace(std::move(name), path_ref);
        path_ref = rel;
    };

    std::function<void(IRNode&)> walk = [&](IRNode& n) {
        if (auto it = n.attributes.find("asset_path"); it != n.attributes.end())
            localize(it->second);
        for (auto& alt : n.alternate_frames) walk(alt);
        for (auto& c : n.children) walk(c);
    };
    walk(ir.root);
    for (auto& fa : ir.font_family_assets) localize(fa.resolved_path);
}

}  // namespace pulp::import_design
