#pragma once

#include <optional>

#include <pluginterfaces/vst/vstspeaker.h>

namespace pulp::format::detail {

/// Return Pulp's canonical VST3 speaker topology for a common channel count.
///
/// PluginDescriptor describes bus width, not individual speaker positions, so
/// VST3 needs one deterministic topology when the adapter first advertises a
/// bus. These choices follow the SDK's conventional music/ITU layouts: quad
/// for four channels, 5.1 for six, and ITU 7.1 for eight.
inline std::optional<Steinberg::Vst::SpeakerArrangement>
vst3_canonical_speaker_arrangement(int channels) noexcept {
    namespace SpeakerArr = Steinberg::Vst::SpeakerArr;
    switch (channels) {
        case 0: return SpeakerArr::kEmpty;
        case 1: return SpeakerArr::kMono;
        case 2: return SpeakerArr::kStereo;
        case 3: return SpeakerArr::k30Cine;
        case 4: return SpeakerArr::k40Music;
        case 5: return SpeakerArr::k50;
        case 6: return SpeakerArr::k51;
        case 7: return SpeakerArr::k70Music;
        case 8: return SpeakerArr::k71Music;
        default: return std::nullopt;
    }
}

/// Translate a host arrangement into the count-only Processor layout model.
/// Reject widths for which Pulp cannot advertise an empty or canonical default
/// topology.
inline std::optional<int> vst3_common_channel_count(
    Steinberg::Vst::SpeakerArrangement arrangement) noexcept {
    const int channels = Steinberg::Vst::SpeakerArr::getChannelCount(arrangement);
    const auto canonical = vst3_canonical_speaker_arrangement(channels);
    if (!canonical || *canonical != arrangement) return std::nullopt;
    return channels;
}

} // namespace pulp::format::detail
