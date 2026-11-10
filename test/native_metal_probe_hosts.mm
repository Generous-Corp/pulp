// native-metal-probe-host clap <module> | au <component>
//
// Loads the built metal-probe plug-in the way a host does, in this process
// only: nothing is installed. For CLAP it reads clap_entry, creates and
// activates the plug-in, processes one block, then opens its GUI into an
// offscreen window. For AU v2 it registers the bundle's factory with
// AudioComponentRegister, instantiates and initializes the unit, renders one
// block, and opens the advertised Cocoa view. Either way the editor must put
// one native surface view (class PulpNativeSurfaceView_<image token>) under
// the parent, at the editor's 360x240, and remove it when the GUI closes. The
// AU round is repeated with the instance disposed while its view is alive.
// Compiled with ARC.

#import <AppKit/AppKit.h>
#import <AudioToolbox/AudioToolbox.h>
#import <AudioUnit/AUCocoaUIView.h>

#include <clap/clap.h>

#include <dlfcn.h>

#include <alloca.h>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

int surfaces_under(NSView* root) {
    int n = [NSStringFromClass([root class]) hasPrefix:@"PulpNativeSurfaceView_"] ? 1 : 0;
    for (NSView* child in root.subviews)
        n += surfaces_under(child);
    return n;
}

NSWindow* make_window() {
    [NSApplication sharedApplication];
    NSWindow* window = [[NSWindow alloc] initWithContentRect:NSMakeRect(-20000, -20000, 800, 600)
                                                   styleMask:NSWindowStyleMaskBorderless
                                                     backing:NSBackingStoreBuffered
                                                       defer:NO];
    window.releasedWhenClosed = NO;
    return window;
}

int fail(const std::string& what) {
    std::fprintf(stderr, "FAIL %s\n", what.c_str());
    return 1;
}

// ── CLAP ─────────────────────────────────────────────────────────────────────

const void* host_get_extension(const clap_host_t*, const char*) {
    return nullptr;
}
void host_noop(const clap_host_t*) {}

int run_clap(const char* path) {
    void* lib = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (!lib)
        return fail(std::string("cannot load ") + path + ": " + dlerror());
    auto* entry = static_cast<const clap_plugin_entry_t*>(dlsym(lib, "clap_entry"));
    if (!entry || !entry->init(path))
        return fail("no usable clap_entry");
    auto* factory =
        static_cast<const clap_plugin_factory_t*>(entry->get_factory(CLAP_PLUGIN_FACTORY_ID));
    if (!factory || factory->get_plugin_count(factory) != 1)
        return fail("the module does not offer exactly one plug-in");
    const auto* desc = factory->get_plugin_descriptor(factory, 0);
    clap_host_t host{CLAP_VERSION, nullptr,  "pulp-native-probe-host", "Pulp",
                     "",           "1.0",    host_get_extension,       host_noop,
                     host_noop,    host_noop};
    const clap_plugin_t* plugin = factory->create_plugin(factory, &host, desc->id);
    if (!plugin || !plugin->init(plugin))
        return fail("create_plugin or init failed");
    if (!plugin->activate(plugin, 48000.0, 32, 512) || !plugin->start_processing(plugin))
        return fail("activate or start_processing failed");

    std::vector<float> in_l(512, 0.5f), in_r(512, 0.5f), out_l(512), out_r(512);
    float* ins[2] = {in_l.data(), in_r.data()};
    float* outs[2] = {out_l.data(), out_r.data()};
    clap_audio_buffer_t in_buf{ins, nullptr, 2, 0, 0};
    clap_audio_buffer_t out_buf{outs, nullptr, 2, 0, 0};
    clap_input_events_t in_events{
        nullptr, [](const clap_input_events_t*) -> uint32_t { return 0; },
        [](const clap_input_events_t*, uint32_t) -> const clap_event_header_t* { return nullptr; }};
    clap_output_events_t out_events{
        nullptr, [](const clap_output_events_t*, const clap_event_header_t*) { return true; }};
    clap_process_t process{};
    process.frames_count = 512;
    process.steady_time = -1;
    process.audio_inputs = &in_buf;
    process.audio_inputs_count = 1;
    process.audio_outputs = &out_buf;
    process.audio_outputs_count = 1;
    process.in_events = &in_events;
    process.out_events = &out_events;
    if (plugin->process(plugin, &process) == CLAP_PROCESS_ERROR)
        return fail("process returned an error");
    // The default gain is 0.5, so a 0.5 input comes out at 0.25.
    if (out_l[100] < 0.2f || out_l[100] > 0.3f)
        return fail("processed output is " + std::to_string(out_l[100]) + ", expected 0.25");

    auto* gui = static_cast<const clap_plugin_gui_t*>(plugin->get_extension(plugin, CLAP_EXT_GUI));
    if (!gui || !gui->is_api_supported(plugin, CLAP_WINDOW_API_COCOA, false) ||
        !gui->create(plugin, CLAP_WINDOW_API_COCOA, false))
        return fail("no Cocoa GUI");
    int result = 0;
    @autoreleasepool {
        NSWindow* window = make_window();
        NSView* parent = window.contentView;
        clap_window_t w{};
        w.api = CLAP_WINDOW_API_COCOA;
        w.cocoa = (__bridge void*)parent;
        uint32_t width = 0, height = 0;
        if (!gui->set_parent(plugin, &w))
            result = fail("gui set_parent failed");
        else if (!gui->get_size(plugin, &width, &height) || width != 360 || height != 240)
            result = fail("gui size is " + std::to_string(width) + "x" + std::to_string(height) +
                          ", not the editor's 360x240");
        else if (surfaces_under(parent) != 1)
            result = fail("the GUI did not put one native surface under its parent");
        gui->destroy(plugin);
        if (result == 0 && surfaces_under(parent) != 0)
            result = fail("closing the GUI left its native surface behind");
        [window close];
    }
    plugin->stop_processing(plugin);
    plugin->deactivate(plugin);
    plugin->destroy(plugin);
    entry->deinit();
    if (result == 0)
        std::printf("ok   CLAP %s processes, opens its native surface GUI and closes it\n", path);
    return result;
}

// ── AU v2 ────────────────────────────────────────────────────────────────────

OSType fourcc(NSString* s) {
    const char* c = s.UTF8String;
    return s.length == 4
               ? (OSType(c[0]) << 24) | (OSType(c[1]) << 16) | (OSType(c[2]) << 8) | OSType(c[3])
               : 0;
}

AudioComponent register_bundle(const char* path) {
    NSURL* url = [NSURL fileURLWithPath:@(path)];
    CFBundleRef bundle = CFBundleCreate(nullptr, (__bridge CFURLRef)url);
    if (!bundle || !CFBundleLoadExecutable(bundle))
        return nullptr;
    NSDictionary* info = (__bridge NSDictionary*)CFBundleGetInfoDictionary(bundle);
    NSDictionary* comp = [info[@"AudioComponents"] firstObject];
    auto factory = (AudioComponentFactoryFunction)CFBundleGetFunctionPointerForName(
        bundle, (__bridge CFStringRef)comp[@"factoryFunction"]);
    if (!factory)
        return nullptr;
    AudioComponentDescription desc{};
    desc.componentType = fourcc(comp[@"type"]);
    desc.componentSubType = fourcc(comp[@"subtype"]);
    desc.componentManufacturer = fourcc(comp[@"manufacturer"]);
    return AudioComponentRegister(&desc, CFSTR("Pulp: native metal probe"),
                                  [comp[@"version"] unsignedIntValue], factory);
}

NSView* open_view(AudioUnit au, std::string& error) {
    AudioUnitCocoaViewInfo info{};
    UInt32 size = sizeof(info);
    if (AudioUnitGetProperty(au, kAudioUnitProperty_CocoaUI, kAudioUnitScope_Global, 0, &info,
                             &size) != noErr) {
        error = "advertises no Cocoa view";
        return nil;
    }
    Class cls = NSClassFromString((__bridge NSString*)info.mCocoaAUViewClass[0]);
    CFRelease(info.mCocoaAUViewBundleLocation);
    CFRelease(info.mCocoaAUViewClass[0]);
    if (!cls || ![cls conformsToProtocol:@protocol(AUCocoaUIBase)]) {
        error = "advertised view factory does not resolve";
        return nil;
    }
    id<AUCocoaUIBase> factory = [[cls alloc] init];
    NSView* view = [factory uiViewForAudioUnit:au withSize:NSMakeSize(640, 480)];
    if (!view)
        error = "view factory returned no view";
    return view;
}

OSStatus silence_input(void*, AudioUnitRenderActionFlags*, const AudioTimeStamp*, UInt32,
                       UInt32 frames, AudioBufferList* io) {
    for (UInt32 b = 0; b < io->mNumberBuffers; ++b) {
        auto* data = static_cast<float*>(io->mBuffers[b].mData);
        for (UInt32 i = 0; i < frames; ++i)
            data[i] = 0.5f;
    }
    return noErr;
}

bool render_one_block(AudioUnit au, std::string& error) {
    AURenderCallbackStruct cb{silence_input, nullptr};
    if (AudioUnitSetProperty(au, kAudioUnitProperty_SetRenderCallback, kAudioUnitScope_Input, 0,
                             &cb, sizeof(cb)) != noErr) {
        error = "cannot set the render callback";
        return false;
    }
    std::vector<float> left(512), right(512);
    AudioBufferList* list =
        static_cast<AudioBufferList*>(alloca(sizeof(AudioBufferList) + sizeof(AudioBuffer)));
    list->mNumberBuffers = 2;
    list->mBuffers[0] = {1, 512 * sizeof(float), left.data()};
    list->mBuffers[1] = {1, 512 * sizeof(float), right.data()};
    AudioUnitRenderActionFlags flags = 0;
    AudioTimeStamp ts{};
    ts.mFlags = kAudioTimeStampSampleTimeValid;
    if (AudioUnitRender(au, &flags, &ts, 0, 512, list) != noErr) {
        error = "AudioUnitRender failed";
        return false;
    }
    if (left[100] < 0.2f || left[100] > 0.3f) {
        error = "rendered output is " + std::to_string(left[100]) + ", expected 0.25";
        return false;
    }
    return true;
}

int run_au(const char* path) {
    AudioComponent comp = register_bundle(path);
    if (!comp)
        return fail(std::string("cannot register ") + path);
    NSWindow* window = make_window();
    for (int round = 0; round < 2; ++round) {
        AudioUnit au = nullptr;
        if (AudioComponentInstanceNew(comp, &au) != noErr || !au ||
            AudioUnitInitialize(au) != noErr)
            return fail("cannot instantiate and initialize the unit");
        std::string error;
        if (round == 0 && !render_one_block(au, error))
            return fail(error);
        NSView* view = nil;
        @autoreleasepool {
            view = open_view(au, error);
            if (!view)
                return fail(error);
            if (view.frame.size.width != 360 || view.frame.size.height != 240)
                return fail("view is not the editor's 360x240");
            [window.contentView addSubview:view];
            if (surfaces_under(view) != 1)
                return fail("the view holds no native surface");
        }
        if (round == 0) {
            @autoreleasepool {
                [view removeFromSuperview];
                view = nil;
            }
            AudioUnitUninitialize(au);
            AudioComponentInstanceDispose(au);
        } else {
            // Disposed while the view is still alive, as some hosts do.
            AudioUnitUninitialize(au);
            AudioComponentInstanceDispose(au);
            @autoreleasepool {
                [view removeFromSuperview];
                view = nil;
            }
        }
        if (surfaces_under(window.contentView) != 0)
            return fail("round " + std::to_string(round) + " left a native surface behind");
    }
    [window close];
    std::printf("ok   AU %s renders, opens its native surface view and closes it twice\n", path);
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    if (argc != 3 || (std::strcmp(argv[1], "clap") != 0 && std::strcmp(argv[1], "au") != 0)) {
        std::fprintf(stderr, "usage: %s clap <module> | au <component>\n", argv[0]);
        return 2;
    }
    @autoreleasepool {
        return std::strcmp(argv[1], "clap") == 0 ? run_clap(argv[2]) : run_au(argv[2]);
    }
}
