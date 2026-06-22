#include "threejs_demo.hpp"

#include <pulp/runtime/log.hpp>
#include <pulp/view/script_engine.hpp>
#include <pulp/view/scripted_ui.hpp>
#include <pulp/view/theme.hpp>
#include <pulp/view/threejs_resources.hpp>
#include <pulp/view/view.hpp>
#include <pulp/view/widget_bridge.hpp>
#include <pulp/view/widgets.hpp>

#include <choc/text/choc_JSON.h>

#include <fstream>
#include <optional>
#include <sstream>
#include <string>

#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif

#if defined(__APPLE__) && (TARGET_OS_IPHONE || TARGET_OS_SIMULATOR)
#import <Foundation/Foundation.h>
#endif

namespace pulp::examples::ios_threejs {

using namespace pulp::format;
using namespace pulp::state;
using namespace pulp::audio;
using namespace pulp::midi;

namespace {

constexpr const char* kFallbackPlaceholder =
    "// PulpThreeJsDemo fallback — bundle resources missing. The C++ side\n"
    "// emitted PULP_THREE_DEMO: ... markers explaining why. This script\n"
    "// just paints a visible \"Three.js not loaded\" message so the\n"
    "// editor pane isn't blank on device.\n"
    "(function () {\n"
    "    var msg = document.createElement('div');\n"
    "    msg.style.color = '#ff4444';\n"
    "    msg.style.fontSize = '20px';\n"
    "    msg.style.fontWeight = '700';\n"
    "    msg.style.padding = '24px';\n"
    "    msg.style.textAlign = 'center';\n"
    "    msg.textContent = 'Three.js not loaded — see PULP_THREE_DEMO log lines';\n"
    "    document.body.style.backgroundColor = '#1a0606';\n"
    "    document.body.appendChild(msg);\n"
    "})();\n";

#if defined(__APPLE__) && (TARGET_OS_IPHONE || TARGET_OS_SIMULATOR)

// Resolve a file inside the .appex's flat `threejs/` directory via
// NSBundle. Walks up from the `PulpAUViewController` class (which is
// the principal class of the AUv3 extension), matching the lookup
// strategy used by `pulp::view::threejs_iife_source()` so the resource
// layout stays consistent across loaders. Returns nullopt if the file
// isn't present — callers decide how loudly to complain.
std::optional<std::string> read_appex_threejs_resource(const char* basename,
                                                       const char* extension) {
    @autoreleasepool {
        Class principalClass = NSClassFromString(@"PulpAUViewController");
        NSBundle* bundle = principalClass ? [NSBundle bundleForClass:principalClass]
                                          : [NSBundle mainBundle];
        if (!bundle) return std::nullopt;

        NSString* nsBasename = [NSString stringWithUTF8String:basename];
        NSString* nsExtension = [NSString stringWithUTF8String:extension];

        NSString* path = [bundle pathForResource:nsBasename
                                          ofType:nsExtension
                                     inDirectory:@"threejs"];
        if (!path) {
            path = [bundle pathForResource:nsBasename ofType:nsExtension];
        }
        if (!path) return std::nullopt;

        NSError* err = nil;
        NSString* contents = [NSString stringWithContentsOfFile:path
                                                       encoding:NSUTF8StringEncoding
                                                          error:&err];
        if (!contents || err) return std::nullopt;
        const char* utf8 = [contents UTF8String];
        if (!utf8) return std::nullopt;
        return std::string{utf8};
    }
}

// Where to land the concatenated script on disk. NSTemporaryDirectory
// is per-process, app-extension-scoped, and survives just long enough
// for the ScriptedUiSession to load it (we delete on Processor
// destruction via on_view_closed). The filename embeds the
// processor's `this` pointer so concurrent instances of the same AU
// (split-view, multiple bus inserts) don't collide.
std::filesystem::path concatenated_script_path_for(const void* tag) {
    @autoreleasepool {
        NSString* tmp = NSTemporaryDirectory();
        if (!tmp) return {};
        std::ostringstream name;
        name << "pulp-threejs-demo-" << tag << ".js";
        std::string base{[tmp UTF8String]};
        return std::filesystem::path(base) / name.str();
    }
}

#else

// Non-iOS stub — the example only builds when IOS is set, so this
// branch never runs in production, but keeping it compiles cleanly
// guards the example from #ifdef rot if someone tries to build the
// folder on macOS for inspection.
std::optional<std::string> read_appex_threejs_resource(const char*, const char*) {
    return std::nullopt;
}
std::filesystem::path concatenated_script_path_for(const void*) { return {}; }

#endif  // __APPLE__ && (TARGET_OS_IPHONE || TARGET_OS_SIMULATOR)

bool write_text_file(const std::filesystem::path& path, const std::string& content) {
    std::ofstream out(path, std::ios::binary);
    if (!out.good()) return false;
    out.write(content.data(), static_cast<std::streamsize>(content.size()));
    return out.good();
}

}  // namespace

PluginDescriptor PulpThreeJsDemo::descriptor() const {
    PluginDescriptor d;
    d.name = "Pulp Three.js Demo";
    d.manufacturer = "Pulp";
    d.bundle_id = "com.pulp.examples.threejsdemo";
    d.version = "0.1.0";
    d.category = PluginCategory::Effect;
    d.accepts_midi = false;
    d.produces_midi = false;
    d.input_buses = {{"Main In", 2, false}};
    d.output_buses = {{"Main Out", 2, false}};
    return d;
}

void PulpThreeJsDemo::define_parameters(StateStore& /*store*/) {
    // No parameters — the demo is exclusively about the editor's JS-
    // driven WebGPU paint, not the audio path.
}

void PulpThreeJsDemo::prepare(const PrepareContext& /*ctx*/) {}

void PulpThreeJsDemo::process(BufferView<float>& out,
                              const BufferView<const float>& in,
                              MidiBuffer& /*midi_in*/,
                              MidiBuffer& /*midi_out*/,
                              const ProcessContext& ctx) {
    // Bit-perfect pass-through. Effect plug-ins with no DSP still must
    // not leave garbage in the output buffers, so we copy every
    // available input channel and zero-fill the rest.
    const int n = ctx.num_samples;
    const int out_chs = out.num_channels();
    const int in_chs = in.num_channels();
    for (int ch = 0; ch < out_chs; ++ch) {
        auto dst = out.channel(ch);
        if (ch < in_chs) {
            auto src = in.channel(ch);
            for (int i = 0; i < n; ++i) dst[i] = src[i];
        } else {
            for (int i = 0; i < n; ++i) dst[i] = 0.0f;
        }
    }
}

std::unique_ptr<pulp::view::View> PulpThreeJsDemo::create_view() {
    using pulp::view::FlexDirection;

    auto root = std::make_unique<pulp::view::View>();
    root->set_theme(pulp::view::Theme::dark());
    root->flex().direction = FlexDirection::column;
    // The scripted editor paints through Skia/Dawn — opt the view into
    // the GPU PluginViewHost so iOS routes through CAMetalLayer +
    // `core/render/src/gpu_surface_dawn.cpp` instead of the CG
    // fallback. Matches the `editor_ui.hpp` default editor wiring.
    root->set_requires_gpu_host(true);

    // ── 1. Read the bundled Three.js IIFE source ────────────────────
    // The iOS AUv3 bundler writes `<appex>/threejs/three.iife.js`. If
    // the build host didn't have Node.js (or the bundler step was
    // skipped) the source returns nullopt — fall through to the
    // placeholder script instead of crashing the AUv3 extension.
    auto iife_source = pulp::view::threejs_iife_source();
    std::string combined;
    if (iife_source) {
        runtime::log_info("PULP_THREE_DEMO: IIFE loaded ({} bytes)",
                          iife_source->size());
        combined.append(*iife_source);
        combined.push_back('\n');
    } else {
        runtime::log_error(
            "PULP_THREE_DEMO: three.iife.js missing from bundle — "
            "Three.js not loaded. Check that the .appex contains "
            "threejs/three.iife.js (the iOS-D.3b POST_BUILD step in "
            "tools/cmake/PulpAuv3.cmake needs Node.js + threejs "
            "FetchContent armed at configure time).");
    }

    // ── 2. Append the diagnostic shim (engine-agnostic) ─────────────
    // Pulp ships a tiny shim that prints PULP_THREE_SHIM markers so
    // device walk-throughs can verify the IIFE actually populated
    // `globalThis.THREE`. Bundled alongside `three.iife.js` by the
    // same POST_BUILD step.
    if (auto shim = read_appex_threejs_resource("web-compat-three-shim", "js")) {
        combined.append(*shim);
        combined.push_back('\n');
    } else {
        runtime::log_info(
            "PULP_THREE_DEMO: web-compat-three-shim.js not bundled "
            "(diagnostic markers will be missing but the demo still runs).");
    }

    // ── 3. Read the scene script bundled with this example ──────────
    if (auto scene = read_appex_threejs_resource("scene", "js")) {
        runtime::log_info("PULP_THREE_DEMO: scene script loaded ({} bytes)",
                          scene->size());
        combined.append(*scene);
        combined.push_back('\n');
    } else {
        runtime::log_error(
            "PULP_THREE_DEMO: scene.js missing from bundle — falling "
            "back to placeholder UI. Check that examples/ios-auv3-jsc-"
            "threejs/CMakeLists.txt's POST_BUILD copy step ran.");
        if (combined.empty() || !iife_source) {
            combined.assign(kFallbackPlaceholder);
        } else {
            // IIFE loaded but scene didn't — paint the placeholder
            // anyway so the failure mode is visible on device.
            combined.append(kFallbackPlaceholder);
        }
    }

    if (combined.empty()) {
        // Both IIFE and scene failed — bail to the placeholder so the
        // editor isn't blank on device.
        combined.assign(kFallbackPlaceholder);
    }

    // ── 4. Write the concatenated script to a per-instance tempfile ─
    // ScriptedUiSession reads from `script_path` on every load, so a
    // path-on-disk is the smallest-change integration. The path is
    // unique per Processor instance so split-view / multi-insert
    // hosts don't collide on it.
    concatenated_script_path_ = concatenated_script_path_for(this);
    if (concatenated_script_path_.empty()
            || !write_text_file(concatenated_script_path_, combined)) {
        runtime::log_error(
            "PULP_THREE_DEMO: could not write concatenated script to "
            "tempfile ({}) — falling back to a placeholder Label so "
            "the editor still renders something.",
            concatenated_script_path_.string());

        // Hard fallback: drop a Label child so the user sees an error
        // instead of an empty AUv3 editor pane.
        auto label = std::make_unique<pulp::view::Label>(
            "Three.js not loaded — see PULP_THREE_DEMO log lines");
        label->set_font_size(20.0f);
        label->set_font_weight(700);
        label->set_text_color(pulp::canvas::Color::rgba(1.0f, 0.27f, 0.27f));
        root->add_child(std::move(label));
        return root;
    }

    // ── 5. Build the ScriptedUiSession + load ───────────────────────
    pulp::view::ScriptedUiOptions options;
    options.script_path = concatenated_script_path_;
    options.enable_hot_reload = false;       // tempfile changes after process exit only
    options.enable_theme_reload = false;     // ditto for theme.json
    scripted_ui_ = std::make_unique<pulp::view::ScriptedUiSession>(
        *root, state(), std::move(options));

    std::string load_error;
    if (!scripted_ui_->load(&load_error)) {
        runtime::log_error(
            "PULP_THREE_DEMO: ScriptedUiSession::load failed ({}). "
            "Editor will fall back to a Label so the failure is "
            "visible on device.",
            load_error);
        scripted_ui_.reset();
        // Same hard fallback path as the tempfile-write failure.
        auto label = std::make_unique<pulp::view::Label>(
            "Three.js script failed — see PULP_THREE_DEMO log lines");
        label->set_font_size(20.0f);
        label->set_font_weight(700);
        label->set_text_color(pulp::canvas::Color::rgba(1.0f, 0.27f, 0.27f));
        root->add_child(std::move(label));
    } else {
        runtime::log_info(
            "PULP_THREE_DEMO: ScriptedUiSession loaded from {} "
            "(concatenated IIFE + shim + scene)",
            concatenated_script_path_.string());
    }
    return root;
}

void PulpThreeJsDemo::on_view_closed(pulp::view::View& /*view*/) {
    // Tear down the session BEFORE deleting the tempfile so the
    // bridge's destructor doesn't race a reload against a missing
    // path (hot reload is disabled, but the contract is cheap to
    // honor and protects future agents who flip the option).
    scripted_ui_.reset();

    if (!concatenated_script_path_.empty()) {
        std::error_code ec;
        std::filesystem::remove(concatenated_script_path_, ec);
        concatenated_script_path_.clear();
    }
}

}  // namespace pulp::examples::ios_threejs
