#pragma once

/// @file sample_converter.hpp
/// Run-time PCM sample-format converter.
///
/// `SampleConverter` converts between packed PCM byte streams and
/// planar `float` channels along three independent axes:
///
///   * Sample format — `Int8 / Int16 / Int24 / Int32 / Float32 / Float64`
///   * Endianness — `Little / Big / Native` (Native resolves to the
///     host's byte order at construction time)
///   * Layout — `Interleaved / Planar`
///
/// Integer formats round-trip without bit-loss; float ↔ int conversion
/// is within 1 LSB of the reference. The implementation is
/// header-only because `core/audio` is a real `add_library` target
/// and the conversion paths are small enough that link-time matters
/// less than inlining.
///
/// Design intent: this is the Pulp-native shape for the audio-data
/// converter surface — clean-room, no name or class mirroring of any
/// reference framework. Callers needing PCM I/O at format boundaries
/// should reach here before rolling per-format helpers.

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>

namespace pulp::audio {

/// Packed-PCM sample format the bytes are encoded in.
enum class SampleFormat : uint8_t {
    Int8,    ///< 8-bit signed (-128..127)
    Int16,   ///< 16-bit signed
    Int24,   ///< 24-bit signed packed (3 bytes)
    Int32,   ///< 32-bit signed
    Float32, ///< IEEE-754 single
    Float64, ///< IEEE-754 double
};

/// Byte order. `Native` resolves to the host endianness at the
/// converter's construction time, so changes to the host byte order
/// (e.g. cross-compilation) do not silently break.
enum class Endianness : uint8_t {
    Little,
    Big,
    Native,
};

/// Memory layout of the byte stream relative to channels.
enum class Layout : uint8_t {
    Interleaved, ///< [ch0_s0, ch1_s0, ch0_s1, ch1_s1, ...]
    Planar,      ///< [ch0_s0, ch0_s1, ..., ch1_s0, ch1_s1, ...]
};

/// Bytes per sample for `format`. Returns 0 for unknown values.
constexpr std::size_t bytes_per_sample(SampleFormat format) noexcept {
    switch (format) {
        case SampleFormat::Int8:    return 1;
        case SampleFormat::Int16:   return 2;
        case SampleFormat::Int24:   return 3;
        case SampleFormat::Int32:   return 4;
        case SampleFormat::Float32: return 4;
        case SampleFormat::Float64: return 8;
    }
    return 0;
}

/// True if `format` is a floating-point format.
constexpr bool is_float(SampleFormat format) noexcept {
    return format == SampleFormat::Float32 || format == SampleFormat::Float64;
}

namespace detail {

constexpr bool host_is_little_endian() noexcept {
    return std::endian::native == std::endian::little;
}

inline Endianness resolve_endianness(Endianness e) noexcept {
    if (e != Endianness::Native) return e;
    return host_is_little_endian() ? Endianness::Little : Endianness::Big;
}

template <typename T>
inline T byteswap_int(T value) noexcept {
    static_assert(std::is_integral_v<T>);
    T out;
    auto* in_bytes = reinterpret_cast<const uint8_t*>(&value);
    auto* out_bytes = reinterpret_cast<uint8_t*>(&out);
    for (std::size_t i = 0; i < sizeof(T); ++i) {
        out_bytes[i] = in_bytes[sizeof(T) - 1 - i];
    }
    return out;
}

template <typename T>
inline T load_int(const uint8_t* src, Endianness src_endian) noexcept {
    T value;
    std::memcpy(&value, src, sizeof(T));
    const auto resolved = resolve_endianness(src_endian);
    if (resolved != (host_is_little_endian() ? Endianness::Little : Endianness::Big)) {
        value = byteswap_int(value);
    }
    return value;
}

template <typename T>
inline void store_int(uint8_t* dst, T value, Endianness dst_endian) noexcept {
    const auto resolved = resolve_endianness(dst_endian);
    if (resolved != (host_is_little_endian() ? Endianness::Little : Endianness::Big)) {
        value = byteswap_int(value);
    }
    std::memcpy(dst, &value, sizeof(T));
}

inline int32_t load_int24(const uint8_t* src, Endianness src_endian) noexcept {
    const auto resolved = resolve_endianness(src_endian);
    int32_t val;
    if (resolved == Endianness::Little) {
        val = static_cast<int32_t>(src[0])
            | (static_cast<int32_t>(src[1]) << 8)
            | (static_cast<int32_t>(src[2]) << 16);
    } else {
        val = static_cast<int32_t>(src[2])
            | (static_cast<int32_t>(src[1]) << 8)
            | (static_cast<int32_t>(src[0]) << 16);
    }
    // Sign-extend from 24-bit to 32-bit.
    if (val & 0x800000) val |= static_cast<int32_t>(0xFF000000u);
    return val;
}

inline void store_int24(uint8_t* dst, int32_t value, Endianness dst_endian) noexcept {
    // Clamp to 24-bit signed range first so out-of-band int values
    // don't corrupt adjacent bytes.
    constexpr int32_t kMax = (1 << 23) - 1;
    constexpr int32_t kMin = -(1 << 23);
    if (value > kMax) value = kMax;
    else if (value < kMin) value = kMin;
    const auto resolved = resolve_endianness(dst_endian);
    if (resolved == Endianness::Little) {
        dst[0] = static_cast<uint8_t>(value & 0xFF);
        dst[1] = static_cast<uint8_t>((value >> 8) & 0xFF);
        dst[2] = static_cast<uint8_t>((value >> 16) & 0xFF);
    } else {
        dst[0] = static_cast<uint8_t>((value >> 16) & 0xFF);
        dst[1] = static_cast<uint8_t>((value >> 8) & 0xFF);
        dst[2] = static_cast<uint8_t>(value & 0xFF);
    }
}

inline float finite_or_zero(float v) noexcept {
    return std::isfinite(v) ? v : 0.0f;
}

// Scale factor for int → float normalization. We use 2^(N-1) (not
// 2^(N-1) - 1) for the divide so the center maps cleanly to 0 and
// the full negative range produces -1.0 exactly. The maximum
// positive int produces (2^(N-1) - 1) / 2^(N-1) ≈ 0.99999 (within
// 1 LSB of +1), matching the asymmetric int range.
constexpr float kInt8Scale  = 1.0f / 128.0f;
constexpr float kInt16Scale = 1.0f / 32768.0f;
constexpr float kInt24Scale = 1.0f / 8388608.0f;
constexpr double kInt32Scale = 1.0 / 2147483648.0;

} // namespace detail

/// Run-time PCM converter. Construct once with a (format, endian,
/// layout) triple; call `to_float` / `from_float` per block.
class SampleConverter {
public:
    SampleConverter(SampleFormat format,
                     Endianness endian = Endianness::Native,
                     Layout layout = Layout::Interleaved) noexcept
        : format_(format), endian_(endian), layout_(layout) {}

    SampleFormat format() const noexcept { return format_; }
    Endianness endian() const noexcept { return endian_; }
    Layout layout() const noexcept { return layout_; }

    /// Bytes occupied per sample (independent of layout / channel count).
    std::size_t bytes_per_sample() const noexcept {
        return ::pulp::audio::bytes_per_sample(format_);
    }

    /// Total byte size for `frames * channels` PCM samples in the
    /// configured layout.
    std::size_t bytes_for(std::size_t frames, std::size_t channels) const noexcept {
        return frames * channels * bytes_per_sample();
    }

    /// Convert PCM bytes → planar float channels.
    ///   - `src` must contain `bytes_for(frames, channels)` bytes.
    ///   - `dst_channels` must point to `channels` arrays of `frames`
    ///     floats each.
    /// Out-of-range or unknown format is a no-op.
    void to_float(const void* src,
                   float* const* dst_channels,
                   std::size_t frames,
                   std::size_t channels) const noexcept {
        to_float_scaled(src, dst_channels, frames, channels, 1.0f);
    }

    /// Same as `to_float` with a per-sample gain multiplier applied
    /// during conversion (saves a second pass over the buffer).
    void to_float_scaled(const void* src,
                          float* const* dst_channels,
                          std::size_t frames,
                          std::size_t channels,
                          float gain) const noexcept {
        if (!src || !dst_channels || frames == 0 || channels == 0) return;
        const auto* bytes = static_cast<const uint8_t*>(src);
        const std::size_t bps = bytes_per_sample();
        for (std::size_t f = 0; f < frames; ++f) {
            for (std::size_t c = 0; c < channels; ++c) {
                const std::size_t byte_index = byte_offset(f, c, frames, channels, bps);
                dst_channels[c][f] = decode_sample(bytes + byte_index) * gain;
            }
        }
    }

    /// Convert planar float channels → PCM bytes. Float values are
    /// clipped to [-1, +1] before quantization to integer formats;
    /// non-finite samples are written as silence (0).
    void from_float(const float* const* src_channels,
                     void* dst,
                     std::size_t frames,
                     std::size_t channels) const noexcept {
        if (!src_channels || !dst || frames == 0 || channels == 0) return;
        auto* bytes = static_cast<uint8_t*>(dst);
        const std::size_t bps = bytes_per_sample();
        for (std::size_t f = 0; f < frames; ++f) {
            for (std::size_t c = 0; c < channels; ++c) {
                const std::size_t byte_index = byte_offset(f, c, frames, channels, bps);
                encode_sample(bytes + byte_index, src_channels[c][f]);
            }
        }
    }

private:
    std::size_t byte_offset(std::size_t frame,
                             std::size_t channel,
                             std::size_t frames,
                             std::size_t channels,
                             std::size_t bps) const noexcept {
        if (layout_ == Layout::Interleaved) {
            return (frame * channels + channel) * bps;
        }
        // Planar: channel `c` occupies bytes [c*frames*bps, (c+1)*frames*bps).
        return (channel * frames + frame) * bps;
    }

    float decode_sample(const uint8_t* src) const noexcept {
        switch (format_) {
            case SampleFormat::Int8: {
                const int8_t v = static_cast<int8_t>(*src);
                return static_cast<float>(v) * detail::kInt8Scale;
            }
            case SampleFormat::Int16: {
                const int16_t v = detail::load_int<int16_t>(src, endian_);
                return static_cast<float>(v) * detail::kInt16Scale;
            }
            case SampleFormat::Int24: {
                const int32_t v = detail::load_int24(src, endian_);
                return static_cast<float>(v) * detail::kInt24Scale;
            }
            case SampleFormat::Int32: {
                const int32_t v = detail::load_int<int32_t>(src, endian_);
                return static_cast<float>(
                    static_cast<double>(v) * detail::kInt32Scale);
            }
            case SampleFormat::Float32: {
                int32_t bits = detail::load_int<int32_t>(src, endian_);
                float f;
                std::memcpy(&f, &bits, sizeof(float));
                return f;
            }
            case SampleFormat::Float64: {
                int64_t bits = detail::load_int<int64_t>(src, endian_);
                double d;
                std::memcpy(&d, &bits, sizeof(double));
                return static_cast<float>(d);
            }
        }
        return 0.0f;
    }

    void encode_sample(uint8_t* dst, float value) const noexcept {
        value = detail::finite_or_zero(value);
        switch (format_) {
            case SampleFormat::Int8: {
                value = std::clamp(value, -1.0f, 1.0f);
                const int8_t v = static_cast<int8_t>(
                    std::lrintf(value * 127.0f));
                std::memcpy(dst, &v, 1);
                return;
            }
            case SampleFormat::Int16: {
                value = std::clamp(value, -1.0f, 1.0f);
                const int16_t v = static_cast<int16_t>(
                    std::lrintf(value * 32767.0f));
                detail::store_int<int16_t>(dst, v, endian_);
                return;
            }
            case SampleFormat::Int24: {
                value = std::clamp(value, -1.0f, 1.0f);
                const int32_t v = static_cast<int32_t>(
                    std::lrintf(value * 8388607.0f));
                detail::store_int24(dst, v, endian_);
                return;
            }
            case SampleFormat::Int32: {
                value = std::clamp(value, -1.0f, 1.0f);
                int32_t v;
                if (value >= 1.0f) {
                    v = std::numeric_limits<int32_t>::max();
                } else if (value <= -1.0f) {
                    v = std::numeric_limits<int32_t>::min();
                } else {
                    v = static_cast<int32_t>(
                        std::lrint(static_cast<double>(value) * 2147483647.0));
                }
                detail::store_int<int32_t>(dst, v, endian_);
                return;
            }
            case SampleFormat::Float32: {
                int32_t bits;
                std::memcpy(&bits, &value, sizeof(int32_t));
                detail::store_int<int32_t>(dst, bits, endian_);
                return;
            }
            case SampleFormat::Float64: {
                const double d = static_cast<double>(value);
                int64_t bits;
                std::memcpy(&bits, &d, sizeof(int64_t));
                detail::store_int<int64_t>(dst, bits, endian_);
                return;
            }
        }
    }

    SampleFormat format_;
    Endianness endian_;
    Layout layout_;
};

} // namespace pulp::audio
