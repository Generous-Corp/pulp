#pragma once

#include <cstdint>
#include <limits>

namespace pulp::host {

// The end-of-stream rule for a finite spectral source at codec geometry
// (fft N, hop H, C channels) covering L source samples. Analysis frames start
// on the grid of hop multiples; frame f starts at (f - floor((N - 1) / H)) * H,
// the first start above -N. A stream holds exactly the frames whose window
// [s, s + N) overlaps [0, L):
//
//     frame_count = floor((L - 1) / H) + floor((N - 1) / H) + 1
//
// A synthesized frame makes every sample before its start final, so the
// synthesis drain is the smallest count of zero frames whose first start
// reaches the end of the last frame's window: drain_frames = ceil(N / H). The
// node publishes frame_count + drain_frames frames. The routed cutoff after
// which output is silent is derived from the source length alone: the
// synthesis latency N + H plus the end of the last frame's window,
// floor((L - 1) / H) * H + N. Frame f, channel c, bin k sits at component
// f * C * (N / 2 + 1) * 2 + c * (N / 2 + 1) * 2 + 2 * k (re, im). Every value
// is derived with checked arithmetic against signed chronology.
//
// The policy is versioned. dividing_hop_v1 admits only hops that divide N,
// where the counts above reduce to ceil((L + N - H) / H) frames and an N / H
// drain; a consumer whose clock depends on that (the asset morph, whose frame
// f averages control hop f) keeps it. analysis_grid_v2 admits every hop from 1
// to N / 2 and produces identical results wherever v1 admits the geometry.
struct SpectralFiniteStreamRule {
    enum class Policy : std::uint8_t { dividing_hop_v1 = 1, analysis_grid_v2 = 2 };
    static constexpr Policy current_policy = Policy::analysis_grid_v2;

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
        capacity_exceeded,
        policy_not_admitted
    };

    Status status = Status::unset;
    Policy policy = current_policy;
    std::uint64_t fft_size = 0, hop = 0, channels = 0, bins = 0;
    std::uint64_t source_samples = 0, frame_count = 0, drain_frames = 0;
    std::uint64_t render_cutoff = 0, finite_frames = 0;

    explicit operator bool() const noexcept {
        return status == Status::ok;
    }

    std::uint32_t policy_version() const noexcept {
        return static_cast<std::uint32_t>(policy);
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
               status == Status::channels_not_admitted || status == Status::policy_not_admitted;
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
        case Status::policy_not_admitted:
            return "finite stream policy version is not admitted";
        }
        return "unknown";
    }

    // The FFT, hop and channel limits are those of SpectralIdentity and the
    // codec. Only dividing_hop_v1 refuses a hop that does not divide N.
    static constexpr Status geometry_status(std::uint64_t n, std::uint64_t h, std::uint64_t c,
                                            Policy policy = current_policy) noexcept {
        if (policy != Policy::dividing_hop_v1 && policy != Policy::analysis_grid_v2)
            return Status::policy_not_admitted;
        if (n < 256 || n > 16384 || (n & (n - 1)) != 0)
            return Status::fft_size_not_admitted;
        if (h < 1 || h > n / 2)
            return Status::hop_not_admitted;
        if (policy == Policy::dividing_hop_v1 && n % h != 0)
            return Status::hop_does_not_divide_fft_size;
        if (c < 1 || c > 16)
            return Status::channels_not_admitted;
        return Status::ok;
    }
    static constexpr bool admissible_geometry(std::uint64_t n, std::uint64_t h, std::uint64_t c,
                                              Policy policy = current_policy) noexcept {
        return geometry_status(n, h, c, policy) == Status::ok;
    }

    static constexpr SpectralFiniteStreamRule make(std::uint64_t n, std::uint64_t h,
                                                   std::uint64_t c, std::uint64_t length,
                                                   Policy policy = current_policy) noexcept {
        SpectralFiniteStreamRule rule;
        rule.policy = policy;
        if (const auto geometry = geometry_status(n, h, c, policy); geometry != Status::ok) {
            rule.status = geometry;
            return rule;
        }
        rule.fft_size = n;
        rule.hop = h;
        rule.channels = c;
        rule.bins = n / 2 + 1;
        constexpr auto limit = static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
        // The window of the last source sample must end inside signed
        // chronology: L + N - 1 <= limit.
        if (!length || length > limit - (n - 1)) {
            rule.status = Status::invalid_length;
            return rule;
        }
        const auto trailing = (length - 1) / h; // grid starts in [0, L)
        const auto leading = (n - 1) / h;       // grid starts in (-N, 0)
        const auto drain = (n + h - 1) / h;     // ceil(N / H)
        const auto last_start = trailing * h;   // <= L - 1, so it cannot overflow
        // Leave one window of headroom past the cutoff for origin arithmetic:
        // cutoff + N = (N + H) + last_start + 2N <= limit.
        if (last_start > limit - (3 * n + h)) {
            rule.status = Status::capacity_exceeded;
            return rule;
        }
        rule.source_samples = length;
        rule.frame_count = trailing + leading + 1;
        rule.drain_frames = drain;
        rule.finite_frames = rule.frame_count + drain;
        rule.render_cutoff = (n + h) + last_start + n;
        rule.status = Status::ok;
        return rule;
    }
};

} // namespace pulp::host
