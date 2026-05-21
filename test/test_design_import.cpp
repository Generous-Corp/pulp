#include <catch2/catch_test_macros.hpp>
#include <pulp/state/store.hpp>
#include <pulp/view/anchor_strategy.hpp>
#include <pulp/view/design_import.hpp>
#include <pulp/view/script_engine.hpp>
#include <pulp/view/widget_bridge.hpp>

#include <algorithm>
#include <cstdlib>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

using namespace pulp::view;
namespace fs = std::filesystem;

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

class TempDir {
public:
    explicit TempDir(const std::string& prefix) {
        const auto tick = std::chrono::steady_clock::now().time_since_epoch().count();
        path = fs::temp_directory_path() / (prefix + "-" + std::to_string(tick));
        fs::create_directories(path);
    }

    ~TempDir() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }

    fs::path path;
};

void write_text(const fs::path& path, const std::string& text) {
    fs::create_directories(path.parent_path());
    std::ofstream f(path, std::ios::binary);
    REQUIRE(f.is_open());
    f << text;
    REQUIRE(f.good());
}

std::optional<std::string> read_env_var(const char* name) {
    if (const char* value = std::getenv(name); value) return std::string(value);
    return std::nullopt;
}

void set_env_var(const char* name, const std::string& value) {
#ifdef _WIN32
    _putenv_s(name, value.c_str());
#else
    setenv(name, value.c_str(), 1);
#endif
}

void unset_env_var(const char* name) {
#ifdef _WIN32
    _putenv_s(name, "");
#else
    unsetenv(name);
#endif
}

class ScopedEnvVar {
public:
    ScopedEnvVar(const char* name, const std::string& value)
        : name_(name), old_(read_env_var(name)) {
        set_env_var(name_.c_str(), value);
    }

    ~ScopedEnvVar() {
        if (old_) set_env_var(name_.c_str(), *old_);
        else unset_env_var(name_.c_str());
    }

    ScopedEnvVar(const ScopedEnvVar&) = delete;
    ScopedEnvVar& operator=(const ScopedEnvVar&) = delete;

private:
    std::string name_;
    std::optional<std::string> old_;
};

bool has_diagnostic(const IRAssetRef& asset, const std::string& code) {
    for (const auto& diagnostic : asset.diagnostics) {
        if (diagnostic.code == code) return true;
    }
    return false;
}

bool has_import_diagnostic(const std::vector<ImportDiagnostic>& diagnostics,
                           const std::string& code) {
    for (const auto& diagnostic : diagnostics) {
        if (diagnostic.code == code) return true;
    }
    return false;
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
    REQUIRE(parse_design_source("designmd") == DesignSource::designmd);
    REQUIRE(parse_design_source("jsx") == DesignSource::jsx);
    REQUIRE_FALSE(parse_design_source("unknown").has_value());
}

TEST_CASE("design_source_name returns display names", "[view][import]") {
    REQUIRE(std::string(design_source_name(DesignSource::figma)) == "Figma");
    REQUIRE(std::string(design_source_name(DesignSource::v0)) == "v0");
    REQUIRE(std::string(design_source_name(DesignSource::claude)) == "Claude Design");
    REQUIRE(std::string(design_source_name(DesignSource::designmd)) == "DESIGN.md");
    REQUIRE(std::string(design_source_name(DesignSource::jsx)) == "JSX instrument");
}

TEST_CASE("DesignIR v1 canonical JSON round-trips source metadata and assets",
          "[view][import][ir-v1][assets]") {
    DesignIR ir;
    ir.source = DesignSource::stitch;
    ir.source_file = "https://example.test/design.html";
    ir.root.type = "frame";
    ir.root.name = "Panel";
    ir.root.layout.display = "flex";
    ir.root.layout.direction = LayoutDirection::row;
    ir.root.layout.gap = 8.0f;
    ir.root.layout.row_gap = 4.0f;
    ir.root.layout.column_gap = 6.0f;
    ir.root.layout.margin_top = 1.0f;
    ir.root.layout.margin_right = 2.0f;
    ir.root.layout.margin_bottom = 3.0f;
    ir.root.layout.margin_left = 4.0f;
    ir.root.layout.justify = LayoutAlign::space_between;
    ir.root.layout.align = LayoutAlign::center;
    ir.root.layout.align_self = "stretch";
    ir.root.layout.align_content = "space-between";
    ir.root.layout.flex_grow = 1.0f;
    ir.root.layout.flex_shrink = 0.0f;
    ir.root.layout.flex_basis = "auto";
    ir.root.layout.order = 2;
    ir.root.layout.aspect_ratio = 1.5f;
    ir.root.layout.overflow_x = "hidden";
    ir.root.layout.overflow_y = "auto";
    ir.root.layout.width_mode = SizingMode::fill;
    ir.root.layout.height_mode = SizingMode::hug;
    ir.root.style.background_color = "#101010";
    ir.root.style.background_image = "url(data:image/svg+xml,%3Csvg%20xmlns='http://www.w3.org/2000/svg'/%3E)";
    ir.root.style.background_repeat = "no-repeat";
    ir.root.style.border_color = "#ffffff";
    ir.root.style.border_width = 2.0f;
    ir.root.style.border_style = "solid";
    ir.root.style.border_top_left_radius = 3.0f;
    ir.root.style.border_top_right_radius = 4.0f;
    ir.root.style.border_bottom_right_radius = 5.0f;
    ir.root.style.border_bottom_left_radius = 6.0f;
    ir.root.style.backdrop_filter = "blur(4px)";
    ir.root.style.text_decoration = "underline";
    ir.root.style.white_space = "nowrap";
    ir.root.style.text_overflow = "ellipsis";
    ir.root.stable_anchor_id = "stitch:panel";
    ir.root.anchor_strategy = "adapter";
    ir.root.source_node_id = "node-1";
    ir.root.source_adapter = "stitch";
    ir.root.source_version = "1";
    ir.root.attributes["zeta"] = "last";
    ir.root.attributes["alpha"] = "first";

    IRNode label;
    label.type = "text";
    label.name = "Title";
    label.text_content = "Gain";
    ir.root.children.push_back(label);

    ir.tokens.colors["accent"] = "#ff00ff";
    ir.tokens.dimensions["spacing.md"] = 8.0f;
    ir.tokens.strings["copy.title"] = "Gain";
    ir.tokens.source_identity["colors.accent"] = IRTokenIdentity{
        "var-1", "palette", "dark", "stitch"
    };

    refresh_design_ir_asset_manifest(ir);

    const auto canonical = serialize_design_ir(ir);
    const auto parsed = parse_design_ir_json(canonical);
    const auto canonical_again = serialize_design_ir(parsed);

    REQUIRE(canonical_again == canonical);
    REQUIRE(parsed.version == 1);
    REQUIRE(parsed.source == DesignSource::stitch);
    REQUIRE(parsed.source_file == "https://example.test/design.html");
    REQUIRE(parsed.root.layout.display == "flex");
    REQUIRE(parsed.root.layout.flex_grow == 1.0f);
    REQUIRE(parsed.root.layout.overflow_y == "auto");
    REQUIRE(parsed.root.style.background_repeat == "no-repeat");
    REQUIRE(parsed.root.style.border_top_left_radius == 3.0f);
    REQUIRE(parsed.root.style.text_overflow == "ellipsis");
    REQUIRE(parsed.root.stable_anchor_id == "stitch:panel");
    REQUIRE(parsed.root.anchor_strategy == "adapter");
    REQUIRE(parsed.root.source_adapter == "stitch");
    REQUIRE(parsed.root.attributes.at("alpha") == "first");
    REQUIRE(parsed.tokens.source_identity.at("colors.accent").source_mode == "dark");
    REQUIRE(parsed.asset_manifest.version == 1);
    REQUIRE(parsed.asset_manifest.assets.size() == 1);
    REQUIRE(parsed.asset_manifest.assets[0].mime == "image/svg+xml");
    REQUIRE_FALSE(parsed.asset_manifest.assets[0].content_hash.empty());
}

TEST_CASE("DesignIR serialization preserves parsed envelope version by default",
          "[view][import][ir-v1]") {
    auto parsed = parse_design_ir_json(R"json({
        "version": 2,
        "source": "jsx",
        "root": { "type": "frame", "name": "Future IR" }
    })json");

    REQUIRE(parsed.version == 2);

    const auto canonical = serialize_design_ir(parsed);
    REQUIRE(canonical.find("\"version\":2") != std::string::npos);
    REQUIRE(parse_design_ir_json(canonical).version == 2);

    DesignIrJsonOptions force_v1;
    force_v1.version = 1;
    REQUIRE(serialize_design_ir(parsed, force_v1).find("\"version\":1") != std::string::npos);
}

TEST_CASE("parse_design_ir_json accepts legacy bare-node IR JSON",
          "[view][import][ir-v1]") {
    const auto legacy = std::string{R"json({
        "type": "frame",
        "name": "Legacy",
        "layout": { "direction": "column", "gap": 12 },
        "style": { "backgroundColor": "#202020" },
        "tokens": {
            "colors": { "bg": "#202020" }
        }
    })json"};

    const auto parsed = parse_design_ir_json(legacy);
    REQUIRE(parsed.version == 1);
    REQUIRE(parsed.root.type == "frame");
    REQUIRE(parsed.root.name == "Legacy");
    REQUIRE(parsed.root.layout.direction == LayoutDirection::column);
    REQUIRE(parsed.root.layout.gap == 12.0f);
    REQUIRE(parsed.root.style.background_color == "#202020");
    REQUIRE(parsed.tokens.colors.at("bg") == "#202020");
}

TEST_CASE("DesignIR v1 canonical equivalence covers static source adapters",
          "[view][import][ir-v1]") {
    auto assert_canonical_round_trip = [](DesignIR ir) {
        refresh_design_ir_asset_manifest(ir);
        const auto canonical = serialize_design_ir(ir);
        const auto parsed = parse_design_ir_json(canonical);
        REQUIRE(serialize_design_ir(parsed) == canonical);
        REQUIRE(parsed.version == 1);
        REQUIRE(parsed.asset_manifest.version == 1);
    };

    SECTION("figma JSON") {
        auto ir = parse_figma_json(R"json({
            "type": "frame",
            "name": "Figma Panel",
            "id": "figma-node-1",
            "style": { "backgroundColor": "#18191c" },
            "tokens": {
                "colors": { "accent": "#57a6ff" },
                "sourceIdentity": {
                    "colors.accent": {
                        "sourceId": "var-accent",
                        "sourceCollection": "palette",
                        "sourceMode": "dark",
                        "sourceAdapter": "figma"
                    }
                }
            },
            "children": [
                { "type": "text", "name": "Title", "content": "Gain" }
            ]
        })json");
        REQUIRE(ir.tokens.source_identity.at("colors.accent").source_adapter == "figma");
        assert_canonical_round_trip(std::move(ir));
    }

    SECTION("stitch HTML") {
        assert_canonical_round_trip(parse_stitch_html(
            "<!doctype html><main><label>Frequency</label><span>8473 Hz</span></main>"));
    }

    SECTION("v0 TSX") {
        assert_canonical_round_trip(parse_v0_tsx(
            "export default function App(){ return <div className=\"flex flex-row gap-2 bg-slate-900\" />; }"));
    }

    SECTION("pencil JSON") {
        assert_canonical_round_trip(parse_pencil_json(R"json({
            "type": "frame",
            "name": "Pencil Card",
            "nodeId": "pencil-node-1",
            "style": { "backgroundImage": "url(data:image/svg+xml,%3Csvg%2F%3E)" },
            "children": [
                { "type": "text", "name": "Label", "content": "Mix" }
            ]
        })json"));
    }

    SECTION("claude HTML") {
        assert_canonical_round_trip(parse_claude_html(
            "<!doctype html><html><body><section><h1>Parameters</h1></section></body></html>"));
    }
}

TEST_CASE("DesignIR asset manifest records data URI local image and font assets",
          "[view][import][assets]") {
    TempDir tmp("pulp-design-ir-assets");
    const auto image_path = tmp.path / "meter.png";
    const auto font_path = tmp.path / "Inter.woff2";
    write_text(image_path, "\x89PNG\r\n\x1a\nnot-a-real-png-but-sniffable");
    write_text(font_path, "wOF2font-bytes");

    DesignIR ir;
    ir.root.type = "frame";
    ir.root.name = "Assets";
    ir.root.style.background_image = "url(data:image/svg+xml,%3Csvg%20xmlns='http://www.w3.org/2000/svg'/%3E)";
    ir.root.attributes["src"] = "meter.png";
    ir.root.attributes["fontUrl"] = "Inter.woff2";

    DesignIrAssetOptions options;
    options.base_directory = tmp.path;
    refresh_design_ir_asset_manifest(ir, options);

    REQUIRE(ir.asset_manifest.assets.size() == 3);

    bool saw_data = false;
    bool saw_image = false;
    bool saw_font = false;
    for (const auto& asset : ir.asset_manifest.assets) {
        REQUIRE_FALSE(asset.asset_id.empty());
        REQUIRE_FALSE(asset.content_hash.empty());
        REQUIRE(asset.diagnostics.empty());
        if (asset.original_uri.rfind("data:", 0) == 0) {
            saw_data = true;
            REQUIRE(asset.mime == "image/svg+xml");
        } else if (asset.original_uri == "meter.png") {
            saw_image = true;
            REQUIRE(asset.mime == "image/png");
            REQUIRE(asset.local_path);
        } else if (asset.original_uri == "Inter.woff2") {
            saw_font = true;
            REQUIRE(asset.mime == "font/woff2");
            REQUIRE(asset.local_path);
        }
    }
    REQUIRE(saw_data);
    REQUIRE(saw_image);
    REQUIRE(saw_font);

    const auto round_trip = parse_design_ir_json(serialize_design_ir(ir));
    REQUIRE(round_trip.asset_manifest.assets.size() == 3);
    REQUIRE(serialize_design_ir(round_trip) == serialize_design_ir(ir));
}

TEST_CASE("DesignIR parses camelCase source metadata and static HTML CSS assets",
          "[view][import][ir-v1][assets]") {
    SECTION("sourceNodeId is accepted as source metadata") {
        auto ir = parse_design_ir_json(R"json({
            "type": "frame",
            "name": "Screen",
            "sourceNodeId": "node-camel-1"
        })json");

        REQUIRE(ir.root.source_node_id);
        REQUIRE(*ir.root.source_node_id == "node-camel-1");
        REQUIRE(serialize_design_ir(ir).find("\"source_node_id\":\"node-camel-1\"")
                != std::string::npos);
    }

    SECTION("static Claude HTML CSS urls and fonts enter the asset manifest") {
        TempDir tmp("pulp-design-ir-static-html-assets");
        write_text(tmp.path / "hero.svg",
                   "<svg xmlns=\"http://www.w3.org/2000/svg\"><rect width=\"1\" height=\"1\"/></svg>");
        write_text(tmp.path / "Inter.woff2", "wOF2font-bytes");

        auto ir = parse_claude_html(R"html(
            <!doctype html>
            <html>
            <head>
            <style>
            @font-face { font-family: "Inter"; src: url("Inter.woff2") format("woff2"); }
            .hero { background-image: url("hero.svg"); }
            </style>
            </head>
            <body><section class="hero"><h1>Parameters</h1></section></body>
            </html>
        )html");

        DesignIrAssetOptions options;
        options.base_directory = tmp.path;
        refresh_design_ir_asset_manifest(ir, options);

        bool saw_svg = false;
        bool saw_font = false;
        for (const auto& asset : ir.asset_manifest.assets) {
            REQUIRE(asset.diagnostics.empty());
            if (asset.original_uri == "hero.svg") {
                saw_svg = true;
                REQUIRE(asset.mime == "image/svg+xml");
                REQUIRE_FALSE(asset.content_hash.empty());
            } else if (asset.original_uri == "Inter.woff2") {
                saw_font = true;
                REQUIRE(asset.mime == "font/woff2");
                REQUIRE(asset.font_family);
                REQUIRE(*asset.font_family == "Inter");
                REQUIRE_FALSE(asset.content_hash.empty());
            }
        }
        REQUIRE(saw_svg);
        REQUIRE(saw_font);
    }
}

TEST_CASE("DesignIR asset manifest preserves top-level asset refs and writes asset ids",
          "[view][import][assets]") {
    TempDir tmp("pulp-design-ir-top-level-assets");
    const auto image_path = tmp.path / "hero.png";
    write_text(image_path, "\x89PNG\r\n\x1a\nhero-bytes");

    auto ir = parse_design_ir_json(R"json({
        "type": "frame",
        "name": "Screen",
        "children": [
            { "type": "image", "name": "Hero", "src": "hero.png" }
        ]
    })json");

    DesignIrAssetOptions options;
    options.base_directory = tmp.path;
    refresh_design_ir_asset_manifest(ir, options);

    REQUIRE(ir.asset_manifest.assets.size() == 1);
    const auto& asset = ir.asset_manifest.assets[0];
    REQUIRE(asset.original_uri == "hero.png");
    REQUIRE(asset.mime == "image/png");
    REQUIRE(asset.local_path);
    REQUIRE_FALSE(asset.content_hash.empty());

    REQUIRE(ir.root.children.size() == 1);
    const auto& image = ir.root.children[0];
    REQUIRE(image.attributes.at("src") == "hero.png");
    REQUIRE(image.attributes.at("srcAssetId") == asset.asset_id);

    const auto round_trip = parse_design_ir_json(serialize_design_ir(ir));
    REQUIRE(round_trip.root.children[0].attributes.at("src") == "hero.png");
    REQUIRE(round_trip.root.children[0].attributes.at("srcAssetId") == asset.asset_id);
}

TEST_CASE("DesignIR asset manifest keeps URI aliases for deduped refs",
          "[view][import][assets]") {
    DesignIR ir;
    ir.root.type = "frame";
    ir.root.name = "Aliases";

    IRNode first;
    first.type = "image";
    first.name = "Plain";
    first.attributes["src"] = "hero.png";
    IRNode second;
    second.type = "image";
    second.name = "DotSlash";
    second.attributes["src"] = "./hero.png";
    ir.root.children.push_back(std::move(first));
    ir.root.children.push_back(std::move(second));

    DesignIrAssetOptions options;
    options.base_url = "https://cdn.example.test/screens/index.html";
    refresh_design_ir_asset_manifest(ir, options);

    REQUIRE(ir.asset_manifest.assets.size() == 1);
    const auto& asset = ir.asset_manifest.assets[0];
    REQUIRE(asset.original_uri == "hero.png");
    REQUIRE(asset.source_url);
    REQUIRE(*asset.source_url == "https://cdn.example.test/screens/hero.png");
    REQUIRE(std::find(asset.original_uri_aliases.begin(),
                      asset.original_uri_aliases.end(),
                      "./hero.png") != asset.original_uri_aliases.end());
    REQUIRE(ir.root.children[0].attributes.at("srcAssetId") == asset.asset_id);
    REQUIRE(ir.root.children[1].attributes.at("srcAssetId") == asset.asset_id);

    const auto round_trip = parse_design_ir_json(serialize_design_ir(ir));
    REQUIRE(round_trip.asset_manifest.assets.size() == 1);
    REQUIRE(round_trip.asset_manifest.assets[0].original_uri_aliases == asset.original_uri_aliases);
    REQUIRE(round_trip.root.children[1].attributes.at("srcAssetId") == asset.asset_id);
}

TEST_CASE("DesignIR asset manifest refresh rewrites stale asset ids",
          "[view][import][assets]") {
    TempDir tmp("pulp-design-ir-refresh-asset-ids");
    const auto asset_dir = tmp.path / "assets";
    fs::create_directories(asset_dir);
    write_text(asset_dir / "hero.png", "\x89PNG\r\n\x1a\nhero-bytes");

    auto ir = parse_design_ir_json(R"json({
        "type": "frame",
        "name": "Screen",
        "children": [
            { "type": "image", "name": "Hero", "src": "assets/hero.png" }
        ]
    })json");

    DesignIrAssetOptions unresolved_options;
    refresh_design_ir_asset_manifest(ir, unresolved_options);
    REQUIRE(ir.asset_manifest.assets.size() == 1);
    const auto stale_id = ir.root.children[0].attributes.at("srcAssetId");
    REQUIRE(has_diagnostic(ir.asset_manifest.assets[0], "asset-unresolved"));

    DesignIrAssetOptions resolved_options;
    resolved_options.base_directory = tmp.path;
    refresh_design_ir_asset_manifest(ir, resolved_options);

    REQUIRE(ir.asset_manifest.assets.size() == 1);
    const auto& resolved = ir.asset_manifest.assets[0];
    REQUIRE(resolved.local_path);
    REQUIRE(resolved.asset_id != stale_id);
    REQUIRE(ir.root.children[0].attributes.at("srcAssetId") == resolved.asset_id);
}

TEST_CASE("DesignIR asset manifest keeps distinct external assets with identical bytes",
          "[view][import][assets]") {
    TempDir tmp("pulp-design-ir-distinct-assets");
    write_text(tmp.path / "a.svg", "<svg xmlns=\"http://www.w3.org/2000/svg\"></svg>");
    write_text(tmp.path / "b.svg", "<svg xmlns=\"http://www.w3.org/2000/svg\"></svg>");

    DesignIR ir;
    ir.root.type = "frame";
    ir.root.name = "Distinct";
    IRNode first;
    first.type = "image";
    first.name = "A";
    first.attributes["src"] = "a.svg";
    IRNode second;
    second.type = "image";
    second.name = "B";
    second.attributes["src"] = "b.svg";
    ir.root.children.push_back(std::move(first));
    ir.root.children.push_back(std::move(second));

    DesignIrAssetOptions options;
    options.base_directory = tmp.path;
    refresh_design_ir_asset_manifest(ir, options);

    REQUIRE(ir.asset_manifest.assets.size() == 2);
    const IRAssetRef* a = nullptr;
    const IRAssetRef* b = nullptr;
    for (const auto& asset : ir.asset_manifest.assets) {
        if (asset.original_uri == "a.svg") a = &asset;
        if (asset.original_uri == "b.svg") b = &asset;
    }
    REQUIRE(a != nullptr);
    REQUIRE(b != nullptr);
    REQUIRE(a->content_hash == b->content_hash);
    REQUIRE(a->asset_id != b->asset_id);
    REQUIRE(ir.root.children[0].attributes.at("srcAssetId") == a->asset_id);
    REQUIRE(ir.root.children[1].attributes.at("srcAssetId") == b->asset_id);
}

TEST_CASE("DesignIR asset manifest records unresolved and network-gated diagnostics",
          "[view][import][assets]") {
    TempDir tmp("pulp-design-ir-asset-diagnostics");

    DesignIR ir;
    ir.root.type = "frame";
    ir.root.name = "Diagnostics";
    ir.root.style.background_image = "url(https://example.test/hero.png)";
    ir.root.attributes["src"] = "missing.png";

    DesignIrAssetOptions options;
    options.base_directory = tmp.path;
    auto manifest = collect_design_ir_assets(ir, options);

    REQUIRE(manifest.assets.size() == 2);
    bool saw_missing = false;
    bool saw_network = false;
    for (const auto& asset : manifest.assets) {
        if (asset.original_uri == "missing.png") {
            saw_missing = true;
            REQUIRE(has_diagnostic(asset, "asset-unresolved"));
            REQUIRE(asset.diagnostics[0].kind == ImportDiagnosticKind::unresolved_asset);
            REQUIRE(asset.content_hash.empty());
        } else if (asset.original_uri == "https://example.test/hero.png") {
            saw_network = true;
            REQUIRE(has_diagnostic(asset, "asset-network-fetch-disabled"));
            REQUIRE(asset.diagnostics[0].kind == ImportDiagnosticKind::unresolved_asset);
            REQUIRE(asset.content_hash.empty());
        }
    }
    REQUIRE(saw_missing);
    REQUIRE(saw_network);
}

TEST_CASE("DesignIR parser normalization promotes interactive frames from library APIs",
          "[view][import][diagnostics]") {
    const auto json = R"json({
        "version": 1,
        "source": "stitch",
        "root": {
            "type": "frame",
            "name": "Root",
            "children": [
                {
                    "type": "frame",
                    "name": "Click Me",
                    "attributes": { "role": "button" },
                    "children": []
                }
            ]
        }
    })json";

    auto ir = parse_design_ir_json(json);
    REQUIRE(ir.root.children.size() == 1);
    REQUIRE(ir.root.children[0].type == "button");
}

TEST_CASE("Interactive promotion ignores presentational cursor-only frames",
          "[view][import][diagnostics]") {
    IRNode node;
    node.type = "frame";
    node.name = "Decorative";
    node.style.cursor = "pointer";
    node.attributes["role"] = "presentation";

    REQUIRE(classify_interactive_signal(node) == WidgetPromotionSignal::none);
    REQUIRE(promote_interactive_frames(node) == 0);
    REQUIRE(node.type == "frame");
}

TEST_CASE("Interactive promotion runs before content-hash anchors",
          "[view][import][diagnostics]") {
    const std::string json = R"json({
        "type": "frame",
        "name": "Root",
        "children": [
            {
                "type": "frame",
                "name": "Click Me",
                "attributes": { "role": "button" },
                "children": []
            }
        ]
    })json";

    auto ir = parse_stitch_html(json);
    REQUIRE(ir.root.children.size() == 1);
    const auto& promoted = ir.root.children[0];
    REQUIRE(promoted.type == "button");

    const auto promoted_anchor = compute_anchor_id(
        promoted, /*parent_anchor=*/"", /*sibling_tag_index_for_path=*/0,
        /*depth=*/1, /*sig_index_for_content_hash=*/0,
        AnchorStrategy::content_hash);
    auto stale_frame = promoted;
    stale_frame.type = "frame";
    const auto stale_anchor = compute_anchor_id(
        stale_frame, /*parent_anchor=*/"", /*sibling_tag_index_for_path=*/0,
        /*depth=*/1, /*sig_index_for_content_hash=*/0,
        AnchorStrategy::content_hash);

    REQUIRE(promoted.stable_anchor_id == promoted_anchor);
    REQUIRE(promoted.stable_anchor_id != stale_anchor);
}

TEST_CASE("DesignIR diagnostics and provenance round trip canonical JSON",
          "[view][import][diagnostics]") {
    DesignIR ir;
    ir.version = 2;
    ir.source = DesignSource::jsx;
    ir.source_file = "panel.bundle.js";
    ir.capture_method = "runtime_snapshot";
    ir.settle_rounds = 4;
    ir.fallback_reason = "none";
    ir.source_adapter = "jsx-runtime";
    ir.source_version = "1";
    ir.imported_at = "2026-05-21T08:00:00Z";
    ir.root.type = "frame";
    ir.root.name = "Panel";

    ImportDiagnostic diagnostic;
    diagnostic.severity = ImportDiagnosticSeverity::warning;
    diagnostic.kind = ImportDiagnosticKind::snapshot_semantics_warning;
    diagnostic.code = "snapshot-dynamic-api";
    diagnostic.path = "<source>";
    diagnostic.message = "setInterval";
    diagnostic.anchor_id = "anchor-1";
    diagnostic.property = "onTick";
    ir.diagnostics.push_back(diagnostic);

    auto json = serialize_design_ir(ir);
    REQUIRE(json.find("\"capture_method\":\"runtime_snapshot\"") != std::string::npos);
    REQUIRE(json.find("\"kind\":\"snapshot_semantics_warning\"") != std::string::npos);
    REQUIRE(json.find("\"anchor_id\":\"anchor-1\"") != std::string::npos);
    REQUIRE(json.find("\"property\":\"onTick\"") != std::string::npos);

    auto reparsed = parse_design_ir_json(json);
    REQUIRE(reparsed.version == 2);
    REQUIRE(reparsed.capture_method == "runtime_snapshot");
    REQUIRE(reparsed.settle_rounds == 4);
    REQUIRE(reparsed.source_adapter == "jsx-runtime");
    REQUIRE(reparsed.imported_at == "2026-05-21T08:00:00Z");
    REQUIRE(reparsed.diagnostics.size() == 1);
    REQUIRE(reparsed.diagnostics[0].kind == ImportDiagnosticKind::snapshot_semantics_warning);
    REQUIRE(reparsed.diagnostics[0].anchor_id == "anchor-1");
    REQUIRE(reparsed.diagnostics[0].property == "onTick");
}

TEST_CASE("DesignIR diagnostic kinds parse and serialize every normalized bucket",
          "[view][import][diagnostics]") {
    const auto parsed = parse_design_ir_json(R"json({
        "version": 1,
        "source": "jsx",
        "captureMethod": "runtime_snapshot",
        "settleRounds": 2,
        "root": { "type": "frame", "name": "Diagnostics" },
        "diagnostics": [
            {
                "severity": "info",
                "kind": "legacy_field_shortcut",
                "code": "legacy-ir",
                "path": "<root>",
                "message": "legacy shortcut"
            },
            {
                "severity": "warning",
                "kind": "capture_partial",
                "code": "capture-partial",
                "path": "<capture>",
                "message": "partial capture"
            },
            {
                "severity": "error",
                "kind": "fallback_used",
                "code": "runtime-fallback",
                "path": "<runtime>",
                "message": "runtime fallback"
            },
            {
                "severity": "warning",
                "code": "asset-fetch-failed",
                "path": "https://example.test/asset.svg",
                "message": "asset failed"
            },
            {
                "severity": "warning",
                "code": "snapshot-dynamic-api",
                "path": "<source>",
                "message": "Date.now"
            },
            {
                "severity": "warning",
                "code": "fallback-used",
                "path": "<root>",
                "message": "fallback"
            },
            {
                "severity": "warning",
                "code": "unknown-code",
                "path": "<root>",
                "message": "unknown"
            }
        ]
    })json");

    REQUIRE(parsed.capture_method == "runtime_snapshot");
    REQUIRE(parsed.settle_rounds == 2);
    REQUIRE(parsed.diagnostics.size() == 7);
    REQUIRE(parsed.diagnostics[0].kind == ImportDiagnosticKind::legacy_field_shortcut);
    REQUIRE(parsed.diagnostics[1].kind == ImportDiagnosticKind::capture_partial);
    REQUIRE(parsed.diagnostics[2].severity == ImportDiagnosticSeverity::error);
    REQUIRE(parsed.diagnostics[2].kind == ImportDiagnosticKind::fallback_used);
    REQUIRE(parsed.diagnostics[3].kind == ImportDiagnosticKind::unresolved_asset);
    REQUIRE(parsed.diagnostics[4].kind == ImportDiagnosticKind::snapshot_semantics_warning);
    REQUIRE(parsed.diagnostics[5].kind == ImportDiagnosticKind::fallback_used);
    REQUIRE(parsed.diagnostics[6].kind == ImportDiagnosticKind::unknown);

    const auto json = serialize_design_ir(parsed);
    REQUIRE(json.find("\"kind\":\"legacy_field_shortcut\"") != std::string::npos);
    REQUIRE(json.find("\"kind\":\"capture_partial\"") != std::string::npos);
    REQUIRE(json.find("\"kind\":\"fallback_used\"") != std::string::npos);
    REQUIRE(json.find("\"kind\":\"unresolved_asset\"") != std::string::npos);
    REQUIRE(json.find("\"kind\":\"snapshot_semantics_warning\"") != std::string::npos);
    REQUIRE(json.find("\"kind\":\"unknown\"") != std::string::npos);
    REQUIRE(json.find("\"severity\":\"error\"") != std::string::npos);
}

TEST_CASE("parse_v0_tsx normalizes JSON and regex fallback diagnostics",
          "[view][import][diagnostics]") {
    auto json_ir = parse_v0_tsx(R"json({
        "type": "frame",
        "name": "JSON Root",
        "children": [{ "type": "text", "content": "Gain" }]
    })json");

    REQUIRE(json_ir.source == DesignSource::v0);
    REQUIRE(json_ir.capture_method == "adapter_parse");
    REQUIRE(json_ir.source_adapter == "v0-tsx");
    REQUIRE(json_ir.source_version == "1");
    REQUIRE(json_ir.root.confidence == IRConfidence::pass);
    REQUIRE(json_ir.root.stable_anchor_id.has_value());

    auto fallback_ir = parse_v0_tsx(
        "<div className=\"flex flex-row gap-2 bg-slate-900\">"
        "<span className=\"text-sm\">Gain</span>"
        "</div>");

    REQUIRE(fallback_ir.source == DesignSource::v0);
    REQUIRE(fallback_ir.capture_method == "adapter_parse");
    REQUIRE(fallback_ir.source_adapter == "v0-tsx");
    REQUIRE(fallback_ir.source_version == "1");
    REQUIRE(fallback_ir.root.confidence == IRConfidence::diverge);
    REQUIRE(fallback_ir.fallback_reason.find("regex TSX class extraction") != std::string::npos);
    REQUIRE(has_import_diagnostic(fallback_ir.diagnostics, "fallback-used"));
    REQUIRE(fallback_ir.diagnostics[0].kind == ImportDiagnosticKind::fallback_used);
    REQUIRE(fallback_ir.root.stable_anchor_id.has_value());
}

TEST_CASE("JSX snapshot dynamic API scanner detects non-deterministic APIs",
          "[view][import][diagnostics]") {
    auto scan = detect_jsx_snapshot_dynamic_apis(
        "setInterval(function(){}, 16); setTimeout(function(){}, 16);"
        "requestAnimationFrame(function(){}); Date.now(); new Date();"
        "performance.now(); Math.random(); fetch('/state');");
    REQUIRE(scan.has_dynamic_apis());
    REQUIRE(scan.tokens.size() == 8);
    REQUIRE(std::find(scan.tokens.begin(), scan.tokens.end(), "setInterval") != scan.tokens.end());
    REQUIRE(std::find(scan.tokens.begin(), scan.tokens.end(), "setTimeout") != scan.tokens.end());
    REQUIRE(std::find(scan.tokens.begin(), scan.tokens.end(), "requestAnimationFrame") != scan.tokens.end());
    REQUIRE(std::find(scan.tokens.begin(), scan.tokens.end(), "Date.now") != scan.tokens.end());
    REQUIRE(std::find(scan.tokens.begin(), scan.tokens.end(), "new Date") != scan.tokens.end());
    REQUIRE(std::find(scan.tokens.begin(), scan.tokens.end(), "performance.now") != scan.tokens.end());
    REQUIRE(std::find(scan.tokens.begin(), scan.tokens.end(), "Math.random") != scan.tokens.end());
    REQUIRE(std::find(scan.tokens.begin(), scan.tokens.end(), "fetch") != scan.tokens.end());

    auto interpolation_scan = detect_jsx_snapshot_dynamic_apis(
        R"(const label = `literal setInterval Date.now ${Date.now()} ${Math.random()} ${fetch("/state")}`;)");
    REQUIRE(interpolation_scan.has_dynamic_apis());
    REQUIRE(interpolation_scan.tokens.size() == 3);
    REQUIRE(std::find(interpolation_scan.tokens.begin(), interpolation_scan.tokens.end(), "Date.now") != interpolation_scan.tokens.end());
    REQUIRE(std::find(interpolation_scan.tokens.begin(), interpolation_scan.tokens.end(), "Math.random") != interpolation_scan.tokens.end());
    REQUIRE(std::find(interpolation_scan.tokens.begin(), interpolation_scan.tokens.end(), "fetch") != interpolation_scan.tokens.end());
    REQUIRE(std::find(interpolation_scan.tokens.begin(), interpolation_scan.tokens.end(), "setInterval") == interpolation_scan.tokens.end());

    auto nested_template_scan = detect_jsx_snapshot_dynamic_apis(
        R"(const label = `outer ${`inner ${performance.now()}`} ${new Date()}`;)");
    REQUIRE(nested_template_scan.has_dynamic_apis());
    REQUIRE(nested_template_scan.tokens.size() == 2);
    REQUIRE(std::find(nested_template_scan.tokens.begin(), nested_template_scan.tokens.end(), "performance.now") != nested_template_scan.tokens.end());
    REQUIRE(std::find(nested_template_scan.tokens.begin(), nested_template_scan.tokens.end(), "new Date") != nested_template_scan.tokens.end());

    auto braced_expression_scan = detect_jsx_snapshot_dynamic_apis(
        R"(const label = `${ { value: Date.now() } } ${format("}")}`;)");
    REQUIRE(braced_expression_scan.has_dynamic_apis());
    REQUIRE(braced_expression_scan.tokens.size() == 1);
    REQUIRE(std::find(braced_expression_scan.tokens.begin(), braced_expression_scan.tokens.end(), "Date.now") != braced_expression_scan.tokens.end());

    REQUIRE_FALSE(detect_jsx_snapshot_dynamic_apis(
        "// setInterval Date.now Math.random fetch(\n"
        "/* setTimeout requestAnimationFrame new Date performance.now */\n"
        "const literal = \"setInterval Date.now Math.random fetch(\";\n"
        "const single = 'setTimeout requestAnimationFrame new Date performance.now';\n"
        "const template = `setInterval Date.now Math.random fetch(`;").has_dynamic_apis());
    REQUIRE_FALSE(detect_jsx_snapshot_dynamic_apis(
        R"JS(const label = `${"Date.now()"} ${/* Math.random() */ 1}`;)JS").has_dynamic_apis());
    REQUIRE_FALSE(detect_jsx_snapshot_dynamic_apis(
        "const single = 'escaped \\' Date.now()';\n"
        "const double_quote = \"escaped \\\" Math.random()\";\n"
        "const template = `escaped \\` fetch(\"/state\")`;\n").has_dynamic_apis());
    REQUIRE_FALSE(detect_jsx_snapshot_dynamic_apis("const x = 1;").has_dynamic_apis());
}

#ifndef _WIN32
TEST_CASE("DesignIR asset manifest fetches network assets through cache and verifies hashes",
          "[view][import][assets][network]") {
    TempDir tmp("pulp-design-ir-network-assets");
    const auto bin = tmp.path / "bin";
    const auto curl = bin / "curl";
    fs::create_directories(bin);
    write_text(curl,
               "#!/bin/sh\n"
               "out=''\n"
               "while [ \"$#\" -gt 0 ]; do\n"
               "  case \"$1\" in\n"
               "    --output) shift; out=\"$1\" ;;\n"
               "  esac\n"
               "  shift\n"
               "done\n"
               "[ -n \"$out\" ] || exit 9\n"
               "printf '%s' '<svg xmlns=\"http://www.w3.org/2000/svg\"><rect width=\"1\" height=\"1\"/></svg>' > \"$out\"\n");
    fs::permissions(curl,
                    fs::perms::owner_exec | fs::perms::owner_read | fs::perms::owner_write,
                    fs::perm_options::add);

    const auto url = std::string("https://example.test/icon.svg");
    DesignIR ir;
    ir.root.type = "frame";
    ir.root.name = "Network";
    ir.root.style.background_image = "url(" + url + ")";

    auto old_path = read_env_var("PATH").value_or("");
    ScopedEnvVar path_override("PATH", bin.string() + ":" + old_path);

    DesignIrAssetOptions options;
    options.allow_network_fetch = true;
    options.network_timeout_ms = 5000;
    options.cache_directory = tmp.path / "asset-cache";
    options.expected_hash_by_uri[url] = "not-the-actual-hash";

    auto manifest = collect_design_ir_assets(ir, options);
    REQUIRE(manifest.assets.size() == 1);
    const auto& fetched = manifest.assets[0];
    REQUIRE(fetched.original_uri == url);
    REQUIRE(fetched.source_url == url);
    REQUIRE(fetched.mime == "image/svg+xml");
    REQUIRE_FALSE(fetched.local_path);
    REQUIRE_FALSE(fetched.content_hash.empty());
    REQUIRE(has_diagnostic(fetched, "asset-hash-mismatch"));
    REQUIRE_FALSE(fs::exists(options.cache_directory / "by-hash"));
    REQUIRE_FALSE(fs::exists(options.cache_directory / "by-url"));

    const auto actual_hash = fetched.content_hash;
    options.expected_hash_by_uri[url] = actual_hash;
    auto verified = collect_design_ir_assets(ir, options);
    REQUIRE(verified.assets.size() == 1);
    REQUIRE(verified.assets[0].content_hash == actual_hash);
    REQUIRE(verified.assets[0].diagnostics.empty());
    REQUIRE(verified.assets[0].local_path);
    REQUIRE(fs::exists(*verified.assets[0].local_path));

    fs::remove(curl);
    options.expected_hash_by_uri.clear();
    auto cached = collect_design_ir_assets(ir, options);
    REQUIRE(cached.assets.size() == 1);
    REQUIRE(cached.assets[0].content_hash == actual_hash);
    REQUIRE(cached.assets[0].diagnostics.empty());

    options.expected_hash_by_uri[url] = "definitely-not-the-cached-hash";
    auto cached_mismatch = collect_design_ir_assets(ir, options);
    REQUIRE(cached_mismatch.assets.size() == 1);
    REQUIRE(cached_mismatch.assets[0].content_hash == actual_hash);
    REQUIRE(has_diagnostic(cached_mismatch.assets[0], "asset-hash-mismatch"));
}

TEST_CASE("DesignIR asset manifest reports network fetch failures and timeouts",
          "[view][import][assets][network]") {
    TempDir tmp("pulp-design-ir-network-diagnostics");
    const auto bin = tmp.path / "bin";
    const auto curl = bin / "curl";
    fs::create_directories(bin);
    write_text(curl,
               "#!/bin/sh\n"
               "url=''\n"
               "while [ \"$#\" -gt 0 ]; do\n"
               "  case \"$1\" in\n"
               "    http://*|https://*) url=\"$1\" ;;\n"
               "  esac\n"
               "  shift\n"
               "done\n"
               "case \"$url\" in\n"
               "  *slow*) sleep 5 ;;\n"
               "  *) printf 'fetch failed for %s\\n' \"$url\" >&2; exit 28 ;;\n"
               "esac\n");
    fs::permissions(curl,
                    fs::perms::owner_exec | fs::perms::owner_read | fs::perms::owner_write,
                    fs::perm_options::add);

    auto old_path = read_env_var("PATH").value_or("");
    ScopedEnvVar path_override("PATH", bin.string() + ":" + old_path);

    DesignIrAssetOptions options;
    options.allow_network_fetch = true;
    options.cache_directory = tmp.path / "asset-cache";
    options.network_timeout_ms = 5000;

    DesignIR failed_ir;
    failed_ir.root.type = "frame";
    failed_ir.root.name = "FetchFail";
    failed_ir.root.style.background_image = "url(https://example.test/fail.svg)";
    auto failed = collect_design_ir_assets(failed_ir, options);
    REQUIRE(failed.assets.size() == 1);
    REQUIRE(has_diagnostic(failed.assets[0], "asset-fetch-failed"));

    options.network_timeout_ms = 100;
    DesignIR timeout_ir;
    timeout_ir.root.type = "frame";
    timeout_ir.root.name = "FetchTimeout";
    timeout_ir.root.style.background_image = "url(https://example.test/slow.svg)";
    auto timed_out = collect_design_ir_assets(timeout_ir, options);
    REQUIRE(timed_out.assets.size() == 1);
    REQUIRE(has_diagnostic(timed_out.assets[0], "asset-fetch-timeout"));
}

TEST_CASE("DesignIR asset manifest reports missing fetcher and empty network downloads",
          "[view][import][assets][network]") {
    TempDir tmp("pulp-design-ir-network-edge-diagnostics");
    const auto bin = tmp.path / "bin";
    fs::create_directories(bin);

    auto old_path = read_env_var("PATH").value_or("");
    ScopedEnvVar path_override("PATH", bin.string());

    DesignIrAssetOptions options;
    options.allow_network_fetch = true;
    options.cache_directory = tmp.path / "asset-cache";
    options.network_timeout_ms = 1000;

    DesignIR missing_fetcher_ir;
    missing_fetcher_ir.root.type = "frame";
    missing_fetcher_ir.root.name = "MissingFetcher";
    missing_fetcher_ir.root.style.background_image =
        "url(https://example.test/missing-fetcher.svg)";
    auto missing_fetcher = collect_design_ir_assets(missing_fetcher_ir, options);
    REQUIRE(missing_fetcher.assets.size() == 1);
    REQUIRE(has_diagnostic(missing_fetcher.assets[0], "asset-fetcher-missing"));

    const auto curl = bin / "curl";
    write_text(curl,
               "#!/bin/sh\n"
               "out=''\n"
               "while [ \"$#\" -gt 0 ]; do\n"
               "  case \"$1\" in\n"
               "    --output) shift; out=\"$1\" ;;\n"
               "  esac\n"
               "  shift\n"
               "done\n"
               "[ -n \"$out\" ] || exit 9\n"
               ": > \"$out\"\n");
    fs::permissions(curl,
                    fs::perms::owner_exec | fs::perms::owner_read | fs::perms::owner_write,
                    fs::perm_options::add);

    DesignIR empty_download_ir;
    empty_download_ir.root.type = "frame";
    empty_download_ir.root.name = "EmptyDownload";
    empty_download_ir.root.style.background_image =
        "url(https://example.test/empty.svg)";
    auto empty_download = collect_design_ir_assets(empty_download_ir, options);
    REQUIRE(empty_download.assets.size() == 1);
    REQUIRE(has_diagnostic(empty_download.assets[0], "asset-empty"));

    set_env_var("PATH", old_path);
}
#endif

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
    opts.mode = CodeGenMode::bridge_native_js;
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

TEST_CASE("generate_pulp_js bridge_native_js mode produces Pulp API", "[view][import]") {
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
    opts.mode = CodeGenMode::bridge_native_js;
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

TEST_CASE("generate_pulp_js bridge_native_js mode handles audio widgets with Yoga constraints", "[view][import]") {
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
    opts.mode = CodeGenMode::bridge_native_js;
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

TEST_CASE("generate_pulp_js bridge_native_js mode covers layout and audio widget edge branches",
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
    opts.mode = CodeGenMode::bridge_native_js;
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

    // Step 3: Generate native-bridge JS
    CodeGenOptions opts;
    opts.mode = CodeGenMode::bridge_native_js;
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

TEST_CASE("generate_pulp_js bridge_native_js mode covers fill-height and leaf fallback branches",
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
    opts.mode = CodeGenMode::bridge_native_js;
    opts.include_comments = false;
    auto js = generate_pulp_js(ir, opts);

    REQUIRE(js.find("createCol('root', '')") != std::string::npos);
    REQUIRE(js.find("setFlex('root', 'flex_grow', 1)") != std::string::npos);
    REQUIRE(js.find("createRow('empty_divider0', 'root')") != std::string::npos);
    REQUIRE(js.find("setFlex('empty_divider0', 'height', 1)") != std::string::npos);
}
