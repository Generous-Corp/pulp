#include <pulp/audio/waveform_overview.hpp>

#include <cstring>
#include <limits>
#include <type_traits>
#include <utility>
#include <vector>

namespace pulp::audio {

// On-disk thumbnail blob format.
//
// Magic     : 4 bytes  "PTHM"
// Version   : 2 bytes  little-endian (currently 1)
// num_channels      : 4 bytes LE
// num_source_frames : 8 bytes LE
// sample_rate       : 4 bytes LE
// num_levels        : 4 bytes LE
//   per-level (num_levels times):
//     samples_per_peak  : 4 bytes LE
//     peaks_per_channel : 4 bytes LE
//       per-channel (num_channels times):
//         per-peak (peaks_per_channel times):
//           min_q7 : 1 byte  (int8)
//           max_q7 : 1 byte  (int8)
//
// Bumping `kOverviewDiskVersion` invalidates every existing on-disk
// cache entry; that is the contract.

namespace {

constexpr char     kOverviewMagic[4] = {'P', 'W', 'O', 'V'};
constexpr uint16_t kOverviewDiskVersion = 1;

template <typename T>
void append_le(std::vector<uint8_t>& out, T value) {
    static_assert(std::is_integral_v<T>, "append_le wants an integer");
    for (std::size_t i = 0; i < sizeof(T); ++i) {
        out.push_back(static_cast<uint8_t>((value >> (8 * i)) & 0xFFu));
    }
}

template <typename T>
bool read_le(const uint8_t* data, std::size_t size, std::size_t& off, T& out) {
    if (off + sizeof(T) > size) return false;
    T v = 0;
    for (std::size_t i = 0; i < sizeof(T); ++i) {
        v = static_cast<T>(v | (static_cast<T>(data[off + i]) << (8 * i)));
    }
    off += sizeof(T);
    out = v;
    return true;
}

}  // namespace

std::vector<uint8_t> serialize_thumbnail(const WaveformOverview& t) {
    const auto info = t.info();
    std::vector<uint8_t> out;
    // 4 (magic) + 2 (ver) + 4 + 8 + 4 + 4 + per-level overhead + peaks.
    out.reserve(26 + info.bytes_used + info.num_levels * 8);
    out.insert(out.end(),
               reinterpret_cast<const uint8_t*>(kOverviewMagic),
               reinterpret_cast<const uint8_t*>(kOverviewMagic) + 4);
    append_le<uint16_t>(out, kOverviewDiskVersion);
    append_le<uint32_t>(out, info.num_channels);
    append_le<uint64_t>(out, info.num_source_frames);
    append_le<uint32_t>(out, info.sample_rate);
    append_le<uint32_t>(out, static_cast<uint32_t>(info.num_levels));
    for (std::size_t i = 0; i < info.num_levels; ++i) {
        const auto& lvl = t.level(i);
        append_le<uint32_t>(out, lvl.samples_per_peak);
        append_le<uint32_t>(out, lvl.peaks_per_channel);
        for (const auto& ch : lvl.peaks) {
            for (const auto& pk : ch) {
                out.push_back(static_cast<uint8_t>(pk.min_q7));
                out.push_back(static_cast<uint8_t>(pk.max_q7));
            }
        }
    }
    return out;
}

std::optional<WaveformOverview> deserialize_thumbnail(const uint8_t* data,
                                                    std::size_t size) {
    if (data == nullptr || size < 26) return std::nullopt;
    if (std::memcmp(data, kOverviewMagic, 4) != 0) return std::nullopt;
    std::size_t off = 4;
    uint16_t ver = 0;
    if (!read_le<uint16_t>(data, size, off, ver)) return std::nullopt;
    if (ver != kOverviewDiskVersion) return std::nullopt;
    uint32_t num_channels = 0;
    uint64_t num_frames = 0;
    uint32_t sample_rate = 0;
    uint32_t num_levels = 0;
    if (!read_le<uint32_t>(data, size, off, num_channels)) return std::nullopt;
    if (!read_le<uint64_t>(data, size, off, num_frames)) return std::nullopt;
    if (!read_le<uint32_t>(data, size, off, sample_rate)) return std::nullopt;
    if (!read_le<uint32_t>(data, size, off, num_levels)) return std::nullopt;
    // Reasonable upper bounds so a corrupt header can't allocate a TB of RAM.
    if (num_channels == 0 || num_channels > 64) return std::nullopt;
    if (num_frames == 0) return std::nullopt;
    if (sample_rate == 0) return std::nullopt;
    if (num_levels == 0 || num_levels > 32) return std::nullopt;

    // Header validation is complete; rebuild the exact serialized level
    // hierarchy through WaveformOverview's internal factory without re-decimating.
    return WaveformOverview::from_serialized_levels(
        num_channels, num_frames, sample_rate, num_levels,
        data, size, off);
}

std::optional<WaveformOverview> WaveformOverview::from_serialized_levels(
    uint32_t num_channels,
    uint64_t num_source_frames,
    uint32_t sample_rate,
    uint32_t num_levels,
    const uint8_t* data,
    std::size_t size,
    std::size_t offset) {
    WaveformOverview t;
    t.num_channels_ = num_channels;
    t.num_source_frames_ = num_source_frames;
    t.sample_rate_ = sample_rate;
    t.levels_.reserve(num_levels);

    std::size_t off = offset;
    for (uint32_t i = 0; i < num_levels; ++i) {
        uint32_t samples_per_peak = 0;
        uint32_t peaks_per_channel = 0;
        if (!read_le<uint32_t>(data, size, off, samples_per_peak)) return std::nullopt;
        if (!read_le<uint32_t>(data, size, off, peaks_per_channel)) return std::nullopt;
        if (samples_per_peak == 0 || peaks_per_channel == 0) return std::nullopt;
        // Per-peak bytes: num_channels * peaks_per_channel * 2 (min,max).
        const std::size_t need = static_cast<std::size_t>(num_channels)
                               * static_cast<std::size_t>(peaks_per_channel)
                               * 2u;
        if (off + need > size) return std::nullopt;
        OverviewLevel lvl;
        lvl.samples_per_peak = samples_per_peak;
        lvl.peaks_per_channel = peaks_per_channel;
        lvl.peaks.resize(num_channels);
        for (uint32_t ch = 0; ch < num_channels; ++ch) {
            lvl.peaks[ch].resize(peaks_per_channel);
            for (uint32_t p = 0; p < peaks_per_channel; ++p) {
                AudioPeak pk;
                pk.min_q7 = static_cast<int8_t>(data[off++]);
                pk.max_q7 = static_cast<int8_t>(data[off++]);
                lvl.peaks[ch][p] = pk;
            }
        }
        t.levels_.push_back(std::move(lvl));
    }
    if (t.levels_.empty() || off != size) return std::nullopt;
    return t;
}

}  // namespace pulp::audio
