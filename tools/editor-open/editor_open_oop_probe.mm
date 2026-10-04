// Out-of-process AU editor open, the way Logic hosts AU v2 plug-ins: the unit
// is instantiated with kAudioComponentInstantiation_LoadOutOfProcess (it runs
// in AUHostingService), the editor arrives as a remote view controller, and
// the window server composites the service's layer into this process's
// window. Every distinct image of that window is read back (a process may
// read its own windows without screen-recording permission) and classified by
// editor_open_stages.hpp into what a user saw before the editor settled.
//
// Prints, per open: the view-controller latency (how long a DAW shows its own
// placeholder before it has a view), the stage sequence, and the number of
// non-UI images the plug-in showed. Never activates; Accessory policy; the
// window is off every screen unless PULP_PROBE_ORIGIN=x,y is set.
//
// The component must be discoverable by AUHostingService, i.e. installed
// (~/Library/Audio/Plug-Ins/Components). Use a development identity and
// remove it afterwards. Run through editor_open_oop_probe.sh, which builds
// this binary and can launch it in the logged-in GUI session from ssh.
//
// Usage: pulp-editor-open-oop-probe --sub SUBT [--type aufx] [--mfr Pulp]
//          [--opens 3] [--fresh-instance] [--follow] [--no-anim]
//          [--view-size WxH] [--watch-ms 2500] [--settled-bg RRGGBB]
//          [--max-non-ui-frames N] [--out DIR]
// Exit: 0 ok, 1 a gate failed, 2 setup error, 3 the read-back saw only the
// host backdrop (a blind instrument, not a result).
#import <AppKit/AppKit.h>
#import <AudioToolbox/AudioToolbox.h>
#import <CoreAudioKit/CoreAudioKit.h>
#import <ImageIO/ImageIO.h>
#include <dlfcn.h>

#include <chrono>
#include <cstdio>
#include <string>
#include <vector>

#include "editor_open_stages.hpp"

using pulp::tools::editor_open::Frame;

namespace {

const auto g_start = std::chrono::steady_clock::now();
double now_ms() {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - g_start).count();
}
void spin(double ms) {
    [[NSRunLoop mainRunLoop] runUntilDate:[NSDate dateWithTimeIntervalSinceNow:ms / 1000.0]];
}
OSType four_cc(const char* s) {
    return OSType(s[0]) << 24 | OSType(s[1]) << 16 | OSType(s[2]) << 8 | OSType(s[3]);
}

// Own-window read-back through the window-list API, looked up at run time
// (deprecated, but the only call that returns a window's composited image
// without a capture session). Returns false when it is unavailable.
typedef CGImageRef (*WindowImageFn)(CGRect, CGWindowListOption, CGWindowID, CGWindowImageOption);
bool read_window(NSWindow* window, Frame& f) {
    static auto fn = (WindowImageFn)dlsym(RTLD_DEFAULT, "CGWindowListCreateImage");
    if (!fn) return false;
    CGImageRef image = fn(CGRectNull, kCGWindowListOptionIncludingWindow, (CGWindowID)window.windowNumber,
                          kCGWindowImageBoundsIgnoreFraming | kCGWindowImageNominalResolution);
    if (!image) return false;
    f.w = CGImageGetWidth(image);
    f.h = CGImageGetHeight(image);
    std::vector<uint8_t> rgba(f.w * f.h * 4);
    CGColorSpaceRef cs = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
    CGContextRef ctx = CGBitmapContextCreate(rgba.data(), f.w, f.h, 8, f.w * 4, cs, kCGImageAlphaPremultipliedLast);
    CGContextDrawImage(ctx, CGRectMake(0, 0, f.w, f.h), image);
    CGContextRelease(ctx);
    CGColorSpaceRelease(cs);
    CGImageRelease(image);
    f.rgb.resize(f.w * f.h * 3);
    for (size_t i = 0; i < f.w * f.h; ++i)
        for (int c = 0; c < 3; ++c) f.rgb[i * 3 + c] = rgba[i * 4 + c];
    return true;
}

void save_png(const Frame& f, const std::string& path) {
    CGColorSpaceRef cs = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
    CFDataRef data = CFDataCreate(nullptr, f.rgb.data(), CFIndex(f.rgb.size()));
    CGDataProviderRef provider = CGDataProviderCreateWithCFData(data);
    CGImageRef image = CGImageCreate(f.w, f.h, 8, 24, f.w * 3, cs, (CGBitmapInfo)kCGImageAlphaNone, provider,
                                     nullptr, false, kCGRenderingIntentDefault);
    NSURL* url = [NSURL fileURLWithPath:[NSString stringWithUTF8String:path.c_str()]];
    CGImageDestinationRef dst = CGImageDestinationCreateWithURL((__bridge CFURLRef)url, CFSTR("public.png"), 1, nullptr);
    if (dst && image) {
        CGImageDestinationAddImage(dst, image, nullptr);
        CGImageDestinationFinalize(dst);
    }
    if (dst) CFRelease(dst);
    if (image) CGImageRelease(image);
    CGDataProviderRelease(provider);
    CFRelease(data);
    CGColorSpaceRelease(cs);
}

AUAudioUnit* instantiate(const AudioComponentDescription& desc, std::string* error) {
    __block AUAudioUnit* unit = nil;
    __block NSString* message = nil;
    __block bool done = false;
    [AUAudioUnit instantiateWithComponentDescription:desc
                                             options:kAudioComponentInstantiation_LoadOutOfProcess
                                   completionHandler:^(AUAudioUnit* u, NSError* e) {
                                       unit = u;
                                       message = e.description;
                                       done = true;
                                   }];
    const double t0 = now_ms();
    while (!done && now_ms() - t0 < 20000) spin(5);
    if (!unit && error) *error = message ? message.UTF8String : "timed out";
    return unit;
}

}  // namespace

int main(int argc, char** argv) {
    std::string type = "aufx", sub, mfr = "Pulp", out;
    int opens = 3, max_non_ui = -1;
    double watch_ms = 2500, vw = 0, vh = 0;
    uint32_t settled_bg = 0x05070A;
    bool fresh = false, follow = false, no_anim = false;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&] { return i + 1 < argc ? std::string(argv[++i]) : std::string(); };
        if (a == "--type") type = next();
        else if (a == "--sub") sub = next();
        else if (a == "--mfr") mfr = next();
        else if (a == "--opens") opens = std::atoi(next().c_str());
        else if (a == "--out") out = next();
        else if (a == "--settled-bg") settled_bg = uint32_t(std::strtoul(next().c_str(), nullptr, 16));
        else if (a == "--watch-ms") watch_ms = std::atof(next().c_str());
        else if (a == "--view-size") std::sscanf(next().c_str(), "%lfx%lf", &vw, &vh);
        else if (a == "--fresh-instance") fresh = true;
        else if (a == "--follow") follow = true;
        else if (a == "--no-anim") no_anim = true;
        else if (a == "--max-non-ui-frames") max_non_ui = std::atoi(next().c_str());
        else { std::fprintf(stderr, "unknown argument: %s\n", a.c_str()); return 2; }
    }
    if (sub.size() != 4 || type.size() != 4 || mfr.size() != 4) {
        std::fprintf(stderr, "--sub, --type and --mfr are four-character codes\n");
        return 2;
    }
    int failures = 0;
    bool blind = false;
    @autoreleasepool {
        [NSApplication sharedApplication];
        [NSApp setActivationPolicy:NSApplicationActivationPolicyAccessory];
        [NSApp finishLaunching];
        const AudioComponentDescription desc{four_cc(type.c_str()), four_cc(sub.c_str()), four_cc(mfr.c_str()), 0, 0};
        std::string error;
        double t0 = now_ms();
        AUAudioUnit* unit = instantiate(desc, &error);
        if (!unit) {
            std::printf("instantiate %s/%s/%s failed: %s\n", type.c_str(), sub.c_str(), mfr.c_str(), error.c_str());
            return 2;
        }
        std::printf("instantiated %s/%s out of process in %.1f ms (%s)\n", sub.c_str(), mfr.c_str(),
                    now_ms() - t0, object_getClassName(unit));
        double ox = -30000, oy = -30000;
        if (const char* o = std::getenv("PULP_PROBE_ORIGIN")) std::sscanf(o, "%lf,%lf", &ox, &oy);
        for (int n = 0; n < opens; ++n) {
            if (fresh && n > 0) {
                unit = nil;
                t0 = now_ms();
                unit = instantiate(desc, &error);
                if (!unit) { std::printf("re-instantiate failed: %s\n", error.c_str()); return 2; }
                std::printf("open %d: instantiated in %.1f ms\n", n + 1, now_ms() - t0);
            }
            __block NSViewController* controller = nil;
            __block bool got = false;
            const double f0 = now_ms();
            [unit requestViewControllerWithCompletionHandler:^(AUViewControllerBase* vc) {
                controller = vc;
                got = true;
            }];
            while (!got && now_ms() - f0 < 20000) spin(1);
            const double f1 = now_ms();
            if (!controller) { std::printf("open %d: no view controller\n", n + 1); ++failures; continue; }
            NSView* view = controller.view;
            NSSize size = view.frame.size;
            if (size.width < 10) size = NSMakeSize(990, 645);
            if (follow && controller.preferredContentSize.width >= 10) size = controller.preferredContentSize;
            if (vw > 0 && !follow) size = NSMakeSize(vw, vh);
            std::printf("open %d: view controller %.1f ms -> %s %.0fx%.0f (preferredContentSize %.0fx%.0f)\n", n + 1,
                        f1 - f0, object_getClassName(view), size.width, size.height,
                        controller.preferredContentSize.width, controller.preferredContentSize.height);
            NSWindow* window = [[NSWindow alloc] initWithContentRect:NSMakeRect(ox, oy, size.width, size.height)
                                                           styleMask:NSWindowStyleMaskBorderless
                                                             backing:NSBackingStoreBuffered
                                                               defer:NO];
            window.releasedWhenClosed = NO;
            if (no_anim) window.animationBehavior = NSWindowAnimationBehaviorNone;
            window.backgroundColor = [NSColor colorWithSRGBRed:1 green:0 blue:1 alpha:1];
            NSView* content = [[NSView alloc] initWithFrame:NSMakeRect(0, 0, size.width, size.height)];
            window.contentView = content;
            view.frame = content.bounds;
            [content addSubview:view];
            [window orderFrontRegardless];
            std::vector<Frame> frames;
            bool resized = false;
            NSSize last_pref = controller.preferredContentSize;
            const double a = now_ms();
            while (now_ms() - a < watch_ms) {
                if (follow) {
                    const NSSize pref = controller.preferredContentSize;
                    if (!NSEqualSizes(pref, last_pref) && pref.width >= 10 && pref.height >= 10) {
                        last_pref = pref;
                        [window setContentSize:pref];
                        content.frame = NSMakeRect(0, 0, pref.width, pref.height);
                        view.frame = content.bounds;
                    }
                    // A host that resizes after open (a restored or minimum
                    // size) does so once it has the view.
                    if (vw > 0 && !resized) {
                        resized = true;
                        [window setContentSize:NSMakeSize(vw, vh)];
                        content.frame = NSMakeRect(0, 0, vw, vh);
                        view.frame = content.bounds;
                    }
                }
                Frame f;
                f.t_ms = now_ms() - f0;
                if (read_window(window, f) && (frames.empty() || frames.back().rgb != f.rgb))
                    frames.push_back(std::move(f));
                spin(4);
            }
            if (frames.empty()) {
                std::printf("open %d: no window image could be read back\n", n + 1);
                ++failures;
            } else {
                const auto stages = pulp::tools::editor_open::classify_open(frames, settled_bg);
                if (stages.blind) {
                    std::printf("open %d: BLIND: every image of the host window is its backdrop; this "
                                "read-back cannot see the remote view here (or the editor never drew). "
                                "Check a trace from the plug-in process before believing either.\n",
                                n + 1);
                    blind = true;
                }
                for (size_t i = 0; i < frames.size(); ++i) {
                    std::printf("  +%7.1f ms  %s\n", frames[i].t_ms, stages.per_frame[i].c_str());
                    if (!out.empty()) {
                        char name[512];
                        std::snprintf(name, sizeof name, "%s/open%d-%03zu-%07.1fms-%s.png", out.c_str(), n + 1, i,
                                      frames[i].t_ms, stages.per_frame[i].c_str());
                        save_png(frames[i], name);
                    }
                }
                std::printf("  stages: %s\n  non-ui-content-frames %d navy-frames %d ui-at %.1f ms\n",
                            stages.stages.c_str(), stages.non_ui_content_frames, stages.navy_frames,
                            stages.blind ? -1.0 : frames[stages.ready].t_ms);
                if (max_non_ui >= 0 && stages.non_ui_content_frames > max_non_ui) {
                    std::printf("FAIL: %d non-UI image(s) before the UI\n", stages.non_ui_content_frames);
                    ++failures;
                }
            }
            [view removeFromSuperview];
            [window orderOut:nil];
            [window close];
            controller = nil;
            spin(400);
        }
    }
    if (failures) return 1;
    return blind ? 3 : 0;
}
