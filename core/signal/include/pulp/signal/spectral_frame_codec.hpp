#pragma once

#include <bit>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <new>
#include <optional>
#include <span>

namespace pulp::signal {

/// Offline profile 1: symmetric float Hann, unscaled forward FFT, 1/N inverse,
/// local-origin phase, indexed channels, full-overlap startup and squared-window
/// OLA normalization (threshold 1e-9f). No graph, epoch or source length is stored.
struct SpectralStreamGeometry {
    std::uint32_t fft_size = 256;
    std::uint32_t hop = 64;
    std::uint16_t channels = 1;
    double sample_rate = 48000;
    std::uint32_t profile = 1;
    std::uint16_t layout = 1;
};

struct SpectralFrameStamp {
    std::uint64_t sequence = 0;
    std::int64_t origin = 0;
};

/// Arrays have exact element counts, with no hidden vector capacity. Components
/// are frame/channel/bin/real-imaginary ordered. Values own their storage.
struct OwnedSpectralStream {
    SpectralStreamGeometry geometry;
    std::uint64_t frame_count = 0;
    std::size_t component_count = 0;
    std::unique_ptr<SpectralFrameStamp[]> frames;
    std::unique_ptr<float[]> components;
};

struct EncodedSpectralStream {
    std::size_t size = 0;
    std::unique_ptr<std::uint8_t[]> bytes;
    [[nodiscard]] std::span<const std::uint8_t> view() const noexcept {
        return {bytes.get(), size};
    }
};

enum class SpectralCodecError {
    None,
    UnsupportedFormat,
    InvalidGeometry,
    ResourceLimit,
    InvalidLength,
    InvalidChronology,
    InvalidComponent,
    NoncanonicalZero,
    AllocationFailure
};

struct SpectralCodecMemory {
    /// Destination arrays plus result object; allocator bookkeeping is excluded.
    std::uint64_t retained_bytes = 0;
    /// Caller-owned input is reported separately and never copied for validation.
    std::uint64_t input_bytes = 0;
    /// Conservative allowance for fixed local validation/result objects; no heap scratch.
    std::uint64_t temporary_bytes = 4096;
};

template <class T> struct SpectralCodecResult {
    SpectralCodecError error = SpectralCodecError::None;
    SpectralCodecMemory memory;
    std::optional<T> value;
    explicit operator bool() const noexcept {
        return value.has_value();
    }
};

inline constexpr std::uint64_t spectral_codec_memory_budget = 128ULL * 1024 * 1024;

namespace spectral_codec_detail {
static_assert(sizeof(float) == 4 && std::numeric_limits<float>::is_iec559);
static_assert(sizeof(double) == 8 && std::numeric_limits<double>::is_iec559);

struct Shape {
    std::uint64_t components = 0, record_bytes = 0, payload = 0;
    std::int64_t first_origin = 0;
};

inline SpectralCodecError shape(const SpectralStreamGeometry& g, std::uint64_t count,
                                Shape& s) noexcept {
    if (g.profile != 1 || g.layout != 1)
        return SpectralCodecError::UnsupportedFormat;
    const auto rate = std::bit_cast<std::uint64_t>(g.sample_rate);
    if (g.fft_size < 256 || g.fft_size > 16384 || (g.fft_size & (g.fft_size - 1)) || !g.hop ||
        g.hop > g.fft_size / 2 || !g.channels || g.channels > 16 || (rate >> 63) ||
        !(rate & 0x7fffffffffffffffULL) || (rate & 0x7ff0000000000000ULL) == 0x7ff0000000000000ULL)
        return SpectralCodecError::InvalidGeometry;
    if (!count || count > 1048576)
        return SpectralCodecError::ResourceLimit;
    // All operands are capped before multiplication: no untrusted product can wrap.
    s.components = 2ULL * g.channels * (g.fft_size / 2 + 1);
    s.record_bytes = 16 + 4 * s.components;
    s.payload = count * s.record_bytes;
    if (s.payload > 67108864)
        return SpectralCodecError::ResourceLimit;
    s.first_origin = -static_cast<std::int64_t>((g.fft_size - 1) / g.hop * g.hop);
    return SpectralCodecError::None;
}

inline std::uint64_t read(std::span<const std::uint8_t> b, std::size_t p, unsigned n) noexcept {
    std::uint64_t v = 0;
    for (unsigned i = 0; i < n; ++i)
        v |= std::uint64_t(b[p + i]) << (8 * i);
    return v;
}
inline void write(std::uint8_t* b, std::size_t p, std::uint64_t v, unsigned n) noexcept {
    for (unsigned i = 0; i < n; ++i)
        b[p + i] = static_cast<std::uint8_t>(v >> (8 * i));
}
inline SpectralCodecError component(std::uint32_t bits, std::uint64_t index, std::uint32_t fft,
                                    bool strict) noexcept {
    if ((bits & 0x7f800000U) == 0x7f800000U)
        return SpectralCodecError::InvalidComponent;
    if (strict && bits == 0x80000000U)
        return SpectralCodecError::NoncanonicalZero;
    const auto local = index % (2ULL * (fft / 2 + 1));
    if ((local == 1 || local == fft + 1) && (bits & 0x7fffffffU))
        return SpectralCodecError::InvalidComponent;
    return SpectralCodecError::None;
}
} // namespace spectral_codec_detail

/// Allocate an editable owned stream with canonical chronology and zero bins.
/// Nonthrowing allocation failure leaves no partially published value.
inline SpectralCodecResult<OwnedSpectralStream>
make_spectral_stream(SpectralStreamGeometry g, std::uint64_t count,
                     std::uint64_t budget = spectral_codec_memory_budget) noexcept {
    SpectralCodecResult<OwnedSpectralStream> r;
    spectral_codec_detail::Shape s;
    r.error = spectral_codec_detail::shape(g, count, s);
    if (r.error != SpectralCodecError::None)
        return r;
    r.memory.retained_bytes =
        sizeof(r) + count * sizeof(SpectralFrameStamp) + count * s.components * sizeof(float);
    if (r.memory.retained_bytes > budget ||
        r.memory.temporary_bytes > budget - r.memory.retained_bytes) {
        r.error = SpectralCodecError::ResourceLimit;
        return r;
    }
    OwnedSpectralStream out;
    out.geometry = g;
    out.frame_count = count;
    out.component_count = static_cast<std::size_t>(count * s.components);
    out.frames.reset(new (std::nothrow) SpectralFrameStamp[static_cast<std::size_t>(count)]);
    out.components.reset(new (std::nothrow) float[out.component_count]{});
    if (!out.frames || !out.components) {
        r.error = SpectralCodecError::AllocationFailure;
        return r;
    }
    for (std::uint64_t i = 0; i < count; ++i)
        out.frames[i] = {i, s.first_origin + static_cast<std::int64_t>(i * g.hop)};
    r.value.emplace(std::move(out));
    return r;
}

/// Validate the whole input before allocating; no partial stream is returned.
/// Bin checks operate on bits so subnormals survive host flush-to-zero modes.
inline SpectralCodecResult<OwnedSpectralStream>
decode_spectral_stream(std::span<const std::uint8_t> bytes,
                       std::uint64_t budget = spectral_codec_memory_budget) noexcept {
    using namespace spectral_codec_detail;
    SpectralCodecResult<OwnedSpectralStream> r;
    r.memory.input_bytes = bytes.size();
    auto fail = [&](SpectralCodecError e) {
        r.error = e;
        return std::move(r);
    };
    if (bytes.size() < 64)
        return fail(SpectralCodecError::InvalidLength);
    constexpr char magic[] = "PULPSFRM";
    for (unsigned i = 0; i < 8; ++i)
        if (bytes[i] != magic[i])
            return fail(SpectralCodecError::UnsupportedFormat);
    if (read(bytes, 8, 2) != 1 || read(bytes, 10, 2) != 64 || read(bytes, 28, 4) ||
        read(bytes, 56, 8))
        return fail(SpectralCodecError::UnsupportedFormat);
    SpectralStreamGeometry g;
    g.profile = static_cast<std::uint32_t>(read(bytes, 12, 4));
    g.fft_size = static_cast<std::uint32_t>(read(bytes, 16, 4));
    g.hop = static_cast<std::uint32_t>(read(bytes, 20, 4));
    g.channels = static_cast<std::uint16_t>(read(bytes, 24, 2));
    g.layout = static_cast<std::uint16_t>(read(bytes, 26, 2));
    g.sample_rate = std::bit_cast<double>(read(bytes, 32, 8));
    const auto count = read(bytes, 40, 8);
    Shape s;
    const auto error = shape(g, count, s);
    if (error != SpectralCodecError::None)
        return fail(error);
    if (read(bytes, 48, 8) != s.payload || bytes.size() != 64 + s.payload)
        return fail(SpectralCodecError::InvalidLength);
    r.memory.retained_bytes =
        sizeof(r) + count * sizeof(SpectralFrameStamp) + count * s.components * sizeof(float);
    if (r.memory.retained_bytes > budget ||
        r.memory.temporary_bytes > budget - r.memory.retained_bytes)
        return fail(SpectralCodecError::ResourceLimit);
    for (std::uint64_t i = 0; i < count; ++i) {
        const auto p = static_cast<std::size_t>(64 + i * s.record_bytes);
        // The capped count/hop keep origin and completion far inside signed i64.
        const auto origin = s.first_origin + static_cast<std::int64_t>(i * g.hop);
        if (read(bytes, p, 8) != i || read(bytes, p + 8, 8) != std::bit_cast<std::uint64_t>(origin))
            return fail(SpectralCodecError::InvalidChronology);
        for (std::uint64_t j = 0; j < s.components; ++j) {
            const auto e = component(static_cast<std::uint32_t>(read(bytes, p + 16 + 4 * j, 4)), j,
                                     g.fft_size, true);
            if (e != SpectralCodecError::None)
                return fail(e);
        }
    }
    r = make_spectral_stream(g, count, budget);
    r.memory.input_bytes = bytes.size();
    if (!r)
        return r;
    for (std::uint64_t i = 0; i < count; ++i)
        for (std::uint64_t j = 0; j < s.components; ++j)
            r.value->components[i * s.components + j] = std::bit_cast<float>(
                static_cast<std::uint32_t>(read(bytes, 80 + i * s.record_bytes + 4 * j, 4)));
    return r;
}

/// Canonicalize signed bin zeros without mutating the caller. Offline only.
inline SpectralCodecResult<EncodedSpectralStream>
encode_spectral_stream(const OwnedSpectralStream& in,
                       std::uint64_t budget = spectral_codec_memory_budget) noexcept {
    using namespace spectral_codec_detail;
    SpectralCodecResult<EncodedSpectralStream> r;
    auto fail = [&](SpectralCodecError e) {
        r.error = e;
        return std::move(r);
    };
    Shape s;
    const auto error = shape(in.geometry, in.frame_count, s);
    if (error != SpectralCodecError::None)
        return fail(error);
    if (in.component_count != in.frame_count * s.components || !in.frames || !in.components)
        return fail(SpectralCodecError::InvalidLength);
    r.memory.input_bytes = sizeof(in) + in.frame_count * sizeof(SpectralFrameStamp) +
                           in.component_count * sizeof(float);
    r.memory.retained_bytes = sizeof(r) + 64 + s.payload;
    if (r.memory.retained_bytes > budget ||
        r.memory.temporary_bytes > budget - r.memory.retained_bytes)
        return fail(SpectralCodecError::ResourceLimit);
    for (std::uint64_t i = 0; i < in.frame_count; ++i) {
        if (in.frames[i].sequence != i ||
            in.frames[i].origin != s.first_origin + static_cast<std::int64_t>(i * in.geometry.hop))
            return fail(SpectralCodecError::InvalidChronology);
    }
    for (std::size_t j = 0; j < in.component_count; ++j) {
        const auto e = component(std::bit_cast<std::uint32_t>(in.components[j]), j,
                                 in.geometry.fft_size, false);
        if (e != SpectralCodecError::None)
            return fail(e);
    }
    EncodedSpectralStream out;
    out.size = static_cast<std::size_t>(64 + s.payload);
    out.bytes.reset(new (std::nothrow) std::uint8_t[out.size]{});
    if (!out.bytes)
        return fail(SpectralCodecError::AllocationFailure);
    auto* b = out.bytes.get();
    constexpr char magic[] = "PULPSFRM";
    for (unsigned i = 0; i < 8; ++i)
        b[i] = magic[i];
    write(b, 8, 1, 2);
    write(b, 10, 64, 2);
    write(b, 12, 1, 4);
    write(b, 16, in.geometry.fft_size, 4);
    write(b, 20, in.geometry.hop, 4);
    write(b, 24, in.geometry.channels, 2);
    write(b, 26, 1, 2);
    write(b, 32, std::bit_cast<std::uint64_t>(in.geometry.sample_rate), 8);
    write(b, 40, in.frame_count, 8);
    write(b, 48, s.payload, 8);
    for (std::uint64_t i = 0; i < in.frame_count; ++i) {
        const auto p = static_cast<std::size_t>(64 + i * s.record_bytes);
        write(b, p, in.frames[i].sequence, 8);
        write(b, p + 8, std::bit_cast<std::uint64_t>(in.frames[i].origin), 8);
        for (std::uint64_t j = 0; j < s.components; ++j) {
            auto bits = std::bit_cast<std::uint32_t>(in.components[i * s.components + j]);
            if ((bits & 0x7fffffffU) == 0)
                bits = 0;
            write(b, p + 16 + 4 * j, bits, 4);
        }
    }
    r.value.emplace(std::move(out));
    return r;
}

} // namespace pulp::signal
