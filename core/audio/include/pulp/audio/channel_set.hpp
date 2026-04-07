#pragma once

// Named channel layouts for surround and immersive audio.
// Maps channel indices to speaker positions.

#include <string>
#include <string_view>
#include <vector>
#include <cstdint>

namespace pulp::audio {

/// Speaker position identifiers
enum class Speaker : uint8_t {
    FrontLeft = 0,
    FrontRight,
    FrontCenter,
    LFE,
    BackLeft,
    BackRight,
    SideLeft,
    SideRight,
    TopFrontLeft,
    TopFrontRight,
    TopBackLeft,
    TopBackRight,
    TopCenter,
    // Discrete (unnamed) channels
    Discrete
};

/// Named channel layout — maps channel indices to speaker positions
struct ChannelSet {
    std::string name;
    std::vector<Speaker> speakers;

    uint32_t size() const { return static_cast<uint32_t>(speakers.size()); }

    bool operator==(const ChannelSet& other) const {
        return speakers == other.speakers;
    }

    // ── Standard layouts ────────────────────────────────────────────────

    static ChannelSet mono() {
        return {"Mono", {Speaker::FrontCenter}};
    }

    static ChannelSet stereo() {
        return {"Stereo", {Speaker::FrontLeft, Speaker::FrontRight}};
    }

    static ChannelSet lrc() {
        return {"LRC", {Speaker::FrontLeft, Speaker::FrontRight, Speaker::FrontCenter}};
    }

    static ChannelSet quad() {
        return {"Quad", {Speaker::FrontLeft, Speaker::FrontRight,
                         Speaker::BackLeft, Speaker::BackRight}};
    }

    static ChannelSet surround_5_0() {
        return {"5.0", {Speaker::FrontLeft, Speaker::FrontRight, Speaker::FrontCenter,
                        Speaker::BackLeft, Speaker::BackRight}};
    }

    static ChannelSet surround_5_1() {
        return {"5.1", {Speaker::FrontLeft, Speaker::FrontRight, Speaker::FrontCenter,
                        Speaker::LFE, Speaker::BackLeft, Speaker::BackRight}};
    }

    static ChannelSet surround_7_1() {
        return {"7.1", {Speaker::FrontLeft, Speaker::FrontRight, Speaker::FrontCenter,
                        Speaker::LFE, Speaker::BackLeft, Speaker::BackRight,
                        Speaker::SideLeft, Speaker::SideRight}};
    }

    static ChannelSet surround_7_1_4() {
        return {"7.1.4 (Atmos bed)", {
            Speaker::FrontLeft, Speaker::FrontRight, Speaker::FrontCenter,
            Speaker::LFE, Speaker::BackLeft, Speaker::BackRight,
            Speaker::SideLeft, Speaker::SideRight,
            Speaker::TopFrontLeft, Speaker::TopFrontRight,
            Speaker::TopBackLeft, Speaker::TopBackRight
        }};
    }

    /// Create a discrete (unnamed) channel set with N channels
    static ChannelSet discrete(uint32_t num_channels) {
        ChannelSet cs;
        cs.name = "Discrete " + std::to_string(num_channels);
        cs.speakers.resize(num_channels, Speaker::Discrete);
        return cs;
    }

    /// Get a standard layout by channel count (best guess)
    static ChannelSet from_channel_count(uint32_t count) {
        switch (count) {
            case 1: return mono();
            case 2: return stereo();
            case 3: return lrc();
            case 4: return quad();
            case 5: return surround_5_0();
            case 6: return surround_5_1();
            case 8: return surround_7_1();
            case 12: return surround_7_1_4();
            default: return discrete(count);
        }
    }

    /// Get a standard layout by name
    static ChannelSet from_name(std::string_view name);
};

/// Get the speaker name as a string (e.g., "Front Left", "LFE")
std::string speaker_name(Speaker speaker);

}  // namespace pulp::audio
