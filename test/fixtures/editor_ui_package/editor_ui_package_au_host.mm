// pulp-test-editor-ui-package-au-host <package .component> <no-package .component>
//
// Registers each AU bundle's factory for this process only (nothing is
// installed) and opens its editor the way an AU v2 host does: read
// kAudioUnitProperty_CocoaUI, instantiate the advertised factory class, ask it
// for a view.
//
// The package module links no Pulp view layer (UI implies NO_EDITOR), so its
// view is the editor-host contract's container. The host opens it, sizes it,
// closes it, then opens it again and disposes the audio unit while the view is
// still alive, as some hosts do: the instance's session registry must close
// the editor in order (detach, never invalidate, since the view still exists)
// before the processor goes, and the view, released afterwards, must not touch
// it again. The module without a package must advertise
// no Cocoa view.

#import <AppKit/AppKit.h>
#import <AudioToolbox/AudioToolbox.h>
#import <AudioUnit/AUCocoaUIView.h>

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <unistd.h>

namespace {

std::string fourcc_string(NSString* s) {
    return s.length == 4 ? std::string(s.UTF8String) : std::string();
}

OSType fourcc(const std::string& s) {
    return (OSType(s[0]) << 24) | (OSType(s[1]) << 16) | (OSType(s[2]) << 8) | OSType(s[3]);
}

// The editor events TaggedEditor logged since the last call, one per line.
std::string take_editor_log(const std::string& path) {
    std::ifstream in(path);
    std::stringstream text;
    text << in.rdbuf();
    std::remove(path.c_str());
    return text.str();
}

AudioComponent register_bundle(const char* path) {
    NSURL* url = [NSURL fileURLWithPath:@(path)];
    CFBundleRef bundle = CFBundleCreate(nullptr, (__bridge CFURLRef)url);
    if (!bundle || !CFBundleLoadExecutable(bundle))
        return nullptr;
    NSDictionary* info = (__bridge NSDictionary*)CFBundleGetInfoDictionary(bundle);
    NSDictionary* comp = [info[@"AudioComponents"] firstObject];
    const auto type = fourcc_string(comp[@"type"]);
    const auto subtype = fourcc_string(comp[@"subtype"]);
    const auto manufacturer = fourcc_string(comp[@"manufacturer"]);
    auto factory = (AudioComponentFactoryFunction)CFBundleGetFunctionPointerForName(
        bundle, (__bridge CFStringRef)comp[@"factoryFunction"]);
    if (type.empty() || subtype.empty() || manufacturer.empty() || !factory)
        return nullptr;
    AudioComponentDescription desc{};
    desc.componentType = fourcc(type);
    desc.componentSubType = fourcc(subtype);
    desc.componentManufacturer = fourcc(manufacturer);
    return AudioComponentRegister(&desc, CFSTR("Pulp: editor UI package fixture"),
                                  [comp[@"version"] unsignedIntValue], factory);
}

// The editor view the AU advertises, or nil with `error` set.
NSView* open_editor(AudioUnit au, std::string& error) {
    AudioUnitCocoaViewInfo info{};
    UInt32 size = sizeof(info);
    if (AudioUnitGetProperty(au, kAudioUnitProperty_CocoaUI, kAudioUnitScope_Global, 0, &info,
                             &size) != noErr) {
        error = "advertises no Cocoa view";
        return nil;
    }
    NSString* class_name = (__bridge NSString*)info.mCocoaAUViewClass[0];
    Class factory_class = NSClassFromString(class_name);
    CFRelease(info.mCocoaAUViewBundleLocation);
    CFRelease(info.mCocoaAUViewClass[0]);
    if (factory_class == nil || ![factory_class conformsToProtocol:@protocol(AUCocoaUIBase)]) {
        error = "advertised view factory does not resolve";
        return nil;
    }
    id<AUCocoaUIBase> factory = [[factory_class alloc] init];
    NSView* view = [factory uiViewForAudioUnit:au withSize:NSMakeSize(640, 480)];
    [factory release];
    if (view == nil)
        error = "view factory returned no view";
    return view;
}

int check_package(const char* path, NSWindow* window, const std::string& log_path) {
    AudioComponent comp = register_bundle(path);
    if (comp == nullptr) {
        std::fprintf(stderr, "FAIL cannot register %s\n", path);
        return 1;
    }
    for (int round = 0; round < 2; ++round) {
        AudioUnit au = nullptr;
        if (AudioComponentInstanceNew(comp, &au) != noErr || au == nullptr ||
            AudioUnitInitialize(au) != noErr) {
            std::fprintf(stderr, "FAIL cannot instantiate %s\n", path);
            return 1;
        }
        // The factory hands back an autoreleased view, so each step that must
        // end with the view gone drains its own pool.
        NSView* view = nil;
        @autoreleasepool {
            std::string error;
            view = [open_editor(au, error) retain];
            if (view == nil) {
                std::fprintf(stderr, "FAIL %s %s\n", path, error.c_str());
                return 1;
            }
            if (view.frame.size.width != 320 || view.frame.size.height != 200) {
                std::fprintf(stderr, "FAIL %s view is %gx%g, not the editor's 320x200\n", path,
                             view.frame.size.width, view.frame.size.height);
                return 1;
            }
            [window.contentView addSubview:view];
        }
        if (round == 0) {
            // An orderly close, then dispose.
            @autoreleasepool {
                [view removeFromSuperview];
                [view release];
            }
            AudioUnitUninitialize(au);
            AudioComponentInstanceDispose(au);
        } else {
            // Disposed while the view is still alive.
            AudioUnitUninitialize(au);
            AudioComponentInstanceDispose(au);
            @autoreleasepool {
                [view removeFromSuperview];
                [view release];
            }
        }
        // An orderly close detaches the editor, and so does disposing the
        // instance first (its session registry goes before the processor),
        // once: the view released afterwards does not detach it again.
        const std::string events = take_editor_log(log_path);
        const std::string expected = "detach\n";
        if (events != expected) {
            std::fprintf(stderr, "FAIL %s round %d: editor events \"%s\", expected \"%s\"\n", path,
                         round, events.c_str(), expected.c_str());
            return 1;
        }
    }
    std::printf("ok   %s opens, closes and outlives its contract editor\n", path);
    return 0;
}

int check_no_package(const char* path) {
    AudioComponent comp = register_bundle(path);
    AudioUnit au = nullptr;
    if (comp == nullptr || AudioComponentInstanceNew(comp, &au) != noErr) {
        std::fprintf(stderr, "FAIL cannot instantiate %s\n", path);
        return 1;
    }
    UInt32 size = 0;
    Boolean writable = false;
    const OSStatus status = AudioUnitGetPropertyInfo(au, kAudioUnitProperty_CocoaUI,
                                                     kAudioUnitScope_Global, 0, &size, &writable);
    AudioComponentInstanceDispose(au);
    if (status == noErr) {
        std::fprintf(stderr, "FAIL %s advertises a Cocoa view without a UI package\n", path);
        return 1;
    }
    std::printf("ok   %s advertises no Cocoa view\n", path);
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    if (argc != 3) {
        std::fprintf(stderr, "usage: %s <package .component> <no-package .component>\n", argv[0]);
        return 2;
    }
    // The package module's editor logs its detach and invalidate here.
    const std::string log_path =
        std::string(std::getenv("TMPDIR") ? std::getenv("TMPDIR") : "/tmp") +
        "/pulp-editor-ui-package-au-" + std::to_string(::getpid()) + ".log";
    ::setenv("PULP_TEST_EDITOR_LOG", log_path.c_str(), 1);
    std::remove(log_path.c_str());
    int failures = 0;
    @autoreleasepool {
        [NSApplication sharedApplication];
        NSWindow* window =
            [[NSWindow alloc] initWithContentRect:NSMakeRect(-20000, -20000, 800, 600)
                                        styleMask:NSWindowStyleMaskBorderless
                                          backing:NSBackingStoreBuffered
                                            defer:NO];
        window.releasedWhenClosed = NO;
        failures += check_package(argv[1], window, log_path);
        failures += check_no_package(argv[2]);
        [window close];
        [window release];
    }
    return failures == 0 ? 0 : 1;
}
