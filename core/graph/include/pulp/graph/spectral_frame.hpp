#pragma once

#include <array>
#include <bit>
#include <cmath>
#include <complex>
#include <cstdint>
#include <optional>
#include <span>
#include <utility>
#include <vector>

namespace pulp::graph {

enum class SpectralError : std::uint8_t {
    None = 0,
    InvalidIdentity = 1,
    InvalidMask = 2,
    IdentityMismatch = 3,
    InvalidConnection = 4,
    UnsupportedOperation = 5,
    RequiresStoppedController = 6,
    TransitionPending = 7,
    Busy = 8,
    PendingMailboxData = 9,
    UnsupportedTransactionalReplacement = 10,
    SpectralPinnedExecutionUnsupported = 11,
    EpochExhausted = 12,
    ChronologyExhausted = 13,
    CapacityExceeded = 14,
    InvalidFrame = 15,
    MissingFrame = 16,
    StageContractViolation = 17,
    SynthesisOverflow = 18,
    OutputUnderrun = 19,
    InvalidContract = 20
};

template <typename T> struct SpectralResult {
    std::optional<T> value;
    SpectralError error = SpectralError::None;
    explicit operator bool() const noexcept {
        return value.has_value();
    }
};

/// Versioned, canonical identity carried by every routed spectral batch.
/// These fields are semantic data, not an object-layout or enum ordinal.
struct SpectralFrameContract {
    static constexpr std::size_t encoded_size = 32;
    std::uint16_t version = 1;
    std::uint16_t profile = 1;
    std::uint32_t fft_size = 0;
    std::uint32_t hop = 0;
    std::uint16_t channels = 0;
    std::uint16_t bins = 0;
    std::uint64_t sample_rate_bits = 0;
    std::uint8_t window = 1;        // symmetric float Hann v1
    std::uint8_t phase = 1;         // local-window-origin v1
    std::uint8_t normalization = 1; // squared-window WOLA v1
    std::uint8_t precision = 1;     // binary32 complex bins v1

    friend bool operator==(const SpectralFrameContract&, const SpectralFrameContract&) = default;

    /// Canonical little-endian value encoding. The representation is fixed at
    /// 32 bytes and is independent of C++ padding, enum values, or host ABI.
    [[nodiscard]] std::array<std::uint8_t, encoded_size> encode() const noexcept {
        std::array<std::uint8_t, encoded_size> out{};
        auto put = [&](std::size_t offset, std::uint64_t value, unsigned bytes) {
            for (unsigned i = 0; i < bytes; ++i)
                out[offset + i] = static_cast<std::uint8_t>(value >> (8 * i));
        };
        put(0, version, 2);
        put(2, profile, 2);
        put(4, fft_size, 4);
        put(8, hop, 4);
        put(12, channels, 2);
        put(14, bins, 2);
        put(16, sample_rate_bits, 8);
        out[24] = window;
        out[25] = phase;
        out[26] = normalization;
        out[27] = precision;
        return out;
    }

    [[nodiscard]] static SpectralResult<SpectralFrameContract>
    decode(std::span<const std::uint8_t> bytes) noexcept {
        if (bytes.size() != encoded_size)
            return {std::nullopt, SpectralError::InvalidContract};
        auto get = [&](std::size_t offset, unsigned count) {
            std::uint64_t value = 0;
            for (unsigned i = 0; i < count; ++i)
                value |= std::uint64_t(bytes[offset + i]) << (8 * i);
            return value;
        };
        SpectralFrameContract contract;
        contract.version = static_cast<std::uint16_t>(get(0, 2));
        contract.profile = static_cast<std::uint16_t>(get(2, 2));
        contract.fft_size = static_cast<std::uint32_t>(get(4, 4));
        contract.hop = static_cast<std::uint32_t>(get(8, 4));
        contract.channels = static_cast<std::uint16_t>(get(12, 2));
        contract.bins = static_cast<std::uint16_t>(get(14, 2));
        contract.sample_rate_bits = get(16, 8);
        contract.window = bytes[24];
        contract.phase = bytes[25];
        contract.normalization = bytes[26];
        contract.precision = bytes[27];
        if (contract.version != 1 || contract.profile != 1 || contract.window != 1 ||
            contract.phase != 1 || contract.normalization != 1 || contract.precision != 1 ||
            bytes[28] != 0 || bytes[29] != 0 || bytes[30] != 0 || bytes[31] != 0 ||
            contract.fft_size < 256 || contract.fft_size > 16384 ||
            (contract.fft_size & (contract.fft_size - 1)) != 0 || contract.hop == 0 ||
            contract.hop > contract.fft_size / 2 || contract.channels == 0 ||
            contract.channels > 16 || contract.bins != contract.fft_size / 2 + 1 ||
            (contract.sample_rate_bits >> 63) ||
            (contract.sample_rate_bits & 0x7fffffffffffffffULL) == 0 ||
            (contract.sample_rate_bits & 0x7ff0000000000000ULL) == 0x7ff0000000000000ULL)
            return {std::nullopt, SpectralError::InvalidContract};
        return {contract};
    }
};

/// Profile-one indexed channels: symmetric float Hann, local-window phase,
/// forward scale one, inverse 1/N and full-overlap startup.
class SpectralIdentity {
  public:
    static SpectralResult<SpectralIdentity> make(int fft_size, int hop, int channels,
                                                 double sample_rate) {
        if (fft_size < 256 || fft_size > 16384 || (fft_size & (fft_size - 1)) != 0 || hop < 1 ||
            hop > fft_size / 2 || channels < 1 || channels > 16 || !std::isfinite(sample_rate) ||
            sample_rate <= 0)
            return {std::nullopt, SpectralError::InvalidIdentity};
        return {SpectralIdentity(fft_size, hop, channels, sample_rate)};
    }
    int fft_size() const noexcept {
        return fft_size_;
    }
    int hop() const noexcept {
        return hop_;
    }
    int channels() const noexcept {
        return channels_;
    }
    int bins() const noexcept {
        return fft_size_ / 2 + 1;
    }
    double sample_rate() const noexcept {
        return std::bit_cast<double>(rate_bits_);
    }
    std::uint64_t sample_rate_bits() const noexcept {
        return rate_bits_;
    }
    SpectralFrameContract frame_contract() const noexcept {
        SpectralFrameContract contract;
        contract.fft_size = static_cast<std::uint32_t>(fft_size_);
        contract.hop = static_cast<std::uint32_t>(hop_);
        contract.channels = static_cast<std::uint16_t>(channels_);
        contract.bins = static_cast<std::uint16_t>(bins());
        contract.sample_rate_bits = rate_bits_;
        return contract;
    }
    friend bool operator==(const SpectralIdentity&, const SpectralIdentity&) = default;

  private:
    SpectralIdentity(int n, int h, int c, double rate)
        : fft_size_(n), hop_(h), channels_(c), rate_bits_(std::bit_cast<std::uint64_t>(rate)) {}
    int fft_size_, hop_, channels_;
    std::uint64_t rate_bits_;
};

/// Closed catalog value. Only the built-in factories can create executable
/// stage descriptions; no callback or caller-supplied qualification exists.
class QualifiedSpectralStage {
  public:
    enum class Kind : std::uint8_t { IdentityV1, RealMaskV1 };
    static SpectralResult<QualifiedSpectralStage> identity_v1(SpectralIdentity identity) {
        return {QualifiedSpectralStage(identity, Kind::IdentityV1, {})};
    }
    static SpectralResult<QualifiedSpectralStage> real_mask_v1(SpectralIdentity identity,
                                                               std::span<const float> gains) {
        if (gains.size() != static_cast<size_t>(identity.channels()) * identity.bins())
            return {std::nullopt, SpectralError::InvalidMask};
        for (const float gain : gains)
            if (!std::isfinite(gain) || gain < 0 || gain > 1)
                return {std::nullopt, SpectralError::InvalidMask};
        std::vector<float> owned(gains.begin(), gains.end());
        for (auto& gain : owned)
            if (gain == 0)
                gain = 0.0f;
        return {QualifiedSpectralStage(identity, Kind::RealMaskV1, std::move(owned))};
    }
    const SpectralIdentity& identity() const noexcept {
        return identity_;
    }
    Kind kind() const noexcept {
        return kind_;
    }
    std::span<const float> gains() const noexcept {
        return gains_;
    }
    std::size_t retained_config_bytes() const noexcept {
        return gains_.capacity() * sizeof(float);
    }

  private:
    QualifiedSpectralStage(SpectralIdentity identity, Kind kind, std::vector<float> gains)
        : identity_(identity), kind_(kind), gains_(std::move(gains)) {}
    SpectralIdentity identity_;
    Kind kind_;
    std::vector<float> gains_;
};

struct SpectralFrameStamp {
    std::uint64_t epoch = 0;
    std::uint64_t sequence = 0;
    std::int64_t origin = 0;
    std::int64_t completion = 0;
    friend bool operator==(const SpectralFrameStamp&, const SpectralFrameStamp&) = default;
};

struct SpectralFrameReadView {
    SpectralFrameStamp stamp;
    std::span<const std::complex<float>> bins;
};

/// Snapshot-owned payload and descriptors; callback borrows never own storage.
struct SpectralFrameBatchView {
    std::span<SpectralFrameStamp> stamps;
    std::span<std::complex<float>> bins;
    std::uint32_t count = 0;
    std::uint32_t bins_per_frame = 0;
    SpectralFrameContract contract;
    bool fatal = false;
    std::int64_t fatal_origin = 0;
};

} // namespace pulp::graph
