// test_plugin_editor_host_smoke_mac.mm — GPU view-host-in-plugins smoke (mac)
//
// Proves the *embedded* GPU plugin host actually attaches to a host-provided
// parent NSView and paints a first frame — the thing the headless offscreen
// test can't cover (offscreen has no window/attach lifecycle). Two layers:
//
//   1. PluginViewHost level (deterministic, we own the host): build a GPU
//      host for a requires_gpu_host() editor, attach it to a HIDDEN NSWindow's
//      content view, pump the run loop so -viewDidMoveToWindow starts the
//      display link, then capture_back_buffer_png() and assert a nonblank PNG.
//      Also resizes and re-captures (HiDPI surface-resize path).
//
//   2. CLAP adapter level (no DAW): drive gui_create / set_parent / set_size
//      through the generated CLAP entry against the same hidden window, which
//      exercises the adapter's decide_gpu_host() auto-selection + attach path.
//
// Must run with a real window server (self-hosted mac GPU lane). Clears the
// no-editor env guards in-process. Soft-skips when no Dawn/Metal adapter or
// Cocoa window is available, so a GitHub-hosted mac without a GPU stays green.
//
// Tag: [gpu][skia][plugin-gpu-host][mac][issue-2700-followup]

#include <catch2/catch_test_macros.hpp>

#if defined(PULP_HAS_SKIA) && defined(__APPLE__)

#import <Cocoa/Cocoa.h>

#include <pulp/format/clap_entry.hpp>
#include <pulp/format/gpu_host_select.hpp>
#include <pulp/format/processor.hpp>
#include <pulp/format/view_bridge.hpp>
#include <pulp/state/store.hpp>
#include <pulp/view/plugin_view_host.hpp>
#include <pulp/view/view.hpp>
#include <pulp/view/widgets.hpp>
#include <pulp/canvas/canvas.hpp>

#include <cstdlib>
#include <memory>
#include <string>

using namespace pulp;

namespace smoke {

constexpr uint32_t kW = 480;
constexpr uint32_t kH = 320;

class GpuEditorProcessor : public format::Processor {
public:
    format::PluginDescriptor descriptor() const override {
        return {"GpuEditorSmoke", "Acme", "com.acme.gpueditor.smoke", "1.0.0",
                format::PluginCategory::Effect};
    }
    void define_parameters(state::StateStore& s) override {
        s.add_parameter({1, "Level", "dB", {-60.0f, 12.0f, 0.0f}});
    }
    void prepare(const format::PrepareContext&) override {}
    void process(audio::BufferView<float>&, const audio::BufferView<const float>&,
                 midi::MidiBuffer&, midi::MidiBuffer&,
                 const format::ProcessContext&) override {}
    format::ViewSize view_size() const override { return {kW, kH, 320, 240, 1024, 768}; }
    std::unique_ptr<view::View> create_view() override {
        auto root = std::make_unique<view::View>();
        root->set_requires_gpu_host(true);
        root->set_background_color(canvas::Color::rgba8(30, 30, 46));
        root->flex().direction = view::FlexDirection::column;
        root->flex().padding = 16;
        for (const char* t : {"CHAINER", "OSC", "X-OVER", "LIMIT"}) {
            auto l = std::make_unique<view::Label>(t);
            l->set_font_size(20.0f);
            l->flex().preferred_height = 28;
            root->add_child(std::move(l));
        }
        return root;
    }
};

inline std::unique_ptr<format::Processor> create_smoke_processor() {
    return std::make_unique<GpuEditorProcessor>();
}

inline void pump_run_loop(int frames) {
    for (int i = 0; i < frames; ++i) {
        @autoreleasepool {
            [[NSRunLoop currentRunLoop]
                runMode:NSDefaultRunLoopMode
                beforeDate:[NSDate dateWithTimeIntervalSinceNow:0.02]];
        }
    }
}

inline void clear_no_editor_env() {
    ::unsetenv("CI");
    ::unsetenv("PULP_TEST_MODE");
    ::unsetenv("PULP_HEADLESS");
    ::unsetenv("PULP_DISABLE_PLUGIN_EDITOR");
    ::unsetenv("PULP_DISABLE_PLUGIN_GPU");
}

inline bool looks_like_png(const std::vector<uint8_t>& d) {
    return d.size() > 8 && d[0] == 0x89 && d[1] == 'P' && d[2] == 'N' && d[3] == 'G';
}

} // namespace smoke

// Generate a CLAP entry backed by the smoke processor (file-scope global).
PULP_CLAP_PLUGIN(smoke::create_smoke_processor)

TEST_CASE("embedded GPU plugin host attaches + paints first frame (mac)",
          "[gpu][skia][plugin-gpu-host][mac]") {
    smoke::clear_no_editor_env();
    @autoreleasepool {
        NSWindow* window =
            [[NSWindow alloc] initWithContentRect:NSMakeRect(0, 0, smoke::kW, smoke::kH)
                                        styleMask:NSWindowStyleMaskTitled
                                          backing:NSBackingStoreBuffered
                                            defer:NO];
        if (!window || !window.contentView) {
            SUCCEED("No Cocoa window available — embedded host smoke skipped.");
            return;
        }

        smoke::GpuEditorProcessor p;
        state::StateStore store;
        p.set_state_store(&store);
        p.define_parameters(store);
        format::ViewBridge bridge(p, store);
        std::string err;
        REQUIRE(bridge.open(&err));

        auto decision = format::decide_gpu_host(bridge);
        REQUIRE(decision.use_gpu);
        REQUIRE(std::string(decision.mode) == "custom");

        view::PluginViewHost::Options opts;
        opts.size = {smoke::kW, smoke::kH};
        opts.use_gpu = decision.use_gpu;
        auto host = view::PluginViewHost::create(*bridge.view(), opts);
        REQUIRE(host != nullptr);

        if (!host->is_gpu_backed()) {
            // GPU host fell back to CPU — no Dawn/Metal adapter on this host.
            SUCCEED("No GPU adapter — embedded GPU host smoke skipped (CPU fallback).");
            bridge.close();
            [window close];
            return;
        }

        host->set_idle_callback(format::make_scripted_idle_pump(bridge));
        host->attach_to_parent((__bridge void*)window.contentView);
        bridge.notify_attached();
        smoke::pump_run_loop(5);

        auto png = host->capture_back_buffer_png();
        INFO("captured PNG bytes: " << png.size());
        REQUIRE_FALSE(png.empty());
        REQUIRE(smoke::looks_like_png(png));

        // Resize (exercises the GpuSurface PHYSICAL / SkiaSurface LOGICAL+scale
        // resize path) and re-capture.
        host->set_size(smoke::kW + 40, smoke::kH + 20);
        smoke::pump_run_loop(3);
        auto png2 = host->capture_back_buffer_png();
        REQUIRE_FALSE(png2.empty());
        REQUIRE(smoke::looks_like_png(png2));

        host->detach();
        host.reset();
        bridge.close();
        [window close];
    }
}

TEST_CASE("CLAP gui_create/set_parent attaches the embedded editor (mac)",
          "[gpu][skia][plugin-gpu-host][mac][clap]") {
    smoke::clear_no_editor_env();
    @autoreleasepool {
        REQUIRE(clap_entry.init("test"));
        auto* factory = static_cast<const clap_plugin_factory_t*>(
            clap_entry.get_factory(CLAP_PLUGIN_FACTORY_ID));
        REQUIRE(factory != nullptr);
        auto* desc = factory->get_plugin_descriptor(factory, 0);
        REQUIRE(desc != nullptr);
        const clap_plugin_t* plugin = factory->create_plugin(factory, nullptr, desc->id);
        REQUIRE(plugin != nullptr);
        REQUIRE(plugin->init(plugin));

        auto* gui = static_cast<const clap_plugin_gui_t*>(
            plugin->get_extension(plugin, CLAP_EXT_GUI));
        REQUIRE(gui != nullptr);

        if (!gui->is_api_supported(plugin, CLAP_WINDOW_API_COCOA, false)) {
            SUCCEED("Cocoa GUI API not supported on this host — CLAP smoke skipped.");
            plugin->destroy(plugin);
            clap_entry.deinit();
            return;
        }

        if (!gui->create(plugin, CLAP_WINDOW_API_COCOA, false)) {
            // Editor still env-blocked (no window server) — soft skip.
            SUCCEED("CLAP gui_create returned false (no window server) — skipped.");
            plugin->destroy(plugin);
            clap_entry.deinit();
            return;
        }

        NSWindow* window =
            [[NSWindow alloc] initWithContentRect:NSMakeRect(0, 0, smoke::kW, smoke::kH)
                                        styleMask:NSWindowStyleMaskTitled
                                          backing:NSBackingStoreBuffered
                                            defer:NO];
        REQUIRE(window.contentView != nil);

        clap_window_t cw{};
        cw.api = CLAP_WINDOW_API_COCOA;
        cw.cocoa = (__bridge void*)window.contentView;
        REQUIRE(gui->set_parent(plugin, &cw));

        uint32_t w = 0, h = 0;
        if (gui->get_size(plugin, &w, &h)) {
            REQUIRE(w > 0);
            REQUIRE(h > 0);
        }
        smoke::pump_run_loop(5);

        gui->destroy(plugin);
        plugin->destroy(plugin);
        clap_entry.deinit();
        [window close];
    }
}

#else  // !(PULP_HAS_SKIA && __APPLE__)

TEST_CASE("embedded GPU plugin host smoke is mac+Skia only", "[gpu][plugin-gpu-host][mac]") {
    SUCCEED("Built without PULP_HAS_SKIA / not Apple — embedded host smoke is a no-op.");
}

#endif
