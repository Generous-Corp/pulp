#include <pulp/audio/channel_set.hpp>

namespace pulp::audio {

ChannelSet ChannelSet::from_name(std::string_view name) {
    if (name == "Mono") return mono();
    if (name == "Stereo") return stereo();
    if (name == "LRC") return lrc();
    if (name == "Quad") return quad();
    if (name == "5.0") return surround_5_0();
    if (name == "5.1") return surround_5_1();
    if (name == "7.1") return surround_7_1();
    if (name == "7.1.4" || name == "7.1.4 (Atmos bed)") return surround_7_1_4();
    return discrete(2);  // Default to stereo if unknown
}

std::string speaker_name(Speaker speaker) {
    switch (speaker) {
        case Speaker::FrontLeft:     return "Front Left";
        case Speaker::FrontRight:    return "Front Right";
        case Speaker::FrontCenter:   return "Front Center";
        case Speaker::LFE:           return "LFE";
        case Speaker::BackLeft:      return "Back Left";
        case Speaker::BackRight:     return "Back Right";
        case Speaker::SideLeft:      return "Side Left";
        case Speaker::SideRight:     return "Side Right";
        case Speaker::TopFrontLeft:  return "Top Front Left";
        case Speaker::TopFrontRight: return "Top Front Right";
        case Speaker::TopBackLeft:   return "Top Back Left";
        case Speaker::TopBackRight:  return "Top Back Right";
        case Speaker::TopCenter:     return "Top Center";
        case Speaker::Discrete:      return "Discrete";
    }
    return "Unknown";
}

}  // namespace pulp::audio
