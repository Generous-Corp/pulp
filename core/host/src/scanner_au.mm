// Audio Unit scanner (macOS only).
//
// Enumerates installed AU v2/v3 components via the AudioComponent API
// instead of walking ~/Library/Audio/Plug-Ins/Components. The API-based
// route is the supported way to discover AUs and also handles AUv3
// app extensions registered with the system.
//
// unique_id encodes the OSType triplet as "TYPE:SUBT:MANU" so the
// loader can reconstruct the AudioComponentDescription.

#include <pulp/host/scanner.hpp>
#include <pulp/runtime/log.hpp>

#include <AudioToolbox/AudioToolbox.h>

#include <string>
#include <vector>

namespace pulp::host {
namespace {

std::string fourcc(OSType code) {
    char s[5] = {
        static_cast<char>((code >> 24) & 0xFF),
        static_cast<char>((code >> 16) & 0xFF),
        static_cast<char>((code >>  8) & 0xFF),
        static_cast<char>((code      ) & 0xFF),
        0,
    };
    return s;
}

std::string encode_triplet(OSType t, OSType s, OSType m) {
    return fourcc(t) + ":" + fourcc(s) + ":" + fourcc(m);
}

std::string cfstring_to_utf8(CFStringRef s) {
    if (!s) return {};
    if (const char* fast = CFStringGetCStringPtr(s, kCFStringEncodingUTF8)) {
        return fast;
    }
    CFIndex len = CFStringGetLength(s);
    CFIndex max = CFStringGetMaximumSizeForEncoding(len, kCFStringEncodingUTF8) + 1;
    std::vector<char> buf(static_cast<size_t>(max), 0);
    if (CFStringGetCString(s, buf.data(), max, kCFStringEncodingUTF8)) {
        return std::string(buf.data());
    }
    return {};
}

PluginFormat infer_format(OSType type, UInt32 flags) {
    // An AU v3 app extension sets kAudioComponentFlag_IsV3AudioComponent
    // (defined in AVFoundation) or returns a non-null sandbox-safe loader.
    // We cannot read that constant directly in pure AudioToolbox, but the
    // flag bit 0x01000000 is the well-known shorthand for v3.
    constexpr UInt32 kV3Flag = 0x01000000;
    if (flags & kV3Flag) return PluginFormat::AudioUnitV3;
    (void)type;
    return PluginFormat::AudioUnit;
}

}  // namespace

std::vector<PluginInfo> scan_audio_units_api() {
    std::vector<PluginInfo> results;

    AudioComponentDescription wildcard{};
    // All zeros = match any type/subtype/manufacturer.

    AudioComponent comp = nullptr;
    while ((comp = AudioComponentFindNext(comp, &wildcard)) != nullptr) {
        AudioComponentDescription desc{};
        if (AudioComponentGetDescription(comp, &desc) != noErr) continue;

        // Filter to plausible audio units: effects, instruments, generators,
        // music effects. Ignore plain AUConverter and output units.
        switch (desc.componentType) {
            case kAudioUnitType_Effect:
            case kAudioUnitType_MusicEffect:
            case kAudioUnitType_MusicDevice:
            case kAudioUnitType_Generator:
            case kAudioUnitType_Mixer:
                break;
            default:
                continue;
        }

        PluginInfo info;
        info.format = infer_format(desc.componentType, desc.componentFlags);
        info.unique_id = encode_triplet(
            desc.componentType, desc.componentSubType, desc.componentManufacturer);

        CFStringRef name_cf = nullptr;
        if (AudioComponentCopyName(comp, &name_cf) == noErr && name_cf) {
            std::string full = cfstring_to_utf8(name_cf);
            CFRelease(name_cf);
            // "Vendor: Name" is the conventional AU name format.
            auto colon = full.find(':');
            if (colon != std::string::npos) {
                info.manufacturer = full.substr(0, colon);
                info.name = full.substr(colon + 1);
                // Trim leading space after colon.
                if (!info.name.empty() && info.name.front() == ' ')
                    info.name.erase(0, 1);
            } else {
                info.name = full;
            }
        }
        if (info.name.empty()) info.name = info.unique_id;

        info.is_instrument = (desc.componentType == kAudioUnitType_MusicDevice);
        info.is_effect = !info.is_instrument;
        info.num_inputs = info.is_instrument ? 0 : 2;
        info.num_outputs = 2;
        // AU version is a packed UInt32; format as major.minor.patch.
        UInt32 version = 0;
        if (AudioComponentGetVersion(comp, &version) == noErr) {
            info.version = std::to_string((version >> 16) & 0xFFFF) + "."
                         + std::to_string((version >>  8) & 0xFF) + "."
                         + std::to_string( version        & 0xFF);
        }

        results.push_back(std::move(info));
    }
    return results;
}

}  // namespace pulp::host
