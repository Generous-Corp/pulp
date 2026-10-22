// Opens a built plug-in bundle's editor through Pulp's host stack, the way a
// DAW does, so a test can see which editor the format adapter built.
//
//   pulp-ui-script-editor-probe <vst3|clap|au> <bundle path>
//
// VST3 and CLAP bundles load from the build tree through PluginScanner and
// PluginSlot. An AU bundle's factory is registered with the Audio Component
// manager for this process only (nothing is installed), then loaded through
// PluginSlot like any other Audio Unit. The adapter's own editor log line and
// the script's console output go to stderr for the caller to assert on.

#import <AppKit/AppKit.h>
#import <AudioToolbox/AudioToolbox.h>

#include <pulp/host/plugin_slot.hpp>
#include <pulp/host/scanner.hpp>

#include <cstdio>
#include <filesystem>
#include <string>

namespace {

using pulp::host::PluginFormat;
using pulp::host::PluginInfo;

std::string canonical(const std::string& path) {
    std::error_code ec;
    auto p = std::filesystem::weakly_canonical(path, ec);
    return ec ? path : p.string();
}

bool find_in_build_tree(const std::string& bundle, PluginFormat format, PluginInfo& out) {
    pulp::host::ScanOptions options;
    options.scan_vst3 = format == PluginFormat::VST3;
    options.scan_clap = format == PluginFormat::CLAP;
    options.scan_au = false;
    options.scan_lv2 = false;
    options.only_extra_paths = true;
    options.extra_paths = {std::filesystem::path(bundle).parent_path().string()};
    for (auto& info : pulp::host::PluginScanner().scan(options)) {
        if (info.format == format && canonical(info.path) == canonical(bundle)) {
            out = info;
            return true;
        }
    }
    return false;
}

std::string fourcc_string(NSString* s) {
    return s.length == 4 ? std::string(s.UTF8String) : std::string();
}

OSType fourcc(const std::string& s) {
    return (OSType(s[0]) << 24) | (OSType(s[1]) << 16) | (OSType(s[2]) << 8) | OSType(s[3]);
}

bool register_au_bundle(const std::string& bundle, PluginInfo& out) {
    NSURL* url = [NSURL fileURLWithPath:@(bundle.c_str())];
    CFBundleRef cf = CFBundleCreate(nullptr, (__bridge CFURLRef)url);
    if (!cf || !CFBundleLoadExecutable(cf))
        return false;
    NSDictionary* info = (__bridge NSDictionary*)CFBundleGetInfoDictionary(cf);
    NSDictionary* comp = [info[@"AudioComponents"] firstObject];
    const auto type = fourcc_string(comp[@"type"]);
    const auto subtype = fourcc_string(comp[@"subtype"]);
    const auto manufacturer = fourcc_string(comp[@"manufacturer"]);
    auto factory = (AudioComponentFactoryFunction)CFBundleGetFunctionPointerForName(
        cf, (__bridge CFStringRef)comp[@"factoryFunction"]);
    if (type.empty() || subtype.empty() || manufacturer.empty() || !factory)
        return false;

    AudioComponentDescription desc{};
    desc.componentType = fourcc(type);
    desc.componentSubType = fourcc(subtype);
    desc.componentManufacturer = fourcc(manufacturer);
    if (!AudioComponentRegister(&desc, CFSTR("Pulp: UI script fixture"),
                                [comp[@"version"] unsignedIntValue], factory)) {
        return false;
    }
    out.path = bundle;
    out.format = PluginFormat::AudioUnit;
    out.unique_id = type + ":" + subtype + ":" + manufacturer;
    out.name = "Pulp UI Script Fixture";
    return true;
}

void pump(double seconds) {
    NSDate* until = [NSDate dateWithTimeIntervalSinceNow:seconds];
    while ([until timeIntervalSinceNow] > 0) {
        @autoreleasepool {
            [[NSRunLoop currentRunLoop] runMode:NSDefaultRunLoopMode
                                     beforeDate:[NSDate dateWithTimeIntervalSinceNow:0.01]];
        }
    }
}

} // namespace

int main(int argc, char** argv) {
    if (argc != 3) {
        std::fprintf(stderr, "usage: %s <vst3|clap|au> <bundle>\n", argv[0]);
        return 2;
    }
    const std::string format = argv[1];
    const std::string bundle = argv[2];

    @autoreleasepool {
        [NSApplication sharedApplication];

        PluginInfo info;
        bool found = false;
        if (format == "vst3")
            found = find_in_build_tree(bundle, PluginFormat::VST3, info);
        else if (format == "clap")
            found = find_in_build_tree(bundle, PluginFormat::CLAP, info);
        else if (format == "au")
            found = register_au_bundle(bundle, info);
        if (!found) {
            std::fprintf(stderr, "PROBE: could not load %s bundle %s\n", format.c_str(),
                         bundle.c_str());
            return 3;
        }

        auto slot = pulp::host::PluginSlot::load(info);
        if (!slot || !slot->prepare(48000.0, 512)) {
            std::fprintf(stderr, "PROBE: PluginSlot could not load %s\n", bundle.c_str());
            return 3;
        }

        // Off screen: the editor is built and attached, never shown.
        NSWindow* window =
            [[NSWindow alloc] initWithContentRect:NSMakeRect(-20000, -20000, 800, 600)
                                        styleMask:NSWindowStyleMaskBorderless
                                          backing:NSBackingStoreBuffered
                                            defer:NO];
        window.releasedWhenClosed = NO;
        auto editor = slot->create_hosted_editor((__bridge void*)window);
        if (!editor) {
            std::fprintf(stderr, "PROBE: no editor for %s\n", bundle.c_str());
            return 4;
        }
        // Let a deferred document load and the first frames run.
        pump(1.5);
        std::fprintf(stderr, "PROBE: editor opened for %s\n", format.c_str());
        slot->destroy_hosted_editor(std::move(editor));
        slot->release();
        slot.reset();
        [window close];
    }
    return 0;
}
