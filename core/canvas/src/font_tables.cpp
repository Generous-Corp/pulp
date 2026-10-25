// font_tables.cpp
//
// Face adjustments read from a font's own tables: optical size from the
// `opsz` axis and tracking from the AAT `trak` table.

#include "font_tables.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <list>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <utility>
#include <vector>

#include "include/core/SkFontParameters.h"
#include "include/core/SkSpan.h"

namespace pulp::canvas {

namespace {
constexpr SkFourByteTag kOpsz = SkSetFourByteTag('o', 'p', 's', 'z');

// Least-recently-used map, so a working set that fits stays put while sizes
// that animate or auto-fit cannot grow it without bound.
template <typename Key, typename Value> class LruCache {
  public:
    std::optional<Value> find(const Key& key) {
        const auto it = index_.find(key);
        if (it == index_.end())
            return std::nullopt;
        order_.splice(order_.begin(), order_, it->second);
        return it->second->second;
    }
    void put(const Key& key, Value value) {
        if (const auto it = index_.find(key); it != index_.end()) {
            it->second->second = std::move(value);
            order_.splice(order_.begin(), order_, it->second);
            return;
        }
        order_.emplace_front(key, std::move(value));
        index_.emplace(key, order_.begin());
        if (index_.size() > kFontTableCacheCapacity) {
            index_.erase(order_.back().first);
            order_.pop_back();
        }
    }
    std::size_t size() const {
        return index_.size();
    }

  private:
    using Entry = std::pair<Key, Value>;
    std::list<Entry> order_;
    std::map<Key, typename std::list<Entry>::iterator> index_;
};

struct OpticalSizeClones {
    std::mutex mutex;
    LruCache<std::pair<SkTypefaceID, float>, sk_sp<SkTypeface>> cache;
};
OpticalSizeClones& optical_size_clones() {
    static OpticalSizeClones clones;
    return clones;
}

// A face's normal track from its `trak` table: the table's sizes in CSS px
// and the tracking at each, in font units. Empty when the face has no usable
// table.
struct NormalTrack {
    std::vector<float> sizes;
    std::vector<float> values;
    int units_per_em = 0;
};
struct TrackTables {
    std::mutex mutex;
    LruCache<SkTypefaceID, std::shared_ptr<const NormalTrack>> cache;
};
TrackTables& track_tables() {
    static TrackTables tables;
    return tables;
}
} // namespace

float optical_size_coordinate(float css_px) {
    return std::round(css_px * 4.0f) / 4.0f;
}

std::vector<SkFontArguments::VariationPosition::Coordinate>
auto_optical_size_position(const SkTypeface* face, float css_px) {
    if (!face || !(css_px > 0.0f))
        return {};
    const int count =
        face->getVariationDesignParameters(SkSpan<SkFontParameters::Variation::Axis>{});
    if (count <= 0)
        return {};
    std::vector<SkFontParameters::Variation::Axis> axes(static_cast<std::size_t>(count));
    if (face->getVariationDesignParameters(SkSpan(axes)) != count)
        return {};
    const auto axis =
        std::find_if(axes.begin(), axes.end(), [](const auto& a) { return a.tag == kOpsz; });
    if (axis == axes.end())
        return {};
    const float opsz = std::clamp(optical_size_coordinate(css_px), axis->min, axis->max);

    // makeClone replaces the whole argument set on some ports, so the face's
    // other coordinates (a weight instance's wght, say) are carried over
    // explicitly rather than trusted to survive.
    std::vector<SkFontArguments::VariationPosition::Coordinate> position(
        static_cast<std::size_t>(count));
    const int current = face->getVariationDesignPosition(SkSpan(position));
    position.resize(current > 0 && current <= count ? static_cast<std::size_t>(current) : 0u);
    const auto existing = std::find_if(position.begin(), position.end(),
                                       [](const auto& c) { return c.axis == kOpsz; });
    if (existing != position.end())
        existing->value = opsz;
    else
        position.push_back({kOpsz, opsz});
    return position;
}

namespace {
float opsz_of(const std::vector<SkFontArguments::VariationPosition::Coordinate>& position) {
    return std::find_if(position.begin(), position.end(),
                        [](const auto& c) { return c.axis == kOpsz; })
        ->value;
}
} // namespace

sk_sp<SkTypeface> typeface_at_auto_optical_size(sk_sp<SkTypeface> face, float css_px) {
    const auto position = auto_optical_size_position(face.get(), css_px);
    if (position.empty())
        return face;

    // Keyed by the source face's identity and the clamped coordinate.
    auto& clones = optical_size_clones();
    const auto key = std::make_pair(face->uniqueID(), opsz_of(position));
    {
        std::lock_guard<std::mutex> lock(clones.mutex);
        if (auto hit = clones.cache.find(key))
            return *hit;
    }
    SkFontArguments args;
    args.setVariationDesignPosition({position.data(), static_cast<int>(position.size())});
    sk_sp<SkTypeface> clone = face->makeClone(args);
    if (!clone)
        return face;
    std::lock_guard<std::mutex> lock(clones.mutex);
    clones.cache.put(key, clone);
    return clone;
}

void apply_auto_optical_size(skia::textlayout::TextStyle& style, const SkTypeface* primary,
                             float css_px) {
    // SkParagraph's FontCollection keeps a clone per face and argument set
    // for its lifetime, so the coordinate is set only when the primary face
    // can use it, and on the same rounded, clamped value the SkFont path uses.
    const auto position = auto_optical_size_position(primary, css_px);
    if (position.empty())
        return;
    const SkFontArguments::VariationPosition::Coordinate coordinate{kOpsz, opsz_of(position)};
    SkFontArguments args;
    args.setVariationDesignPosition({&coordinate, 1});
    style.setFontArguments(args); // copies the coordinate
}

namespace {
std::shared_ptr<const NormalTrack> read_normal_track(const SkTypeface* face) {
    auto track = std::make_shared<NormalTrack>();
    constexpr SkFontTableTag kTrak = SkSetFourByteTag('t', 'r', 'a', 'k');
    const size_t size = face->getTableSize(kTrak);
    if (size < 12)
        return track;
    std::vector<uint8_t> data(size);
    if (face->getTableData(kTrak, 0, size, data.data()) != size)
        return track;
    auto u16 = [&](size_t at) -> uint32_t {
        return at + 2 <= data.size() ? (static_cast<uint32_t>(data[at]) << 8) | data[at + 1] : 0u;
    };
    auto u32 = [&](size_t at) -> uint32_t {
        return at + 4 <= data.size() ? (u16(at) << 16) | u16(at + 2) : 0u;
    };
    // Header: version(4) format(2) horizOffset(2) vertOffset(2) reserved(2).
    const size_t horiz = u16(6);
    if (horiz == 0 || horiz + 8 > data.size())
        return track;
    // TrackData: nTracks(2) nSizes(2) sizeTableOffset(4), then nTracks
    // entries of track(Fixed 4) nameIndex(2) offset(2).
    const uint32_t tracks = u16(horiz);
    const uint32_t sizes = u16(horiz + 2);
    const size_t size_table = u32(horiz + 4);
    if (tracks == 0 || sizes == 0 || size_table + sizes * 4 > data.size())
        return track;
    size_t values = 0;
    for (uint32_t t = 0; t < tracks; ++t) {
        const size_t entry = horiz + 8 + t * 8;
        if (entry + 8 > data.size())
            return track;
        if (u32(entry) == 0) { // the normal track
            values = u16(entry + 6);
            break;
        }
    }
    if (values == 0 || values + sizes * 2 > data.size())
        return track;
    const int upem = face->getUnitsPerEm();
    if (upem <= 0)
        return track;
    for (uint32_t i = 0; i < sizes; ++i) {
        track->sizes.push_back(static_cast<float>(static_cast<int32_t>(u32(size_table + i * 4))) /
                               65536.0f);
        track->values.push_back(static_cast<float>(static_cast<int16_t>(u16(values + i * 2))));
    }
    track->units_per_em = upem;
    return track;
}

float tracking_at(const NormalTrack& track, float css_px) {
    if (track.sizes.empty() || track.units_per_em <= 0)
        return 0.0f;
    const auto& sizes = track.sizes;
    const auto& values = track.values;
    float tracking = values.front();
    if (css_px >= sizes.back()) {
        tracking = values.back();
    } else if (css_px > sizes.front()) {
        for (std::size_t i = 1; i < sizes.size(); ++i) {
            if (css_px <= sizes[i]) {
                const float a = sizes[i - 1];
                const float b = sizes[i];
                const float t = b > a ? (css_px - a) / (b - a) : 0.0f;
                tracking = values[i - 1] + (values[i] - values[i - 1]) * t;
                break;
            }
        }
    }
    return tracking * css_px / static_cast<float>(track.units_per_em);
}
} // namespace

float aat_tracking_px(const SkTypeface* face, float css_px) {
    if (!face || !(css_px > 0.0f))
        return 0.0f;
    // Measured on every text measurement, so each face's table is read once
    // and the size is applied to it arithmetically.
    auto& tables = track_tables();
    std::shared_ptr<const NormalTrack> track;
    {
        std::lock_guard<std::mutex> lock(tables.mutex);
        if (auto hit = tables.cache.find(face->uniqueID()))
            track = *hit;
    }
    if (!track) {
        track = read_normal_track(face);
        std::lock_guard<std::mutex> lock(tables.mutex);
        tables.cache.put(face->uniqueID(), track);
    }
    return tracking_at(*track, css_px);
}

FontTableCacheSizes font_table_cache_sizes() {
    FontTableCacheSizes sizes;
    {
        auto& clones = optical_size_clones();
        std::lock_guard<std::mutex> lock(clones.mutex);
        sizes.optical_size_clones = clones.cache.size();
    }
    {
        auto& tables = track_tables();
        std::lock_guard<std::mutex> lock(tables.mutex);
        sizes.track_tables = tables.cache.size();
    }
    return sizes;
}

} // namespace pulp::canvas
