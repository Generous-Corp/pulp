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
    enum class Status { ok, invalid_geometry, invalid_length, capacity_exceeded };

    Status status = Status::invalid_geometry;
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

    // Admits the codec geometry range where the hop divides the FFT size. Only
    // there does ceil((L + N - H) / H) equal the analysis grid's count of frames
    // overlapping [0, L) and N / H equal the overlap-add drain; a non-dividing
    // hop is refused rather than given a count no analysis grid produces.
    static constexpr bool admissible_geometry(std::uint64_t n, std::uint64_t h,
                                              std::uint64_t c) noexcept {
        return n >= 256 && n <= 16384 && (n & (n - 1)) == 0 && h >= 1 && h <= n / 2 && n % h == 0 &&
               c >= 1 && c <= 16;
    }

    static constexpr SpectralFiniteStreamRule make(std::uint64_t n, std::uint64_t h,
                                                   std::uint64_t c, std::uint64_t length) noexcept {
        SpectralFiniteStreamRule rule;
        if (!admissible_geometry(n, h, c))
            return rule;
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
