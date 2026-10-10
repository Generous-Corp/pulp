// Load built plug-in bundles into one host process and record what a host
// observes: load, prepare, audio output with the editor closed and open, and
// the editor's open / resize / close / reopen lifecycle with a read-back of
// what the host window composited. Several --plugin arguments load into the
// SAME process, so two plug-ins that each carry a GPU runtime are exercised
// side by side (dyld image census included).
//
// The output is data for a paired comparison (Vellum tools/parity host lane),
// not a verdict: every field that two builds of the same plug-in must agree on
// is recorded so the comparison can decide. Exit status only says whether the
// probe could run.
//
// CLAP and VST3 load from their bundle path. An AU v2 .component is registered
// with the current process only (AudioComponentRegister on the bundle's own
// factory), so nothing is installed and no system component cache is touched.
//
// Audio is processed offline through PluginSlot::process; no audio device is
// opened. Editor windows are borderless and placed off every screen.
//
// Usage: pulp-host-lane-probe --plugin BUNDLE [--plugin BUNDLE ...] --out DIR
//          [--blocks 94] [--block-size 512] [--sample-rate 48000]
//          [--opens 2] [--watch-ms 1500] [--resize-by 120x80]
//          [--fault-null-parent]
// Exit: 0 observations written, 2 setup error (bad arguments, a plug-in did
// not load or prepare).
#import <AppKit/AppKit.h>
#import <AudioToolbox/AudioToolbox.h>
#import <CommonCrypto/CommonDigest.h>
#import <ImageIO/ImageIO.h>
#include <dlfcn.h>
#include <mach-o/dyld.h>

#include <pulp/audio/buffer.hpp>
#include <pulp/host/hosted_editor_container.hpp>
#include <pulp/host/plugin_slot.hpp>
#include <pulp/host/scanner.hpp>
#include <pulp/midi/buffer.hpp>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <set>
#include <sstream>
#include <string>
#include <vector>

using pulp::host::PluginFormat;
using pulp::host::PluginInfo;
using pulp::host::PluginSlot;

namespace {

constexpr const char* kSchema = "pulp.host-lane.observation.v1";

struct Options {
    std::vector<std::string> plugins;
    std::string out;
    int blocks = 94;
    int block_size = 512;
    double sample_rate = 48000.0;
    int opens = 2;
    double watch_ms = 1500;
    uint32_t grow_w = 120, grow_h = 80;
    bool null_parent = false;
};

const auto g_start = std::chrono::steady_clock::now();
double now_ms() {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - g_start).count();
}
void spin(double ms) {
    const double end = now_ms() + ms;
    while (now_ms() < end) {
        @autoreleasepool {
            [[NSRunLoop mainRunLoop] runUntilDate:[NSDate dateWithTimeIntervalSinceNow:0.01]];
        }
    }
}

std::string hex(const unsigned char* d, size_t n) {
    static const char* k = "0123456789abcdef";
    std::string s;
    for (size_t i = 0; i < n; ++i) {
        s += k[d[i] >> 4];
        s += k[d[i] & 15];
    }
    return s;
}
std::string sha256(const void* data, size_t n) {
    unsigned char d[CC_SHA256_DIGEST_LENGTH];
    CC_SHA256(data, CC_LONG(n), d);
    return hex(d, sizeof d);
}

std::string quote(const std::string& s) {
    std::string o = "\"";
    for (char c : s) {
        if (c == '"' || c == '\\') { o += '\\'; o += c; }
        else if (c == '\n') o += "\\n";
        else if (static_cast<unsigned char>(c) < 0x20) o += ' ';
        else o += c;
    }
    return o + "\"";
}

// ── stimulus and render ─────────────────────────────────────────────────────

// Deterministic stereo stimulus: a 220 Hz / 330 Hz pair plus seeded noise, so
// a delay or filter has content to act on and two runs feed identical input.
std::vector<float> stimulus(int frames, double sr, int channel) {
    std::vector<float> v(frames);
    uint32_t state = 0x9E3779B9u ^ uint32_t(channel * 7919);
    const double f = channel == 0 ? 220.0 : 330.0;
    for (int i = 0; i < frames; ++i) {
        state = state * 1664525u + 1013904223u;
        const float noise = (float(state >> 8) / float(1u << 24)) * 2.0f - 1.0f;
        v[i] = 0.25f * float(std::sin(2.0 * M_PI * f * i / sr)) + 0.05f * noise;
        if (i > frames / 2) v[i] *= 0.0f;  // silence tail exposes delay/reverb output
    }
    return v;
}

struct Render {
    std::string sha256;
    double peak = 0, rms = 0;
    int frames = 0;
    bool finite = true;
};

Render render(PluginSlot& slot, const Options& o, const std::string& raw_path) {
    const int frames = o.blocks * o.block_size;
    auto in_l = stimulus(frames, o.sample_rate, 0), in_r = stimulus(frames, o.sample_rate, 1);
    std::vector<float> out_l(frames, 0.f), out_r(frames, 0.f);
    pulp::midi::MidiBuffer mi, mo;
    pulp::host::ParameterEventQueue pe;
    for (int b = 0; b < o.blocks; ++b) {
        const size_t off = size_t(b) * o.block_size;
        const float* ip[2] = {in_l.data() + off, in_r.data() + off};
        float* op[2] = {out_l.data() + off, out_r.data() + off};
        pulp::audio::BufferView<const float> in(ip, 2, o.block_size);
        pulp::audio::BufferView<float> out(op, 2, o.block_size);
        mi.clear();
        mo.clear();
        slot.process(out, in, mi, mo, pe, o.block_size);
    }
    std::vector<float> inter(size_t(frames) * 2);
    Render r;
    r.frames = frames;
    double sum = 0;
    for (int i = 0; i < frames; ++i) {
        inter[size_t(i) * 2] = out_l[i];
        inter[size_t(i) * 2 + 1] = out_r[i];
        for (float s : {out_l[i], out_r[i]}) {
            if (!std::isfinite(s)) r.finite = false;
            r.peak = std::max(r.peak, double(std::fabs(s)));
            sum += double(s) * s;
        }
    }
    r.rms = std::sqrt(sum / (2.0 * frames));
    r.sha256 = sha256(inter.data(), inter.size() * sizeof(float));
    std::ofstream(raw_path, std::ios::binary)
        .write(reinterpret_cast<const char*>(inter.data()), std::streamsize(inter.size() * sizeof(float)));
    return r;
}

std::string render_json(const Render& r, const std::string& file) {
    std::ostringstream s;
    s << "{\"sha256\":" << quote(r.sha256) << ",\"frames\":" << r.frames << ",\"channels\":2"
      << ",\"peak\":" << r.peak << ",\"rms\":" << r.rms << ",\"finite\":" << (r.finite ? "true" : "false")
      << ",\"file\":" << quote(file) << "}";
    return s.str();
}

// ── editor read-back ────────────────────────────────────────────────────────

// The host window's backdrop. Any pixel still this colour after an editor
// attached is a place the editor did not draw.
constexpr uint8_t kBackdrop[3] = {255, 0, 255};

struct Frame {
    size_t w = 0, h = 0;
    std::vector<uint8_t> rgb;
};

// Own-window read-back through the window-list API, looked up at run time
// (deprecated, but the only call that returns a window's composited image
// without a capture session or screen-recording permission).
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

std::string frame_json(bool read, const Frame& f, const std::string& png) {
    if (!read) return "{\"read\":false}";
    size_t backdrop = 0;
    std::set<uint32_t> colours;
    for (size_t i = 0; i < f.w * f.h; ++i) {
        const uint8_t* p = &f.rgb[i * 3];
        if (p[0] == kBackdrop[0] && p[1] == kBackdrop[1] && p[2] == kBackdrop[2]) ++backdrop;
        if (colours.size() < 4096) colours.insert(uint32_t(p[0]) << 16 | uint32_t(p[1]) << 8 | p[2]);
    }
    std::ostringstream s;
    s << "{\"read\":true,\"width\":" << f.w << ",\"height\":" << f.h << ",\"backdrop_pixels\":" << backdrop
      << ",\"pixels\":" << f.w * f.h << ",\"unique_colors\":" << colours.size()
      << ",\"png\":" << quote(png) << ",\"sha256\":" << quote(sha256(f.rgb.data(), f.rgb.size())) << "}";
    return s.str();
}

// ── loading ─────────────────────────────────────────────────────────────────

OSType four_cc(NSString* s) {
    const char* c = s.UTF8String;
    if (!c || std::strlen(c) != 4) return 0;
    return OSType(c[0]) << 24 | OSType(c[1]) << 16 | OSType(c[2]) << 8 | OSType(c[3]);
}

// Register an AU v2 bundle's first component with this process only.
bool register_au(const std::filesystem::path& bundle, PluginInfo& info, std::string& error) {
    NSBundle* b = [NSBundle bundleWithPath:[NSString stringWithUTF8String:bundle.c_str()]];
    NSArray* comps = b.infoDictionary[@"AudioComponents"];
    NSDictionary* c = comps.count ? comps[0] : nil;
    if (!c) { error = "no AudioComponents entry in Info.plist"; return false; }
    if (![b load]) { error = "bundle executable did not load"; return false; }
    NSString* factory = c[@"factoryFunction"];
    auto fn = (AudioComponentFactoryFunction)CFBundleGetFunctionPointerForName(
        CFBundleCreate(nullptr, (__bridge CFURLRef)b.bundleURL), (__bridge CFStringRef)factory);
    if (!fn) { error = "factory function not found: " + std::string(factory.UTF8String); return false; }
    AudioComponentDescription d{four_cc(c[@"type"]), four_cc(c[@"subtype"]), four_cc(c[@"manufacturer"]), 0, 0};
    NSString* name = c[@"name"] ? c[@"name"] : @"probe";
    if (!AudioComponentRegister(&d, (__bridge CFStringRef)name, [c[@"version"] unsignedIntValue], fn)) {
        error = "AudioComponentRegister refused the component";
        return false;
    }
    info.unique_id = std::string([c[@"type"] UTF8String]) + ":" + [c[@"subtype"] UTF8String] + ":" +
                     [c[@"manufacturer"] UTF8String];
    return true;
}

struct Loaded {
    std::string path, format, error;
    std::shared_ptr<PluginSlot> slot;
    std::vector<std::string> json;  // assembled member strings
    NSWindow* window = nil;
    std::unique_ptr<PluginSlot::HostedEditor> editor;
};

std::shared_ptr<PluginSlot> load(const std::string& arg, Loaded& L) {
    std::filesystem::path p(arg);
    if (p.filename().empty()) p = p.parent_path();
    L.path = p.string();
    PluginInfo info;
    info.path = p.string();
    info.name = p.stem().string();
    const auto ext = p.extension().string();
    if (ext == ".clap") info.format = PluginFormat::CLAP, L.format = "CLAP";
    else if (ext == ".vst3") info.format = PluginFormat::VST3, L.format = "VST3";
    else if (ext == ".component") {
        info.format = PluginFormat::AudioUnit, L.format = "AU";
        if (!register_au(p, info, L.error)) return nullptr;
    } else {
        L.error = "unsupported bundle extension " + ext;
        return nullptr;
    }
    auto slot = std::shared_ptr<PluginSlot>(PluginSlot::load(info));
    if (!slot) L.error = "PluginSlot::load returned null";
    return slot;
}

NSWindow* make_window(int index, uint32_t w, uint32_t h) {
    NSWindow* win = [[NSWindow alloc] initWithContentRect:NSMakeRect(-30000 + index * 3000, -30000, w, h)
                                                styleMask:NSWindowStyleMaskBorderless
                                                  backing:NSBackingStoreBuffered
                                                    defer:NO];
    win.releasedWhenClosed = NO;
    win.animationBehavior = NSWindowAnimationBehaviorNone;
    win.backgroundColor = [NSColor colorWithSRGBRed:1 green:0 blue:1 alpha:1];
    [win orderFrontRegardless];
    return win;
}

// Open every editor that has one, all at once (so several GPU editors are
// alive in this process together), then read each back.
void open_all(std::vector<Loaded>& all, const Options& o, int open_index, std::vector<std::string>& events) {
    for (size_t i = 0; i < all.size(); ++i) {
        auto& L = all[i];
        if (!L.slot || !L.slot->has_editor()) continue;
        L.window = make_window(int(i), 640, 480);
        const double t0 = now_ms();
        L.editor = L.slot->create_hosted_editor(o.null_parent ? nullptr : (__bridge void*)L.window);
        const double t1 = now_ms();
        std::ostringstream s;
        s << "{\"plugin\":" << i << ",\"open\":" << open_index << ",\"event\":\"create\",\"attached\":"
          << (L.editor ? "true" : "false");
        if (L.editor) {
            s << ",\"width\":" << L.editor->width << ",\"height\":" << L.editor->height
              << ",\"resizable\":" << (L.editor->resizable ? "true" : "false");
            [L.window setContentSize:NSMakeSize(L.editor->width, L.editor->height)];
            pulp::host::resize_editor_container(L.editor->native_handle, L.editor->width, L.editor->height);
        }
        s << ",\"create_ms\":" << (t1 - t0) << "}";
        events.push_back(s.str());
    }
    spin(o.watch_ms);
    for (size_t i = 0; i < all.size(); ++i) {
        auto& L = all[i];
        if (!L.window) continue;
        Frame f;
        const std::string png = o.out + "/editor-" + std::to_string(i) + "-open" + std::to_string(open_index) + ".png";
        const bool read = read_window(L.window, f);
        if (read) save_png(f, png);
        events.push_back("{\"plugin\":" + std::to_string(i) + ",\"open\":" + std::to_string(open_index) +
                         ",\"event\":\"frame\",\"frame\":" + frame_json(read, f, png) + "}");
    }
}

void resize_all(std::vector<Loaded>& all, const Options& o, std::vector<std::string>& events) {
    for (size_t i = 0; i < all.size(); ++i) {
        auto& L = all[i];
        if (!L.editor) continue;
        uint32_t w = L.editor->width + o.grow_w, h = L.editor->height + o.grow_h;
        const uint32_t req_w = w, req_h = h;
        const bool accepted = L.slot->set_hosted_editor_size(w, h);
        if (accepted) {
            [L.window setContentSize:NSMakeSize(w, h)];
            pulp::host::resize_editor_container(L.editor->native_handle, w, h);
        }
        std::ostringstream s;
        s << "{\"plugin\":" << i << ",\"event\":\"resize\",\"requested\":[" << req_w << "," << req_h
          << "],\"accepted\":" << (accepted ? "true" : "false") << ",\"size\":[" << w << "," << h << "]}";
        events.push_back(s.str());
    }
    spin(o.watch_ms);
    for (size_t i = 0; i < all.size(); ++i) {
        auto& L = all[i];
        if (!L.editor) continue;
        Frame f;
        const std::string png = o.out + "/editor-" + std::to_string(i) + "-resized.png";
        const bool read = read_window(L.window, f);
        if (read) save_png(f, png);
        events.push_back("{\"plugin\":" + std::to_string(i) + ",\"event\":\"resized-frame\",\"frame\":" +
                         frame_json(read, f, png) + "}");
    }
}

void close_all(std::vector<Loaded>& all, int open_index, std::vector<std::string>& events) {
    for (size_t i = 0; i < all.size(); ++i) {
        auto& L = all[i];
        if (!L.window) continue;
        const bool had = bool(L.editor);
        if (L.editor) L.slot->destroy_hosted_editor(std::move(L.editor));
        [L.window orderOut:nil];
        [L.window close];
        L.window = nil;
        events.push_back("{\"plugin\":" + std::to_string(i) + ",\"open\":" + std::to_string(open_index) +
                         ",\"event\":\"close\",\"had_editor\":" + (had ? "true" : "false") + "}");
    }
    spin(100);
}

std::string dyld_census() {
    std::ostringstream s;
    s << "[";
    bool first = true;
    for (uint32_t i = 0; i < _dyld_image_count(); ++i) {
        const std::string path = _dyld_get_image_name(i);
        const bool interesting = path.find(".clap/") != std::string::npos || path.find(".vst3/") != std::string::npos ||
                                 path.find(".component/") != std::string::npos || path.find("wgpu") != std::string::npos ||
                                 path.find("dawn") != std::string::npos || path.find("webgpu") != std::string::npos ||
                                 path.find("skia") != std::string::npos;
        if (!interesting) continue;
        s << (first ? "" : ",") << quote(path);
        first = false;
    }
    return s.str() + "]";
}

int parse(int argc, char** argv, Options& o) {
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&] { return i + 1 < argc ? std::string(argv[++i]) : std::string(); };
        if (a == "--plugin") o.plugins.push_back(next());
        else if (a == "--out") o.out = next();
        else if (a == "--blocks") o.blocks = std::atoi(next().c_str());
        else if (a == "--block-size") o.block_size = std::atoi(next().c_str());
        else if (a == "--sample-rate") o.sample_rate = std::atof(next().c_str());
        else if (a == "--opens") o.opens = std::atoi(next().c_str());
        else if (a == "--watch-ms") o.watch_ms = std::atof(next().c_str());
        else if (a == "--resize-by") std::sscanf(next().c_str(), "%ux%u", &o.grow_w, &o.grow_h);
        else if (a == "--fault-null-parent") o.null_parent = true;
        else { std::fprintf(stderr, "unknown argument: %s\n", a.c_str()); return 2; }
    }
    if (o.plugins.empty() || o.out.empty() || o.blocks <= 0 || o.block_size <= 0 || o.opens <= 0) {
        std::fprintf(stderr, "usage: pulp-host-lane-probe --plugin BUNDLE [...] --out DIR\n");
        return 2;
    }
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    Options o;
    if (int rc = parse(argc, argv, o)) return rc;
    std::filesystem::create_directories(o.out);
    @autoreleasepool {
        [NSApplication sharedApplication];
        [NSApp setActivationPolicy:NSApplicationActivationPolicyAccessory];
        [NSApp finishLaunching];

        std::vector<Loaded> all(o.plugins.size());
        for (size_t i = 0; i < o.plugins.size(); ++i) {
            all[i].slot = load(o.plugins[i], all[i]);
            if (!all[i].slot) {
                std::fprintf(stderr, "load %s failed: %s\n", o.plugins[i].c_str(), all[i].error.c_str());
                return 2;
            }
            if (!all[i].slot->prepare(o.sample_rate, o.block_size)) {
                std::fprintf(stderr, "prepare %s failed\n", o.plugins[i].c_str());
                return 2;
            }
        }
        spin(200);  // let asynchronous initialization land before the first render

        std::vector<std::string> events;
        std::vector<Render> initial(all.size()), closed_before(all.size()), open_r(all.size()),
            closed_after(all.size());
        auto raw = [&](size_t i, const char* tag) { return o.out + "/audio-" + std::to_string(i) + "-" + tag + ".f32"; };
        // "initial" is the first render after instantiation. The other three
        // each follow a release + prepare, so they start from the same state
        // and differ only in whether editors were open.
        for (size_t i = 0; i < all.size(); ++i) {
            initial[i] = render(*all[i].slot, o, raw(i, "initial"));
            all[i].slot->release();
            all[i].slot->prepare(o.sample_rate, o.block_size);
            closed_before[i] = render(*all[i].slot, o, raw(i, "closed"));
        }

        for (int n = 0; n < o.opens; ++n) {
            open_all(all, o, n, events);
            if (n == 0) {
                resize_all(all, o, events);
                // Audio with every editor open, from a fresh prepare so it is
                // comparable with the closed-editor render.
                for (size_t i = 0; i < all.size(); ++i) {
                    all[i].slot->release();
                    all[i].slot->prepare(o.sample_rate, o.block_size);
                    open_r[i] = render(*all[i].slot, o, raw(i, "open"));
                }
            }
            close_all(all, n, events);
        }
        for (size_t i = 0; i < all.size(); ++i) {
            all[i].slot->release();
            all[i].slot->prepare(o.sample_rate, o.block_size);
            closed_after[i] = render(*all[i].slot, o, raw(i, "reclosed"));
        }

        std::ostringstream doc;
        doc << "{\"schema\":" << quote(kSchema) << ",\"settings\":{\"blocks\":" << o.blocks
            << ",\"block_size\":" << o.block_size << ",\"sample_rate\":" << o.sample_rate << ",\"opens\":" << o.opens
            << ",\"watch_ms\":" << o.watch_ms << ",\"resize_by\":[" << o.grow_w << "," << o.grow_h
            << "],\"fault_null_parent\":" << (o.null_parent ? "true" : "false") << "},\"plugins\":[";
        for (size_t i = 0; i < all.size(); ++i) {
            auto& s = *all[i].slot;
            doc << (i ? "," : "") << "{\"index\":" << i << ",\"path\":" << quote(all[i].path)
                << ",\"format\":" << quote(all[i].format) << ",\"name\":" << quote(s.info().name)
                << ",\"has_editor\":" << (s.has_editor() ? "true" : "false")
                << ",\"latency_samples\":" << s.latency_samples() << ",\"parameters\":" << s.parameters().size()
                << ",\"audio\":{\"initial\":" << render_json(initial[i], raw(i, "initial"))
                << ",\"closed\":" << render_json(closed_before[i], raw(i, "closed"))
                << ",\"open\":" << render_json(open_r[i], raw(i, "open"))
                << ",\"reclosed\":" << render_json(closed_after[i], raw(i, "reclosed")) << "}}";
        }
        doc << "],\"events\":[";
        for (size_t i = 0; i < events.size(); ++i) doc << (i ? "," : "") << events[i];
        doc << "],\"dyld_images\":" << dyld_census() << "}\n";
        std::ofstream(o.out + "/host-observation.json") << doc.str();
        for (auto& L : all) L.slot->release();
    }
    std::printf("wrote %s/host-observation.json\n", o.out.c_str());
    return 0;
}
