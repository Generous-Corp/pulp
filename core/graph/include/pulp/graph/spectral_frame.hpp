#pragma once

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
    None,
    InvalidIdentity,
    InvalidMask,
    IdentityMismatch,
    InvalidConnection,
    UnsupportedOperation,
    RequiresStoppedController,
    TransitionPending,
    Busy,
    PendingMailboxData,
    UnsupportedTransactionalReplacement,
    SpectralPinnedExecutionUnsupported,
    EpochExhausted,
    ChronologyExhausted,
    CapacityExceeded,
    InvalidFrame,
    MissingFrame,
    StageContractViolation,
    SynthesisOverflow,
    OutputUnderrun
};

template <typename T> struct SpectralResult {
    std::optional<T> value;
    SpectralError error = SpectralError::None;
    explicit operator bool() const noexcept {
        return value.has_value();
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
    bool fatal = false;
    std::int64_t fatal_origin = 0;
};

} // namespace pulp::graph
