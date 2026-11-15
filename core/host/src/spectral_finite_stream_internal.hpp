#pragma once

#include <cstdint>
#include <limits>

namespace pulp::host {

// The end-of-stream rule for a finite spectral source at codec geometry
// (fft N, hop H dividing N, C channels) covering L source samples. A stream holds exactly
// ceil((L + N - H) / H) frames; the routed cutoff after which output is silent
// is N + H + frames * H samples; the node publishes frames + N / H frames so
// the synthesis tail drains; frame f, channel c, bin k sits at component
// f * C * (N / 2 + 1) * 2 + c * (N / 2 + 1) * 2 + 2 * k (re, im).
// Every value is derived with checked arithmetic against signed chronology.
struct SpectralFiniteStreamRule {
    // Each refused geometry carries its own status so a caller can name the
    // reason; reason() spells it.
    enum class Status {
        ok,
        unset,
        fft_size_not_admitted,
        hop_not_admitted,
        hop_does_not_divide_fft_size,
        channels_not_admitted,
        invalid_length,
        capacity_exceeded
    };

    Status status = Status::unset;
    std::uint64_t fft_size = 0, hop = 0, channels = 0, bins = 0;
    std::uint64_t source_samples = 0, frame_count = 0;
    std::uint64_t render_cutoff = 0, finite_frames = 0;

    explicit operator bool() const noexcept {
        return status == Status::ok;
    }

    std::uint64_t frame_stride() const noexcept {
        return channels * bins * 2;
    }
    std::uint64_t component(std::uint64_t frame, std::uint64_t channel,
                            std::uint64_t bin) const noexcept {
        return frame * frame_stride() + channel * bins * 2 + 2 * bin;
    }

    bool geometry_refused() const noexcept {
        return status == Status::fft_size_not_admitted || status == Status::hop_not_admitted ||
               status == Status::hop_does_not_divide_fft_size ||
               status == Status::channels_not_admitted;
    }

    static constexpr const char* reason(Status s) noexcept {
        switch (s) {
        case Status::ok:
            return "ok";
        case Status::unset:
            return "rule not computed";
        case Status::fft_size_not_admitted:
            return "fft size is not a power of two from 256 to 16384";
        case Status::hop_not_admitted:
            return "hop is not from 1 to half the fft size";
        case Status::hop_does_not_divide_fft_size:
            return "hop does not divide the fft size";
        case Status::channels_not_admitted:
            return "channel count is not from 1 to 16";
        case Status::invalid_length:
            return "source length is zero or exceeds signed chronology";
        case Status::capacity_exceeded:
            return "frame count exceeds signed chronology";
        }
        return "unknown";
    }

    // Admits the codec geometry range where the hop divides the FFT size. Only
    // there does ceil((L + N - H) / H) equal the analysis grid's count of frames
    // overlapping [0, L) and N / H equal the overlap-add drain; a non-dividing
    // hop is refused rather than given a count no analysis grid produces. The
    // FFT, hop and channel limits are those of SpectralIdentity and the codec.
    static constexpr Status geometry_status(std::uint64_t n, std::uint64_t h,
                                            std::uint64_t c) noexcept {
        if (n < 256 || n > 16384 || (n & (n - 1)) != 0)
            return Status::fft_size_not_admitted;
        if (h < 1 || h > n / 2)
            return Status::hop_not_admitted;
        if (n % h != 0)
            return Status::hop_does_not_divide_fft_size;
        if (c < 1 || c > 16)
            return Status::channels_not_admitted;
        return Status::ok;
    }
    static constexpr bool admissible_geometry(std::uint64_t n, std::uint64_t h,
                                              std::uint64_t c) noexcept {
        return geometry_status(n, h, c) == Status::ok;
    }

    static constexpr SpectralFiniteStreamRule make(std::uint64_t n, std::uint64_t h,
                                                   std::uint64_t c, std::uint64_t length) noexcept {
        SpectralFiniteStreamRule rule;
        if (const auto geometry = geometry_status(n, h, c); geometry != Status::ok) {
            rule.status = geometry;
            return rule;
        }
        rule.fft_size = n;
        rule.hop = h;
        rule.channels = c;
        rule.bins = n / 2 + 1;
        constexpr auto limit = static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
        // Bound the numerator before adding the window overhang and rounding.
        if (!length || length > limit - (n - h) - (h - 1)) {
            rule.status = Status::invalid_length;
            return rule;
        }
        const auto frames = (length + (n - h) + (h - 1)) / h;
        // Leave one window of headroom past the cutoff for origin arithmetic.
        if (frames > (limit - (n + h) - n) / h) {
            rule.status = Status::capacity_exceeded;
            return rule;
        }
        rule.source_samples = length;
        rule.frame_count = frames;
        rule.render_cutoff = n + h + frames * h;
        rule.finite_frames = frames + n / h;
        rule.status = Status::ok;
        return rule;
    }
};

} // namespace pulp::host
