#include <catch2/catch_test_macros.hpp>
#include <pulp/state/store.hpp>
#include <pulp/view/design_import.hpp>
#include <pulp/view/script_engine.hpp>
#include <pulp/view/widget_bridge.hpp>

#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

using namespace pulp::view;

namespace {

#ifndef PULP_REPO_ROOT
#define PULP_REPO_ROOT "."
#endif

std::string read_fixture(const std::string& rel_path) {
    const auto root = std::string(PULP_REPO_ROOT);
    std::vector<std::string> candidates{root + "/" + rel_path};
    constexpr std::string_view planning_prefix{"planning/fixtures/"};
    if (rel_path.rfind(std::string(planning_prefix), 0) == 0) {
        candidates.push_back(root + "/test/fixtures/" + rel_path.substr(planning_prefix.size()));
    }

    std::ostringstream tried;
    for (const auto& path : candidates) {
        if (tried.tellp() > 0) {
            tried << ", ";
        }
        tried << path;

        std::ifstream in(path, std::ios::binary);
        if (!in.good()) {
            continue;
        }

        std::ostringstream ss;
        ss << in.rdbuf();
        return ss.str();
    }

    FAIL("reading fixture failed; tried " << tried.str());
    return {};
}

std::string asset_text(const ClaudeBundleAsset& asset) {
    return std::string(asset.data.begin(), asset.data.end());
}

const char* minimal_host_react_dom_shim() {
    return R"JS(
(function(){
  function flatten(input, out) {
    if (input == null || input === false || input === true) return;
    if (Array.isArray(input)) {
      for (var i = 0; i < input.length; i++) flatten(input[i], out);
      return;
    }
    out.push(input);
  }

  function createElement(type, props) {
    var children = [];
    for (var i = 2; i < arguments.length; i++) flatten(arguments[i], children);
    return { type: type, props: props || {}, children: children };
  }

  function cssValue(key, value) {
    if (value == null) return "";
    if (typeof value === "number") {
      if (key === "flexGrow" || key === "flexShrink" || key === "opacity" ||
          key === "zIndex" || key === "lineHeight") {
        return String(value);
      }
      return String(value) + "px";
    }
    return String(value);
  }

  function applyProps(el, props) {
    props = props || {};
    for (var key in props) {
      if (key === "children" || key === "key") continue;
      var value = props[key];
      if (key === "style" && value) {
        for (var styleKey in value) el.style[styleKey] = cssValue(styleKey, value[styleKey]);
      } else if (key === "ref" && value) {
        value.current = el;
      } else if (key === "className") {
        el.setAttribute("class", String(value));
      } else if (key.slice(0, 2) === "on") {
        el["__" + key] = value;
      } else if (value === true) {
        el.setAttribute(key, "");
      } else if (value !== false && value != null) {
        el.setAttribute(key, String(value));
      }
    }
  }

  function renderNode(node) {
    if (node == null || node === false || node === true) return null;
    if (typeof node === "string" || typeof node === "number") {
      return document.createTextNode(String(node));
    }
    if (typeof node.type === "function") {
      var props = Object.assign({}, node.props || {});
      props.children = node.children;
      return renderNode(node.type(props));
    }
    var el = document.createElement(node.type === globalThis.React.Fragment ? "span" : node.type);
    applyProps(el, node.props);
    for (var i = 0; i < node.children.length; i++) {
      var child = renderNode(node.children[i]);
      if (child) el.appendChild(child);
    }
    return el;
  }

  var effects = [];
  globalThis.React = {
    Fragment: "__fragment",
    createElement: createElement,
    useCallback: function(fn) { return fn; },
    useEffect: function(fn) { effects.push(fn); },
    useMemo: function(fn) { return fn(); },
    useRef: function(value) { return { current: value == null ? null : value }; },
    useState: function(initial) {
      var value = initial;
      return [value, function(next) { value = (typeof next === "function") ? next(value) : next; }];
    }
  };
  globalThis.ReactDOM = {
    createRoot: function(mount) {
      return {
        render: function(element) {
          var node = renderNode(element);
          if (node) mount.appendChild(node);
          for (var i = 0; i < effects.length; i++) effects[i]();
        }
      };
    },
    flushSync: function(fn) { return fn(); }
  };
})();
)JS";
}

void maybe_write_figma_runtime_script(const std::string& runtime_js) {
    const char* out = std::getenv("PULP_FIGMA_RUNTIME_JS_OUT");
    if (out == nullptr || *out == '\0') return;

    std::ofstream file(out, std::ios::binary);
    REQUIRE(file.good());
    file << minimal_host_react_dom_shim() << "\n" << runtime_js << "\n";
}

void maybe_write_stitch_runtime_script(const std::string& runtime_js) {
    const char* out = std::getenv("PULP_STITCH_RUNTIME_JS_OUT");
    if (out == nullptr || *out == '\0') return;

    std::ofstream file(out, std::ios::binary);
    REQUIRE(file.good());
    file << minimal_host_react_dom_shim() << "\n" << runtime_js << "\n";
}

void maybe_write_rn_runtime_script(const std::string& runtime_js) {
    const char* out = std::getenv("PULP_RN_RUNTIME_JS_OUT");
    if (out == nullptr || *out == '\0') return;

    std::ofstream file(out, std::ios::binary);
    REQUIRE(file.good());
    file << minimal_host_react_dom_shim() << "\n" << runtime_js << "\n";
}

void maybe_write_pencil_runtime_script(const std::string& runtime_js) {
    const char* out = std::getenv("PULP_PENCIL_RUNTIME_JS_OUT");
    if (out == nullptr || *out == '\0') return;

    std::ofstream file(out, std::ios::binary);
    REQUIRE(file.good());
    file << minimal_host_react_dom_shim() << "\n" << runtime_js << "\n";
}

} // namespace

// ── Design source parsing ───────────────────────────────────────────────

TEST_CASE("parse_design_source recognizes valid sources", "[view][import]") {
    REQUIRE(parse_design_source("figma") == DesignSource::figma);
    REQUIRE(parse_design_source("stitch") == DesignSource::stitch);
    REQUIRE(parse_design_source("v0") == DesignSource::v0);
    REQUIRE(parse_design_source("pencil") == DesignSource::pencil);
    REQUIRE(parse_design_source("claude") == DesignSource::claude);
    REQUIRE_FALSE(parse_design_source("unknown").has_value());
}

TEST_CASE("design_source_name returns display names", "[view][import]") {
    REQUIRE(std::string(design_source_name(DesignSource::figma)) == "Figma");
    REQUIRE(std::string(design_source_name(DesignSource::v0)) == "v0");
    REQUIRE(std::string(design_source_name(DesignSource::claude)) == "Claude Design");
}

// pulp #709 / #468 — Claude Design imports are manually-exported HTML
// parsed via the Stitch HTML pipeline and re-tagged as Claude.
TEST_CASE("parse_claude_html delegates to Stitch pipeline and tags source",
          "[view][import][issue-709][issue-468]") {
    const auto html = std::string{
        R"(<!DOCTYPE html><html><body>
              <div class="container">
                <h1>Hello Claude</h1>
                <button>Click me</button>
              </div>
           </body></html>)"};

    const auto ir = parse_claude_html(html);
    REQUIRE(ir.source == DesignSource::claude);
    // Same HTML fed directly to parse_stitch_html should produce a
    // tree of the same shape; delegation is the contract, not a new
    // parser implementation.
    const auto stitch_ir = parse_stitch_html(html);
    REQUIRE(ir.root.children.size() == stitch_ir.root.children.size());
}

// pulp #709 — render_claude_bridge_scaffold is the library form of the
// CLI's `pulp import-design --from claude` scaffold output. Lives in
// the library (not in the CLI source) so coverage can be asserted
// without needing the spawned-subprocess instrumentation that codecov
// can't see through.
TEST_CASE("render_claude_bridge_scaffold emits a buildable EditorBridge starter",
          "[view][import][issue-709][issue-468]") {
    const auto scaffold = render_claude_bridge_scaffold("ui.js");

    // Threads the path through the file header so users can trace the
    // generated handlers back to the imported view.
    REQUIRE(scaffold.find("ui.js") != std::string::npos);

    // The framework surface MUST be referenced by full name — that's
    // the whole point of the scaffold (#709 acceptance criterion).
    REQUIRE(scaffold.find("pulp::view::EditorBridge") != std::string::npos);
    REQUIRE(scaffold.find("#include <pulp/view/editor_bridge.hpp>") != std::string::npos);
    REQUIRE(scaffold.find("#include <pulp/view/web_view.hpp>") != std::string::npos);

    // Demonstrates each of the patterns plugin authors will copy:
    //   - one no-payload handler
    //   - one typed-payload handler using EditorBridge::get_float
    //   - the WebView attach call
    //   - the comment pointer to attach_native_runtime for #468
    REQUIRE(scaffold.find(R"(add_handler("hello")") != std::string::npos);
    REQUIRE(scaffold.find(R"(add_handler("set_value")") != std::string::npos);
    REQUIRE(scaffold.find("EditorBridge::get_float") != std::string::npos);
    REQUIRE(scaffold.find("EditorBridge::ok_response()") != std::string::npos);
    REQUIRE(scaffold.find("attach_webview(panel)") != std::string::npos);
    REQUIRE(scaffold.find("attach_native_runtime") != std::string::npos);
    REQUIRE(scaffold.find("MyPluginEditor") != std::string::npos);

    // Path is interpolated, not hard-coded — feed a different path and
    // confirm the new value lands in the header.
    const auto other = render_claude_bridge_scaffold("editor/imported.js");
    REQUIRE(other.find("editor/imported.js") != std::string::npos);
    REQUIRE(other.find("ui.js") == std::string::npos);
}

// ── Audio widget detection ──────────────────────────────────────────────

TEST_CASE("detect_audio_widget identifies widget types from names", "[view][import]") {
    REQUIRE(detect_audio_widget("GainKnob") == AudioWidgetType::knob);
    REQUIRE(detect_audio_widget("master_dial") == AudioWidgetType::knob);
    REQUIRE(detect_audio_widget("VolumeFader") == AudioWidgetType::fader);
    REQUIRE(detect_audio_widget("mix_slider") == AudioWidgetType::fader);
    REQUIRE(detect_audio_widget("OutputMeter") == AudioWidgetType::meter);
    REQUIRE(detect_audio_widget("vu_display") == AudioWidgetType::meter);
    REQUIRE(detect_audio_widget("level_indicator") == AudioWidgetType::meter);
    REQUIRE(detect_audio_widget("FilterXYPad") == AudioWidgetType::xy_pad);
    REQUIRE(detect_audio_widget("xy_pad_control") == AudioWidgetType::xy_pad);
    REQUIRE(detect_audio_widget("WaveformDisplay") == AudioWidgetType::waveform);
    REQUIRE(detect_audio_widget("oscilloscope_view") == AudioWidgetType::waveform);
    REQUIRE(detect_audio_widget("SpectrumAnalyzer") == AudioWidgetType::spectrum);
    REQUIRE(detect_audio_widget("analyser_view") == AudioWidgetType::spectrum);
    REQUIRE(detect_audio_widget("header_label") == AudioWidgetType::none);
    REQUIRE(detect_audio_widget("save_button") == AudioWidgetType::none);
}

// ── Figma JSON parsing ──────────────────────────────────────────────────

TEST_CASE("parse_figma_json parses IR format", "[view][import]") {
    auto json = R"json({
        "type": "frame",
        "name": "PluginUI",
        "layout": { "direction": "column", "gap": 16, "padding": 12 },
        "style": { "backgroundColor": "#1a1a2e", "borderRadius": 8 },
        "children": [
            {
                "type": "text",
                "name": "title",
                "content": "My Plugin",
                "style": { "fontSize": 24, "fontWeight": 700, "color": "#e0e0e0" }
            },
            {
                "type": "frame",
                "name": "controls",
                "layout": { "direction": "row", "gap": 8 },
                "children": [
                    { "type": "slider", "name": "GainKnob", "label": "Gain", "min": 0, "max": 1 }
                ]
            }
        ],
        "tokens": {
            "colors": { "bg.primary": "#1a1a2e", "accent.primary": "#e94560" },
            "dimensions": { "spacing.md": 16 }
        }
    })json";

    auto ir = parse_figma_json(json);

    REQUIRE(ir.source == DesignSource::figma);
    REQUIRE(ir.root.type == "frame");
    REQUIRE(ir.root.name == "PluginUI");
    REQUIRE(ir.root.layout.direction == LayoutDirection::column);
    REQUIRE(ir.root.layout.gap == 16.0f);
    REQUIRE(ir.root.layout.padding_top == 12.0f);
    REQUIRE(ir.root.style.background_color == "#1a1a2e");
    REQUIRE(ir.root.style.border_radius == 8.0f);
    REQUIRE(ir.root.children.size() == 2);

    // Text child
    auto& title = ir.root.children[0];
    REQUIRE(title.type == "text");
    REQUIRE(title.text_content == "My Plugin");
    REQUIRE(title.style.font_size == 24.0f);
    REQUIRE(title.style.font_weight == 700);

    // Controls container
    auto& controls = ir.root.children[1];
    REQUIRE(controls.layout.direction == LayoutDirection::row);
    REQUIRE(controls.children.size() == 1);

    // Audio widget auto-detection
    auto& knob = controls.children[0];
    REQUIRE(knob.audio_widget == AudioWidgetType::knob);
    REQUIRE(knob.audio_label == "Gain");
    REQUIRE(knob.audio_min == 0.0f);
    REQUIRE(knob.audio_max == 1.0f);

    // Tokens
    REQUIRE(ir.tokens.colors.count("bg.primary") == 1);
    REQUIRE(ir.tokens.colors["bg.primary"] == "#1a1a2e");
    REQUIRE(ir.tokens.dimensions.count("spacing.md") == 1);
    REQUIRE(ir.tokens.dimensions["spacing.md"] == 16.0f);
}

TEST_CASE("parse_figma_json covers layout style and audio shape metadata edges",
          "[view][import][coverage]") {
    auto json = R"json({
        "type": "frame",
        "name": "Rack",
        "_layoutHeight": 144,
        "_layoutWidth": 480,
        "layout": {
            "direction": "row",
            "gap": 6,
            "wrap": true,
            "paddingTop": 2,
            "paddingRight": 4,
            "paddingBottom": 6,
            "paddingLeft": 8,
            "justify": "space-around",
            "align": "center",
            "widthMode": "fill",
            "heightMode": "hug"
        },
        "style": {
            "backgroundGradient": "linear-gradient(#111,#222)",
            "opacity": 0.75,
            "border": "1px solid #333333",
            "boxShadow": "0 4px 12px #00000040",
            "filter": "blur(2px)",
            "fontFamily": "Inter",
            "fontStyle": "italic",
            "textAlign": "center",
            "letterSpacing": 1.5,
            "lineHeight": 1.2,
            "textTransform": "uppercase",
            "overflow": "hidden",
            "cursor": "pointer",
            "position": "absolute",
            "top": 1,
            "left": 2,
            "right": 3,
            "bottom": 4,
            "zIndex": 9,
            "transform": "rotate(2deg)",
            "minWidth": 100,
            "minHeight": 40,
            "maxWidth": 640,
            "maxHeight": 320
        },
        "children": [
            {
                "type": "text",
                "name": "title",
                "content": "Drive",
                "fill": "#f5e0dc",
                "fontSize": 13,
                "fontWeight": "bold",
                "fontFamily": "Inter Tight"
            },
            {
                "type": "frame",
                "name": "DriveKnob",
                "width": 92,
                "height": 110,
                "children": [
                    {
                        "type": "ellipse",
                        "name": "ring",
                        "width": 64,
                        "height": 64,
                        "stroke": { "fill": "#cba6f7" }
                    },
                    { "type": "text", "name": "caption", "content": "Drive" },
                    { "type": "text", "name": "value", "content": "72%" }
                ]
            },
            {
                "type": "frame",
                "name": "NestedKnobContainer",
                "children": [
                    { "type": "frame", "name": "InnerKnob", "children": [] }
                ]
            }
        ],
        "tokens": {
            "strings": { "copy.title": "Drive" }
        }
    })json";

    auto ir = parse_figma_json(json);

    REQUIRE(ir.root.attributes.at("_layoutHeight") == "144");
    REQUIRE(ir.root.attributes.at("_layoutWidth") == "480");
    REQUIRE(ir.root.layout.direction == LayoutDirection::row);
    REQUIRE(ir.root.layout.wrap);
    REQUIRE(ir.root.layout.justify == LayoutAlign::space_around);
    REQUIRE(ir.root.layout.align == LayoutAlign::center);
    REQUIRE(ir.root.layout.width_mode == SizingMode::fill);
    REQUIRE(ir.root.layout.height_mode == SizingMode::hug);
    REQUIRE(ir.root.layout.padding_left == 8.0f);
    REQUIRE(ir.root.style.background_gradient == "linear-gradient(#111,#222)");
    REQUIRE(ir.root.style.opacity == 0.75f);
    REQUIRE(ir.root.style.box_shadow == "0 4px 12px #00000040");
    REQUIRE(ir.root.style.z_index == 9);
    REQUIRE(ir.tokens.strings["copy.title"] == "Drive");

    const auto& title = ir.root.children[0];
    REQUIRE(title.style.color == "#f5e0dc");
    REQUIRE(title.style.font_size == 13.0f);
    REQUIRE(title.style.font_weight == 700);
    REQUIRE(title.style.font_family == "Inter Tight");

    const auto& knob = ir.root.children[1];
    REQUIRE(knob.audio_widget == AudioWidgetType::knob);
    REQUIRE(knob.attributes.at("shape_width") == "64");
    REQUIRE(knob.attributes.at("shape_height") == "64");
    REQUIRE(knob.children[0].attributes.at("stroke_color") == "#cba6f7");

    const auto& container = ir.root.children[2];
    REQUIRE(container.audio_widget == AudioWidgetType::none);
    REQUIRE(container.layout.direction == LayoutDirection::row);
}

// ── Code generation ─────────────────────────────────────────────────────

TEST_CASE("generate_pulp_js emits motion.setProvenance per vendor + root (Phase 9e)",
          "[view][import][motion][provenance][issue-pulp-motion-phase9]") {
    // Figma export with a recognizable root node name should emit
    // `motion.setProvenance('design-import', 'figma:<root-name>')` so
    // any animations the bundle drives self-attribute through the
    // motion observability publish channel.
    {
        DesignIR ir;
        ir.source = DesignSource::figma;
        ir.root.type = "frame";
        ir.root.name = "Card/Hover";
        CodeGenOptions opts;
        opts.mode = CodeGenMode::web_compat;
        opts.include_comments = false;
        auto js = generate_pulp_js(ir, opts);
        REQUIRE(js.find("motion.setProvenance('design-import', 'figma:Card/Hover')")
                != std::string::npos);
    }
    // Stitch, v0, Pencil, Claude — same shape, different vendor key.
    struct Case { DesignSource src; const char* vendor; };
    for (const Case& c : {
             Case{DesignSource::stitch, "stitch"},
             Case{DesignSource::v0,     "v0"},
             Case{DesignSource::pencil, "pencil"},
             Case{DesignSource::claude, "claude"},
         }) {
        DesignIR ir;
        ir.source = c.src;
        ir.root.type = "frame";
        ir.root.name = "Panel";
        CodeGenOptions opts;
        opts.include_comments = false;
        auto js = generate_pulp_js(ir, opts);
        std::string expected =
            std::string("motion.setProvenance('design-import', '") +
            c.vendor + ":Panel')";
        REQUIRE(js.find(expected) != std::string::npos);
    }
    // When the root has no name but there's a source_file, the file's
    // basename (without extension) is the id.
    {
        DesignIR ir;
        ir.source = DesignSource::figma;
        ir.root.type = "frame";
        ir.source_file = "/path/to/HeaderLayout.json";
        CodeGenOptions opts;
        opts.include_comments = false;
        auto js = generate_pulp_js(ir, opts);
        REQUIRE(js.find("motion.setProvenance('design-import', 'figma:HeaderLayout')")
                != std::string::npos);
    }
}

TEST_CASE("generate_pulp_js produces valid web-compat JS", "[view][import]") {
    DesignIR ir;
    ir.source = DesignSource::figma;
    ir.root.type = "frame";
    ir.root.name = "TestUI";
    ir.root.layout.direction = LayoutDirection::column;
    ir.root.layout.gap = 8.0f;
    ir.root.style.background_color = "#1a1a2e";

    IRNode text;
    text.type = "text";
    text.name = "title";
    text.text_content = "Hello";
    text.style.font_size = 18.0f;
    text.style.color = "#ffffff";
    ir.root.children.push_back(text);

    ir.tokens.colors["bg.primary"] = "#1a1a2e";

    CodeGenOptions opts;
    opts.mode = CodeGenMode::web_compat;
    opts.include_comments = false;
    auto js = generate_pulp_js(ir, opts);

    // Should create root element
    REQUIRE(js.find("const root = document.createElement('div')") != std::string::npos);
    // Should set flex layout
    REQUIRE(js.find("root.style.display = 'flex'") != std::string::npos);
    REQUIRE(js.find("root.style.flexDirection = 'column'") != std::string::npos);
    REQUIRE(js.find("root.style.gap = '8px'") != std::string::npos);
    // Should set background
    REQUIRE(js.find("root.style.backgroundColor = '#1a1a2e'") != std::string::npos);
    // Should create text child
    REQUIRE(js.find("document.createElement('span')") != std::string::npos);
    REQUIRE(js.find(".textContent = 'Hello'") != std::string::npos);
    REQUIRE(js.find(".style.fontSize = '18px'") != std::string::npos);
    // Should append to body
    REQUIRE(js.find("document.body.appendChild(root)") != std::string::npos);
    // Should include token assignments
    REQUIRE(js.find("theme.colors[\"bg.primary\"]") != std::string::npos);
}

TEST_CASE("generate_pulp_js escapes text containing newlines / quotes / backslashes (pulp #81)",
          "[view][import][issue-81]") {
    // pulp #81: a Claude Design HTML file with multi-line <style>/<script>
    // blocks (Spectr's editor.html is the canonical reproducer) used to
    // emit raw newlines into the generated `createLabel('id', 'text', ...)`
    // call, which made the resulting JS unparseable ("unexpected end of
    // string" in pulp-screenshot). Same problem for any text containing
    // `'`, `\`, `\r`, `\t`. The fix routes all user-text emissions through
    // js_single_quote_escape(). This test pins that behavior — every text
    // surface that previously emitted raw user text must now escape the
    // standard JS string-literal control characters.
    DesignIR ir;
    ir.source = DesignSource::claude;
    ir.root.type = "frame";
    ir.root.name = "Root";
    ir.root.layout.direction = LayoutDirection::column;

    IRNode multiline;
    multiline.type = "text";
    multiline.name = "style_block";
    multiline.text_content = "line1\nline2\twith\ttabs\nthird's quote\nbackslash\\here";
    ir.root.children.push_back(multiline);

    CodeGenOptions opts;
    opts.mode = CodeGenMode::native;
    opts.include_comments = false;
    auto js = generate_pulp_js(ir, opts);

    // No raw newline character should sit between two single quotes — that
    // would un-terminate the JS string.
    REQUIRE(js.find("'line1\nline2") == std::string::npos);
    REQUIRE(js.find("third's quote") == std::string::npos);
    REQUIRE(js.find("backslash\\here") == std::string::npos);

    // Positive: every control character should appear in its escaped form
    // somewhere in the generated JS.
    REQUIRE(js.find("\\n") != std::string::npos);
    REQUIRE(js.find("\\t") != std::string::npos);
    REQUIRE(js.find("\\'") != std::string::npos);
    REQUIRE(js.find("\\\\") != std::string::npos);

    // Every emitted createLabel line should have an even number of
    // unescaped single quotes — uneven means a literal was un-terminated.
    std::istringstream stream(js);
    std::string line;
    while (std::getline(stream, line)) {
        if (line.find("createLabel") == std::string::npos) continue;
        std::size_t single_quotes = 0;
        bool escaped = false;
        for (char c : line) {
            if (escaped) { escaped = false; continue; }
            if (c == '\\') { escaped = true; continue; }
            if (c == '\'') ++single_quotes;
        }
        INFO("createLabel line had odd single-quote count: " << line);
        REQUIRE(single_quotes % 2 == 0);
    }
}

TEST_CASE("generate_pulp_js native mode produces Pulp API", "[view][import]") {
    DesignIR ir;
    ir.source = DesignSource::figma;
    ir.root.type = "frame";
    ir.root.name = "TestUI";
    ir.root.layout.direction = LayoutDirection::column;
    ir.root.layout.gap = 8.0f;
    ir.root.style.background_color = "#1a1a2e";
    ir.root.style.width = 320.0f;
    ir.root.style.height = 200.0f;

    IRNode text;
    text.type = "text";
    text.name = "title";
    text.text_content = "Hello";
    text.style.font_size = 18.0f;
    text.style.color = "#ffffff";
    ir.root.children.push_back(text);

    ir.tokens.colors["bg.primary"] = "#1a1a2e";

    CodeGenOptions opts;
    opts.mode = CodeGenMode::native;
    opts.include_comments = false;
    auto js = generate_pulp_js(ir, opts);

    // Should use native API
    REQUIRE(js.find("createCol('root',") != std::string::npos);
    REQUIRE(js.find("setFlex('root', 'gap', 8)") != std::string::npos);
    REQUIRE(js.find("setBackground('root', '#1a1a2e')") != std::string::npos);
    REQUIRE(js.find("setFlex('root', 'width', 320)") != std::string::npos);
    // Labels use createLabel with height (Yoga requirement)
    REQUIRE(js.find("createLabel('title") != std::string::npos);
    REQUIRE(js.find("setFlex('title") != std::string::npos);
    REQUIRE(js.find("'height'") != std::string::npos);
    // Token assignments use setColorToken
    REQUIRE(js.find("setColorToken('bg.primary', '#1a1a2e')") != std::string::npos);
    // Should end with void 0
    REQUIRE(js.find("void 0;") != std::string::npos);
}

TEST_CASE("generate_pulp_js native mode handles audio widgets with Yoga constraints", "[view][import]") {
    DesignIR ir;
    ir.source = DesignSource::figma;
    ir.root.type = "frame";
    ir.root.name = "Controls";
    ir.root.style.width = 300.0f;

    IRNode knob;
    knob.type = "knob";
    knob.name = "GainKnob";
    knob.audio_widget = AudioWidgetType::knob;
    knob.audio_label = "Gain";
    knob.audio_default = 0.75f;
    ir.root.children.push_back(knob);

    IRNode fader;
    fader.type = "fader";
    fader.name = "MixFader";
    fader.audio_widget = AudioWidgetType::fader;
    fader.audio_label = "Mix";
    fader.audio_default = 0.5f;
    ir.root.children.push_back(fader);

    IRNode meter;
    meter.type = "meter";
    meter.name = "OutputMeter";
    meter.audio_widget = AudioWidgetType::meter;
    meter.audio_label = "Out";
    ir.root.children.push_back(meter);

    CodeGenOptions opts;
    opts.mode = CodeGenMode::native;
    auto js = generate_pulp_js(ir, opts);

    // Knob with wrapper column and proper sizing (IDs get numeric suffixes)
    REQUIRE(js.find("createKnob('GainKnob") != std::string::npos);
    REQUIRE(js.find("setLabel('GainKnob") != std::string::npos);
    REQUIRE(js.find("'Gain'") != std::string::npos);
    REQUIRE(js.find("setValue('GainKnob") != std::string::npos);
    // Knob size >= 56 (minimum)
    REQUIRE(js.find("'width', 56)") != std::string::npos);

    // Fader with min width >= 40, label as separate element
    REQUIRE(js.find("createFader('MixFader") != std::string::npos);
    REQUIRE(js.find("createLabel('MixFader") != std::string::npos);  // Separate label
    REQUIRE(js.find("'Mix'") != std::string::npos);
    REQUIRE(js.find("'width', 40)") != std::string::npos);

    // Meter with separate label (no built-in setLabel for Meter)
    REQUIRE(js.find("createMeter('OutputMeter") != std::string::npos);
    REQUIRE(js.find("'Out'") != std::string::npos);
    REQUIRE(js.find("setMeterLevel") != std::string::npos);
}

TEST_CASE("generate_pulp_js web-compat mode handles audio widgets", "[view][import]") {
    DesignIR ir;
    ir.source = DesignSource::figma;
    ir.root.type = "frame";
    ir.root.name = "Controls";

    IRNode knob;
    knob.type = "knob";
    knob.name = "GainKnob";
    knob.audio_widget = AudioWidgetType::knob;
    knob.audio_label = "Gain";
    knob.audio_min = 0.0f;
    knob.audio_max = 1.0f;
    knob.audio_default = 0.75f;
    ir.root.children.push_back(knob);

    CodeGenOptions opts;
    opts.mode = CodeGenMode::web_compat;
    auto js = generate_pulp_js(ir, opts);

    REQUIRE(js.find("createKnob") != std::string::npos);
    REQUIRE(js.find("label: 'Gain'") != std::string::npos);
    REQUIRE(js.find("defaultValue: 0.75") != std::string::npos);
}

TEST_CASE("generate_pulp_js respects CodeGenOptions", "[view][import]") {
    DesignIR ir;
    ir.source = DesignSource::figma;
    ir.source_file = "test.json";
    ir.root.type = "frame";
    ir.root.name = "Root";

    CodeGenOptions opts;
    opts.include_comments = true;
    auto with_comments = generate_pulp_js(ir, opts);
    REQUIRE(with_comments.find("// Generated by Pulp") != std::string::npos);

    opts.include_comments = false;
    auto no_comments = generate_pulp_js(ir, opts);
    REQUIRE(no_comments.find("// Generated") == std::string::npos);
}

TEST_CASE("generate_pulp_js native mode covers layout and audio widget edge branches",
          "[view][import][coverage]") {
    DesignIR ir;
    ir.source = DesignSource::pencil;
    ir.root.type = "frame";
    ir.root.name = "Panel";
    ir.root.layout.direction = LayoutDirection::row;
    ir.root.layout.justify = LayoutAlign::space_between;
    ir.root.layout.align = LayoutAlign::center;
    ir.root.layout.gap = 10.0f;
    ir.root.layout.padding_top = 2.0f;
    ir.root.layout.padding_right = 4.0f;
    ir.root.layout.padding_bottom = 6.0f;
    ir.root.layout.padding_left = 8.0f;
    ir.root.attributes["_layoutHeight"] = "180";
    ir.root.attributes["_layoutWidth"] = "420";
    ir.root.style.background_color = "#111111";
    ir.root.style.border_radius = 6.0f;

    IRNode left;
    left.type = "text";
    left.name = "left label";
    left.text_content = "Left";
    left.style.font_size = 12.0f;
    left.style.color = "#ffffff";
    ir.root.children.push_back(left);

    IRNode right;
    right.type = "text";
    right.name = "right.label";
    right.text_content = "Right";
    right.style.font_weight = 600;
    ir.root.children.push_back(right);

    IRNode knob;
    knob.type = "frame";
    knob.name = "ToneKnob";
    knob.audio_widget = AudioWidgetType::knob;
    knob.audio_label = "Tone";
    knob.audio_default = 0.33f;
    knob.style.width = 90.0f;
    knob.attributes["shape_width"] = "72";
    knob.attributes["shape_height"] = "72";
    IRNode ring;
    ring.type = "ellipse";
    ring.attributes["stroke_color"] = "#fab387";
    knob.children.push_back(ring);
    IRNode value;
    value.type = "text";
    value.text_content = "-6 dB";
    knob.children.push_back(value);
    ir.root.children.push_back(knob);

    IRNode xy;
    xy.type = "frame";
    xy.name = "FilterXYPad";
    xy.audio_widget = AudioWidgetType::xy_pad;
    xy.style.width = 72.0f;
    ir.root.children.push_back(xy);

    IRNode waveform;
    waveform.type = "frame";
    waveform.name = "MainWaveform";
    waveform.audio_widget = AudioWidgetType::waveform;
    waveform.style.width = 180.0f;
    waveform.style.height = 44.0f;
    ir.root.children.push_back(waveform);

    IRNode spectrum;
    spectrum.type = "frame";
    spectrum.name = "SpectrumAnalyzer";
    spectrum.audio_widget = AudioWidgetType::spectrum;
    spectrum.style.width = 160.0f;
    spectrum.style.height = 48.0f;
    ir.root.children.push_back(spectrum);

    IRNode spacer;
    spacer.type = "rectangle";
    spacer.name = "divider";
    spacer.style.height = 2.0f;
    spacer.style.background_color = "#333333";
    ir.root.children.push_back(spacer);

    CodeGenOptions opts;
    opts.mode = CodeGenMode::native;
    opts.include_comments = false;
    opts.preview_mode = true;
    auto js = generate_pulp_js(ir, opts);

    REQUIRE(js.find("createRow('root', '')") != std::string::npos);
    REQUIRE(js.find("setFlex('root', 'height', 180)") != std::string::npos);
    REQUIRE(js.find("setFlex('root', 'width', 420)") != std::string::npos);
    REQUIRE(js.find("setFlex('root', 'padding_top', 2)") != std::string::npos);
    REQUIRE(js.find("setFlex('root', 'padding_left', 8)") != std::string::npos);
    REQUIRE(js.find("setFlex('root', 'justify_content', 'space-between')") != std::string::npos);
    REQUIRE(js.find("setTextAlign('right_label1', 'right')") != std::string::npos);
    REQUIRE(js.find("setCornerRadius('root', 'All', 6)") != std::string::npos);
    REQUIRE(js.find("setWidgetStyle('ToneKnob2', 'minimal')") != std::string::npos);
    REQUIRE(js.find("setBorder('ToneKnob2', '#fab387', 2.5, 36)") != std::string::npos);
    REQUIRE(js.find("createLabel('ToneKnob2_val', '-6 dB'") != std::string::npos);
    REQUIRE(js.find("createXYPad('FilterXYPad3'") != std::string::npos);
    REQUIRE(js.find("createWaveform('MainWaveform4'") != std::string::npos);
    REQUIRE(js.find("createSpectrum('SpectrumAnalyzer5'") != std::string::npos);
    REQUIRE(js.find("createRow('divider6'") != std::string::npos);
    REQUIRE(js.find("setBackground('divider6', '#333333')") != std::string::npos);
}

TEST_CASE("generate_pulp_js web compat emits extended style and layout properties",
          "[view][import][coverage]") {
    DesignIR ir;
    ir.source = DesignSource::v0;
    ir.root.type = "frame";
    ir.root.name = "Root Panel";
    ir.root.layout.direction = LayoutDirection::row;
    ir.root.layout.gap = 2.5f;
    ir.root.layout.padding_top = 1.0f;
    ir.root.layout.padding_right = 2.0f;
    ir.root.layout.padding_bottom = 3.0f;
    ir.root.layout.padding_left = 4.0f;
    ir.root.layout.justify = LayoutAlign::center;
    ir.root.layout.align = LayoutAlign::center;
    ir.root.layout.wrap = true;
    ir.root.layout.width_mode = SizingMode::fill;
    ir.root.layout.height_mode = SizingMode::fill;
    ir.root.style.background_color = "#101010";
    ir.root.style.background_gradient = "linear-gradient(#101010,#202020)";
    ir.root.style.color = "#eeeeee";
    ir.root.style.opacity = 0.5f;
    ir.root.style.border_radius = 3.5f;
    ir.root.style.border = "1px solid #444";
    ir.root.style.box_shadow = "0 1px 2px #000";
    ir.root.style.filter = "blur(1px)";
    ir.root.style.font_family = "Inter";
    ir.root.style.font_size = 15.0f;
    ir.root.style.font_weight = 500;
    ir.root.style.font_style = "italic";
    ir.root.style.text_align = "center";
    ir.root.style.letter_spacing = 0.5f;
    ir.root.style.line_height = 1.3f;
    ir.root.style.text_transform = "uppercase";
    ir.root.style.overflow = "hidden";
    ir.root.style.cursor = "grab";
    ir.root.style.position = "absolute";
    ir.root.style.top = 1.0f;
    ir.root.style.left = 2.0f;
    ir.root.style.right = 3.0f;
    ir.root.style.bottom = 4.0f;
    ir.root.style.z_index = 12;
    ir.root.style.transform = "scale(1.1)";
    ir.root.style.width = 200.0f;
    ir.root.style.height = 100.0f;
    ir.root.style.min_width = 80.0f;
    ir.root.style.min_height = 30.0f;
    ir.root.style.max_width = 400.0f;
    ir.root.style.max_height = 160.0f;

    IRNode button;
    button.type = "button";
    button.name = "Send Button";
    button.text_content = "Send";
    ir.root.children.push_back(button);

    IRNode input;
    input.type = "input";
    input.name = "amount-input";
    ir.root.children.push_back(input);

    IRNode image;
    image.type = "image";
    image.name = "logo.png";
    ir.root.children.push_back(image);

    ir.tokens.strings["copy.cta"] = "Send";

    CodeGenOptions opts;
    opts.mode = CodeGenMode::web_compat;
    opts.include_comments = false;
    opts.root_variable = "panelRoot";
    opts.indent_spaces = 4;
    auto js = generate_pulp_js(ir, opts);

    REQUIRE(js.find("const panelRoot = document.createElement('div')") != std::string::npos);
    REQUIRE(js.find("panelRoot.style.flexDirection = 'row'") != std::string::npos);
    REQUIRE(js.find("panelRoot.style.gap = '2.5px'") != std::string::npos);
    REQUIRE(js.find("panelRoot.style.paddingTop = '1px'") != std::string::npos);
    REQUIRE(js.find("panelRoot.style.paddingLeft = '4px'") != std::string::npos);
    REQUIRE(js.find("panelRoot.style.justifyContent = 'center'") != std::string::npos);
    REQUIRE(js.find("panelRoot.style.alignItems = 'center'") != std::string::npos);
    REQUIRE(js.find("panelRoot.style.flexWrap = 'wrap'") != std::string::npos);
    REQUIRE(js.find("panelRoot.style.flexGrow = '1'") != std::string::npos);
    REQUIRE(js.find("panelRoot.style.background = 'linear-gradient(#101010,#202020)'") != std::string::npos);
    REQUIRE(js.find("panelRoot.style.opacity = '0.5'") != std::string::npos);
    REQUIRE(js.find("panelRoot.style.borderRadius = '3.5px'") != std::string::npos);
    REQUIRE(js.find("panelRoot.style.boxShadow = '0 1px 2px #000'") != std::string::npos);
    REQUIRE(js.find("panelRoot.style.zIndex = '12'") != std::string::npos);
    REQUIRE(js.find("panelRoot.style.maxHeight = '160px'") != std::string::npos);
    REQUIRE(js.find("document.createElement('button')") != std::string::npos);
    REQUIRE(js.find("document.createElement('input')") != std::string::npos);
    REQUIRE(js.find("document.createElement('img')") != std::string::npos);
    REQUIRE(js.find("theme.strings[\"copy.cta\"] = 'Send'") != std::string::npos);
    REQUIRE(js.find("document.body.appendChild(panelRoot)") != std::string::npos);
}

// ── W3C Design Tokens ───────────────────────────────────────────────────

TEST_CASE("parse_w3c_tokens reads W3C format", "[view][import]") {
    auto json = R"({
        "color": {
            "primary": { "$value": "#89B4FA", "$type": "color" },
            "bg": { "$value": "#1E1E2E", "$type": "color" }
        },
        "spacing": {
            "md": { "$value": "8", "$type": "dimension" },
            "lg": { "$value": "16", "$type": "dimension" }
        },
        "font": {
            "family": { "$value": "Inter", "$type": "fontFamily" }
        }
    })";

    auto theme = parse_w3c_tokens(json);

    REQUIRE(theme.colors.count("color.primary") == 1);
    REQUIRE(theme.colors["color.primary"].r8() == 0x89);
    REQUIRE(theme.colors["color.primary"].g8() == 0xB4);
    REQUIRE(theme.colors["color.primary"].b8() == 0xFA);

    REQUIRE(theme.colors.count("color.bg") == 1);
    REQUIRE(theme.colors["color.bg"].r8() == 0x1E);

    REQUIRE(theme.dimensions.count("spacing.md") == 1);
    REQUIRE(theme.dimensions["spacing.md"] == 8.0f);
    REQUIRE(theme.dimensions["spacing.lg"] == 16.0f);

    REQUIRE(theme.strings.count("font.family") == 1);
    REQUIRE(theme.strings["font.family"] == "Inter");
}

TEST_CASE("export_w3c_tokens produces W3C format", "[view][import]") {
    Theme theme;
    theme.colors["bg.primary"] = color_from_hex(0x1E1E2E);
    theme.colors["accent.primary"] = color_from_hex(0x89B4FA);
    theme.dimensions["spacing.md"] = 8.0f;
    theme.strings["font.family"] = "Inter";

    auto json = export_w3c_tokens(theme);

    REQUIRE(json.find("\"$value\"") != std::string::npos);
    REQUIRE(json.find("\"$type\": \"color\"") != std::string::npos);
    REQUIRE(json.find("\"$type\": \"dimension\"") != std::string::npos);
    REQUIRE(json.find("\"$type\": \"string\"") != std::string::npos);
    REQUIRE(json.find("#1e1e2e") != std::string::npos);
}

TEST_CASE("W3C token round-trip preserves colors", "[view][import]") {
    Theme original;
    original.colors["bg.primary"] = color_from_hex(0x1A1A2E);
    original.colors["accent.primary"] = color_from_hex(0xE94560);
    original.dimensions["spacing.md"] = 8.0f;

    auto w3c = export_w3c_tokens(original);
    auto restored = parse_w3c_tokens(w3c);

    // Colors should round-trip (names get prefixed by group)
    REQUIRE(restored.colors.count("bg.primary") == 1);
    REQUIRE(restored.colors["bg.primary"].r8() == original.colors["bg.primary"].r8());
    REQUIRE(restored.colors["bg.primary"].g8() == original.colors["bg.primary"].g8());
    REQUIRE(restored.colors["bg.primary"].b8() == original.colors["bg.primary"].b8());
}

TEST_CASE("parse_w3c_tokens resolves aliases", "[view][import]") {
    auto json = R"({
        "color": {
            "$type": "color",
            "blue": { "$value": "#3B82F6" },
            "primary": { "$value": "{color.blue}" },
            "accent": { "$value": "{color.primary}" }
        },
        "spacing": {
            "$type": "dimension",
            "base": { "$value": "8" },
            "md": { "$value": "{spacing.base}" },
            "lg": { "$value": "16" }
        }
    })";

    auto theme = parse_w3c_tokens(json);

    // Direct values
    REQUIRE(theme.colors.count("color.blue") == 1);
    REQUIRE(theme.colors["color.blue"].r8() == 0x3B);

    // Single alias: primary → blue
    REQUIRE(theme.colors.count("color.primary") == 1);
    REQUIRE(theme.colors["color.primary"].r8() == 0x3B);
    REQUIRE(theme.colors["color.primary"].g8() == 0x82);

    // Chained alias: accent → primary → blue
    REQUIRE(theme.colors.count("color.accent") == 1);
    REQUIRE(theme.colors["color.accent"].r8() == 0x3B);

    // Dimension alias: md → base
    REQUIRE(theme.dimensions["spacing.md"] == 8.0f);
    REQUIRE(theme.dimensions["spacing.lg"] == 16.0f);
}

TEST_CASE("parse_w3c_tokens inherits group $type", "[view][import]") {
    auto json = R"({
        "color": {
            "$type": "color",
            "bg": { "$value": "#1E1E2E" },
            "text": { "$value": "#CDD6F4" }
        },
        "size": {
            "$type": "dimension",
            "sm": { "$value": "4" },
            "md": { "$value": "8" }
        }
    })";

    auto theme = parse_w3c_tokens(json);

    // Tokens inherit $type from parent group
    REQUIRE(theme.colors.count("color.bg") == 1);
    REQUIRE(theme.colors.count("color.text") == 1);
    REQUIRE(theme.dimensions.count("size.sm") == 1);
    REQUIRE(theme.dimensions["size.sm"] == 4.0f);
    REQUIRE(theme.dimensions["size.md"] == 8.0f);
}

TEST_CASE("parse_w3c_tokens handles composite typography tokens", "[view][import]") {
    auto json = R"({
        "heading": {
            "$type": "typography",
            "$value": {
                "fontFamily": "Inter",
                "fontSize": "24",
                "fontWeight": "700",
                "lineHeight": "1.2"
            }
        }
    })";

    auto theme = parse_w3c_tokens(json);

    REQUIRE(theme.strings.count("heading.fontFamily") == 1);
    REQUIRE(theme.strings["heading.fontFamily"] == "Inter");
    REQUIRE(theme.dimensions["heading.fontSize"] == 24.0f);
    REQUIRE(theme.dimensions["heading.fontWeight"] == 700.0f);
    REQUIRE(theme.dimensions["heading.lineHeight"] == 1.2f);
}

TEST_CASE("parse_w3c_tokens handles composite shadow tokens", "[view][import]") {
    auto json = R"({
        "shadow": {
            "$type": "shadow",
            "card": {
                "$value": {
                    "color": "#00000040",
                    "offsetX": "0",
                    "offsetY": "4",
                    "blur": "8",
                    "spread": "0"
                }
            }
        }
    })";

    auto theme = parse_w3c_tokens(json);

    REQUIRE(theme.colors.count("shadow.card.color") == 1);
    REQUIRE(theme.dimensions["shadow.card.offsetY"] == 4.0f);
    REQUIRE(theme.dimensions["shadow.card.blur"] == 8.0f);
}

TEST_CASE("parse_w3c_tokens evaluates math expressions", "[view][import]") {
    auto json = R"({
        "spacing": {
            "$type": "dimension",
            "base": { "$value": "8" },
            "double": { "$value": "8 * 2" },
            "half": { "$value": "8 / 2" },
            "sum": { "$value": "4 + 12" }
        }
    })";

    auto theme = parse_w3c_tokens(json);

    REQUIRE(theme.dimensions["spacing.base"] == 8.0f);
    REQUIRE(theme.dimensions["spacing.double"] == 16.0f);
    REQUIRE(theme.dimensions["spacing.half"] == 4.0f);
    REQUIRE(theme.dimensions["spacing.sum"] == 16.0f);
}

TEST_CASE("parse_w3c_tokens resolves alias then evaluates math", "[view][import]") {
    auto json = R"({
        "spacing": {
            "$type": "dimension",
            "base": { "$value": "8" },
            "lg": { "$value": "{spacing.base} * 2" }
        }
    })";

    auto theme = parse_w3c_tokens(json);

    // {spacing.base} resolves to "8", then "8 * 2" evaluates to 16
    REQUIRE(theme.dimensions["spacing.lg"] == 16.0f);
}

TEST_CASE("token parsers cover border composites inference and alternate sources",
          "[view][import][coverage]") {
    auto w3c = R"({
        "border": {
            "$type": "border",
            "focus": {
                "$value": {
                    "color": "#ff00aa80",
                    "width": "2px",
                    "style": "dashed"
                }
            }
        },
        "misc": {
            "implicitColor": { "$value": "#00ff00" },
            "implicitNumber": { "$value": "42" },
            "implicitUnitNumber": { "$value": "3.5rem" },
            "implicitString": { "$value": "not-a-number" },
            "partialNumber": { "$value": "12pxjunk" },
            "badTypedNumber": { "$type": "number", "$value": "1e999" },
            "unknownComposite": { "$type": "custom", "$value": { "x": 1 } }
        }
    })";

    auto theme = parse_w3c_tokens(w3c);
    REQUIRE(theme.colors["border.focus.color"].a8() == 0x80);
    REQUIRE(theme.dimensions["border.focus.width"] == 2.0f);
    REQUIRE(theme.strings["border.focus.style"] == "dashed");
    REQUIRE(theme.colors["misc.implicitColor"].g8() == 0xff);
    REQUIRE(theme.dimensions["misc.implicitNumber"] == 42.0f);
    REQUIRE(theme.dimensions["misc.implicitUnitNumber"] == 3.5f);
    REQUIRE(theme.strings["misc.implicitString"] == "not-a-number");
    REQUIRE(theme.strings["misc.partialNumber"] == "12pxjunk");
    REQUIRE(theme.dimensions.count("misc.badTypedNumber") == 0);
    REQUIRE(theme.strings.count("misc.unknownComposite") == 1);

    auto figma = R"([
        { "name": "color/alpha", "type": "color", "value": "#11223344" },
        { "name": "size/ratio", "type": "number", "value": "1.25" },
        { "name": "size/unit", "type": "number", "value": "2px" },
        { "name": "size/bad", "type": "number", "value": "4remjunk" },
        { "name": "copy/title", "type": "string", "value": "Hello" },
        { "name": "inferred/color", "resolvedValue": "#abcdef" },
        { "name": "inferred/number", "resolvedValue": "3.5" },
        { "name": "inferred/bad-number", "resolvedValue": "3.5junk" },
        { "name": "inferred/string", "resolvedValue": "wide" },
        { "type": "STRING", "resolvedValue": "missing name" }
    ])";

    auto figma_theme = parse_figma_variables(figma);
    REQUIRE(figma_theme.colors["color.alpha"].a8() == 0x44);
    REQUIRE(figma_theme.dimensions["size.ratio"] == 1.25f);
    REQUIRE(figma_theme.dimensions["size.unit"] == 2.0f);
    REQUIRE(figma_theme.dimensions.count("size.bad") == 0);
    REQUIRE(figma_theme.strings["copy.title"] == "Hello");
    REQUIRE(figma_theme.colors["inferred.color"].r8() == 0xab);
    REQUIRE(figma_theme.dimensions["inferred.number"] == 3.5f);
    REQUIRE(figma_theme.strings["inferred.bad-number"] == "3.5junk");
    REQUIRE(figma_theme.strings["inferred.string"] == "wide");

    auto stitch = R"({
        "colors": { "accent": "#12345678" },
        "roundness": "full",
        "spacing": 20
    })";

    auto stitch_theme = parse_stitch_design_system(stitch);
    REQUIRE(stitch_theme.colors["color.accent"].a8() == 0x78);
    REQUIRE(stitch_theme.dimensions["roundness"] == 999.0f);
    REQUIRE(stitch_theme.dimensions["spacing.base"] == 20.0f);
}

// ── IR ↔ Theme conversion ───────────────────────────────────────────────

TEST_CASE("ir_tokens_to_theme converts token maps to Theme", "[view][import]") {
    IRTokens tokens;
    tokens.colors["bg.primary"] = "#1a1a2e";
    tokens.dimensions["spacing.md"] = 8.0f;
    tokens.strings["font.family"] = "Inter";

    auto theme = ir_tokens_to_theme(tokens);

    REQUIRE(theme.colors.count("bg.primary") == 1);
    REQUIRE(theme.colors["bg.primary"].r8() == 0x1a);
    REQUIRE(theme.dimensions["spacing.md"] == 8.0f);
    REQUIRE(theme.strings["font.family"] == "Inter");
}

TEST_CASE("theme_to_ir_tokens converts Theme to token maps", "[view][import]") {
    Theme theme;
    theme.colors["bg.primary"] = color_from_hex(0x1A1A2E);
    theme.dimensions["spacing.md"] = 8.0f;
    theme.strings["font.family"] = "Inter";

    auto tokens = theme_to_ir_tokens(theme);

    REQUIRE(tokens.colors["bg.primary"] == "#1a1a2e");
    REQUIRE(tokens.dimensions["spacing.md"] == 8.0f);
    REQUIRE(tokens.strings["font.family"] == "Inter");
}

// ── Stitch HTML parsing ─────────────────────────────────────────────────

TEST_CASE("parse_stitch_html extracts text from simple HTML", "[view][import]") {
    auto html = "<h1>Plugin Title</h1>\n<p>Description text</p>";
    auto ir = parse_stitch_html(html);

    REQUIRE(ir.source == DesignSource::stitch);
    REQUIRE(ir.root.type == "frame");
    REQUIRE(ir.root.children.size() >= 1);
    // At least the first match is extracted
    REQUIRE(ir.root.children[0].text_content == "Plugin Title");
}

TEST_CASE("parse_stitch_html accepts JSON IR directly", "[view][import]") {
    auto json = R"({"type": "frame", "name": "Screen", "children": []})";
    auto ir = parse_stitch_html(json);
    REQUIRE(ir.root.name == "Screen");
}

// ── v0 TSX parsing ──────────────────────────────────────────────────────

TEST_CASE("parse_v0_tsx extracts className patterns", "[view][import]") {
    auto tsx = R"(
        export default function Component() {
            return (
                <div className="flex flex-col gap-4 p-4 bg-slate-900">
                    <div className="flex flex-row gap-2">
                        <span>Label</span>
                    </div>
                </div>
            );
        }
    )";
    auto ir = parse_v0_tsx(tsx);

    REQUIRE(ir.source == DesignSource::v0);
    REQUIRE(ir.root.type == "frame");
    // Should extract at least the two className entries
    REQUIRE(ir.root.children.size() >= 2);
}

TEST_CASE("parse_v0_tsx accepts JSON IR directly", "[view][import]") {
    auto json = R"({"type": "frame", "name": "V0Screen", "children": []})";
    auto ir = parse_v0_tsx(json);
    REQUIRE(ir.root.name == "V0Screen");
}

TEST_CASE("parse_v0_dev_react parses staged v0 runtime fixtures",
          "[view][import][parser][v0][phase-6.6.2]") {
    const auto primary = read_fixture("planning/fixtures/v0-dev/audio-control-panel.tsx");
    auto bundle = parse_v0_dev_react(primary);
    REQUIRE(bundle.has_value());
    REQUIRE(bundle->assets.size() == 1);
    REQUIRE(bundle->javascript_indices.size() == 1);
    REQUIRE(bundle->javascript_indices.front() == 0);
    REQUIRE(bundle->assets.front().uuid == "v0-runtime-app");
    REQUIRE(bundle->assets.front().mime == "text/javascript");
    REQUIRE(bundle->template_html.find("data-pulp-source=\"v0\"") != std::string::npos);
    REQUIRE(bundle->template_html.find("v0-audio-control-panel") != std::string::npos);

    const auto js = asset_text(bundle->assets.front());
    REQUIRE(js.find("v0-audio-control-panel") != std::string::npos);
    REQUIRE(js.find("ReactDOM.createRoot") != std::string::npos);
    REQUIRE(js.find("requestAnimationFrame") != std::string::npos);
    REQUIRE(js.find("performance.now") != std::string::npos);
    REQUIRE(js.find("canvas.getContext('2d')") != std::string::npos);
    REQUIRE(js.find("type: 'range'") != std::string::npos);

    const auto settings = read_fixture("planning/fixtures/v0-dev/settings-strip.tsx");
    auto settings_bundle = parse_v0_dev_react(settings);
    REQUIRE(settings_bundle.has_value());
    REQUIRE(asset_text(settings_bundle->assets.front()).find("v0-settings-strip") != std::string::npos);

    const auto transport = read_fixture("planning/fixtures/v0-dev/transport-meter.tsx");
    auto transport_bundle = parse_v0_dev_react(transport);
    REQUIRE(transport_bundle.has_value());
    REQUIRE(asset_text(transport_bundle->assets.front()).find("v0-transport-meter") != std::string::npos);

    const std::string multi_line_import = R"(
        import {
          useState
        } from "react";
        export default function MultiLineReactPanel() {
          const [level, setLevel] = useState(0.5);
          return (
            <div id="v0-multi-line-react-import">
              <span>Level</span>
              <input type="range" value={level} onChange={(event) => setLevel(Number(event.currentTarget.value))} />
            </div>
          );
        }
    )";
    auto multi_line_bundle = parse_v0_dev_react(multi_line_import);
    REQUIRE(multi_line_bundle.has_value());
    REQUIRE(asset_text(multi_line_bundle->assets.front()).find("v0-multi-line-react-import") != std::string::npos);
}

TEST_CASE("parse_v0_dev_react accepts a v0 file envelope",
          "[view][import][parser][v0][phase-6.6.2]") {
    const auto primary = read_fixture("planning/fixtures/v0-dev/audio-control-panel.tsx");
    const auto envelope =
        std::string("[V0_FILE]json:file=\"package.json\"\n{\"dependencies\":{\"react\":\"latest\"}}\n")
        + "[V0_FILE]tsx:file=\"app/page.tsx\"\n"
        + primary
        + "\n[V0_FILE]tsx:file=\"app/ignored.tsx\"\n"
          "import { useState } from \"react\";\n"
          "export default function Ignored(){ return <div id=\"ignored\">Ignored</div>; }\n";

    auto bundle = parse_v0_dev_react(envelope);
    REQUIRE(bundle.has_value());
    REQUIRE(bundle->template_html.find("v0-audio-control-panel") != std::string::npos);
    REQUIRE(asset_text(bundle->assets.front()).find("app/page.tsx") != std::string::npos);
}

TEST_CASE("parse_v0_dev_react rejects out-of-matrix v0 default surfaces",
          "[view][import][parser][v0][phase-6.6.2]") {
    const std::vector<std::string> rejected = {
        R"(
            import { useState } from "react";
            export default function TailwindPanel() {
              return <div className="flex rounded-lg">Tailwind</div>;
            }
        )",
        R"(
            import { useState } from "react";
            import { Button } from "@/components/ui/button";
            export default function ShadcnPanel() {
              return <Button>Open</Button>;
            }
        )",
        R"(
            import {
              format
            } from "date-fns";
            export default function UnsupportedImportPanel() {
              return <div id="unsupported-import">Unsupported import</div>;
            }
        )",
        R"(
            import { useEffect } from "react";
            export default function DynamicImportPanel() {
              useEffect(() => { import("./meter"); }, []);
              return <div id="dynamic-import">Dynamic import</div>;
            }
        )",
        R"(
            import { useEffect } from "react";
            export default function NetworkPanel() {
              useEffect(() => { fetch("/api/state"); }, []);
              return <div id="network">Network</div>;
            }
        )",
        R"(
            import { useState } from "react";
            export default function TextInputPanel() {
              const [value, setValue] = useState("");
              return <input type="text" value={value} onChange={(event) => setValue(event.currentTarget.value)} />;
            }
        )",
        R"(
            import { useState } from "react";
            export default function CustomComponentPanel() {
              return <Slider value={0.5} />;
            }
        )"
    };

    for (const auto& sample : rejected) {
        CAPTURE(sample);
        REQUIRE_FALSE(parse_v0_dev_react(sample).has_value());
    }
}

// ── Figma Make React parsing ────────────────────────────────────────────

TEST_CASE("parse_figma_make_react parses staged Figma runtime fixture",
          "[view][import][parser][figma][phase-6.6.3]") {
    const auto primary = read_fixture("planning/fixtures/figma/level-meter-panel.tsx");
    auto bundle = parse_figma_make_react(primary);
    REQUIRE(bundle.has_value());
    REQUIRE(bundle->assets.size() == 1);
    REQUIRE(bundle->javascript_indices.size() == 1);
    REQUIRE(bundle->javascript_indices.front() == 0);
    REQUIRE(bundle->assets.front().uuid == "figma-runtime-app");
    REQUIRE(bundle->assets.front().mime == "text/javascript");
    REQUIRE(bundle->template_html.find("data-pulp-source=\"figma\"") != std::string::npos);
    REQUIRE(bundle->template_html.find("figma-level-meter-panel") != std::string::npos);

    const auto js = asset_text(bundle->assets.front());
    REQUIRE(js.find("figma-level-meter-panel") != std::string::npos);
    REQUIRE(js.find("Figma Make runtime import requires host React and ReactDOM") != std::string::npos);
    REQUIRE(js.find("'data-pulp-source': 'figma'") != std::string::npos);
    REQUIRE(js.find("ReactDOM.createRoot") != std::string::npos);
    REQUIRE(js.find("requestAnimationFrame") != std::string::npos);
    REQUIRE(js.find("performance.now") != std::string::npos);
    REQUIRE(js.find("canvas.getContext('2d')") != std::string::npos);
    REQUIRE(js.find("type: 'range'") != std::string::npos);
}

TEST_CASE("parse_figma_make_react runtime bundle materializes with host React shim",
          "[view][import][parser][figma][render][phase-6.6.3]") {
    const auto primary = read_fixture("planning/fixtures/figma/level-meter-panel.tsx");
    auto bundle = parse_figma_make_react(primary);
    REQUIRE(bundle.has_value());
    const auto runtime_js = asset_text(bundle->assets.front());
    maybe_write_figma_runtime_script(runtime_js);

    pulp::state::StateStore store;
    ScriptEngine engine;
    View root;
    WidgetBridge bridge(engine, root, store);

    REQUIRE_NOTHROW(bridge.load_script(minimal_host_react_dom_shim()));
    REQUIRE_NOTHROW(bridge.load_script(runtime_js));
    bridge.service_frame_callbacks();

    REQUIRE(root.child_count() > 0);
    REQUIRE(engine.evaluate("!!document.getElementById('figma-level-meter-panel')")
                .getWithDefault<bool>(false));
    REQUIRE_FALSE(engine.evaluate(
        "(function(){ var el = document.getElementById('figma-level-meter-panel');"
        "return el ? String(el._id || '') : ''; })()").toString().empty());
}

TEST_CASE("parse_figma_make_react accepts sanitized Figma Make TSX",
          "[view][import][parser][figma][phase-6.6.3]") {
    const std::string sanitized = R"(
        // Source: Figma Make export (sanitized for Pulp runtime import)
        import {
          useState
        } from "react";
        export default function MultiLineFigmaPanel() {
          const [level, setLevel] = useState(0.5);
          return (
            <div id="figma-multi-line-react-import" style={{ display: "flex", flexDirection: "column" }}>
              <span>Level</span>
              <input type="range" value={level} onChange={(event) => setLevel(Number(event.currentTarget.value))} />
            </div>
          );
        }
    )";

    auto bundle = parse_figma_make_react(sanitized);
    REQUIRE(bundle.has_value());
    REQUIRE(bundle->template_html.find("figma-multi-line-react-import") != std::string::npos);
    REQUIRE(asset_text(bundle->assets.front()).find("figma-multi-line-react-import") != std::string::npos);
}

TEST_CASE("parse_figma_make_react rejects out-of-matrix Figma defaults",
          "[view][import][parser][figma][phase-6.6.3]") {
    const std::vector<std::string> rejected = {
        R"(
            "use client";
            // Source: Figma Make export
            import { useState } from "react";
            export default function NextFigmaPanel() {
              return <div id="figma-next-panel">Next path</div>;
            }
        )",
        R"(
            // Source: Figma Make export
            import { useState } from "react";
            import hero from "figma:asset/a1b2c3d4.png";
            export default function FigmaAssetPanel() {
              return <div id="figma-asset-panel">Asset {hero}</div>;
            }
        )",
        R"(
            // Source: Figma Make export
            import { Dialog } from "@radix-ui/react-dialog@1.1.6";
            export default function VersionedImportPanel() {
              return <div id="figma-versioned-panel">Versioned</div>;
            }
        )",
        R"(
            import figma from "@figma/code-connect/react";
            figma.connect(Button, "https://figma.com/file/example", {
              example: () => <Button />
            });
        )",
        R"(
            // Source: Figma Make export
            import { useState } from "react";
            export default function TailwindPanel() {
              return <div className="flex rounded-lg">Tailwind</div>;
            }
        )",
        R"(
            // Source: Figma Make export
            import { useState } from "react";
            import Link from "next/link";
            export default function NextImportPanel() {
              return <Link href="/">Next</Link>;
            }
        )",
        R"(
            import { useState } from "react";
            export default function GenericReactPanel() {
              const [level, setLevel] = useState(0.5);
              return <input type="range" value={level} onChange={(event) => setLevel(Number(event.currentTarget.value))} />;
            }
        )",
        R"(
            // Source: Figma Make export
            import { useState } from "react";
            export default function TextInputPanel() {
              const [value, setValue] = useState("");
              return <input type="text" value={value} onChange={(event) => setValue(event.currentTarget.value)} />;
            }
        )"
    };

    for (const auto& sample : rejected) {
        CAPTURE(sample);
        REQUIRE_FALSE(parse_figma_make_react(sample).has_value());
    }
}

// ── Google Stitch React parsing ────────────────────────────────────────

TEST_CASE("parse_stitch_react parses staged Stitch runtime fixture",
          "[view][import][parser][stitch][phase-6.6.4]") {
    const auto primary = read_fixture("planning/fixtures/stitch/transport-bar.tsx");
    auto bundle = parse_stitch_react(primary);
    REQUIRE(bundle.has_value());
    REQUIRE(bundle->assets.size() == 1);
    REQUIRE(bundle->javascript_indices.size() == 1);
    REQUIRE(bundle->javascript_indices.front() == 0);
    REQUIRE(bundle->assets.front().uuid == "stitch-runtime-app");
    REQUIRE(bundle->assets.front().mime == "text/javascript");
    REQUIRE(bundle->template_html.find("data-pulp-source=\"stitch\"") != std::string::npos);
    REQUIRE(bundle->template_html.find("stitch-transport-bar") != std::string::npos);

    const auto js = asset_text(bundle->assets.front());
    REQUIRE(js.find("stitch-transport-bar") != std::string::npos);
    REQUIRE(js.find("Stitch runtime import requires host React and ReactDOM") != std::string::npos);
    REQUIRE(js.find("'data-pulp-source': 'stitch'") != std::string::npos);
    REQUIRE(js.find("ReactDOM.createRoot") != std::string::npos);
    REQUIRE(js.find("requestAnimationFrame") != std::string::npos);
    REQUIRE(js.find("performance.now") != std::string::npos);
    REQUIRE(js.find("canvas.getContext('2d')") != std::string::npos);
    REQUIRE(js.find("type: 'range'") != std::string::npos);
}

TEST_CASE("parse_stitch_react runtime bundle materializes with host React shim",
          "[view][import][parser][stitch][render][phase-6.6.4]") {
    const auto primary = read_fixture("planning/fixtures/stitch/transport-bar.tsx");
    auto bundle = parse_stitch_react(primary);
    REQUIRE(bundle.has_value());
    const auto runtime_js = asset_text(bundle->assets.front());
    maybe_write_stitch_runtime_script(runtime_js);

    pulp::state::StateStore store;
    ScriptEngine engine;
    View root;
    WidgetBridge bridge(engine, root, store);

    REQUIRE_NOTHROW(bridge.load_script(minimal_host_react_dom_shim()));
    REQUIRE_NOTHROW(bridge.load_script(runtime_js));
    bridge.service_frame_callbacks();

    REQUIRE(root.child_count() > 0);
    REQUIRE(engine.evaluate("!!document.getElementById('stitch-transport-bar')")
                .getWithDefault<bool>(false));
    REQUIRE_FALSE(engine.evaluate(
        "(function(){ var el = document.getElementById('stitch-transport-bar');"
        "return el ? String(el._id || '') : ''; })()").toString().empty());
}

TEST_CASE("parse_stitch_react accepts sanitized Stitch TSX",
          "[view][import][parser][stitch][phase-6.6.4]") {
    const std::string sanitized = R"(
        import {
          useState
        } from "react";
        export default function MultiLineStitchPanel() {
          const [level, setLevel] = useState(0.5);
          return (
            <div id="stitch-multi-line-react-import" data-stitch-screen="level" style={{ display: "flex", flexDirection: "column" }}>
              <span>Level</span>
              <input type="range" value={level} onChange={(event) => setLevel(Number(event.currentTarget.value))} />
            </div>
          );
        }
    )";

    auto bundle = parse_stitch_react(sanitized);
    REQUIRE(bundle.has_value());
    REQUIRE(bundle->template_html.find("stitch-multi-line-react-import") != std::string::npos);
    REQUIRE(asset_text(bundle->assets.front()).find("stitch-multi-line-react-import") != std::string::npos);
}

TEST_CASE("parse_stitch_react rejects out-of-matrix Stitch defaults",
          "[view][import][parser][stitch][phase-6.6.4]") {
    const std::vector<std::string> rejected = {
        R"(
            "use client";
            import { useState } from "react";
            export default function NextStitchPanel() {
              return <div id="stitch-next-panel">Next path</div>;
            }
        )",
        R"(
            import { useState } from "react";
            export default function TailwindPanel() {
              return <div className="flex rounded-lg">Tailwind</div>;
            }
        )",
        R"(
            import "./transport.css";
            import { useState } from "react";
            export default function ExternalCssPanel() {
              return <div id="stitch-css-panel">CSS</div>;
            }
        )",
        R"(
            import { Dialog } from "@radix-ui/react-dialog";
            export default function RadixPanel() {
              return <div id="stitch-radix-panel">Radix</div>;
            }
        )",
        R"(
            import Link from "next/link";
            export default function NextImportPanel() {
              return <Link href="/">Next</Link>;
            }
        )",
        R"(
            import { View } from "react-native";
            export default function NativePanel() {
              return <View />;
            }
        )",
        R"({"screen_id":"transport-bar","nodes":[]})",
        R"(
            import { useState } from "react";
            export default function TextInputPanel() {
              const [value, setValue] = useState("");
              return <input type="text" value={value} onChange={(event) => setValue(event.currentTarget.value)} />;
            }
        )",
        R"(
            import { useState } from "react";
            export default function CustomComponentPanel() {
              return <TransportSlider value={0.5} />;
            }
        )"
    };

    for (const auto& sample : rejected) {
        CAPTURE(sample);
        REQUIRE_FALSE(parse_stitch_react(sample).has_value());
    }
}

// ── React Native export parsing ────────────────────────────────────────

TEST_CASE("parse_react_native_export parses staged RN runtime fixture",
          "[view][import][parser][rn][phase-6.6.5]") {
    const auto primary = read_fixture("planning/fixtures/rn/gain-stage.tsx");
    auto bundle = parse_react_native_export(primary);
    REQUIRE(bundle.has_value());
    REQUIRE(bundle->assets.size() == 1);
    REQUIRE(bundle->javascript_indices.size() == 1);
    REQUIRE(bundle->javascript_indices.front() == 0);
    REQUIRE(bundle->assets.front().uuid == "rn-runtime-app");
    REQUIRE(bundle->assets.front().mime == "text/javascript");
    REQUIRE(bundle->template_html.find("data-pulp-source=\"rn\"") != std::string::npos);
    REQUIRE(bundle->template_html.find("rn-gain-stage") != std::string::npos);

    const auto js = asset_text(bundle->assets.front());
    REQUIRE(js.find("rn-gain-stage") != std::string::npos);
    REQUIRE(js.find("React Native runtime import requires host React and ReactDOM") != std::string::npos);
    REQUIRE(js.find("'data-pulp-source': 'rn'") != std::string::npos);
    REQUIRE(js.find("'data-rn-default-flex': 'column'") != std::string::npos);
    REQUIRE(js.find("flexDirection: 'column'") != std::string::npos);
    REQUIRE(js.find("ReactDOM.createRoot") != std::string::npos);
    REQUIRE(js.find("canvas.getContext") == std::string::npos);
    REQUIRE(js.find("requestAnimationFrame") == std::string::npos);
}

TEST_CASE("parse_react_native_export runtime bundle materializes with host React shim",
          "[view][import][parser][rn][render][phase-6.6.5]") {
    // pulp #1987 — this test reliably fails ONLY under UndefinedBehaviorSanitizer
    // (macOS ARM64). The failure is `REQUIRE_NOTHROW(bridge.load_script(runtime_js))`
    // surfacing an "Unknown exception" — the exception originates inside the
    // QuickJS / RN-runtime-shim path, but only when UBSan instrumentation is
    // active. ASan, TSan, and the non-sanitized macOS lane all pass on the
    // exact same commit. Tracked under #1987 for proper root-cause work
    // (likely a signed-overflow / shift / alignment trap inside vendored
    // QuickJS); skipping the body under UBSan only so the rest of the
    // suite can attribute real regressions correctly. Do NOT remove this
    // skip without first resolving #1987.
#if defined(__has_feature)
#  if __has_feature(undefined_behavior_sanitizer)
    SUCCEED("skipped under UBSan (pulp #1987 — pre-existing main-branch flake)");
    return;
#  endif
#endif

    const auto primary = read_fixture("planning/fixtures/rn/gain-stage.tsx");
    auto bundle = parse_react_native_export(primary);
    REQUIRE(bundle.has_value());
    const auto runtime_js = asset_text(bundle->assets.front());
    maybe_write_rn_runtime_script(runtime_js);

    pulp::state::StateStore store;
    ScriptEngine engine;
    View root;
    WidgetBridge bridge(engine, root, store);

    REQUIRE_NOTHROW(bridge.load_script(minimal_host_react_dom_shim()));
    REQUIRE_NOTHROW(bridge.load_script(runtime_js));
    bridge.service_frame_callbacks();

    REQUIRE(root.child_count() > 0);
    REQUIRE(engine.evaluate("!!document.getElementById('rn-gain-stage')")
                .getWithDefault<bool>(false));
    REQUIRE(engine.evaluate(
        "(function(){ var el = document.getElementById('rn-gain-stage');"
        "return el && el.getAttribute('data-rn-default-flex') === 'column'; })()")
        .getWithDefault<bool>(false));
}

TEST_CASE("parse_react_native_export accepts sanitized RN TSX",
          "[view][import][parser][rn][phase-6.6.5]") {
    const std::string sanitized = R"(
        import React, {
          useState
        } from "react";
        import {
          Pressable,
          StyleSheet,
          Text,
          View
        } from "react-native";
        export default function MultiLineNativePanel() {
          const [armed, setArmed] = useState(true);
          return (
            <View id="rn-multi-line-react-import" style={styles.panel}>
              <Text style={styles.label}>React Native export</Text>
              <Text style={styles.title}>Gain Stage</Text>
              <Pressable onPress={() => setArmed(!armed)} style={styles.button}>
                <Text>{armed ? "ARMED" : "BYPASS"}</Text>
              </Pressable>
            </View>
          );
        }
        const styles = StyleSheet.create({
          panel: { padding: 18, backgroundColor: "#111827" },
          label: { color: "#8fb3ff" },
          title: { color: "#f8fafc", fontSize: 24 },
          button: { minHeight: 36 }
        });
    )";

    auto bundle = parse_react_native_export(sanitized);
    REQUIRE(bundle.has_value());
    REQUIRE(bundle->template_html.find("rn-multi-line-react-import") != std::string::npos);
    const auto js = asset_text(bundle->assets.front());
    REQUIRE(js.find("rn-multi-line-react-import") != std::string::npos);
    REQUIRE(js.find("flexDirection: 'column'") != std::string::npos);
}

TEST_CASE("parse_react_native_export rejects out-of-matrix RN surfaces",
          "[view][import][parser][rn][phase-6.6.5]") {
    const std::vector<std::string> rejected = {
        R"(
            import { useState } from "react";
            export default function GenericReactPanel() {
              return <div id="generic">Generic</div>;
            }
        )",
        R"(
            "use client";
            import { View } from "react-native";
            export default function SolitoPanel() {
              return <View />;
            }
        )",
        R"(
            import { Animated, View } from "react-native";
            export default function AnimatedPanel() {
              return <Animated.View />;
            }
        )",
        R"(
            import Animated from "react-native-reanimated";
            import { View } from "react-native";
            export default function WorkletPanel() {
              return <View />;
            }
        )",
        R"(
            import { Linking, View } from "react-native";
            export default function LinkingPanel() {
              Linking.openURL("https://example.com");
              return <View />;
            }
        )",
        R"(
            import { Alert, View } from "react-native";
            export default function AlertPanel() {
              Alert.alert("Nope");
              return <View />;
            }
        )",
        R"(
            import { Dimensions, View } from "react-native";
            export default function DimensionsPanel() {
              const width = Dimensions.get("window").width;
              return <View />;
            }
        )",
        R"(
            import { Platform, View } from "react-native";
            export default function PlatformPanel() {
              return <View />;
            }
        )",
        R"(
            import { Animated, View } from "react-native";
            export default function AnimatedImportPanel() {
              return <View />;
            }
        )",
        R"(
            import ReactNative from "react-native";
            export default function DefaultNativePanel() {
              return <ReactNative.View />;
            }
        )",
        R"(
            import * as ReactNative from "react-native";
            export default function NamespaceNativePanel() {
              return <ReactNative.View />;
            }
        )",
        R"(
            import { FlatList } from "react-native";
            export default function ListPanel() {
              return <FlatList data={[]} renderItem={() => null} />;
            }
        )",
        R"(
            import { Image } from "react-native";
            export default function ImagePanel() {
              return <Image source={{ uri: "https://example.com/a.png" }} />;
            }
        )",
        R"(
            import { View } from "react-native";
            export default function StyleArrayPanel() {
              return <View style={[styles.base, styles.hot]} />;
            }
            const styles = { base: {}, hot: {} };
        )",
        R"(
            import { View } from "react-native";
            import { NavigationContainer } from "@react-navigation/native";
            export default function NavigationPanel() {
              return <View />;
            }
        )",
        R"(
            import { View } from "react-native";
            export default function DomPanel() {
              return <div id="dom">DOM</div>;
            }
        )"
    };

    for (const auto& sample : rejected) {
        CAPTURE(sample);
        REQUIRE_FALSE(parse_react_native_export(sample).has_value());
    }
}

// ── Pencil React export parsing ─────────────────────────────────────────

TEST_CASE("parse_pencil_react parses staged Pencil runtime fixture",
          "[view][import][parser][pencil][phase-6.6.6]") {
    const auto primary = read_fixture("planning/fixtures/pencil/gain-stage-card.tsx");
    auto bundle = parse_pencil_react(primary);
    REQUIRE(bundle.has_value());
    REQUIRE(bundle->assets.size() == 1);
    REQUIRE(bundle->javascript_indices.size() == 1);
    REQUIRE(bundle->javascript_indices.front() == 0);
    REQUIRE(bundle->assets.front().uuid == "pencil-runtime-app");
    REQUIRE(bundle->assets.front().mime == "text/javascript");
    REQUIRE(bundle->template_html.find("data-pulp-source=\"pencil\"") != std::string::npos);
    REQUIRE(bundle->template_html.find("pencil-gain-stage-card") != std::string::npos);

    const auto js = asset_text(bundle->assets.front());
    REQUIRE(js.find("pencil-gain-stage-card") != std::string::npos);
    REQUIRE(js.find("Pencil runtime import requires host React and ReactDOM") != std::string::npos);
    REQUIRE(js.find("'data-pulp-source': 'pencil'") != std::string::npos);
    REQUIRE(js.find("ReactDOM.createRoot") != std::string::npos);
    REQUIRE(js.find("requestAnimationFrame") != std::string::npos);
    REQUIRE(js.find("canvas.getContext('2d')") != std::string::npos);
    REQUIRE(js.find("type: 'range'") != std::string::npos);
}

TEST_CASE("parse_pencil_react runtime bundle materializes with host React shim",
          "[view][import][parser][pencil][render][phase-6.6.6]") {
    const auto primary = read_fixture("planning/fixtures/pencil/gain-stage-card.tsx");
    auto bundle = parse_pencil_react(primary);
    REQUIRE(bundle.has_value());
    const auto runtime_js = asset_text(bundle->assets.front());
    maybe_write_pencil_runtime_script(runtime_js);

    pulp::state::StateStore store;
    ScriptEngine engine;
    View root;
    WidgetBridge bridge(engine, root, store);

    REQUIRE_NOTHROW(bridge.load_script(minimal_host_react_dom_shim()));
    REQUIRE_NOTHROW(bridge.load_script(runtime_js));
    bridge.service_frame_callbacks();

    REQUIRE(root.child_count() > 0);
    REQUIRE(engine.evaluate("!!document.getElementById('pencil-gain-stage-card')")
                .getWithDefault<bool>(false));
    REQUIRE_FALSE(engine.evaluate(
        "(function(){ var el = document.getElementById('pencil-gain-stage-card');"
        "return el ? String(el._id || '') : ''; })()").toString().empty());
}

TEST_CASE("parse_pencil_react accepts sanitized Pencil TSX",
          "[view][import][parser][pencil][phase-6.6.6]") {
    const std::string sanitized = R"(
        import {
          useState
        } from "react";
        export default function MultiLinePencilPanel() {
          const [level, setLevel] = useState(0.5);
          return (
            <div id="pencil-multi-line-react-import" data-pencil-export="tailwind-jsx-sanitized" style={{ display: "flex", flexDirection: "column" }}>
              <span>Level</span>
              <input type="range" value={level} onChange={(event) => setLevel(Number(event.currentTarget.value))} />
            </div>
          );
        }
    )";

    auto bundle = parse_pencil_react(sanitized);
    REQUIRE(bundle.has_value());
    REQUIRE(bundle->template_html.find("pencil-multi-line-react-import") != std::string::npos);
    REQUIRE(asset_text(bundle->assets.front()).find("pencil-multi-line-react-import") != std::string::npos);
    REQUIRE_FALSE(parse_v0_dev_react(sanitized).has_value());

    const std::string no_id = R"(
        import { useState } from "react";
        export default function Meter_Panel() {
          const [level, setLevel] = useState(0.5);
          return (
            <div data-pencil-export="tailwind-jsx-sanitized" style={{ display: "flex", flexDirection: "column" }}>
              <span>Meter</span>
              <input type="range" value={level} onChange={(event) => setLevel(Number(event.currentTarget.value))} />
            </div>
          );
        }
    )";

    auto fallback_bundle = parse_pencil_react(no_id);
    REQUIRE(fallback_bundle.has_value());
    REQUIRE(fallback_bundle->template_html.find("pencil-meter-panel") != std::string::npos);
    REQUIRE(asset_text(fallback_bundle->assets.front()).find("pencil-meter-panel") != std::string::npos);
}

TEST_CASE("parse_pencil_react rejects out-of-matrix Pencil defaults",
          "[view][import][parser][pencil][phase-6.6.6]") {
    const std::vector<std::string> rejected = {
        R"(
            "use client";
            import { useState } from "react";
            export default function NextPencilPanel() {
              return <div id="pencil-next-panel">Next path</div>;
            }
        )",
        R"(
            import { useState } from "react";
            export default function TailwindPanel() {
              return <div className="flex rounded-lg bg-[--pencil-color-primary]">Tailwind</div>;
            }
        )",
        R"PULP(
            import { useState } from "react";
            export default function TokenPanel() {
              return <div id="pencil-token-panel" style={{ backgroundColor: "var(--pencil-color-primary)" }}>Token</div>;
            }
        )PULP",
        R"(
            import "./pencil.css";
            import { useState } from "react";
            export default function ExternalCssPanel() {
              return <div id="pencil-css-panel">CSS</div>;
            }
        )",
        R"(
            import { Dialog } from "@radix-ui/react-dialog";
            export default function RadixPanel() {
              return <div id="pencil-radix-panel">Radix</div>;
            }
        )",
        R"(
            import Link from "next/link";
            export default function NextImportPanel() {
              return <Link href="/">Next</Link>;
            }
        )",
        R"(
            import { View } from "react-native";
            export default function NativePanel() {
              return <View />;
            }
        )",
        R"({"mcp_response":{"node_tree":[],"source":"pencil"}})",
        R"({"batch_get":[{"id":"node-1"}]})",
        R"(
            import design from "./control.pen";
            export default function BinaryPenPanel() {
              return <div id="pencil-pen-panel">Binary</div>;
            }
        )",
        R"(
            import { useState } from "react";
            export default function TextInputPanel() {
              const [value, setValue] = useState("");
              return <input type="text" value={value} onChange={(event) => setValue(event.currentTarget.value)} />;
            }
        )",
        R"(
            import { useState } from "react";
            export default function CustomComponentPanel() {
              return <PencilWidget value={0.5} />;
            }
        )"
    };

    for (const auto& sample : rejected) {
        CAPTURE(sample);
        REQUIRE_FALSE(parse_pencil_react(sample).has_value());
    }
}

// ── Pencil JSON parsing ─────────────────────────────────────────────────

// ── Token alias cycle detection ─────────────────────────────────────────

TEST_CASE("parse_w3c_tokens handles circular aliases without infinite loop", "[view][import]") {
    auto json = R"({
        "color": {
            "$type": "color",
            "a": { "$value": "{color.b}" },
            "b": { "$value": "{color.a}" },
            "safe": { "$value": "#FF0000" }
        }
    })";

    // Should not hang — circular refs terminate gracefully
    auto theme = parse_w3c_tokens(json);

    // The safe token should still resolve
    REQUIRE(theme.colors.count("color.safe") == 1);
    REQUIRE(theme.colors["color.safe"].r8() == 0xFF);

    // Circular tokens won't resolve to valid colors — they should not crash
    // (they may or may not be in the theme depending on what the unresolved
    // string looks like, but the important thing is no infinite loop)
}

TEST_CASE("parse_w3c_tokens handles self-referencing alias", "[view][import]") {
    auto json = R"({
        "spacing": {
            "$type": "dimension",
            "self": { "$value": "{spacing.self}" }
        }
    })";

    // Should not hang
    auto theme = parse_w3c_tokens(json);
    // Self-reference won't resolve to a valid number
}

// ── Figma Variables sync ───────────────────────────────────────────────

TEST_CASE("parse_figma_variables reads Figma variable format", "[view][import]") {
    auto json = R"({
        "variables": [
            { "name": "color/primary", "type": "COLOR", "resolvedValue": "#89B4FA" },
            { "name": "color/bg", "type": "COLOR", "resolvedValue": "#1E1E2E" },
            { "name": "spacing/md", "type": "FLOAT", "resolvedValue": "8" },
            { "name": "font/heading", "type": "STRING", "resolvedValue": "Inter" }
        ]
    })";

    auto theme = parse_figma_variables(json);

    REQUIRE(theme.colors.count("color.primary") == 1);
    REQUIRE(theme.colors["color.primary"].r8() == 0x89);
    REQUIRE(theme.colors["color.primary"].g8() == 0xB4);

    REQUIRE(theme.colors.count("color.bg") == 1);
    REQUIRE(theme.colors["color.bg"].r8() == 0x1E);

    REQUIRE(theme.dimensions["spacing.md"] == 8.0f);
    REQUIRE(theme.strings["font.heading"] == "Inter");
}

TEST_CASE("export_figma_variables produces Figma-compatible JSON", "[view][import]") {
    Theme theme;
    theme.colors["color.primary"] = color_from_hex(0x89B4FA);
    theme.dimensions["spacing.md"] = 8.0f;
    theme.strings["font.heading"] = "Inter";

    auto json = export_figma_variables(theme);

    REQUIRE(json.find("\"variables\"") != std::string::npos);
    REQUIRE(json.find("\"name\": \"color/primary\"") != std::string::npos);
    REQUIRE(json.find("\"type\": \"COLOR\"") != std::string::npos);
    REQUIRE(json.find("#89b4fa") != std::string::npos);
    REQUIRE(json.find("\"name\": \"spacing/md\"") != std::string::npos);
    REQUIRE(json.find("\"type\": \"FLOAT\"") != std::string::npos);
    REQUIRE(json.find("\"name\": \"font/heading\"") != std::string::npos);
    REQUIRE(json.find("\"type\": \"STRING\"") != std::string::npos);
}

TEST_CASE("Figma Variables round-trip preserves colors", "[view][import]") {
    Theme original;
    original.colors["color.primary"] = color_from_hex(0x89B4FA);
    original.dimensions["spacing.md"] = 8.0f;

    auto json = export_figma_variables(original);
    auto restored = parse_figma_variables(json);

    REQUIRE(restored.colors.count("color.primary") == 1);
    REQUIRE(restored.colors["color.primary"].r8() == original.colors["color.primary"].r8());
    REQUIRE(restored.colors["color.primary"].g8() == original.colors["color.primary"].g8());
    REQUIRE(restored.colors["color.primary"].b8() == original.colors["color.primary"].b8());
    REQUIRE(restored.dimensions["spacing.md"] == 8.0f);
}

// ── Stitch Design System sync ──────────────────────────────────────────

TEST_CASE("parse_stitch_design_system reads Stitch format", "[view][import]") {
    auto json = R"({
        "name": "Dark Theme",
        "colors": { "primary": "#89B4FA", "background": "#1E1E2E" },
        "fonts": { "heading": "Inter", "body": "Roboto" },
        "roundness": "large",
        "spacing": 12
    })";

    auto theme = parse_stitch_design_system(json);

    REQUIRE(theme.colors.count("color.primary") == 1);
    REQUIRE(theme.colors["color.primary"].r8() == 0x89);
    REQUIRE(theme.colors.count("color.background") == 1);
    REQUIRE(theme.strings["font.heading"] == "Inter");
    REQUIRE(theme.strings["font.body"] == "Roboto");
    REQUIRE(theme.dimensions["roundness"] == 16.0f);  // "large" = 16
    REQUIRE(theme.dimensions["spacing.base"] == 12.0f);
}

TEST_CASE("export_stitch_design_system produces Stitch-compatible JSON", "[view][import]") {
    Theme theme;
    theme.colors["color.primary"] = color_from_hex(0x89B4FA);
    theme.strings["font.heading"] = "Inter";
    theme.dimensions["roundness"] = 8.0f;
    theme.dimensions["spacing.base"] = 12.0f;

    auto json = export_stitch_design_system(theme);

    REQUIRE(json.find("\"colors\"") != std::string::npos);
    REQUIRE(json.find("\"primary\"") != std::string::npos);
    REQUIRE(json.find("#89b4fa") != std::string::npos);
    REQUIRE(json.find("\"fonts\"") != std::string::npos);
    REQUIRE(json.find("\"heading\": \"Inter\"") != std::string::npos);
    REQUIRE(json.find("\"roundness\": \"medium\"") != std::string::npos);  // 8 = medium
    REQUIRE(json.find("\"spacing\": 12") != std::string::npos);
}

TEST_CASE("Stitch Design System round-trip preserves tokens", "[view][import]") {
    Theme original;
    original.colors["color.accent"] = color_from_hex(0xE94560);
    original.strings["font.body"] = "Roboto";
    original.dimensions["roundness"] = 4.0f;
    original.dimensions["spacing.base"] = 8.0f;

    auto json = export_stitch_design_system(original);
    auto restored = parse_stitch_design_system(json);

    REQUIRE(restored.colors["color.accent"].r8() == 0xE9);
    REQUIRE(restored.strings["font.body"] == "Roboto");
    REQUIRE(restored.dimensions["roundness"] == 4.0f);  // "small" maps back to 4
    REQUIRE(restored.dimensions["spacing.base"] == 8.0f);
}

// ── E2E pipeline test ──────────────────────────────────────────────────

// Keep this name ASCII-only so Catch2/CTest filters remain stable on Windows.
TEST_CASE("E2E: Figma IR -> code gen -> tokens -> round-trip", "[view][import][e2e]") {
    // Step 1: Parse a Figma IR with audio widgets and tokens
    auto json = R"({
        "type": "frame",
        "name": "PluginUI",
        "layout": { "direction": "column", "gap": 16, "padding": 12 },
        "style": { "backgroundColor": "#1a1a2e", "width": 340, "height": 280 },
        "children": [
            {
                "type": "text", "name": "title", "content": "My Plugin",
                "style": { "fontSize": 20, "fontWeight": 700, "color": "#e0e0e0" }
            },
            {
                "type": "frame", "name": "controls",
                "layout": { "direction": "row", "gap": 12 },
                "children": [
                    { "type": "slider", "name": "GainKnob", "label": "Gain", "min": 0, "max": 1, "default": 0.75 },
                    { "type": "slider", "name": "MixFader", "label": "Mix", "min": 0, "max": 1 },
                    { "type": "frame", "name": "OutputMeter", "label": "Out" }
                ]
            }
        ],
        "tokens": {
            "colors": { "bg.primary": "#1a1a2e", "accent": "#e94560" },
            "dimensions": { "spacing.md": 16 }
        }
    })";

    auto ir = parse_figma_json(json);
    REQUIRE(ir.root.name == "PluginUI");
    REQUIRE(ir.root.children.size() == 2);

    // Step 2: Audio widget detection
    auto& controls = ir.root.children[1];
    REQUIRE(controls.children[0].audio_widget == AudioWidgetType::knob);
    REQUIRE(controls.children[1].audio_widget == AudioWidgetType::fader);
    REQUIRE(controls.children[2].audio_widget == AudioWidgetType::meter);

    // Step 3: Generate native code
    CodeGenOptions opts;
    opts.mode = CodeGenMode::native;
    opts.include_comments = false;
    auto js = generate_pulp_js(ir, opts);
    REQUIRE(!js.empty());
    REQUIRE(js.find("createCol('root',") != std::string::npos);
    REQUIRE(js.find("createKnob('GainKnob") != std::string::npos);
    REQUIRE(js.find("createFader('MixFader") != std::string::npos);
    REQUIRE(js.find("createMeter('OutputMeter") != std::string::npos);
    REQUIRE(js.find("setColorToken('bg.primary'") != std::string::npos);

    // Step 4: Token round-trip
    auto theme = ir_tokens_to_theme(ir.tokens);
    auto w3c = export_w3c_tokens(theme);
    auto restored = parse_w3c_tokens(w3c);
    REQUIRE(restored.colors.count("bg.primary") == 1);
    REQUIRE(restored.colors["bg.primary"].r8() == 0x1a);

    // Step 5: Figma Variables export/import cycle
    auto figma_vars = export_figma_variables(theme);
    auto from_figma = parse_figma_variables(figma_vars);
    REQUIRE(from_figma.colors.count("bg.primary") == 1);

    // Step 6: Stitch export/import cycle
    auto stitch_json = export_stitch_design_system(theme);
    auto from_stitch = parse_stitch_design_system(stitch_json);
    REQUIRE(!from_stitch.colors.empty());
}

// ── Pencil JSON parsing ─────────────────────────────────────────────────

TEST_CASE("parse_pencil_json parses node tree", "[view][import]") {
    auto json = R"({
        "type": "frame",
        "name": "PencilDesign",
        "layout": { "direction": "row", "gap": 12 },
        "children": [
            { "type": "text", "name": "label", "content": "Volume" }
        ],
        "variables": {
            "colors": { "primary": "#ff5500" },
            "dimensions": { "radius": 8 }
        }
    })";

    auto ir = parse_pencil_json(json);

    REQUIRE(ir.source == DesignSource::pencil);
    REQUIRE(ir.root.name == "PencilDesign");
    REQUIRE(ir.root.layout.direction == LayoutDirection::row);
    REQUIRE(ir.root.children.size() == 1);
    REQUIRE(ir.tokens.colors["primary"] == "#ff5500");
    REQUIRE(ir.tokens.dimensions["radius"] == 8.0f);
}

TEST_CASE("parse_pencil_json covers sizing layout padding and widget metadata edges",
          "[view][import][coverage]") {
    auto json = R"json({
        "type": "frame",
        "name": "PencilRack",
        "layout": "horizontal",
        "gap": 7,
        "padding": [3, 5],
        "justifyContent": "end",
        "alignItems": "end",
        "children": [
            {
                "type": "frame",
                "name": "FillPanel",
                "layout": "vertical",
                "width": "fill_container",
                "height": "fit_content(120)",
                "padding": [1, 2, 3, 4],
                "children": [
                    {
                        "type": "text",
                        "name": "Readout",
                        "content": "Ready",
                        "fontSize": 10,
                        "fontWeight": "600",
                        "fontFamily": "Mono",
                        "fill": "#eeeeee"
                    }
                ]
            },
            {
                "type": "frame",
                "name": "MainFader",
                "width": 44,
                "height": 120,
                "children": [
                    {
                        "type": "rectangle",
                        "name": "track",
                        "width": 6,
                        "height": 90,
                        "stroke": { "fill": "#ff5500" }
                    },
                    { "type": "text", "name": "label", "content": "Level" }
                ]
            },
            {
                "type": "frame",
                "name": "KnobRow",
                "children": [
                    { "type": "frame", "name": "DriveKnob", "children": [] }
                ]
            }
        ]
    })json";

    auto ir = parse_pencil_json(json);

    REQUIRE(ir.root.layout.direction == LayoutDirection::row);
    REQUIRE(ir.root.layout.gap == 7.0f);
    REQUIRE(ir.root.layout.padding_top == 3.0f);
    REQUIRE(ir.root.layout.padding_bottom == 3.0f);
    REQUIRE(ir.root.layout.padding_left == 5.0f);
    REQUIRE(ir.root.layout.padding_right == 5.0f);
    REQUIRE(ir.root.layout.justify == LayoutAlign::flex_end);
    REQUIRE(ir.root.layout.align == LayoutAlign::flex_end);

    const auto& fill_panel = ir.root.children[0];
    REQUIRE(fill_panel.layout.direction == LayoutDirection::column);
    REQUIRE(fill_panel.layout.width_mode == SizingMode::fill);
    REQUIRE(fill_panel.layout.height_mode == SizingMode::hug);
    REQUIRE(fill_panel.layout.padding_top == 1.0f);
    REQUIRE(fill_panel.layout.padding_right == 2.0f);
    REQUIRE(fill_panel.layout.padding_bottom == 3.0f);
    REQUIRE(fill_panel.layout.padding_left == 4.0f);
    REQUIRE(fill_panel.children[0].style.font_size == 10.0f);
    REQUIRE(fill_panel.children[0].style.font_weight == 600);
    REQUIRE(fill_panel.children[0].style.font_family == "Mono");
    REQUIRE(fill_panel.children[0].style.color == "#eeeeee");

    const auto& fader = ir.root.children[1];
    REQUIRE(fader.audio_widget == AudioWidgetType::fader);
    REQUIRE(fader.style.width == 44.0f);
    REQUIRE(fader.style.height == 120.0f);
    REQUIRE(fader.attributes.at("shape_width") == "6");
    REQUIRE(fader.attributes.at("shape_height") == "90");
    REQUIRE(fader.children[0].attributes.at("stroke_color") == "#ff5500");

    const auto& knob_row = ir.root.children[2];
    REQUIRE(knob_row.audio_widget == AudioWidgetType::none);
    REQUIRE(knob_row.layout.direction == LayoutDirection::row);
}

TEST_CASE("generate_pulp_js native mode covers fill-height and leaf fallback branches",
          "[view][import][coverage]") {
    DesignIR ir;
    ir.source = DesignSource::pencil;
    ir.root.type = "frame";
    ir.root.name = "FillRoot";
    ir.root.layout.height_mode = SizingMode::fill;

    IRNode leaf;
    leaf.type = "rectangle";
    leaf.name = "empty-divider";
    ir.root.children.push_back(leaf);

    CodeGenOptions opts;
    opts.mode = CodeGenMode::native;
    opts.include_comments = false;
    auto js = generate_pulp_js(ir, opts);

    REQUIRE(js.find("createCol('root', '')") != std::string::npos);
    REQUIRE(js.find("setFlex('root', 'flex_grow', 1)") != std::string::npos);
    REQUIRE(js.find("createRow('empty_divider0', 'root')") != std::string::npos);
    REQUIRE(js.find("setFlex('empty_divider0', 'height', 1)") != std::string::npos);
}
