// font_tables.cpp
//
// Face adjustments read from a font's own tables: optical size from the
// `opsz` axis and tracking from the AAT `trak` table.

#include "font_tables.hpp"

#include <algorithm>
#include <cstdint>
#include <map>
#include <mutex>
#include <utility>
#include <vector>

#include "include/core/SkFontParameters.h"
#include "include/core/SkSpan.h"

namespace pulp::canvas {

namespace {
constexpr SkFourByteTag kOpsz = SkSetFourByteTag('o', 'p', 's', 'z');
}  // namespace

std::vector<SkFontArguments::VariationPosition::Coordinate>
auto_optical_size_position(const SkTypeface* face, float css_px) {
    if (!face || !(css_px > 0.0f)) return {};
    const int count = face->getVariationDesignParameters(
        SkSpan<SkFontParameters::Variation::Axis>{});
    if (count <= 0) return {};
    std::vector<SkFontParameters::Variation::Axis> axes(
        static_cast<std::size_t>(count));
    if (face->getVariationDesignParameters(SkSpan(axes)) != count) return {};
    const auto axis = std::find_if(axes.begin(), axes.end(),
                                   [](const auto& a) { return a.tag == kOpsz; });
    if (axis == axes.end()) return {};
    const float opsz = std::clamp(css_px, axis->min, axis->max);

    // makeClone replaces the whole argument set on some ports, so the face's
    // other coordinates (a weight instance's wght, say) are carried over
    // explicitly rather than trusted to survive.
    std::vector<SkFontArguments::VariationPosition::Coordinate> position(
        static_cast<std::size_t>(count));
    const int current = face->getVariationDesignPosition(SkSpan(position));
    position.resize(current > 0 && current <= count
                        ? static_cast<std::size_t>(current) : 0u);
    const auto existing = std::find_if(
        position.begin(), position.end(),
        [](const auto& c) { return c.axis == kOpsz; });
    if (existing != position.end()) existing->value = opsz;
    else position.push_back({kOpsz, opsz});
    return position;
}

sk_sp<SkTypeface> typeface_at_auto_optical_size(sk_sp<SkTypeface> face,
                                                float css_px) {
    const auto position = auto_optical_size_position(face.get(), css_px);
    if (position.empty()) return face;
    const float opsz = std::find_if(position.begin(), position.end(),
                                    [](const auto& c) { return c.axis == kOpsz; })
                           ->value;

    // Keyed by the source face's identity and the clamped coordinate.
    static std::mutex mutex;
    static std::map<std::pair<SkTypefaceID, float>, sk_sp<SkTypeface>> cache;
    const auto key = std::make_pair(face->uniqueID(), opsz);
    {
        std::lock_guard<std::mutex> lock(mutex);
        if (auto it = cache.find(key); it != cache.end()) return it->second;
    }
    SkFontArguments args;
    args.setVariationDesignPosition(
        {position.data(), static_cast<int>(position.size())});
    sk_sp<SkTypeface> clone = face->makeClone(args);
    if (!clone) return face;
    std::lock_guard<std::mutex> lock(mutex);
    if (cache.size() > 512) cache.clear();
    cache.emplace(key, clone);
    return clone;
}

void apply_auto_optical_size(skia::textlayout::TextStyle& style, float css_px) {
    if (!(css_px > 0.0f)) return;
    const SkFontArguments::VariationPosition::Coordinate coordinate{kOpsz, css_px};
    SkFontArguments args;
    args.setVariationDesignPosition({&coordinate, 1});
    style.setFontArguments(args);  // copies the coordinate
}

namespace {
float aat_tracking_px_uncached(const SkTypeface* face, float css_px) {
    constexpr SkFontTableTag kTrak = SkSetFourByteTag('t', 'r', 'a', 'k');
    const size_t size = face->getTableSize(kTrak);
    if (size < 12) return 0.0f;
    std::vector<uint8_t> data(size);
    if (face->getTableData(kTrak, 0, size, data.data()) != size) return 0.0f;
    auto u16 = [&](size_t at) -> uint32_t {
        return at + 2 <= data.size()
            ? (static_cast<uint32_t>(data[at]) << 8) | data[at + 1] : 0u;
    };
    auto u32 = [&](size_t at) -> uint32_t {
        return at + 4 <= data.size() ? (u16(at) << 16) | u16(at + 2) : 0u;
    };
    // Header: version(4) format(2) horizOffset(2) vertOffset(2) reserved(2).
    const size_t horiz = u16(6);
    if (horiz == 0 || horiz + 8 > data.size()) return 0.0f;
    // TrackData: nTracks(2) nSizes(2) sizeTableOffset(4), then nTracks
    // entries of track(Fixed 4) nameIndex(2) offset(2).
    const uint32_t tracks = u16(horiz);
    const uint32_t sizes = u16(horiz + 2);
    const size_t size_table = u32(horiz + 4);
    if (tracks == 0 || sizes == 0 || size_table + sizes * 4 > data.size())
        return 0.0f;
    size_t values = 0;
    for (uint32_t t = 0; t < tracks; ++t) {
        const size_t entry = horiz + 8 + t * 8;
        if (entry + 8 > data.size()) return 0.0f;
        if (u32(entry) == 0) {  // the normal track
            values = u16(entry + 6);
            break;
        }
    }
    if (values == 0 || values + sizes * 2 > data.size()) return 0.0f;
    auto size_at = [&](uint32_t i) {
        return static_cast<float>(static_cast<int32_t>(u32(size_table + i * 4))) /
               65536.0f;
    };
    auto value_at = [&](uint32_t i) {
        return static_cast<float>(static_cast<int16_t>(u16(values + i * 2)));
    };
    float tracking = value_at(0);
    if (css_px >= size_at(sizes - 1)) {
        tracking = value_at(sizes - 1);
    } else if (css_px > size_at(0)) {
        for (uint32_t i = 1; i < sizes; ++i) {
            if (css_px <= size_at(i)) {
                const float a = size_at(i - 1);
                const float b = size_at(i);
                const float t = b > a ? (css_px - a) / (b - a) : 0.0f;
                tracking = value_at(i - 1) + (value_at(i) - value_at(i - 1)) * t;
                break;
            }
        }
    }
    const int upem = face->getUnitsPerEm();
    if (upem <= 0) return 0.0f;
    return tracking * css_px / static_cast<float>(upem);
}
}  // namespace

float aat_tracking_px(const SkTypeface* face, float css_px) {
    if (!face || !(css_px > 0.0f)) return 0.0f;
    // Measured on every text measurement, so the table is read once per face
    // and size.
    static std::mutex mutex;
    static std::map<std::pair<SkTypefaceID, float>, float> cache;
    const auto key = std::make_pair(face->uniqueID(), css_px);
    {
        std::lock_guard<std::mutex> lock(mutex);
        if (auto it = cache.find(key); it != cache.end()) return it->second;
    }
    const float tracking = aat_tracking_px_uncached(face, css_px);
    std::lock_guard<std::mutex> lock(mutex);
    if (cache.size() > 4096) cache.clear();
    cache.emplace(key, tracking);
    return tracking;
}

}  // namespace pulp::canvas
