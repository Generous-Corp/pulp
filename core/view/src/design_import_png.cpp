// design_import_png.cpp — the design-import PNG codec and pixel surgery.
//
// A self-contained 8-bit non-interlaced PNG decoder / RGBA re-encoder plus the
// pixel passes the asset pipeline runs over captured art (opaque-core bbox,
// baked-indicator erase, shape gradient sampling). Split out of
// design_import.cpp so the per-source parsers and the asset pipeline are not
// interleaved with byte-level image work.
//
// Definitions only; the declarations the asset pipeline needs live in
// design_import_internal.hpp.

#include "design_import_internal.hpp"

#include <pulp/runtime/zip.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <optional>
#include <string>
#include <vector>

namespace pulp::view {

static uint32_t png_be32(const uint8_t* p) {
    return (static_cast<uint32_t>(p[0]) << 24) |
           (static_cast<uint32_t>(p[1]) << 16) |
           (static_cast<uint32_t>(p[2]) << 8) |
           static_cast<uint32_t>(p[3]);
}

std::pair<int, int> png_dimensions_from_bytes(const std::vector<uint8_t>& bytes) {
    static const uint8_t sig[8] = {0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A};
    if (bytes.size() < 24 || std::memcmp(bytes.data(), sig, sizeof(sig)) != 0) return {0, 0};
    const int w = static_cast<int>(png_be32(bytes.data() + 16));
    const int h = static_cast<int>(png_be32(bytes.data() + 20));
    if (w <= 0 || h <= 0) return {0, 0};
    return {w, h};
}

std::optional<ImportDecodedPng> decode_png_rgba_for_import(const std::vector<uint8_t>& bytes) {
    ImportDecodedPng out;
    static const uint8_t sig[8] = {0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A};
    if (bytes.size() < 33 || std::memcmp(bytes.data(), sig, sizeof(sig)) != 0) return std::nullopt;

    const int width = static_cast<int>(png_be32(bytes.data() + 16));
    const int height = static_cast<int>(png_be32(bytes.data() + 20));
    const int bit_depth = bytes[24];
    const int color_type = bytes[25];
    const int interlace = bytes[28];
    if (width <= 0 || height <= 0 || bit_depth != 8 || interlace != 0) return std::nullopt;
    const auto pixels = static_cast<uint64_t>(width) * static_cast<uint64_t>(height);
    if (pixels > 50'000'000ull) return std::nullopt;

    int channels = 0;
    switch (color_type) {
        case 0: channels = 1; break;  // gray
        case 2: channels = 3; break;  // RGB
        case 4: channels = 2; break;  // gray + alpha
        case 6: channels = 4; break;  // RGBA
        default: return std::nullopt;
    }

    std::vector<uint8_t> idat;
    size_t pos = 8;
    while (pos + 8 <= bytes.size()) {
        const uint32_t clen = png_be32(bytes.data() + pos);
        const uint8_t* ctype = bytes.data() + pos + 4;
        const size_t body = pos + 8;
        if (body + clen + 4 > bytes.size()) break;
        if (std::memcmp(ctype, "IDAT", 4) == 0) {
            idat.insert(idat.end(), bytes.data() + body, bytes.data() + body + clen);
        } else if (std::memcmp(ctype, "IEND", 4) == 0) {
            break;
        }
        pos = body + clen + 4;
    }
    if (idat.empty()) return std::nullopt;

    const size_t stride = static_cast<size_t>(width) * static_cast<size_t>(channels);
    const size_t expected = static_cast<size_t>(height) * (stride + 1);
    auto raw = pulp::runtime::gzip_decompress(idat.data(), idat.size());
    if (!raw || raw->size() < expected) return std::nullopt;

    std::vector<uint8_t> img(static_cast<size_t>(height) * stride);
    auto paeth = [](int a, int b, int c) {
        const int p = a + b - c;
        const int pa = std::abs(p - a);
        const int pb = std::abs(p - b);
        const int pc = std::abs(p - c);
        if (pa <= pb && pa <= pc) return a;
        return pb <= pc ? b : c;
    };
    for (int y = 0; y < height; ++y) {
        const uint8_t* src = raw->data() + static_cast<size_t>(y) * (stride + 1);
        uint8_t* row = img.data() + static_cast<size_t>(y) * stride;
        const uint8_t* prev = y > 0 ? img.data() + static_cast<size_t>(y - 1) * stride : nullptr;
        const uint8_t filter = src[0];
        for (size_t x = 0; x < stride; ++x) {
            const int a = x >= static_cast<size_t>(channels) ? row[x - channels] : 0;
            const int b = prev ? prev[x] : 0;
            const int c = (prev && x >= static_cast<size_t>(channels)) ? prev[x - channels] : 0;
            int v = src[1 + x];
            switch (filter) {
                case 0: break;
                case 1: v += a; break;
                case 2: v += b; break;
                case 3: v += (a + b) / 2; break;
                case 4: v += paeth(a, b, c); break;
                default: return std::nullopt;
            }
            row[x] = static_cast<uint8_t>(v & 0xff);
        }
    }

    out.rgba.resize(static_cast<size_t>(width) * static_cast<size_t>(height) * 4);
    for (int i = 0; i < width * height; ++i) {
        const uint8_t* s = img.data() + static_cast<size_t>(i) * static_cast<size_t>(channels);
        uint8_t* d = out.rgba.data() + static_cast<size_t>(i) * 4;
        if (channels == 4) {
            d[0] = s[0]; d[1] = s[1]; d[2] = s[2]; d[3] = s[3];
        } else if (channels == 3) {
            d[0] = s[0]; d[1] = s[1]; d[2] = s[2]; d[3] = 255;
        } else if (channels == 2) {
            d[0] = d[1] = d[2] = s[0]; d[3] = s[1];
        } else {
            d[0] = d[1] = d[2] = s[0]; d[3] = 255;
        }
    }
    out.width = width;
    out.height = height;
    return out;
}

// ── Minimal RGBA-PNG re-encoder (for cleaning captured knob art) ──────────
// Re-encodes a decoded RGBA buffer to a valid 8-bit RGBA PNG using the runtime
// zlib codec. The decode→encode round-trip is lossless (pixel values are
// preserved), so editing a few pixels and re-encoding leaves the rest of the
// image byte-for-byte identical after decode.
uint32_t import_png_crc32(const uint8_t* data, size_t len) {
    static uint32_t table[256];
    static bool init = false;
    if (!init) {
        for (uint32_t n = 0; n < 256; ++n) {
            uint32_t c = n;
            for (int k = 0; k < 8; ++k)
                c = (c & 1u) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
            table[n] = c;
        }
        init = true;
    }
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; ++i) c = table[(c ^ data[i]) & 0xffu] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

void import_png_put_be32(std::vector<uint8_t>& v, uint32_t x) {
    v.push_back(static_cast<uint8_t>((x >> 24) & 0xff));
    v.push_back(static_cast<uint8_t>((x >> 16) & 0xff));
    v.push_back(static_cast<uint8_t>((x >> 8) & 0xff));
    v.push_back(static_cast<uint8_t>(x & 0xff));
}

void import_png_put_chunk(std::vector<uint8_t>& out, const char* type,
                          const std::vector<uint8_t>& data) {
    import_png_put_be32(out, static_cast<uint32_t>(data.size()));
    const size_t crc_start = out.size();
    out.insert(out.end(), type, type + 4);
    out.insert(out.end(), data.begin(), data.end());
    out.insert(out.end(), 4, 0);  // placeholder, overwritten below
    const uint32_t crc = import_png_crc32(out.data() + crc_start, 4 + data.size());
    out[out.size() - 4] = static_cast<uint8_t>((crc >> 24) & 0xff);
    out[out.size() - 3] = static_cast<uint8_t>((crc >> 16) & 0xff);
    out[out.size() - 2] = static_cast<uint8_t>((crc >> 8) & 0xff);
    out[out.size() - 1] = static_cast<uint8_t>(crc & 0xff);
}

std::optional<std::vector<uint8_t>> encode_rgba_png_for_import(
        const ImportDecodedPng& img) {
    if (!img.valid()) return std::nullopt;
    std::vector<uint8_t> raw;
    raw.reserve(static_cast<size_t>(img.height) * (static_cast<size_t>(img.width) * 4 + 1));
    for (int y = 0; y < img.height; ++y) {
        raw.push_back(0);  // filter type: none
        const uint8_t* row = img.rgba.data() + static_cast<size_t>(y) * img.width * 4;
        raw.insert(raw.end(), row, row + static_cast<size_t>(img.width) * 4);
    }
    auto comp = pulp::runtime::zlib_compress(raw.data(), raw.size(), 6);
    if (!comp) return std::nullopt;
    std::vector<uint8_t> out = {0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A};
    std::vector<uint8_t> ihdr;
    import_png_put_be32(ihdr, static_cast<uint32_t>(img.width));
    import_png_put_be32(ihdr, static_cast<uint32_t>(img.height));
    ihdr.push_back(8);  // bit depth
    ihdr.push_back(6);  // color type RGBA
    ihdr.push_back(0); ihdr.push_back(0); ihdr.push_back(0);  // compression/filter/interlace
    import_png_put_chunk(out, "IHDR", ihdr);
    import_png_put_chunk(out, "IDAT", *comp);
    import_png_put_chunk(out, "IEND", {});
    return out;
}

// Erase the indicator the design BAKED into a captured knob disc — ELYSIUM's is
// a thin vertical ANTENNA standing straight up ABOVE the disc at 12 o'clock. We
// draw our own rotating pointer, so the baked one is a stuck second line.
//
// The erase MUST be non-destructive to the disc: it ONLY clears the narrow
// antenna column sitting above the disc body, and STOPS the instant a scan row
// widens into the disc itself — so the ring outline and face are never touched
// (an earlier copy-from-beside + alpha-punch version cut a notch into the ring's
// top, which read as a gap). It never copies pixels and never modifies the disc.
// Mutates `img`.
// Pure pixel logic (declared in design_import_internal.hpp so it's unit-testable
// without the internal Import* structs). Scans the disc bbox from the top down:
// a NARROW opaque span is the antenna → cleared; the first WIDE span is the disc
// body → stop. The antenna is located by its actual span per row, NOT assumed at
// the bbox center (the min/max ticks skew the bbox).
void clear_baked_knob_antenna(std::vector<uint8_t>& rgba, int img_w, int img_h,
                              int core_x, int core_y, int core_w, int core_h) {
    if (img_w <= 0 || img_h <= 0 || core_w <= 0 || core_h <= 0) return;
    if (rgba.size() < static_cast<size_t>(img_w) * img_h * 4) return;
    const int x_lo = std::max(0, core_x);
    const int x_hi = std::min(img_w, core_x + core_w);
    const int y_hi = std::min(img_h, core_y + core_h);
    // Antenna width ceiling: the disc body's span is ~core_w; the antenna is a
    // thin line. A row whose opaque span exceeds this is the disc, not antenna.
    const int narrow = std::max(6, core_w * 18 / 100);
    auto alpha = [&](int x, int y) -> uint8_t& {
        return rgba[(static_cast<size_t>(y) * img_w + x) * 4 + 3];
    };
    for (int y = std::max(0, core_y); y < y_hi; ++y) {
        int xmin = -1, xmax = -1;
        for (int x = x_lo; x < x_hi; ++x)
            if (alpha(x, y) >= 24) { if (xmin < 0) xmin = x; xmax = x; }
        if (xmin < 0) continue;                       // empty row above the antenna
        if (xmax - xmin + 1 > narrow) break;          // reached the disc — stop
        for (int x = xmin; x <= xmax; ++x) alpha(x, y) = 0;  // clear the antenna
    }
}

void clean_baked_knob_indicator(ImportDecodedPng& img, const ImportOpaqueCore& core) {
    if (!img.valid()) return;
    clear_baked_knob_antenna(img.rgba, img.width, img.height,
                             core.x, core.y, core.w, core.h);
}

// Sample a shape illustration's OWN vertical color gradient from its art, so a
// value-driven fill reproduces the shape's real colors (ELYSIUM: the cylinder's
// purple, the prism's magenta, the cube's green, the tuning shape's amber) —
// each shape filling with ITS gradient, not one generic color. Returns up to
// `n` comma-joined "#rrggbb" stops bottom→top, or "" when the shape isn't
// colorful enough to be a gradient fill (a near-grey logo/icon yields nothing,
// which keeps the capability from latching onto things that shouldn't fill).
std::string sample_shape_fill_gradient(const ImportDecodedPng& img,
                                       const ImportOpaqueCore& core, int n) {
    if (!img.valid() || core.w <= 1 || core.h <= 1 || n < 2) return {};
    auto at = [&](int x, int y) -> const uint8_t* {
        return &img.rgba[(static_cast<size_t>(y) * img.width + x) * 4];
    };
    std::vector<std::array<int, 3>> stops;   // averaged RGB per band
    float max_sat = 0.0f;
    for (int k = 0; k < n; ++k) {
        // Stop k: band centered up the shape — k=0 bottom, k=n-1 top.
        const float fy = 1.0f - (static_cast<float>(k) + 0.5f) / static_cast<float>(n);
        const int band_c = core.y + static_cast<int>(fy * core.h);
        const int band_h = std::max(1, core.h / (n * 2));
        long sr = 0, sg = 0, sb = 0, cnt = 0;
        for (int y = band_c - band_h; y <= band_c + band_h; ++y) {
            if (y < 0 || y >= img.height) continue;
            for (int x = core.x; x < core.x + core.w && x < img.width; ++x) {
                const uint8_t* p = at(x, y);
                if (p[3] < 96) continue;          // skip transparent / soft edges
                sr += p[0]; sg += p[1]; sb += p[2]; ++cnt;
            }
        }
        if (cnt == 0) return {};                  // a gappy band ⇒ not a solid shape
        const int r = static_cast<int>(sr / cnt);
        const int g = static_cast<int>(sg / cnt);
        const int bl = static_cast<int>(sb / cnt);
        stops.push_back({r, g, bl});
        const int mx = std::max({r, g, bl}), mn = std::min({r, g, bl});
        if (mx > 0) max_sat = std::max(max_sat, static_cast<float>(mx - mn) / mx);
    }
    if (max_sat < 0.18f) return {};               // ~grey ⇒ logo/icon, not a fill
    std::string out;
    char buf[8];
    for (size_t i = 0; i < stops.size(); ++i) {
        std::snprintf(buf, sizeof(buf), "#%02x%02x%02x",
                      stops[i][0], stops[i][1], stops[i][2]);
        if (i) out += ',';
        out += buf;
    }
    return out;
}

std::optional<ImportOpaqueCore> compute_import_opaque_core(const std::vector<uint8_t>& bytes,
                                                          float min_alpha) {
    auto img = decode_png_rgba_for_import(bytes);
    if (!img || !img->valid()) return std::nullopt;
    const uint8_t threshold = static_cast<uint8_t>(
        std::clamp(min_alpha, 0.0f, 1.0f) * 255.0f + 0.5f);
    int min_x = img->width;
    int min_y = img->height;
    int max_x = -1;
    int max_y = -1;
    for (int y = 0; y < img->height; ++y) {
        const uint8_t* row = img->rgba.data() + static_cast<size_t>(y) * img->width * 4;
        for (int x = 0; x < img->width; ++x) {
            if (row[x * 4 + 3] < threshold) continue;
            min_x = std::min(min_x, x);
            min_y = std::min(min_y, y);
            max_x = std::max(max_x, x);
            max_y = std::max(max_y, y);
        }
    }
    if (max_x < min_x || max_y < min_y) return std::nullopt;
    return ImportOpaqueCore{
        min_x,
        min_y,
        max_x - min_x + 1,
        max_y - min_y + 1,
        img->width,
        img->height,
    };
}

}  // namespace pulp::view
