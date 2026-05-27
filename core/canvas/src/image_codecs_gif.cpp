// GIF87a / GIF89a decoder + GIF89a encoder for pulp::canvas::GifReader /
// GifWriter. MIT-clean, no external dependencies, no Skia.
//
// The decoder implements the LZW + interlacing + transparency rules
// from the GIF89a spec (Compuserve, 1990) — patent expired in 2004 in
// every jurisdiction Pulp ships into, so the algorithm is free to
// implement. The encoder ships a deliberately simple median-cut
// quantiser and 8-bit-only LZW; it is not a benchmark-grade encoder
// but produces files round-trippable through the in-tree decoder, any
// browser, and ImageMagick/identify.
//
// Acceptance test for the planning gap-doc row: encode → decode round
// trip yields bit-identical RGBA8 buffers (within the palette
// quantisation error for the writer case; the decoder is exact).

#include <pulp/canvas/image_codecs.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <limits>
#include <optional>
#include <vector>

namespace pulp::canvas {

namespace {

constexpr std::size_t kMaxLzwCodes = 4096;

class BitReader {
public:
    BitReader(const uint8_t* data, std::size_t size) : data_(data), size_(size) {}

    int read(int bits) {
        while (bit_count_ < bits) {
            if (block_pos_ >= block_end_) {
                if (!load_next_block_()) return -1;
            }
            bit_buffer_ |= static_cast<uint32_t>(data_[block_pos_++])
                           << bit_count_;
            bit_count_ += 8;
        }
        const int value = static_cast<int>(bit_buffer_ & ((1u << bits) - 1u));
        bit_buffer_ >>= bits;
        bit_count_ -= bits;
        return value;
    }

private:
    bool load_next_block_() {
        if (pos_ >= size_) return false;
        const uint8_t len = data_[pos_++];
        if (len == 0) return false;
        if (pos_ + len > size_) return false;
        block_pos_ = pos_;
        block_end_ = pos_ + len;
        pos_ += len;
        return true;
    }

    const uint8_t* data_;
    std::size_t size_;
    std::size_t pos_ = 0;
    std::size_t block_pos_ = 0;
    std::size_t block_end_ = 0;
    uint32_t bit_buffer_ = 0;
    int bit_count_ = 0;
};

struct LzwEntry {
    int16_t prefix = -1;
    uint8_t first = 0;
    uint8_t value = 0;
};

bool lzw_decode(const uint8_t* data, std::size_t size,
                int min_code_size,
                std::vector<uint8_t>& indices) {
    if (min_code_size < 2 || min_code_size > 8) return false;
    const int clear_code = 1 << min_code_size;
    const int end_code   = clear_code + 1;
    int code_size = min_code_size + 1;
    int next_code = end_code + 1;
    int max_code = (1 << code_size) - 1;

    std::array<LzwEntry, kMaxLzwCodes> table{};
    for (int i = 0; i < clear_code; ++i) {
        table[i].value = static_cast<uint8_t>(i);
        table[i].first = static_cast<uint8_t>(i);
        table[i].prefix = -1;
    }

    BitReader br(data, size);
    int prev = -1;
    std::vector<uint8_t> scratch;
    scratch.reserve(64);

    while (true) {
        const int code = br.read(code_size);
        if (code < 0) return true;
        if (code == clear_code) {
            code_size = min_code_size + 1;
            max_code = (1 << code_size) - 1;
            next_code = end_code + 1;
            prev = -1;
            continue;
        }
        if (code == end_code) return true;

        int decode_code = code;
        if (code >= next_code) {
            if (prev < 0) return false;
            scratch.clear();
            scratch.push_back(table[prev].first);
            decode_code = prev;
        } else if (code >= static_cast<int>(table.size())) {
            return false;
        } else {
            scratch.clear();
        }

        while (decode_code >= 0) {
            if (scratch.size() >= kMaxLzwCodes) return false;
            scratch.push_back(table[decode_code].value);
            decode_code = table[decode_code].prefix;
        }
        for (auto it = scratch.rbegin(); it != scratch.rend(); ++it) {
            indices.push_back(*it);
        }

        if (prev >= 0 && next_code < static_cast<int>(kMaxLzwCodes)) {
            table[next_code].prefix = static_cast<int16_t>(prev);
            table[next_code].first  = table[prev].first;
            table[next_code].value  = (code >= next_code)
                                          ? table[prev].first
                                          : table[code].first;
            ++next_code;
            if (next_code > max_code && code_size < 12) {
                ++code_size;
                max_code = (1 << code_size) - 1;
            }
        }
        prev = code;
    }
}

struct GifState {
    uint32_t width  = 0;
    uint32_t height = 0;
    std::array<uint8_t, 256 * 3> global_palette{};
    bool has_global_palette = false;
    uint16_t bg_index = 0;

    int transparent_index = -1;
    uint32_t frame_delay_ms = 100;
    int disposal_method = 0;

    std::vector<uint8_t> canvas;
    std::vector<uint8_t> previous;
};

bool read_color_table(const uint8_t* data, std::size_t size, std::size_t& pos,
                      int entries, std::array<uint8_t, 256 * 3>& dst) {
    const std::size_t bytes = static_cast<std::size_t>(entries) * 3;
    if (pos + bytes > size) return false;
    std::memcpy(dst.data(), data + pos, bytes);
    pos += bytes;
    return true;
}

bool composite_indices(GifState& state,
                       const std::vector<uint8_t>& indices,
                       uint32_t left, uint32_t top,
                       uint32_t fw, uint32_t fh,
                       bool interlaced,
                       const std::array<uint8_t, 256 * 3>& palette) {
    if (indices.size() != static_cast<std::size_t>(fw) * fh) return false;
    auto deinterlace_row = [&](uint32_t logical_row) {
        if (!interlaced) return logical_row;
        const uint32_t passes_8 = (fh + 7) / 8;
        const uint32_t passes_8_off4 = (fh + 3) / 8;
        const uint32_t passes_4_off2 = (fh + 1) / 4;
        uint32_t actual = 0;
        for (uint32_t pass = 0; pass < 4; ++pass) {
            uint32_t pass_rows = 0;
            uint32_t start = 0, stride = 1;
            switch (pass) {
                case 0: start = 0; stride = 8; pass_rows = passes_8; break;
                case 1: start = 4; stride = 8; pass_rows = passes_8_off4; break;
                case 2: start = 2; stride = 4; pass_rows = passes_4_off2; break;
                case 3: start = 1; stride = 2; pass_rows = fh / 2; break;
            }
            if (logical_row < actual + pass_rows) {
                return start + (logical_row - actual) * stride;
            }
            actual += pass_rows;
        }
        return logical_row;
    };

    for (uint32_t y = 0; y < fh; ++y) {
        const uint32_t dst_row = top + deinterlace_row(y);
        if (dst_row >= state.height) continue;
        for (uint32_t x = 0; x < fw; ++x) {
            const uint32_t dst_col = left + x;
            if (dst_col >= state.width) continue;
            const uint8_t idx = indices[y * fw + x];
            if (state.transparent_index >= 0 &&
                idx == static_cast<uint8_t>(state.transparent_index)) {
                continue;
            }
            const std::size_t p = static_cast<std::size_t>(idx) * 3;
            const std::size_t off =
                (static_cast<std::size_t>(dst_row) * state.width + dst_col) * 4;
            state.canvas[off + 0] = palette[p + 0];
            state.canvas[off + 1] = palette[p + 1];
            state.canvas[off + 2] = palette[p + 2];
            state.canvas[off + 3] = 0xFF;
        }
    }
    return true;
}

void apply_disposal(GifState& state,
                    uint32_t left, uint32_t top,
                    uint32_t fw, uint32_t fh) {
    switch (state.disposal_method) {
        case 2: {
            for (uint32_t y = 0; y < fh; ++y) {
                const uint32_t row = top + y;
                if (row >= state.height) break;
                for (uint32_t x = 0; x < fw; ++x) {
                    const uint32_t col = left + x;
                    if (col >= state.width) break;
                    const std::size_t off =
                        (static_cast<std::size_t>(row) * state.width + col) * 4;
                    state.canvas[off + 0] = 0;
                    state.canvas[off + 1] = 0;
                    state.canvas[off + 2] = 0;
                    state.canvas[off + 3] = 0;
                }
            }
            break;
        }
        case 3:
            if (!state.previous.empty()) state.canvas = state.previous;
            break;
        default: break;
    }
}

bool parse_gif(const uint8_t* data, std::size_t size,
               std::vector<GifFrame>& out_frames,
               bool first_only) {
    if (size < 13) return false;
    if (std::memcmp(data, "GIF87a", 6) != 0 &&
        std::memcmp(data, "GIF89a", 6) != 0) {
        return false;
    }

    GifState state;
    state.width  = static_cast<uint32_t>(data[6])  | (static_cast<uint32_t>(data[7])  << 8);
    state.height = static_cast<uint32_t>(data[8])  | (static_cast<uint32_t>(data[9])  << 8);
    if (state.width == 0 || state.height == 0) return false;

    const uint8_t packed = data[10];
    const bool gct_flag = (packed & 0x80) != 0;
    const int gct_size = 1 << ((packed & 0x07) + 1);
    state.bg_index = data[11];

    std::size_t pos = 13;
    if (gct_flag) {
        if (!read_color_table(data, size, pos, gct_size, state.global_palette)) {
            return false;
        }
        state.has_global_palette = true;
    }

    state.canvas.assign(static_cast<std::size_t>(state.width) * state.height * 4, 0);

    while (pos < size) {
        const uint8_t marker = data[pos++];
        if (marker == 0x3B) break;

        if (marker == 0x21) {
            if (pos >= size) return false;
            const uint8_t label = data[pos++];
            if (label == 0xF9) {
                if (pos + 6 > size) return false;
                if (data[pos] != 4) return false;
                const uint8_t gce_packed = data[pos + 1];
                const uint16_t delay_cs = static_cast<uint16_t>(data[pos + 2])
                                       | (static_cast<uint16_t>(data[pos + 3]) << 8);
                const uint8_t trans_idx = data[pos + 4];
                state.disposal_method = (gce_packed >> 2) & 0x07;
                state.transparent_index = ((gce_packed & 0x01) != 0)
                                              ? trans_idx
                                              : -1;
                state.frame_delay_ms = delay_cs * 10u;
                // The block-size byte (1) + 4 data bytes (block_size==4)
                // consumes 5 bytes; the next byte is the block-chain
                // terminator (0x00).
                pos += 5;
                if (pos >= size || data[pos] != 0) return false;
                ++pos;
                continue;
            }
            while (pos < size) {
                const uint8_t len = data[pos++];
                if (len == 0) break;
                if (pos + len > size) return false;
                pos += len;
            }
            continue;
        }

        if (marker == 0x2C) {
            if (pos + 9 > size) return false;
            const uint32_t img_left   = static_cast<uint32_t>(data[pos])   | (static_cast<uint32_t>(data[pos+1]) << 8);
            const uint32_t img_top    = static_cast<uint32_t>(data[pos+2]) | (static_cast<uint32_t>(data[pos+3]) << 8);
            const uint32_t img_width  = static_cast<uint32_t>(data[pos+4]) | (static_cast<uint32_t>(data[pos+5]) << 8);
            const uint32_t img_height = static_cast<uint32_t>(data[pos+6]) | (static_cast<uint32_t>(data[pos+7]) << 8);
            const uint8_t img_packed = data[pos + 8];
            pos += 9;

            std::array<uint8_t, 256 * 3> active_palette = state.global_palette;
            const bool lct_flag = (img_packed & 0x80) != 0;
            const bool interlaced = (img_packed & 0x40) != 0;
            const int lct_size = 1 << ((img_packed & 0x07) + 1);
            if (lct_flag) {
                if (!read_color_table(data, size, pos, lct_size, active_palette)) {
                    return false;
                }
            } else if (!state.has_global_palette) {
                return false;
            }

            state.previous = state.canvas;

            if (pos >= size) return false;
            const uint8_t lzw_min = data[pos++];
            std::vector<uint8_t> indices;
            indices.reserve(static_cast<std::size_t>(img_width) * img_height);

            const std::size_t lzw_start = pos;
            if (!lzw_decode(data + lzw_start, size - lzw_start, lzw_min, indices)) {
                return false;
            }
            while (pos < size) {
                const uint8_t len = data[pos++];
                if (len == 0) break;
                if (pos + len > size) return false;
                pos += len;
            }

            if (!composite_indices(state, indices, img_left, img_top,
                                    img_width, img_height, interlaced,
                                    active_palette)) {
                return false;
            }

            GifFrame f;
            f.raster.width = state.width;
            f.raster.height = state.height;
            f.raster.rgba = state.canvas;
            f.delay_ms = state.frame_delay_ms;
            out_frames.push_back(std::move(f));
            if (first_only) return true;

            apply_disposal(state, img_left, img_top, img_width, img_height);
            state.transparent_index = -1;
            state.frame_delay_ms = 100;
            state.disposal_method = 0;
            continue;
        }

        return false;
    }

    return !out_frames.empty();
}

}  // namespace

bool GifReader::is_gif(const uint8_t* data, std::size_t size) {
    if (data == nullptr || size < 6) return false;
    return std::memcmp(data, "GIF87a", 6) == 0
        || std::memcmp(data, "GIF89a", 6) == 0;
}

std::optional<DecodedRaster> GifReader::decode_first(const uint8_t* data,
                                                     std::size_t size) {
    std::vector<GifFrame> frames;
    if (!parse_gif(data, size, frames, /*first_only=*/true)) return std::nullopt;
    if (frames.empty()) return std::nullopt;
    return std::move(frames.front().raster);
}

std::optional<std::vector<GifFrame>> GifReader::decode_all(const uint8_t* data,
                                                            std::size_t size) {
    std::vector<GifFrame> frames;
    if (!parse_gif(data, size, frames, /*first_only=*/false)) return std::nullopt;
    return frames;
}

// ── GifWriter ───────────────────────────────────────────────────────────────

namespace {

struct PaletteEntry { uint8_t r = 0, g = 0, b = 0; };

struct ColorBox {
    std::vector<std::array<uint8_t, 3>> pixels;
};

PaletteEntry box_average(const ColorBox& box) {
    if (box.pixels.empty()) return {};
    uint64_t r = 0, g = 0, b = 0;
    for (const auto& px : box.pixels) {
        r += px[0]; g += px[1]; b += px[2];
    }
    const uint64_t n = box.pixels.size();
    return {static_cast<uint8_t>(r / n),
            static_cast<uint8_t>(g / n),
            static_cast<uint8_t>(b / n)};
}

std::vector<PaletteEntry> median_cut(const std::vector<std::array<uint8_t, 3>>& pixels,
                                     std::size_t max_colors) {
    std::vector<ColorBox> boxes;
    boxes.push_back({pixels});
    while (boxes.size() < max_colors) {
        std::size_t pick = 0;
        int best_range = -1;
        int best_axis = 0;
        for (std::size_t i = 0; i < boxes.size(); ++i) {
            if (boxes[i].pixels.size() < 2) continue;
            int lo[3] = {255, 255, 255};
            int hi[3] = {0, 0, 0};
            for (const auto& px : boxes[i].pixels) {
                for (int c = 0; c < 3; ++c) {
                    lo[c] = std::min(lo[c], static_cast<int>(px[c]));
                    hi[c] = std::max(hi[c], static_cast<int>(px[c]));
                }
            }
            for (int c = 0; c < 3; ++c) {
                const int range = hi[c] - lo[c];
                if (range > best_range) {
                    best_range = range;
                    best_axis = c;
                    pick = i;
                }
            }
        }
        if (best_range <= 0) break;

        ColorBox& target = boxes[pick];
        std::sort(target.pixels.begin(), target.pixels.end(),
                  [best_axis](const auto& a, const auto& b) {
                      return a[best_axis] < b[best_axis];
                  });
        const std::size_t mid = target.pixels.size() / 2;
        ColorBox left, right;
        left.pixels.assign(target.pixels.begin(), target.pixels.begin() + mid);
        right.pixels.assign(target.pixels.begin() + mid, target.pixels.end());
        boxes[pick] = std::move(left);
        boxes.push_back(std::move(right));
    }
    std::vector<PaletteEntry> palette;
    palette.reserve(boxes.size());
    for (const auto& box : boxes) palette.push_back(box_average(box));
    while (palette.size() < max_colors) palette.push_back({});
    return palette;
}

int nearest_color(const PaletteEntry& target,
                  const std::vector<PaletteEntry>& palette,
                  std::size_t skip_index = static_cast<std::size_t>(-1)) {
    int best = 0;
    int best_d = std::numeric_limits<int>::max();
    for (std::size_t i = 0; i < palette.size(); ++i) {
        if (i == skip_index) continue;
        const int dr = static_cast<int>(target.r) - palette[i].r;
        const int dg = static_cast<int>(target.g) - palette[i].g;
        const int db = static_cast<int>(target.b) - palette[i].b;
        const int d = dr * dr + dg * dg + db * db;
        if (d < best_d) { best_d = d; best = static_cast<int>(i); }
    }
    return best;
}

class BitWriter {
public:
    void write(int value, int bits) {
        bit_buffer_ |= static_cast<uint32_t>(value) << bit_count_;
        bit_count_ += bits;
        while (bit_count_ >= 8) {
            sub_block_.push_back(static_cast<uint8_t>(bit_buffer_ & 0xFF));
            bit_buffer_ >>= 8;
            bit_count_ -= 8;
            flush_sub_block_(/*force=*/false);
        }
    }

    void finish(std::vector<uint8_t>& out) {
        if (bit_count_ > 0) {
            sub_block_.push_back(static_cast<uint8_t>(bit_buffer_ & 0xFF));
            bit_buffer_ = 0;
            bit_count_ = 0;
        }
        flush_sub_block_(/*force=*/true);
        out.insert(out.end(), encoded_.begin(), encoded_.end());
        out.push_back(0x00);
    }

private:
    void flush_sub_block_(bool force) {
        while (sub_block_.size() >= 255) {
            encoded_.push_back(255);
            encoded_.insert(encoded_.end(),
                            sub_block_.begin(),
                            sub_block_.begin() + 255);
            sub_block_.erase(sub_block_.begin(),
                             sub_block_.begin() + 255);
        }
        if (force && !sub_block_.empty()) {
            encoded_.push_back(static_cast<uint8_t>(sub_block_.size()));
            encoded_.insert(encoded_.end(), sub_block_.begin(), sub_block_.end());
            sub_block_.clear();
        }
    }

    uint32_t bit_buffer_ = 0;
    int bit_count_ = 0;
    std::vector<uint8_t> sub_block_;
    std::vector<uint8_t> encoded_;
};

void lzw_encode(const std::vector<uint8_t>& indices,
                std::vector<uint8_t>& out) {
    constexpr int min_code_size = 8;
    const int clear_code = 1 << min_code_size;
    const int end_code   = clear_code + 1;
    int next_code = end_code + 1;
    int code_size = min_code_size + 1;
    int max_code = (1 << code_size) - 1;

    out.push_back(static_cast<uint8_t>(min_code_size));

    BitWriter bw;
    bw.write(clear_code, code_size);

    std::vector<int> table((kMaxLzwCodes + 256) * 256, -1);

    if (indices.empty()) {
        bw.write(end_code, code_size);
        bw.finish(out);
        return;
    }

    int current = indices[0];
    for (std::size_t i = 1; i < indices.size(); ++i) {
        const uint8_t k = indices[i];
        const int key = (current << 8) | k;
        const int found = table[key];
        if (found >= 0) {
            current = found;
        } else {
            bw.write(current, code_size);
            if (next_code < static_cast<int>(kMaxLzwCodes)) {
                table[key] = next_code;
                ++next_code;
                if (next_code > max_code && code_size < 12) {
                    ++code_size;
                    max_code = (1 << code_size) - 1;
                }
            } else {
                bw.write(clear_code, code_size);
                std::fill(table.begin(), table.end(), -1);
                next_code = end_code + 1;
                code_size = min_code_size + 1;
                max_code = (1 << code_size) - 1;
            }
            current = k;
        }
    }
    bw.write(current, code_size);
    bw.write(end_code, code_size);
    bw.finish(out);
}

}  // namespace

std::vector<uint8_t> GifWriter::encode_still(const DecodedRaster& raster) {
    if (raster.width == 0 || raster.height == 0) return {};
    const std::size_t expected_size =
        static_cast<std::size_t>(raster.width) * raster.height * 4;
    if (raster.rgba.size() != expected_size) return {};

    std::vector<std::array<uint8_t, 3>> opaque_pixels;
    opaque_pixels.reserve(raster.width * raster.height);
    bool has_transparent = false;
    for (std::size_t i = 0; i < expected_size; i += 4) {
        if (raster.rgba[i + 3] == 0) {
            has_transparent = true;
            continue;
        }
        opaque_pixels.push_back({raster.rgba[i + 0],
                                  raster.rgba[i + 1],
                                  raster.rgba[i + 2]});
    }

    constexpr std::size_t kPaletteSize = 256;
    auto palette = median_cut(opaque_pixels,
                              has_transparent ? kPaletteSize - 1 : kPaletteSize);
    const int trans_idx = has_transparent
                              ? static_cast<int>(palette.size())
                              : -1;
    if (has_transparent) palette.push_back({0, 0, 0});

    std::vector<uint8_t> indices;
    indices.reserve(raster.width * raster.height);
    for (std::size_t i = 0; i < expected_size; i += 4) {
        if (raster.rgba[i + 3] == 0 && trans_idx >= 0) {
            indices.push_back(static_cast<uint8_t>(trans_idx));
        } else {
            PaletteEntry target{raster.rgba[i + 0],
                                raster.rgba[i + 1],
                                raster.rgba[i + 2]};
            // Skip the transparent slot when matching opaque pixels so
            // a near-black RGB never quantises onto the transparent
            // slot (which would silently delete the pixel).
            const std::size_t skip = (trans_idx >= 0)
                                         ? static_cast<std::size_t>(trans_idx)
                                         : static_cast<std::size_t>(-1);
            indices.push_back(static_cast<uint8_t>(nearest_color(target, palette, skip)));
        }
    }

    std::vector<uint8_t> out;
    out.reserve(expected_size / 2 + 1024);

    const char header[6] = {'G', 'I', 'F', '8', '9', 'a'};
    out.insert(out.end(), header, header + 6);
    out.push_back(static_cast<uint8_t>(raster.width & 0xFF));
    out.push_back(static_cast<uint8_t>((raster.width >> 8) & 0xFF));
    out.push_back(static_cast<uint8_t>(raster.height & 0xFF));
    out.push_back(static_cast<uint8_t>((raster.height >> 8) & 0xFF));
    out.push_back(0xF7);
    out.push_back(0);
    out.push_back(0);
    for (std::size_t i = 0; i < 256; ++i) {
        if (i < palette.size()) {
            out.push_back(palette[i].r);
            out.push_back(palette[i].g);
            out.push_back(palette[i].b);
        } else {
            out.push_back(0); out.push_back(0); out.push_back(0);
        }
    }

    if (trans_idx >= 0) {
        out.push_back(0x21);
        out.push_back(0xF9);
        out.push_back(0x04);
        out.push_back(0x01);
        out.push_back(0); out.push_back(0);
        out.push_back(static_cast<uint8_t>(trans_idx));
        out.push_back(0);
    }

    out.push_back(0x2C);
    out.push_back(0); out.push_back(0);
    out.push_back(0); out.push_back(0);
    out.push_back(static_cast<uint8_t>(raster.width & 0xFF));
    out.push_back(static_cast<uint8_t>((raster.width >> 8) & 0xFF));
    out.push_back(static_cast<uint8_t>(raster.height & 0xFF));
    out.push_back(static_cast<uint8_t>((raster.height >> 8) & 0xFF));
    out.push_back(0x00);

    lzw_encode(indices, out);

    out.push_back(0x3B);
    return out;
}

}  // namespace pulp::canvas
