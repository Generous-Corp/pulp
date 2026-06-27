// pulp-screenshot — Render a built-in demo or scripted UI to PNG for visual validation
// Used by: kit verification, MCP server, CI pipelines

#include <pulp/view/view.hpp>
#include <pulp/view/widgets.hpp>
#include <pulp/view/theme.hpp>
#include <pulp/view/script_engine.hpp>
#include <pulp/view/widget_bridge.hpp>
#include <pulp/view/screenshot.hpp>
#include <pulp/view/screenshot_compare.hpp>
#include <pulp/state/store.hpp>
#include <pulp/view/viewport_reconcile.hpp>
#include <iostream>
#include <fstream>
#include <string>
#include <cstdlib>

using namespace pulp::view;
using namespace pulp::state;

// Viewport reconciliation for runtime-imported content.
//
// Imports (Spectr, v0.dev, Stitch, Figma exports) routinely ship a
// top-level container with literal-CSS hardcoded dimensions that
// exceed the screenshot viewport. Canonical Spectr case
// (`spectr-editor-extracted.js`):
//   `<div style={{ position:'absolute', top:0, left:0,
//                  width:1320, height:860, … }}>`
// In a 1280×800 viewport, the App anchors to (0,0) and overflows by
// 40×60 — `bottom:0`-anchored chrome (action rail, frequency-axis
// labels) lands entirely off-screen.
//
// In a real browser the same content renders inside the same
// viewport, because the editor.html body uses
//   `display:flex; align-items:center; justify-content:center;
//    min-height:100vh`.
// With `flex-shrink: 1` (default), the App flex item shrinks on its
// main axis to fit the body's content box — so a 1320×860 child in a
// 1280×800 body lands at 1280×800 with internal layout proportionally
// compressed. All bottom-anchored chrome ends up in frame; the top
// bar at `top:0` stays at `top:0`; the rail at `bottom:0` stays at
// `bottom:0` of the now-fitted container.
//
// The recursive subtree-clamp implementation lives in
// `viewport_reconcile.hpp` so unit tests can exercise it without
// linking the screenshot CLI binary. See that header for the full
// design rationale and the dom-adapter background.

static void print_usage() {
    std::cerr << "Usage: pulp-screenshot [options]\n";
    std::cerr << "  --script <file.js>   JS UI script to render\n";
    std::cerr << "  --output <file.png>  Output PNG path (default: screenshot.png)\n";
    std::cerr << "  --width <px>         Width in points (default: 400)\n";
    std::cerr << "  --height <px>        Height in points (default: 300)\n";
    std::cerr << "  --scale <factor>     Scale factor (default: 2.0)\n";
    std::cerr << "  --theme <name>       Theme: dark, light, pro_audio (default: dark)\n";
    std::cerr << "  --backend <name>     Render backend: auto, skia, coregraphics, gpu (default: auto — smart: native-overlay refuse / GPU view / raster)\n";
    std::cerr << "  --runtime-trace <file.json>\n";
    std::cerr << "                       Dump JS listener/callback trace after settle\n";
    std::cerr << "  --base64             Output base64-encoded PNG to stdout\n";
    std::cerr << "  --demo               Render a demo UI (no script needed)\n";
    std::cerr << "  --compare A.png B.png [--threshold 0.85] [--diff D.png]  Parity check: print similarity, exit 0 if >= threshold\n";
}

static std::string read_file(const std::string& path) {
    std::ifstream f(path);
    if (!f.is_open()) return {};
    return std::string(std::istreambuf_iterator<char>(f), {});
}

static bool write_text_file(const std::string& path, const std::string& text) {
    std::ofstream f(path, std::ios::binary);
    if (!f.is_open()) return false;
    f << text;
    return f.good();
}

static std::string base64_encode(const std::vector<uint8_t>& data) {
    static const char* table = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve((data.size() + 2) / 3 * 4);

    for (size_t i = 0; i < data.size(); i += 3) {
        uint32_t n = static_cast<uint32_t>(data[i]) << 16;
        if (i + 1 < data.size()) n |= static_cast<uint32_t>(data[i + 1]) << 8;
        if (i + 2 < data.size()) n |= static_cast<uint32_t>(data[i + 2]);

        out += table[(n >> 18) & 0x3F];
        out += table[(n >> 12) & 0x3F];
        out += (i + 1 < data.size()) ? table[(n >> 6) & 0x3F] : '=';
        out += (i + 2 < data.size()) ? table[n & 0x3F] : '=';
    }
    return out;
}

struct ScreenshotCliOptions {
    std::string script_path;
    std::string output_path = "screenshot.png";
    uint32_t width = 400;
    uint32_t height = 300;
    float scale = 2.0f;
    std::string theme_name = "dark";
#ifdef PULP_HAS_SKIA
    std::string backend_name = "auto";  // smart dispatch (capture_view): native-overlay
                                        // refuse / GPU-required → gpu / else raster
#else
    std::string backend_name = "coregraphics";
#endif
    std::string runtime_trace_path;
    bool backend_was_defaulted = true;
    bool output_base64 = false;
    bool demo = false;
    bool help = false;
};

static ScreenshotCliOptions parse_options(int argc, char* argv[]) {
    ScreenshotCliOptions options;
    // Short-circuit on --help / -h BEFORE running the option loop. The
    // option loop calls std::stoi / std::stof on `--width`, `--height`,
    // `--scale` arguments, which throw on malformed input. Without this
    // pre-scan, a command like `pulp-screenshot --width foo --help`
    // would throw before reaching the help check and exit non-zero
    // instead of printing usage with exit code 0.
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--help" || arg == "-h") {
            options.help = true;
            return options;
        }
    }
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--script" && i + 1 < argc) options.script_path = argv[++i];
        else if (arg == "--output" && i + 1 < argc) options.output_path = argv[++i];
        else if (arg == "--width" && i + 1 < argc) options.width = static_cast<uint32_t>(std::stoi(argv[++i]));
        else if (arg == "--height" && i + 1 < argc) options.height = static_cast<uint32_t>(std::stoi(argv[++i]));
        else if (arg == "--scale" && i + 1 < argc) options.scale = std::stof(argv[++i]);
        else if (arg == "--theme" && i + 1 < argc) options.theme_name = argv[++i];
        else if (arg == "--backend" && i + 1 < argc) {
            options.backend_name = argv[++i];
            options.backend_was_defaulted = false;
        }
        else if (arg == "--runtime-trace" && i + 1 < argc) options.runtime_trace_path = argv[++i];
        else if (arg == "--base64") options.output_base64 = true;
        else if (arg == "--demo") options.demo = true;
    }
    return options;
}

static bool normalize_backend(ScreenshotCliOptions& options) {
    // Only default to Skia when the build actually compiled it in.
    // Otherwise the CLI would report `backend=skia` while silently
    // producing CoreGraphics output. We defer the warning until after
    // argument parsing so that an explicit `--backend coregraphics`
    // doesn't print a spurious "falling back" line.
#ifdef PULP_HAS_SKIA
    constexpr bool kHasSkia = true;
#else
    constexpr bool kHasSkia = false;
#endif
    if (!kHasSkia && options.backend_name == "skia") {
        if (options.backend_was_defaulted) {
            // Defaulted to skia at compile-time, but Skia is absent —
            // downgrade silently-ish to CoreGraphics with a one-line warning.
            std::cerr << "Skia not compiled — falling back to CoreGraphics. "
                         "Build with -DPULP_HAS_SKIA=1 to enable Skia.\n";
            options.backend_name = "coregraphics";
            return true;
        }

        std::cerr << "Error: --backend=skia requested but Skia is not compiled in. "
                     "Build with -DPULP_HAS_SKIA=1 to enable Skia.\n";
        return false;
    }
    return true;
}

static const char* runtime_trace_script() {
    return R"JS(
(function () {
    function keys(obj) {
        return obj ? Object.keys(obj).sort() : [];
    }
    function listenerSummary(target) {
        if (!target || !target._listeners) return [];
        return keys(target._listeners).map(function (type) {
            var list = target._listeners[type] || [];
            return { type: type, count: list.length };
        });
    }
    function callbackSummary() {
        if (typeof __callbacks__ === 'undefined') return [];
        return keys(__callbacks__).map(function (key) {
            var idx = key.lastIndexOf(':');
            return {
                key: key,
                id: idx >= 0 ? key.slice(0, idx) : key,
                type: idx >= 0 ? key.slice(idx + 1) : ''
            };
        });
    }
    function nativeRegistrationSummary() {
        if (typeof __nativeRegistered__ === 'undefined') return [];
        return keys(__nativeRegistered__).map(function (key) {
            var idx = key.lastIndexOf(':');
            return {
                key: key,
                id: idx >= 0 ? key.slice(0, idx) : key,
                group: idx >= 0 ? key.slice(idx + 1) : ''
            };
        });
    }
    function cloneObject(obj) {
        var out = {};
        if (!obj) return out;
        keys(obj).forEach(function (key) {
            var value = obj[key];
            if (value != null && typeof value !== 'function') out[key] = String(value);
        });
        return out;
    }
    function textPreview(el) {
        var text = el && el._textContent != null ? String(el._textContent) : '';
        return text.length > 80 ? text.slice(0, 80) : text;
    }
    function rectFor(el) {
        if (!el || !el._nativeCreated || typeof getLayoutRect !== 'function') return null;
        try {
            var r = getLayoutRect(el._id);
            if (!r) return null;
            return {
                x: Number(r.x || 0),
                y: Number(r.y || 0),
                width: Number(r.width || 0),
                height: Number(r.height || 0),
                top: Number(r.top || 0),
                right: Number(r.right || 0),
                bottom: Number(r.bottom || 0),
                left: Number(r.left || 0)
            };
        } catch (e) {
            return null;
        }
    }
    function ancestorChainFor(el) {
        if (el && el._nativeCreated && typeof getLayoutAncestorRects === 'function') {
            try {
                var nativeChain = getLayoutAncestorRects(el._id);
                if (nativeChain && typeof nativeChain.length === 'number') {
                    var normalized = [];
                    for (var i = 0; i < nativeChain.length; i++) {
                        var entry = nativeChain[i] || {};
                        var bounds = entry.bounds || null;
                        normalized.push({
                            id: String(entry.id || ''),
                            tag: '',
                            bounds_source: bounds ? 'getLayoutAncestorRects' : 'none',
                            bounds: bounds ? {
                                x: Number(bounds.x || 0),
                                y: Number(bounds.y || 0),
                                width: Number(bounds.width || 0),
                                height: Number(bounds.height || 0),
                                top: Number(bounds.top || 0),
                                right: Number(bounds.right || 0),
                                bottom: Number(bounds.bottom || 0),
                                left: Number(bounds.left || 0)
                            } : null
                        });
                    }
                    if (normalized.length) return normalized;
                }
            } catch (e) {
                // Fall back to the JS-side parent chain below.
            }
        }
        var chain = [];
        var seen = {};
        var cur = el || null;
        while (cur && cur._id && !seen[cur._id] && chain.length < 128) {
            seen[cur._id] = true;
            var bounds = rectFor(cur);
            chain.unshift({
                id: String(cur._id || ''),
                tag: cur.tagName ? String(cur.tagName).toLowerCase() : '',
                bounds_source: bounds ? 'getLayoutRect' : 'none',
                bounds: bounds
            });
            cur = cur._parentElement || null;
        }
        return chain;
    }
    function nativeBoundsSummary() {
        if (typeof __nativeElements__ === 'undefined') return [];
        var ancestorTraceIds = {};
        if (typeof __nativeRegistered__ !== 'undefined') {
            keys(__nativeRegistered__).forEach(function (key) {
                var idx = key.lastIndexOf(':');
                var id = idx >= 0 ? key.slice(0, idx) : key;
                if (id) ancestorTraceIds[id] = true;
            });
        }
        return keys(__nativeElements__).map(function (id) {
            var el = __nativeElements__[id];
            var attrs = cloneObject(el && el._attributes);
            var bounds = rectFor(el);
            return {
                id: id,
                tag: el && el.tagName ? String(el.tagName).toLowerCase() : '',
                user_id: el && el._userIdSet ? String(attrs.id || '') : '',
                class_name: el && el._className ? String(el._className) : '',
                text: textPreview(el),
                native_created: !!(el && el._nativeCreated),
                attributes: attrs,
                bounds_source: bounds ? 'getLayoutRect' : 'none',
                bounds: bounds,
                ancestor_chain: ancestorTraceIds[id] ? ancestorChainFor(el) : []
            };
        });
    }
    function traceReferenceFrame() {
        var rootSize = null;
        if (typeof getRootSize === 'function') {
            try {
                var s = getRootSize();
                if (s) rootSize = { width: Number(s.width || 0), height: Number(s.height || 0) };
            } catch (e) {
                rootSize = null;
            }
        }
        var body = (typeof document !== 'undefined') ? document.body : null;
        return {
            coordinate_space: 'root-view-css-points',
            origin: 'top-left',
            root_size: rootSize,
            document_body_id: body && body._id ? String(body._id) : '',
            document_body_bounds: rectFor(body)
        };
    }
    var addEventLog = Array.isArray(globalThis.__pulpAddELLog__)
        ? globalThis.__pulpAddELLog__.map(function (entry) {
            return { op: String(entry.op || ''), type: String(entry.type || ''), fn: String(entry.fn || '') };
          })
        : [];
    var callbacks = callbackSummary();
    var nativeRegistered = nativeRegistrationSummary();
    var nativeBounds = nativeBoundsSummary();
    return JSON.stringify({
        schema: 'pulp-screenshot-runtime-trace-v1',
        reference_frame: traceReferenceFrame(),
        callback_count: callbacks.length,
        callbacks: callbacks,
        native_registered_count: nativeRegistered.length,
        native_registered: nativeRegistered,
        window_listeners: listenerSummary(globalThis.window),
        document_listeners: listenerSummary(globalThis.document),
        add_event_listener_log_count: addEventLog.length,
        add_event_listener_log: addEventLog,
        dispatch_hits: globalThis.__pulpDispatchHits__ || null,
        native_element_count: (typeof __nativeElements__ !== 'undefined') ? keys(__nativeElements__).length : 0,
        native_bounds_count: nativeBounds.length,
        native_bounds: nativeBounds
    }, null, 2);
})()
)JS";
}

int main(int argc, char* argv[]) {
    // Parity mode: `pulp-screenshot --compare <reference.png> <rendered.png>
    //               [--threshold 0.85] [--diff <out.png>]`
    // Prints similarity + mean error; exits 0 if similarity >= threshold else 1.
    // Reuses the design-import / visual-regression comparison (compare_screenshot_files).
    for (int i = 1; i + 2 < argc; ++i) {
        if (std::string(argv[i]) != "--compare") continue;
        const std::string ref = argv[i + 1];
        const std::string rendered = argv[i + 2];
        float threshold = 0.85f;
        std::string diff_out;
        for (int j = 1; j < argc; ++j) {
            const std::string a = argv[j];
            if (a == "--threshold" && j + 1 < argc) threshold = std::stof(argv[j + 1]);
            else if (a == "--diff" && j + 1 < argc) diff_out = argv[j + 1];
        }
        const auto result = pulp::view::compare_screenshot_files(ref, rendered);
        if (!result.valid) {
            std::cerr << "Error: compare failed — could not read or size-match '" << ref
                      << "' and '" << rendered << "'\n";
            return 2;
        }
        const bool pass = result.passes(threshold);
        std::cout << "similarity=" << result.similarity << " mean_error=" << result.mean_error
                  << " threshold=" << threshold << " => " << (pass ? "PASS" : "FAIL") << "\n";
        if (!diff_out.empty()) {
            const auto a = read_file(ref), b = read_file(rendered);
            const std::vector<uint8_t> ab(a.begin(), a.end()), bb(b.begin(), b.end());
            const auto diff = pulp::view::generate_diff_image(ab, bb);
            if (!diff.empty()) {
                std::ofstream(diff_out, std::ios::binary)
                    .write(reinterpret_cast<const char*>(diff.data()),
                           static_cast<std::streamsize>(diff.size()));
                std::cout << "diff image saved to " << diff_out << "\n";
            }
        }
        return pass ? 0 : 1;
    }

    auto options = parse_options(argc, argv);
    if (options.help) { print_usage(); return 0; }

    // Refuse a silent downgrade. If the caller explicitly asked for Skia
    // but Skia isn't compiled in, fail loudly with exit code 2 so CI and
    // harness diffs catch the mismatch instead of comparing CoreGraphics
    // output against a Skia baseline.
    if (!normalize_backend(options)) return 2;

    ScreenshotBackend backend = ScreenshotBackend::skia;
    if (options.backend_name == "coregraphics" || options.backend_name == "cg") {
        backend = ScreenshotBackend::coregraphics;
    } else if (options.backend_name == "skia") {
        backend = ScreenshotBackend::skia;
    } else if (options.backend_name == "auto") {
        backend = ScreenshotBackend::auto_select;
    } else if (options.backend_name == "gpu") {
        backend = ScreenshotBackend::gpu;
    } else if (options.backend_name == "default") {
        backend = ScreenshotBackend::default_backend;
    } else {
        std::cerr << "Error: unknown --backend '" << options.backend_name
                  << "' (valid: auto, skia, coregraphics, gpu, default)\n";
        return 1;
    }

    if (!options.demo && options.script_path.empty()) {
        std::cerr << "Error: --script or --demo required\n";
        print_usage();
        return 1;
    }

    // Set up state store
    StateStore store;

    // Create root view with theme
    View root;
    if (options.theme_name == "light") root.set_theme(Theme::light());
    else if (options.theme_name == "pro_audio") root.set_theme(Theme::pro_audio());
    else root.set_theme(Theme::dark());

    // Apply --width/--height to the root's bounds BEFORE any script runs.
    // Without this, root.local_bounds() is (0,0,0,0)
    // when yoga_layout reads it; YGNodeStyleSetWidth/Height(root) then
    // gets 0, and every position:absolute + inset:0 child computes to
    // 0×0 — blanking any chain of absolute-positioned containers (the
    // canonical "fill containing block" CSS pattern). First surfaced
    // via Spectr's Editor / FilterBank / canvas / Chrome hierarchy,
    // which is exactly this chain.
    root.set_bounds({0, 0, static_cast<float>(options.width), static_cast<float>(options.height)});

    root.flex().direction = FlexDirection::column;
    // Only set padding/gap for demo mode — scripts manage their own layout
    if (options.demo) {
        root.flex().padding = 16;
        root.flex().gap = 8;
    }

    // Set up scripting
    ScriptEngine engine;
    WidgetBridge bridge(engine, root, store);

    // Install runtime-import handlers so React-imported trees (Spectr's
    // editor.js et al.) can register & drain useEffect /
    // requestAnimationFrame / setTimeout callbacks. These are registered
    // conditionally because plain createKnob / createFader scripts don't
    // need them. Always install in pulp-screenshot since the tool's
    // raison d'etre includes capturing React-driven imports.
    bridge.install_runtime_import_handlers();

    if (options.demo) {
        // Built-in demo UI
        bridge.load_script(R"(
            createLabel('title', 'Pulp Plugin Demo', 0, 0, 300, 24);
            createKnob('gain', 0, 0, 64, 64);
            createKnob('mix', 0, 0, 64, 64);
            createFader('volume', 0, 0, 200, 20, 'horizontal');
            createToggle('bypass', 0, 0, 60, 28);
        )");
        engine.evaluate("setValue('gain', 0.6)");
        engine.evaluate("setValue('mix', 0.8)");
        engine.evaluate("setValue('volume', 0.7)");
    } else {
        // Load library JS files from the same directory
        auto js_dir = std::filesystem::path(options.script_path).parent_path();
        for (auto& lib : {"oklch.js"}) {
            auto lib_path = js_dir / lib;
            if (std::filesystem::exists(lib_path)) {
                bridge.load_script(read_file(lib_path.string()));
            }
        }

        auto code = read_file(options.script_path);
        if (code.empty()) {
            std::cerr << "Error: could not read " << options.script_path << "\n";
            return 1;
        }
        bridge.load_script(code);

        // After React mount, reconcile any oversize absolute descendants
        // with the viewport so bottom-anchored content lands within the
        // captured frame. No-op when content fits. Walks the entire
        // subtree, not just direct children of root_, because runtime-import
        // adapters such as Spectr's dom-adapter propagate the hardcoded
        // oversize through multiple wrappers.
        pulp::view::reconcile_oversize_absolute_subtree(root, options.width, options.height);
    }

    // Drain React's useEffect callbacks, requestAnimationFrame queue, and
    // setTimeout/setInterval timers BEFORE rendering. Without this,
    // headless captures of React-imported trees show only what mounts
    // synchronously — any drawing that lives inside useEffect
    // (canvas paint, dB axis labels, frequency labels, grid lines) never
    // runs because the underlying message loop doesn't tick between
    // script-load and render. Spectr's editor uses this pattern for
    // drawSpectrum / drawRulers; the live host's NSRunLoop ticks them
    // naturally, but pulp-screenshot's headless path has to pump
    // explicitly. WidgetBridge registers __pulpRuntimeSettle__ exactly
    // for this case.
    bridge.load_script("if (typeof __pulpRuntimeSettle__ === 'function') __pulpRuntimeSettle__(64);");

    if (!options.runtime_trace_path.empty()) {
        try {
            auto trace = engine.evaluate(runtime_trace_script()).toString();
            if (!write_text_file(options.runtime_trace_path, trace + "\n")) {
                std::cerr << "Error: could not write runtime trace " << options.runtime_trace_path << "\n";
                return 1;
            }
        } catch (const std::exception& e) {
            std::cerr << "Error: runtime trace failed: " << e.what() << "\n";
            return 1;
        }
    }

    // Render. For the smart backends (auto/gpu) route through capture_view so the
    // backend is auto-selected, native-overlay views are refused, and a blank /
    // clear-only frame is a hard error (exit 3) instead of a silently saved blank.
    const bool smart = (backend == ScreenshotBackend::auto_select ||
                        backend == ScreenshotBackend::gpu);
    std::vector<uint8_t> png;
    std::string used_label = options.backend_name;
    if (smart) {
        CaptureResult cap =
            capture_view(root, options.width, options.height, options.scale, backend);
        if (!cap.ok) {
            std::cerr << "Error: capture is not trustworthy — " << cap.reason << "\n";
            return 3;  // native overlay / blank / no backend
        }
        png = std::move(cap.png);
        used_label = (cap.used == ScreenshotBackend::gpu)          ? "gpu"
                     : (cap.used == ScreenshotBackend::coregraphics) ? "coregraphics"
                                                                     : "skia";
    } else {
        png = render_to_png(root, options.width, options.height, options.scale, backend);
        if (png.empty()) {
            std::cerr << "Error: rendering failed\n";
            return 1;
        }
    }

    if (options.output_base64) {
        std::cout << base64_encode(png);
        return 0;
    }
    std::ofstream out(options.output_path, std::ios::binary);
    if (!out) {
        std::cerr << "Error: cannot open output '" << options.output_path << "'\n";
        return 1;
    }
    out.write(reinterpret_cast<const char*>(png.data()),
              static_cast<std::streamsize>(png.size()));
    // Check the stream state before reporting success: out.write() can fail
    // silently (disk full, quota exceeded, I/O error, short write on a network
    // filesystem). Flush/close explicitly so a deferred write error surfaces in
    // the stream state rather than after the success message has been printed.
    out.close();
    if (!out) {
        std::cerr << "Error: failed writing screenshot to '" << options.output_path
                  << "' (disk full or I/O error)\n";
        return 1;
    }
    std::cout << "Screenshot saved to " << options.output_path << " (" << options.width
              << "x" << options.height << " @" << options.scale << "x, backend="
              << used_label << ")\n";
    return 0;
}
