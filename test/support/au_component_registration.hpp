#pragma once

// Finds an AU component bundle's description from its Info.plist and, when
// asked, registers the component with this process through the factory the
// plist names, so a test can instantiate it in process without an install.
// macOS only.

#include <AudioToolbox/AudioToolbox.h>
#include <CoreFoundation/CoreFoundation.h>

#include <cstdint>
#include <cstring>
#include <optional>
#include <string>

namespace pulp::test {

inline std::string cf_string(CFStringRef s) {
    if (!s)
        return {};
    char buf[256] = {};
    CFStringGetCString(s, buf, sizeof(buf), kCFStringEncodingUTF8);
    return buf;
}

inline OSType four_cc(CFStringRef s) {
    const auto text = cf_string(s);
    if (text.size() != 4)
        return 0;
    return OSType(std::uint8_t(text[0])) << 24 | OSType(std::uint8_t(text[1])) << 16 |
           OSType(std::uint8_t(text[2])) << 8 | OSType(std::uint8_t(text[3]));
}

inline std::string four_cc_text(OSType v) {
    return std::string{char(v >> 24), char(v >> 16), char(v >> 8), char(v)};
}

/// The first AudioComponents entry of the bundle at `bundle_path`. With
/// `register_in_process`, the bundle's executable is loaded and the component
/// registered with this process. Empty when the plist or the registration
/// fails.
inline std::optional<AudioComponentDescription> au_component_description(const char* bundle_path,
                                                                         bool register_in_process) {
    CFURLRef url = CFURLCreateFromFileSystemRepresentation(
        nullptr, reinterpret_cast<const UInt8*>(bundle_path), (CFIndex)std::strlen(bundle_path),
        true);
    CFBundleRef bundle = url ? CFBundleCreate(nullptr, url) : nullptr;
    if (url)
        CFRelease(url);
    if (!bundle)
        return std::nullopt;
    auto components =
        (CFArrayRef)CFBundleGetValueForInfoDictionaryKey(bundle, CFSTR("AudioComponents"));
    if (!components || CFArrayGetCount(components) < 1)
        return std::nullopt;
    auto entry = (CFDictionaryRef)CFArrayGetValueAtIndex(components, 0);
    AudioComponentDescription desc{};
    desc.componentType = four_cc((CFStringRef)CFDictionaryGetValue(entry, CFSTR("type")));
    desc.componentSubType = four_cc((CFStringRef)CFDictionaryGetValue(entry, CFSTR("subtype")));
    desc.componentManufacturer =
        four_cc((CFStringRef)CFDictionaryGetValue(entry, CFSTR("manufacturer")));
    if (register_in_process) {
        auto factory_name = (CFStringRef)CFDictionaryGetValue(entry, CFSTR("factoryFunction"));
        UInt32 version = 0;
        if (auto v = (CFNumberRef)CFDictionaryGetValue(entry, CFSTR("version")))
            CFNumberGetValue(v, kCFNumberSInt32Type, &version);
        if (!CFBundleLoadExecutable(bundle))
            return std::nullopt;
        auto factory =
            (AudioComponentFactoryFunction)CFBundleGetFunctionPointerForName(bundle, factory_name);
        if (!factory ||
            !AudioComponentRegister(&desc,
                                    (CFStringRef)CFDictionaryGetValue(entry, CFSTR("name"))
                                        ?: CFSTR("Pulp probe"),
                                    version, factory))
            return std::nullopt;
    }
    return desc;
}

/// "TYPE:SUBT:MANU" for a description.
inline std::string au_component_id(const AudioComponentDescription& desc) {
    return four_cc_text(desc.componentType) + ":" + four_cc_text(desc.componentSubType) + ":" +
           four_cc_text(desc.componentManufacturer);
}

} // namespace pulp::test
