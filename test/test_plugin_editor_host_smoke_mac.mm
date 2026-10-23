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
#include <pulp/view/screenshot.hpp>
#include <pulp/view/view.hpp>
#include <pulp/view/widgets.hpp>
#include <pulp/view/audio_widgets.hpp>
#include <pulp/view/value_source.hpp>
#include <pulp/canvas/canvas.hpp>

#include <atomic>
#include <objc/runtime.h>
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
    // A fixed-aspect design editor: declaring aspect_ratio (> 0) opts into the
    // aspect-lock resize contract (clap_entry.hpp `free_resize = resizable &&
    // aspect_ratio <= 0`), matching what view_size_from_design derives for a
    // real design-imported plugin. Without it a resizable editor drags freely.
    format::ViewSize view_size() const override {
        return {kW, kH, 320, 240, 1024, 768, static_cast<double>(kW) / static_cast<double>(kH)};
    }
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

// Minimal non-blank CPU content tree — no Processor/ViewBridge needed since the
// CPU reopen test below drives PluginViewHost directly. Mirrors
// GpuEditorProcessor::create_view()'s look-and-feel (dark bg + a label) without
// requires_gpu_host(), so PluginViewHost::create(..., {use_gpu=false}) takes
// the deterministic CoreGraphics CPU path rather than falling back from a
// failed GPU init.
inline std::unique_ptr<view::View> make_cpu_content_view() {
    auto root = std::make_unique<view::View>();
    root->set_background_color(canvas::Color::rgba8(30, 30, 46));
    root->flex().direction = view::FlexDirection::column;
    root->flex().padding = 16;
    auto l = std::make_unique<view::Label>("REOPEN");
    l->set_font_size(20.0f);
    l->flex().preferred_height = 28;
    root->add_child(std::move(l));
    return root;
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
            SKIP("No Cocoa window available — embedded host smoke skipped.");
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

        host->set_idle_callback(format::make_editor_idle_pump(bridge));
        // Attach seam: a real parent must report attached so a foreign embedder
        // knows it is safe to fire notify_attached().
        REQUIRE_FALSE(host->is_attached());
        REQUIRE(host->try_attach_to_parent((__bridge void*)window.contentView));
        REQUIRE(host->is_attached());
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
        REQUIRE_FALSE(host->is_attached());
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

TEST_CASE("CLAP gui resize negotiation snaps to design aspect (mac)",
          "[gpu][skia][plugin-gpu-host][mac][clap][resize]") {
    // Drives the Phase 3 resize-negotiation path end to end through the
    // CLAP entry: gui_can_resize, gui_get_resize_hints (preserve aspect),
    // gui_adjust_size (snap to design aspect), then create+set_parent+
    // set_size on a hidden NSWindow + capture. A second set_size lands on
    // a different rect — the captured PNG after the second resize differs
    // in size from the first, proving the surface tracks gui_set_size and
    // the design viewport scales content rather than re-laying it out.
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

        // ── Pre-create assertions (resize hints work without a window) ──
        REQUIRE(gui->can_resize(plugin));

        if (!gui->is_api_supported(plugin, CLAP_WINDOW_API_COCOA, false)) {
            SUCCEED("Cocoa GUI API not supported — CLAP resize smoke skipped.");
            plugin->destroy(plugin);
            clap_entry.deinit();
            return;
        }

        if (!gui->create(plugin, CLAP_WINDOW_API_COCOA, false)) {
            SUCCEED("CLAP gui_create returned false (no window server) — skipped.");
            plugin->destroy(plugin);
            clap_entry.deinit();
            return;
        }

        // ── Resize hints expose design aspect ─────────────────────────
        clap_gui_resize_hints_t hints{};
        REQUIRE(gui->get_resize_hints(plugin, &hints));
        REQUIRE(hints.can_resize_horizontally);
        REQUIRE(hints.can_resize_vertically);
        REQUIRE(hints.preserve_aspect_ratio);
        REQUIRE(hints.aspect_ratio_width == smoke::kW);
        REQUIRE(hints.aspect_ratio_height == smoke::kH);

        // ── adjust_size snaps a non-aspect rect to design aspect ──────
        const double design_aspect =
            static_cast<double>(smoke::kW) / static_cast<double>(smoke::kH);
        {
            // Request a much wider rect than the design aspect — width
            // should shrink to height * design_aspect.
            uint32_t adj_w = smoke::kW * 4;
            uint32_t adj_h = smoke::kH;
            REQUIRE(gui->adjust_size(plugin, &adj_w, &adj_h));
            const double adj_aspect =
                static_cast<double>(adj_w) / static_cast<double>(adj_h);
            REQUIRE(std::abs(adj_aspect - design_aspect) < 0.01);
        }
        {
            // Request a much taller rect — height should shrink to
            // width / design_aspect.
            uint32_t adj_w = smoke::kW;
            uint32_t adj_h = smoke::kH * 4;
            REQUIRE(gui->adjust_size(plugin, &adj_w, &adj_h));
            const double adj_aspect =
                static_cast<double>(adj_w) / static_cast<double>(adj_h);
            REQUIRE(std::abs(adj_aspect - design_aspect) < 0.01);
        }

        // ── Attach to a hidden window so capture_back_buffer_png can
        //    acquire a drawable. Skip if no window server is available. ─
        NSWindow* window =
            [[NSWindow alloc] initWithContentRect:NSMakeRect(0, 0, smoke::kW, smoke::kH)
                                        styleMask:NSWindowStyleMaskBorderless
                                          backing:NSBackingStoreBuffered
                                            defer:NO];
        if (!window || !window.contentView) {
            SUCCEED("No Cocoa window — CLAP resize+capture smoke skipped.");
            gui->destroy(plugin);
            plugin->destroy(plugin);
            clap_entry.deinit();
            return;
        }

        clap_window_t cw{};
        cw.api = CLAP_WINDOW_API_COCOA;
        cw.cocoa = (__bridge void*)window.contentView;
        REQUIRE(gui->set_parent(plugin, &cw));
        REQUIRE(gui->set_size(plugin, smoke::kW, smoke::kH));
        smoke::pump_run_loop(5);

        // Reach into the CLAP instance for the editor host so we can
        // capture the back buffer and inspect the host-side size — proof
        // that gui_set_size threaded through to host->set_size.
        auto* clap_inst = static_cast<pulp::format::clap_adapter::PulpClapPlugin*>(plugin->plugin_data);
        REQUIRE(clap_inst != nullptr);
        REQUIRE(clap_inst->editor_host != nullptr);

        if (!clap_inst->editor_host->is_gpu_backed()) {
            // GPU host fell back to CPU (no Dawn adapter) — capture path
            // is GPU-only; the structural assertions above still ran.
            SUCCEED("No GPU adapter — CLAP capture-resize smoke partial-skip.");
            gui->destroy(plugin);
            plugin->destroy(plugin);
            clap_entry.deinit();
            [window close];
            return;
        }

        // First capture at design size — proves the design viewport set
        // in gui_create is active and the capture path works.
        auto png_design = clap_inst->editor_host->capture_back_buffer_png();
        REQUIRE_FALSE(png_design.empty());
        REQUIRE(smoke::looks_like_png(png_design));

        // Resize to 2x design — the design viewport scales content; the
        // back buffer changes physical dimensions.
        REQUIRE(gui->set_size(plugin, smoke::kW * 2, smoke::kH * 2));
        smoke::pump_run_loop(3);
        const auto host_size = clap_inst->editor_host->get_size();
        REQUIRE(host_size.width == smoke::kW * 2);
        REQUIRE(host_size.height == smoke::kH * 2);

        auto png_2x = clap_inst->editor_host->capture_back_buffer_png();
        REQUIRE_FALSE(png_2x.empty());
        REQUIRE(smoke::looks_like_png(png_2x));
        // PNGs at different surface sizes encode different byte streams
        // (size is in the IHDR chunk). If both byte streams were
        // identical, gui_set_size never reached the host — different
        // bytes prove the wire works end to end.
        REQUIRE(png_design.size() != png_2x.size());

        gui->destroy(plugin);
        plugin->destroy(plugin);
        clap_entry.deinit();
        [window close];
    }
}

TEST_CASE("mac CPU plugin host reopen survives a display-link teardown race",
          "[gpu][skia][plugin-gpu-host][mac][display-link][reopen]") {
    // Regression coverage for the reopen path around MacPluginViewHost's
    // CVDisplayLink teardown (plugin_view_host_mac.mm ~L1138-1274): the
    // destructor flips link_state_->alive BEFORE stop_render_link() so a
    // main-queue block already dispatched from a vsync tick becomes a no-op
    // instead of touching freed host state, and a fresh host reopened on the
    // SAME native window/view keeps painting afterward. The production code
    // is already UAF-safe (the shared FrameLink token); this test proves it
    // stays that way and never post-teardown-fires the idle callback.
    smoke::clear_no_editor_env();
    @autoreleasepool {
        NSWindow* window =
            [[NSWindow alloc] initWithContentRect:NSMakeRect(0, 0, smoke::kW, smoke::kH)
                                        styleMask:NSWindowStyleMaskTitled
                                          backing:NSBackingStoreBuffered
                                            defer:NO];
        if (!window || !window.contentView) {
            SKIP("No Cocoa window available — mac CPU reopen smoke skipped.");
        }

        view::PluginViewHost::Options opts;
        opts.size = {smoke::kW, smoke::kH};
        opts.use_gpu = false;  // deterministic CoreGraphics CPU host — see create().

        auto root1 = smoke::make_cpu_content_view();
        auto host = view::PluginViewHost::create(*root1, opts);
        REQUIRE(host != nullptr);
        REQUIRE_FALSE(host->is_gpu_backed());

        auto counter = std::make_shared<std::atomic<int>>(0);
        // Capture the shared_ptr by value (production liveness style) — never
        // a raw pointer into this test's stack, so the callback is safe to
        // outlive the frame it was armed on.
        host->set_idle_callback([counter]() {
            counter->fetch_add(1, std::memory_order_relaxed);
        });

        REQUIRE_FALSE(host->is_attached());
        REQUIRE(host->try_attach_to_parent((__bridge void*)window.contentView));
        REQUIRE(host->is_attached());

        // Bounded pump: on a headless runner without an active CGDisplay,
        // CVDisplayLinkCreateWithActiveCGDisplays never produces a usable
        // link and the idle callback never fires — soft-skip rather than
        // spin. ~200 frames at pump_run_loop's 20ms wait is a ~4s budget,
        // comfortably above one vsync interval (~16.7ms @ 60Hz) when a real
        // display link is available.
        constexpr int kMaxPumpFrames = 200;
        int frames = 0;
        while (counter->load(std::memory_order_relaxed) == 0 && frames < kMaxPumpFrames) {
            smoke::pump_run_loop(1);
            ++frames;
        }
        if (counter->load(std::memory_order_relaxed) == 0) {
            SUCCEED("No display link fired (headless runner, no active CGDisplay) — "
                     "mac CPU reopen smoke skipped.");
            host->detach();
            host.reset();
            [window close];
            return;
        }

        const int count_before_teardown = counter->load(std::memory_order_relaxed);

        // The race under test: destroy the host WITHOUT detaching first. A
        // vsync tick may already have a main-queue block in flight; the
        // destructor's alive.store(false) (before stop_render_link()) must
        // make that block bail out instead of touching freed `this`.
        host.reset();
        smoke::pump_run_loop(10);
        REQUIRE(counter->load(std::memory_order_relaxed) == count_before_teardown);

        // Reopen: a fresh host attaches to the SAME native window content
        // view and must resume driving idle callbacks + painting — proving
        // the teardown race didn't leave the window/view hierarchy wedged
        // against a subsequent open.
        auto root2 = smoke::make_cpu_content_view();
        auto host2 = view::PluginViewHost::create(*root2, opts);
        REQUIRE(host2 != nullptr);

        auto counter2 = std::make_shared<std::atomic<int>>(0);
        host2->set_idle_callback([counter2]() {
            counter2->fetch_add(1, std::memory_order_relaxed);
        });

        REQUIRE_FALSE(host2->is_attached());
        REQUIRE(host2->try_attach_to_parent((__bridge void*)window.contentView));
        REQUIRE(host2->is_attached());

        frames = 0;
        while (counter2->load(std::memory_order_relaxed) == 0 && frames < kMaxPumpFrames) {
            smoke::pump_run_loop(1);
            ++frames;
        }
        REQUIRE(counter2->load(std::memory_order_relaxed) > 0);

        // Host-independent raster proof that the reopened view tree actually
        // paints: capture_back_buffer_png() is a GPU-host-only override (the
        // CPU host inherits the base class's empty-vector default), so use
        // the offscreen Skia raster path over the same root view instead.
        auto png = view::render_to_png(*root2, smoke::kW, smoke::kH, 2.0f,
                                        view::ScreenshotBackend::skia);
        INFO("reopened CPU host raster capture bytes: " << png.size());
        REQUIRE_FALSE(png.empty());
        REQUIRE(smoke::looks_like_png(png));

        host2->detach();
        host2.reset();
        [window close];
    }
}

namespace smoke {

/// Counts every paint the host drives through the tree.
class PaintCountingView : public view::View {
  public:
    void paint(canvas::Canvas&) override {
        paints.fetch_add(1, std::memory_order_relaxed);
    }
    std::atomic<int> paints{0};
};

} // namespace smoke

TEST_CASE("a silent source-bound meter editor stops painting on both mac plugin hosts",
          "[gpu][skia][plugin-gpu-host][mac][display-link][idle-gate]") {
    // A bound meter is a FrameClock subscriber for as long as it is bound. The
    // hosts used to composite the whole editor every vsync while any subscriber
    // was live, so a static editor with a silent meter repainted at the display
    // rate forever. The clock must keep ticking (the meter keeps reading its
    // source) while no frame is painted, and a real level must still paint.
    smoke::clear_no_editor_env();
    for (const bool gpu : {false, true}) {
        DYNAMIC_SECTION((gpu ? "GPU" : "CPU") << " host") {
            @autoreleasepool {
                NSWindow* window =
                    [[NSWindow alloc] initWithContentRect:NSMakeRect(0, 0, smoke::kW, smoke::kH)
                                                styleMask:NSWindowStyleMaskTitled
                                                  backing:NSBackingStoreBuffered
                                                    defer:NO];
                if (!window || !window.contentView)
                    SKIP("No Cocoa window available.");

                auto root = std::make_unique<view::View>();
                if (gpu)
                    root->set_requires_gpu_host(true);
                auto counting = std::make_unique<smoke::PaintCountingView>();
                auto* counter = counting.get();
                counter->set_bounds({0, 0, 40, 40});
                root->add_child(std::move(counting));
                auto meter = std::make_unique<view::Meter>();
                view::Meter* m = meter.get();
                m->set_bounds({60, 10, 20, 200});
                root->add_child(std::move(meter));
                auto source = std::make_shared<view::MeterSource>();
                view::MeterFrame silent;
                silent.channels = 1;
                source->publish(silent);
                m->set_source(source, 0);

                view::PluginViewHost::Options opts;
                opts.size = {smoke::kW, smoke::kH};
                opts.use_gpu = gpu;
                auto host = view::PluginViewHost::create(*root, opts);
                REQUIRE(host != nullptr);
                if (gpu && !host->is_gpu_backed()) {
                    host.reset();
                    [window close];
                    SKIP("GPU plugin host unavailable on this runner.");
                }
                auto ticks = std::make_shared<std::atomic<int>>(0);
                host->set_idle_callback([ticks]() { ticks->fetch_add(1); });
                REQUIRE(host->try_attach_to_parent((__bridge void*)window.contentView));

                int frames = 0;
                while (ticks->load() == 0 && frames < 200) {
                    smoke::pump_run_loop(1);
                    ++frames;
                }
                if (ticks->load() == 0) {
                    host->detach();
                    host.reset();
                    [window close];
                    SKIP("No display link fired (no active CGDisplay).");
                }
                // Let the first frame(s) land, then measure a quiet window.
                smoke::pump_run_loop(10);
                const int paints_before = counter->paints.load();
                const int ticks_before = ticks->load();
                smoke::pump_run_loop(25); // ~0.5 s
                const int quiet_ticks = ticks->load() - ticks_before;
                const int quiet_paints = counter->paints.load() - paints_before;
                INFO("ticks during the quiet window: " << quiet_ticks
                                                       << ", paints: " << quiet_paints);
                REQUIRE(quiet_ticks >= 5); // the editor is live and being pumped
                CHECK(quiet_paints == 0);

                // Control: a real level must paint.
                view::MeterFrame loud;
                loud.channels = 1;
                loud.peak[0] = 0.8f;
                loud.rms[0] = 0.5f;
                source->publish(loud);
                smoke::pump_run_loop(10);
                CHECK(counter->paints.load() > paints_before + quiet_paints);

                host->detach();
                host.reset();
                root.reset();
                [window close];
            }
        }
    }
}

namespace smoke {

/// Records the region AppKit is redrawing (-getRectsBeingDrawn:count: on the
/// host's NSView, valid only inside its drawRect:) while the CPU host paints
/// the tree.
class ClipProbeView : public view::View {
  public:
    void paint(canvas::Canvas&) override {
        if (!host_view)
            return;
        const NSRect* rects = nullptr;
        NSInteger count = 0;
        [host_view getRectsBeingDrawn:&rects count:&count];
        NSRect u = NSZeroRect;
        for (NSInteger i = 0; i < count; ++i)
            u = NSUnionRect(u, rects[i]);
        last_clip = u;
        ++paints;
    }
    NSView* __unsafe_unretained host_view = nil;
    NSRect last_clip = NSZeroRect;
    int paints = 0;
};

} // namespace smoke

TEST_CASE("the mac CPU plugin host redraws only a bounded repaint's rect",
          "[plugin-host][mac][display-link][partial-repaint]") {
    // A knob value tick asks for its own box (request_repaint_self). The CPU
    // host used to answer every request with -setNeedsDisplay:YES, so AppKit
    // re-rasterized the whole editor for one knob or one meter; now the rect
    // reaches -setNeedsDisplayInRect: and only that region is redrawn.
    smoke::clear_no_editor_env();
    @autoreleasepool {
        NSWindow* window =
            [[NSWindow alloc] initWithContentRect:NSMakeRect(0, 0, smoke::kW, smoke::kH)
                                        styleMask:NSWindowStyleMaskTitled
                                          backing:NSBackingStoreBuffered
                                            defer:NO];
        if (!window || !window.contentView)
            SKIP("No Cocoa window available.");

        auto root = std::make_unique<view::View>();
        auto probe_owned = std::make_unique<smoke::ClipProbeView>();
        auto* probe = probe_owned.get();
        probe_owned->set_position(view::View::Position::absolute);
        probe_owned->flex().preferred_width = static_cast<float>(smoke::kW);
        probe_owned->flex().preferred_height = static_cast<float>(smoke::kH);
        root->add_child(std::move(probe_owned));
        auto knob_owned = std::make_unique<view::Knob>();
        auto* knob = knob_owned.get();
        knob_owned->set_position(view::View::Position::absolute);
        knob_owned->set_left(20);
        knob_owned->set_top(20);
        knob_owned->flex().preferred_width = 64;
        knob_owned->flex().preferred_height = 64;
        root->add_child(std::move(knob_owned));

        view::PluginViewHost::Options opts;
        opts.size = {smoke::kW, smoke::kH};
        opts.use_gpu = false;
        auto host = view::PluginViewHost::create(*root, opts);
        REQUIRE(host != nullptr);
        REQUIRE_FALSE(host->is_gpu_backed());
        REQUIRE(host->try_attach_to_parent((__bridge void*)window.contentView));
        probe->host_view = (__bridge NSView*)host->native_handle();

        int frames = 0;
        while (probe->paints == 0 && frames < 200) {
            smoke::pump_run_loop(1);
            ++frames;
        }
        if (probe->paints == 0) {
            host->detach();
            host.reset();
            [window close];
            SKIP("The CPU host never painted (no display link / window server).");
        }
        smoke::pump_run_loop(5);

        // A bounded request: only the knob's box (plus halo) is redrawn.
        const int before = probe->paints;
        knob->set_value(knob->value() > 0.5f ? 0.1f : 0.9f);
        smoke::pump_run_loop(5);
        REQUIRE(probe->paints > before);
        INFO("bounded clip: " << probe->last_clip.size.width << "x"
                              << probe->last_clip.size.height);
        CHECK(probe->last_clip.size.width <= 64 + 16);
        CHECK(probe->last_clip.size.height <= 64 + 16);
        CHECK(probe->last_clip.size.width >= 64);

        // Control: a rect-less request still redraws the whole editor.
        const int before_full = probe->paints;
        root->request_repaint();
        smoke::pump_run_loop(5);
        REQUIRE(probe->paints > before_full);
        CHECK(probe->last_clip.size.width >= smoke::kW);
        CHECK(probe->last_clip.size.height >= smoke::kH);

        // A bounded request made from the idle half of a tick (host automation
        // reaching the editor through the store listeners) stays bounded too.
        auto from_idle = std::make_shared<std::atomic<bool>>(false);
        host->set_idle_callback([from_idle, knob]() {
            if (from_idle->exchange(false))
                knob->set_value(knob->value() > 0.5f ? 0.15f : 0.85f);
        });
        smoke::pump_run_loop(3);
        const int before_idle = probe->paints;
        from_idle->store(true);
        smoke::pump_run_loop(5);
        if (from_idle->load()) {
            // The idle half only runs from the display link, which does not
            // fire while the display sleeps.
            host->set_idle_callback({});
            host->detach();
            host.reset();
            root.reset();
            [window close];
            SKIP("No display link fired; the idle-half request was not exercised.");
        } else {
            REQUIRE(probe->paints > before_idle);
            INFO("idle-half clip: " << probe->last_clip.size.width << "x"
                                    << probe->last_clip.size.height);
            CHECK(probe->last_clip.size.width <= 64 + 16);
            CHECK(probe->last_clip.size.height <= 64 + 16);
        }
        host->set_idle_callback({});

        host->detach();
        host.reset();
        root.reset();
        [window close];
    }
}

TEST_CASE("the mac CPU plugin host draws at the display rate, not the request rate",
          "[plugin-host][mac][display-link][partial-repaint]") {
    // Host automation and pointer drags arrive faster than the display
    // refreshes. Each request used to mark the NSView at once, and AppKit
    // re-rasterized at the end of every run-loop turn that carried one; the
    // CPU host now holds requests for its display-link tick.
    smoke::clear_no_editor_env();
    @autoreleasepool {
        NSWindow* window =
            [[NSWindow alloc] initWithContentRect:NSMakeRect(0, 0, smoke::kW, smoke::kH)
                                        styleMask:NSWindowStyleMaskTitled
                                          backing:NSBackingStoreBuffered
                                            defer:NO];
        if (!window || !window.contentView)
            SKIP("No Cocoa window available.");

        auto root = std::make_unique<view::View>();
        auto counting = std::make_unique<smoke::PaintCountingView>();
        auto* counter = counting.get();
        counter->set_bounds({0, 0, 40, 40});
        root->add_child(std::move(counting));

        view::PluginViewHost::Options opts;
        opts.size = {smoke::kW, smoke::kH};
        opts.use_gpu = false;
        auto host = view::PluginViewHost::create(*root, opts);
        REQUIRE(host != nullptr);
        auto ticks = std::make_shared<std::atomic<int>>(0);
        host->set_idle_callback([ticks]() { ticks->fetch_add(1); });
        REQUIRE(host->try_attach_to_parent((__bridge void*)window.contentView));
        int frames = 0;
        while (ticks->load() == 0 && frames < 200) {
            smoke::pump_run_loop(1);
            ++frames;
        }
        if (ticks->load() == 0) {
            host->detach();
            host.reset();
            [window close];
            SKIP("No display link fired (no active CGDisplay).");
        }
        smoke::pump_run_loop(5);

        // 40 requests, one per ~1 ms run-loop turn: about 40 ms, a handful of
        // vsyncs at any refresh rate.
        const int before = counter->paints.load();
        const int ticks_before = ticks->load();
        for (int i = 0; i < 40; ++i) {
            root->request_repaint();
            @autoreleasepool {
                [[NSRunLoop currentRunLoop] runMode:NSDefaultRunLoopMode
                                         beforeDate:[NSDate dateWithTimeIntervalSinceNow:0.001]];
            }
        }
        smoke::pump_run_loop(3);
        const int paints = counter->paints.load() - before;
        const int link_ticks = ticks->load() - ticks_before;
        INFO("paints: " << paints << " for 40 requests over " << link_ticks << " link ticks");
        CHECK(paints >= 1);
        CHECK(paints <= link_ticks + 1);
        CHECK(paints < 20);

        host->detach();
        host.reset();
        root.reset();
        [window close];
    }
}

TEST_CASE("a knob drag on the mac CPU plugin host redraws the knob once per frame",
          "[plugin-host][mac][display-link][partial-repaint][drag]") {
    // Pointer motion is held for the next display-link frame. The held run used
    // to mark the whole NSView, so AppKit redrew the full editor mid-frame and
    // again when the frame delivered the motion; now only the gate opens and
    // the knob's own bounded repaint is drawn, once per frame.
    smoke::clear_no_editor_env();
    @autoreleasepool {
        NSWindow* window =
            [[NSWindow alloc] initWithContentRect:NSMakeRect(0, 0, smoke::kW, smoke::kH)
                                        styleMask:NSWindowStyleMaskTitled
                                          backing:NSBackingStoreBuffered
                                            defer:NO];
        if (!window || !window.contentView)
            SKIP("No Cocoa window available.");

        auto root = std::make_unique<view::View>();
        auto probe_owned = std::make_unique<smoke::ClipProbeView>();
        auto* probe = probe_owned.get();
        probe_owned->set_position(view::View::Position::absolute);
        probe_owned->flex().preferred_width = static_cast<float>(smoke::kW);
        probe_owned->flex().preferred_height = static_cast<float>(smoke::kH);
        probe_owned->set_hit_testable(false);
        root->add_child(std::move(probe_owned));
        auto knob_owned = std::make_unique<view::Knob>();
        auto* knob = knob_owned.get();
        knob_owned->set_position(view::View::Position::absolute);
        knob_owned->set_left(20);
        knob_owned->set_top(20);
        knob_owned->flex().preferred_width = 64;
        knob_owned->flex().preferred_height = 64;
        root->add_child(std::move(knob_owned));

        view::PluginViewHost::Options opts;
        opts.size = {smoke::kW, smoke::kH};
        opts.use_gpu = false;
        auto host = view::PluginViewHost::create(*root, opts);
        REQUIRE(host != nullptr);
        auto ticks = std::make_shared<std::atomic<int>>(0);
        host->set_idle_callback([ticks]() { ticks->fetch_add(1); });
        REQUIRE(host->try_attach_to_parent((__bridge void*)window.contentView));
        NSView* hv = (__bridge NSView*)host->native_handle();
        probe->host_view = hv;
        int frames = 0;
        while (ticks->load() == 0 && frames < 200) {
            smoke::pump_run_loop(1);
            ++frames;
        }
        if (ticks->load() == 0) {
            host->detach();
            host.reset();
            [window close];
            SKIP("No display link fired (no active CGDisplay).");
        }
        smoke::pump_run_loop(5);

        // The knob's centre, in window coordinates (the view is unflipped).
        const NSPoint centre = [hv convertPoint:NSMakePoint(52, smoke::kH - 52) toView:nil];
        auto mouse = [&](NSEventType type, NSPoint p) {
            return [NSEvent mouseEventWithType:type
                                      location:p
                                 modifierFlags:0
                                     timestamp:[[NSProcessInfo processInfo] systemUptime]
                                  windowNumber:window.windowNumber
                                       context:nil
                                   eventNumber:0
                                    clickCount:1
                                      pressure:1.0];
        };
        const float before_value = knob->value();
        [hv mouseDown:mouse(NSEventTypeLeftMouseDown, centre)];
        smoke::pump_run_loop(2);
        const int paints_before = probe->paints;
        const int ticks_before = ticks->load();
        for (int i = 1; i <= 40; ++i) {
            [hv mouseDragged:mouse(NSEventTypeLeftMouseDragged,
                                   NSMakePoint(centre.x, centre.y + i))];
            @autoreleasepool {
                [[NSRunLoop currentRunLoop] runMode:NSDefaultRunLoopMode
                                         beforeDate:[NSDate dateWithTimeIntervalSinceNow:0.001]];
            }
        }
        smoke::pump_run_loop(3);
        const int paints = probe->paints - paints_before;
        const int link_ticks = ticks->load() - ticks_before;
        const NSRect clip = probe->last_clip;
        [hv mouseUp:mouse(NSEventTypeLeftMouseUp, NSMakePoint(centre.x, centre.y + 40))];
        smoke::pump_run_loop(2);

        INFO("paints " << paints << " over " << link_ticks << " ticks; last clip "
                       << clip.size.width << "x" << clip.size.height);
        REQUIRE(knob->value() != before_value); // the drag reached the knob
        CHECK(paints >= 1);
        CHECK(paints <= link_ticks + 1);
        CHECK(clip.size.width <= 64 + 16);
        CHECK(clip.size.height <= 64 + 16);

        host->detach();
        host.reset();
        root.reset();
        [window close];
    }
}

#else  // !(PULP_HAS_SKIA && __APPLE__)

TEST_CASE("embedded GPU plugin host smoke is mac+Skia only", "[gpu][plugin-gpu-host][mac]") {
    SUCCEED("Built without PULP_HAS_SKIA / not Apple — embedded host smoke is a no-op.");
}

#endif
