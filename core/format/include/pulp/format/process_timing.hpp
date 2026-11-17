#pragma once

// Format-neutral timing facts for one process block.
//
// Each plugin format reports where a block sits in time differently: CLAP has
// a monotonic `steady_time` counter and a fixed-point seconds timeline, VST3 a
// continuous-time counter guarded by a validity bit, Audio Unit a render
// `AudioTimeStamp` plus a transport callback, and LV2 only a `time:Position`
// atom with a `time:speed`. The adapter's job is to read those native fields
// and publish what they mean, in samples, on `ProcessContext::timing`. It does
// not decide what is a seek, group blocks into epochs, or retime events:
// `pulp::host::HostBlockTimeline` owns that projection, and only code that
// owns an event timeline (for example a Processor hosting a SignalGraph) needs
// to run it. Every other Processor can ignore these fields.
//
// Rules the builders below follow:
//  * Every value is in samples. A wall-clock or host-time value is never
//    reinterpreted as a sample count; a format without a sample counter
//    reports none.
//  * A value is published only when the host supplied it and it is
//    representable. A missing value is reported as missing, not as zero.
//  * Playback rate is reported only when the host states it. VST3, AU and CLAP
//    carry no rate field, so they report `PlaybackRate::Unknown`; an adapter
//    that learns of varispeed some other way sets `NonUnit`.
//
// Header-only, SDK-free, allocation-free and `noexcept`: the builders take the
// native fields as plain values, so they are testable without any SDK and safe
// on the audio thread.

#include <pulp/format/detail/host_sample_position.hpp>

#include <cstdint>
#include <optional>

namespace pulp::format {

/// What the host said about the speed of its transport.
enum class PlaybackRate : std::uint8_t {
    /// The host reports no rate. Transport movement is assumed to be one
    /// sample per frame.
    Unknown = 0,
    /// The host reports normal-speed playback (LV2 `time:speed` exactly 1).
    Unit,
    /// The transport moves at some other rate (LV2 `time:speed` other than 0
    /// or 1, or an adapter that knows the host is varispeeding). Its position
    /// delta cannot be compared with the block length.
    NonUnit,
};

/// Neutral per-block timing facts, published on `ProcessContext::timing`.
struct ProcessTiming {
    /// True when the host supplied a monotonic sample counter for this block.
    bool has_steady_sample = false;
    /// Host monotonic sample counter at the first frame of the block. It keeps
    /// counting across seeks and loops (CLAP `steady_time`, VST3
    /// `continousTimeSamples`, AU `mSampleTime`). Meaningful only when
    /// `has_steady_sample` is true, and never negative then.
    std::int64_t steady_sample = 0;
    /// True when the host supplied a transport sample position.
    bool has_transport_sample = false;
    /// Transport (timeline) position in samples at the first frame of the
    /// block. Matches `ProcessContext::position_samples` when both are set.
    std::int64_t transport_sample = 0;
    /// True when the host reported its transport as moving.
    bool transport_moving = false;
    /// Rate of a moving transport, as far as the host reported it.
    PlaybackRate playback_rate = PlaybackRate::Unknown;
    /// True when the adapter knows this block does not follow the previous one
    /// it delivered: the first block after activation, a host reset, or a
    /// processing restart.
    bool discontinuity = false;

    /// True when the transport position advances one sample per frame, so a
    /// projection may compare its delta with elapsed samples.
    constexpr bool transport_tracks_samples() const noexcept {
        return has_transport_sample && transport_moving && playback_rate != PlaybackRate::NonUnit;
    }

    friend constexpr bool operator==(const ProcessTiming&, const ProcessTiming&) noexcept = default;
};

// ---------------------------------------------------------------------------
// Native field mirrors. Each constant equals the SDK value it names; the
// timing tests check them against the real headers wherever those exist.
// ---------------------------------------------------------------------------

/// `Steinberg::Vst::ProcessContext::StatesAndFlags::kPlaying`.
inline constexpr std::uint32_t kVst3ContextPlaying = 1u << 1;
/// `Steinberg::Vst::ProcessContext::StatesAndFlags::kContTimeValid`.
inline constexpr std::uint32_t kVst3ContextContTimeValid = 1u << 17;
/// `kAudioTimeStampSampleTimeValid` from CoreAudioTypes.
inline constexpr std::uint32_t kAuTimeStampSampleTimeValid = 1u << 0;
/// `CLAP_SECTIME_FACTOR` from clap/fixedpoint.h: `clap_sectime` is seconds
/// scaled by 2^31.
inline constexpr std::int64_t kClapSectimeFactor = std::int64_t{1} << 31;

/// Convert a CLAP fixed-point `clap_sectime` to the nearest sample at
/// `sample_rate`, or nullopt when the rate is not positive or the result is
/// not representable. Dividing by the power-of-two factor first is exact, so
/// the only rounding is the final one.
inline std::optional<std::int64_t> clap_sectime_to_samples(std::int64_t sectime,
                                                           double sample_rate) noexcept {
    if (!(sample_rate > 0.0))
        return std::nullopt;
    const double seconds = static_cast<double>(sectime) / static_cast<double>(kClapSectimeFactor);
    return detail::host_sample_position(seconds * sample_rate);
}

/// CLAP: `clap_process_t::steady_time` (negative when the host has none) and
/// the transport sample decoded from `song_pos_seconds`, when the transport
/// carried `CLAP_TRANSPORT_HAS_SECONDS_TIMELINE`. `playing` is
/// `CLAP_TRANSPORT_IS_PLAYING`. CLAP reports no playback rate.
inline constexpr ProcessTiming clap_process_timing(std::int64_t steady_time,
                                                   std::optional<std::int64_t> transport_sample,
                                                   bool playing, bool discontinuity) noexcept {
    ProcessTiming timing;
    timing.discontinuity = discontinuity;
    if (steady_time >= 0) {
        timing.has_steady_sample = true;
        timing.steady_sample = steady_time;
    }
    if (transport_sample) {
        timing.has_transport_sample = true;
        timing.transport_sample = *transport_sample;
        timing.transport_moving = playing;
    }
    return timing;
}

/// VST3: `ProcessData::processContext` present, its `state`,
/// `projectTimeSamples` and `continousTimeSamples`. The continuous counter is
/// the steady clock only while `kContTimeValid` is set and it is non-negative.
/// `projectTimeSamples` has no validity bit and is required whenever a context
/// is supplied, so it is the transport position of every block with a context.
/// `non_unit_rate` is adapter-owned: VST3 has no rate field.
inline constexpr ProcessTiming vst3_process_timing(bool has_process_context, std::uint32_t state,
                                                   std::int64_t project_time_samples,
                                                   std::int64_t continuous_time_samples,
                                                   bool non_unit_rate,
                                                   bool discontinuity) noexcept {
    ProcessTiming timing;
    timing.discontinuity = discontinuity;
    if (!has_process_context)
        return timing;
    if ((state & kVst3ContextContTimeValid) != 0 && continuous_time_samples >= 0) {
        timing.has_steady_sample = true;
        timing.steady_sample = continuous_time_samples;
    }
    timing.has_transport_sample = true;
    timing.transport_sample = project_time_samples;
    timing.transport_moving = (state & kVst3ContextPlaying) != 0;
    timing.playback_rate = non_unit_rate ? PlaybackRate::NonUnit : PlaybackRate::Unknown;
    return timing;
}

/// Audio Unit (AUv2 `Render`, AUv3 render block): `AudioTimeStamp::mFlags`
/// and `mSampleTime`, then the transport callback's result. `mSampleTime` is
/// the host's render counter, which keeps running across seeks, so it is the
/// steady clock when `kAudioTimeStampSampleTimeValid` is set and the rounded
/// value is representable and non-negative. The transport position is used
/// only when the callback succeeded (`has_transport`) and the position is
/// representable. `non_unit_rate` is adapter-owned: AU has no rate field.
inline ProcessTiming au_process_timing(std::uint32_t timestamp_flags, double sample_time,
                                       bool has_transport, bool transport_moving,
                                       double transport_sample_position, bool non_unit_rate,
                                       bool discontinuity) noexcept {
    ProcessTiming timing;
    timing.discontinuity = discontinuity;
    if ((timestamp_flags & kAuTimeStampSampleTimeValid) != 0) {
        if (const auto sample = detail::host_sample_position(sample_time); sample && *sample >= 0) {
            timing.has_steady_sample = true;
            timing.steady_sample = *sample;
        }
    }
    if (has_transport) {
        if (const auto sample = detail::host_sample_position(transport_sample_position)) {
            timing.has_transport_sample = true;
            timing.transport_sample = *sample;
            timing.transport_moving = transport_moving;
            timing.playback_rate = non_unit_rate ? PlaybackRate::NonUnit : PlaybackRate::Unknown;
        }
    }
    return timing;
}

/// LV2: the adapter's latched `time:Position` (`time:frame` and
/// `time:speed`), already placed at this block's first frame. LV2 defines no
/// monotonic sample counter, so there is never a steady sample. The transport
/// moves while `time:speed` is non-zero; speed exactly 1 is reported as
/// `Unit`, any other moving speed as `NonUnit`. Without a `time:speed` the
/// transport is reported as stopped.
inline constexpr ProcessTiming lv2_process_timing(bool has_frame, std::int64_t frame,
                                                  bool has_speed, double speed,
                                                  bool discontinuity) noexcept {
    ProcessTiming timing;
    timing.discontinuity = discontinuity;
    if (!has_frame)
        return timing;
    timing.has_transport_sample = true;
    timing.transport_sample = frame;
    timing.transport_moving = has_speed && speed != 0.0;
    if (timing.transport_moving)
        timing.playback_rate = speed == 1.0 ? PlaybackRate::Unit : PlaybackRate::NonUnit;
    return timing;
}

} // namespace pulp::format
