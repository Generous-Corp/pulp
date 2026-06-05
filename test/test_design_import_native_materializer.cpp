#include <pulp/canvas/canvas.hpp>
#include <pulp/platform/child_process.hpp>
#include <pulp/state/store.hpp>
#include <pulp/view/buttons.hpp>
#include <pulp/view/design_import.hpp>
#include <pulp/view/layout_snapshot.hpp>
#include <pulp/view/script_engine.hpp>
#include <pulp/view/input_events.hpp>
#include <pulp/view/text_editor.hpp>
#include <pulp/view/ui_components.hpp>
#include <pulp/view/view.hpp>
#include <pulp/view/widget_bridge.hpp>
#include <pulp/view/widgets.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace pulp::view;
namespace fs = std::filesystem;

namespace pulp::test::generated_binding_runtime {
std::unique_ptr<pulp::view::View> build_generated_binding_runtime_ui();
void bind_generated_binding_runtime_ui(pulp::view::View& root,
                                       pulp::view::NativeImportBindingContext& ctx);
} // namespace pulp::test::generated_binding_runtime

#ifndef PULP_TEST_CXX_COMPILER
#define PULP_TEST_CXX_COMPILER ""
#endif

#ifndef PULP_REPO_ROOT
#define PULP_REPO_ROOT ""
#endif

namespace {

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
    std::ofstream out(path);
    REQUIRE(out.is_open());
    out << text;
    REQUIRE(out.good());
}

bool compile_generated_source(const fs::path& source_path,
                              const fs::path& output_path,
                              std::string* diagnostics) {
    const fs::path compiler(PULP_TEST_CXX_COMPILER);
    if (compiler.empty() || !fs::exists(compiler)) {
        if (diagnostics != nullptr) *diagnostics = "C++ compiler path is unavailable";
        return false;
    }

    const fs::path root(PULP_REPO_ROOT);
    std::vector<std::string> include_dirs = {
        root.string(),
        (root / "core" / "view" / "include").string(),
        (root / "core" / "canvas" / "include").string(),
        (root / "core" / "runtime" / "include").string(),
        (root / "core" / "platform" / "include").string(),
        (root / "core" / "events" / "include").string(),
        (root / "core" / "state" / "include").string(),
        (root / "core" / "audio" / "include").string(),
        (root / "core" / "midi" / "include").string(),
        (root / "core" / "signal" / "include").string(),
        (root / "core" / "host" / "include").string(),
    };

    std::vector<std::string> args;
#if defined(_WIN32)
    const auto filename = compiler.filename().string();
    const bool msvc_style = filename.find("cl") != std::string::npos;
    if (msvc_style) {
        args = {"/nologo", "/std:c++20", "/EHsc"};
        for (const auto& dir : include_dirs) args.push_back("/I" + dir);
        args.push_back("/c");
        args.push_back(source_path.string());
        args.push_back("/Fo" + output_path.string());
    } else
#endif
    {
        args = {"-std=c++20"};
        for (const auto& dir : include_dirs) {
            args.push_back("-I");
            args.push_back(dir);
        }
        args.push_back("-c");
        args.push_back(source_path.string());
        args.push_back("-o");
        args.push_back(output_path.string());
    }

    auto result = pulp::platform::exec(compiler.string(), args, 30000);
    if (diagnostics != nullptr)
        *diagnostics = result.stdout_output + result.stderr_output;
    return !result.timed_out && result.exit_code == 0 && fs::exists(output_path);
}

const pulp::canvas::DrawCommand* first_meter_fill_rect(const pulp::canvas::RecordingCanvas& canvas) {
    for (const auto& command : canvas.commands()) {
        if (command.type == pulp::canvas::DrawCommand::Type::fill_rect)
            return &command;
    }
    return nullptr;
}

std::string minimal_live_react_shim() {
    return R"JS(
(function() {
  function flatten(input, out) {
    if (Array.isArray(input)) {
      for (var i = 0; i < input.length; i++) flatten(input[i], out);
    } else if (input !== null && input !== undefined && input !== false && input !== true) {
      out.push(input);
    }
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
      } else if (key === "id") {
        el.id = String(value);
        el.setAttribute("id", String(value));
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
    var el = document.createElement(node.type);
    applyProps(el, node.props);
    var scalarText = "";
    var nonScalarChildren = [];
    for (var i = 0; i < node.children.length; i++) {
      var child = node.children[i];
      if (typeof child === "string" || typeof child === "number") {
        scalarText += String(child);
      } else {
        nonScalarChildren.push(child);
      }
    }
    if (scalarText) el.textContent = scalarText;
    for (var j = 0; j < nonScalarChildren.length; j++) {
      var rendered = renderNode(nonScalarChildren[j]);
      if (rendered) el.appendChild(rendered);
    }
    return el;
  }

  globalThis.React = { createElement: createElement };
  globalThis.ReactDOM = {
    createRoot: function(mount) {
      return {
        render: function(element) {
          var node = renderNode(element);
          if (node) mount.appendChild(node);
        }
      };
    },
    flushSync: function(fn) { return fn(); }
  };
})();
)JS";
}

std::unique_ptr<View> build_live_plugin_panel() {
    auto root = std::make_unique<View>();
    root->set_bounds({0, 0, 360, 160});

    ScriptEngine engine;
    pulp::state::StateStore store;
    WidgetBridge bridge(engine, *root, store);

    bridge.load_script(minimal_live_react_shim());
    bridge.load_script(R"JS(
function Control(props) {
  return React.createElement(
    'div',
    {
      style: {
        display: 'flex',
        flexDirection: 'column',
        width: 104,
        height: 90,
        padding: 6,
        gap: 4
      }
    },
    React.createElement('span', { style: { width: 92, height: 20 } }, props.label),
    React.createElement('span', { style: { width: 92, height: 20 } }, props.value)
  );
}

function Panel() {
  return React.createElement(
    'div',
    {
      id: 'phase-four-plugin-panel',
      style: {
        display: 'flex',
        flexDirection: 'column',
        width: 360,
        height: 160,
        padding: 12,
        gap: 10,
        overflow: 'hidden'
      }
    },
    React.createElement(
      'div',
      {
        style: {
          display: 'flex',
          flexDirection: 'row',
          width: 336,
          height: 24,
          gap: 96
        }
      },
      React.createElement('span', { style: { width: 160, height: 24 } }, 'Cloud Chorus'),
      React.createElement('span', { style: { width: 80, height: 24 } }, '-12 dB')
    ),
    React.createElement(
      'div',
      {
        style: {
          display: 'flex',
          flexDirection: 'row',
          width: 336,
          height: 90,
          gap: 12
        }
      },
      React.createElement(Control, { label: 'Depth', value: '67%' }),
      React.createElement(Control, { label: 'Rate', value: '1.8 Hz' }),
      React.createElement(Control, { label: 'Mix', value: '42%' })
    )
  );
}

ReactDOM.createRoot(document.body).render(React.createElement(Panel));
layout();
)JS");
    root->layout_children();
    return root;
}

IRNode frame(std::string id, float width, float height, LayoutDirection direction) {
    IRNode node;
    node.type = "frame";
    node.stable_anchor_id = std::move(id);
    node.style.width = width;
    node.style.height = height;
    node.layout.direction = direction;
    return node;
}

IRNode label(std::string id, std::string text, float width, float height) {
    IRNode node;
    node.type = "text";
    node.stable_anchor_id = std::move(id);
    node.text_content = std::move(text);
    node.style.width = width;
    node.style.height = height;
    return node;
}

IRNode control_from_live(const View& card, std::string label_text, std::string value_text) {
    REQUIRE(card.child_count() == 2);
    auto node = frame(card.id(), 104.0f, 90.0f, LayoutDirection::column);
    node.layout.padding_top = 6.0f;
    node.layout.padding_right = 6.0f;
    node.layout.padding_bottom = 6.0f;
    node.layout.padding_left = 6.0f;
    node.layout.gap = 4.0f;
    node.children.push_back(label(card.child_at(0)->id(), std::move(label_text), 92.0f, 20.0f));
    node.children.push_back(label(card.child_at(1)->id(), std::move(value_text), 92.0f, 20.0f));
    return node;
}

DesignIR build_plugin_panel_ir_from_live(const View& live_root) {
    REQUIRE(live_root.child_count() == 1);
    const auto* panel = live_root.child_at(0);
    REQUIRE(panel != nullptr);
    REQUIRE(panel->child_count() == 2);
    const auto* header = panel->child_at(0);
    const auto* controls = panel->child_at(1);
    REQUIRE(header != nullptr);
    REQUIRE(controls != nullptr);
    REQUIRE(header->child_count() == 2);
    REQUIRE(controls->child_count() == 3);

    // This IR is the baked snapshot of the same JSX fixture rendered by
    // build_live_plugin_panel(). The parity oracle compares native View IDs,
    // so the baked fixture mirrors the bridge-assigned live IDs.
    DesignIR ir;
    ir.source = DesignSource::jsx;
    ir.source_adapter = "phase4-live-react-fixture";
    ir.root = frame(panel->id(), 360.0f, 160.0f, LayoutDirection::column);
    ir.root.layout.padding_top = 12.0f;
    ir.root.layout.padding_right = 12.0f;
    ir.root.layout.padding_bottom = 12.0f;
    ir.root.layout.padding_left = 12.0f;
    ir.root.layout.gap = 10.0f;
    ir.root.style.overflow = "hidden";

    auto header_ir = frame(header->id(), 336.0f, 24.0f, LayoutDirection::row);
    header_ir.layout.gap = 96.0f;
    header_ir.children.push_back(label(header->child_at(0)->id(), "Cloud Chorus", 160.0f, 24.0f));
    header_ir.children.push_back(label(header->child_at(1)->id(), "-12 dB", 80.0f, 24.0f));
    ir.root.children.push_back(std::move(header_ir));

    auto controls_ir = frame(controls->id(), 336.0f, 90.0f, LayoutDirection::row);
    controls_ir.layout.gap = 12.0f;
    controls_ir.children.push_back(control_from_live(*controls->child_at(0), "Depth", "67%"));
    controls_ir.children.push_back(control_from_live(*controls->child_at(1), "Rate", "1.8 Hz"));
    controls_ir.children.push_back(control_from_live(*controls->child_at(2), "Mix", "42%"));
    ir.root.children.push_back(std::move(controls_ir));

    return ir;
}

std::string diff_messages(const LayoutTreeDiff& diff) {
    std::ostringstream out;
    for (const auto& message : diff.messages) out << message << '\n';
    return out.str();
}

bool diagnostics_contain(const std::vector<ImportDiagnostic>& diagnostics,
                         std::string_view code) {
    for (const auto& diagnostic : diagnostics) {
        if (diagnostic.code == code) return true;
    }
    return false;
}

std::size_t diagnostics_count(const std::vector<ImportDiagnostic>& diagnostics,
                              std::string_view code) {
    std::size_t count = 0;
    for (const auto& diagnostic : diagnostics) {
        if (diagnostic.code == code) ++count;
    }
    return count;
}

struct BoundKnobDescriptor {
    std::string route_id;
    std::string param_key;
    std::string binding_module;
    std::string binding_param;
    std::string event_contract;
    std::string gesture_contract;
};

struct BoundCheckboxDescriptor {
    std::string route_id;
    std::string param_key;
    std::string binding_module;
    std::string binding_param;
    std::string event_contract;
    std::string gesture_contract;
};

struct BoundTextDescriptor {
    std::string route_id;
    std::string value_key;
    std::string initial_value;
    std::string placeholder;
    std::string event_contract;
    std::string focus_contract;
};

class BindingBackedKnobContext final : public NativeImportBindingContext {
public:
    BindingBackedKnobContext() {
        store_.set_gesture_callbacks(
            [this](pulp::state::ParamID) { ++gesture_begin_count; },
            [this](pulp::state::ParamID) { ++gesture_end_count; });
    }

    void bind_knob(Knob& knob, const NativeImportBindingDescriptor& descriptor) override {
        bound_knobs.push_back(BoundKnobDescriptor{
            .route_id = std::string(descriptor.route_id),
            .param_key = std::string(descriptor.param_key),
            .binding_module = std::string(descriptor.binding_module),
            .binding_param = std::string(descriptor.binding_param),
            .event_contract = std::string(descriptor.event_contract),
            .gesture_contract = std::string(descriptor.gesture_contract)});

        pulp::state::ParamInfo info;
        info.id = param_id_;
        info.name = std::string(descriptor.param_key);
        info.range = {0.0f, 1.0f, knob.value()};
        store_.add_parameter(info);
        store_.set_normalized(param_id_, knob.value());

        knob.on_gesture_begin = [this] {
            store_.begin_gesture(param_id_);
        };
        knob.on_change = [this](float normalized) {
            store_.set_normalized(param_id_, normalized);
            changes.push_back(normalized);
        };
        knob.on_gesture_end = [this] {
            store_.end_gesture(param_id_);
        };
    }

    void bind_checkbox(Checkbox& checkbox, const NativeImportBindingDescriptor& descriptor) override {
        bound_checkboxes.push_back(BoundCheckboxDescriptor{
            .route_id = std::string(descriptor.route_id),
            .param_key = std::string(descriptor.param_key),
            .binding_module = std::string(descriptor.binding_module),
            .binding_param = std::string(descriptor.binding_param),
            .event_contract = std::string(descriptor.event_contract),
            .gesture_contract = std::string(descriptor.gesture_contract)});

        const auto param_key = std::string(descriptor.param_key);
        checkbox.on_change = [this, param_key](bool checked) {
            checkbox_changes.push_back({param_key, checked ? 1.0f : 0.0f});
        };
    }

    void bind_text_editor(TextEditor& editor, const NativeImportTextBindingDescriptor& descriptor) override {
        bound_text_editors.push_back(BoundTextDescriptor{
            .route_id = std::string(descriptor.route_id),
            .value_key = std::string(descriptor.value_key),
            .initial_value = std::string(descriptor.initial_value),
            .placeholder = std::string(descriptor.placeholder),
            .event_contract = std::string(descriptor.event_contract),
            .focus_contract = std::string(descriptor.focus_contract)});

        editor.on_change = [this](const std::string& text) {
            text_changes.push_back(text);
        };
    }

    float normalized_value() const {
        return store_.get_normalized(param_id_);
    }

    std::vector<BoundKnobDescriptor> bound_knobs;
    std::vector<BoundCheckboxDescriptor> bound_checkboxes;
    std::vector<BoundTextDescriptor> bound_text_editors;
    std::vector<float> changes;
    std::vector<std::pair<std::string, float>> checkbox_changes;
    std::vector<std::string> text_changes;
    int gesture_begin_count = 0;
    int gesture_end_count = 0;

private:
    pulp::state::StateStore store_;
    pulp::state::ParamID param_id_ = 1;
};

} // namespace

TEST_CASE("baked native materializer matches live React layout parity for a plugin panel",
          "[view][import][native-materializer][phase-4]") {
    auto live = build_live_plugin_panel();
    auto ir = build_plugin_panel_ir_from_live(*live);

    std::vector<ImportDiagnostic> diagnostics;
    auto materialized = build_native_view_tree(ir, ir.asset_manifest, {.diagnostics_out = &diagnostics});
    REQUIRE(materialized != nullptr);

    auto baked = std::make_unique<View>();
    baked->set_bounds({0, 0, 360, 160});
    baked->add_child(std::move(materialized));
    baked->layout_children();

    const LayoutTreeSnapshotOptions options{
        .surface = "phase4-baked-native",
        .fixture = "plugin-panel-live-react-vs-native",
        .viewport_width = 360,
        .viewport_height = 160,
    };
    const auto live_json = dump_layout_tree(*live, options);
    const auto baked_json = dump_layout_tree(*baked, options);

    LayoutTreeDiff diff;
    const bool equivalent = layout_tree_snapshots_equivalent(live_json, baked_json, {}, &diff);
    INFO(diff_messages(diff));
    INFO("live:\n" << live_json);
    INFO("baked:\n" << baked_json);
    REQUIRE(equivalent);
    REQUIRE_FALSE(diagnostics_contain(diagnostics, "native-materialize-failed"));
}

TEST_CASE("baked native materializer resolves image sources through the asset manifest",
          "[view][import][native-materializer][phase-4]") {
    DesignIR ir;
    ir.root.type = "image";
    ir.root.stable_anchor_id = "logo";
    ir.root.attributes["src"] = "/raw/should-not-be-used.png";
    ir.root.attributes["srcAssetId"] = "asset-logo";
    ir.root.style.width = 64.0f;
    ir.root.style.height = 32.0f;

    IRAssetRef asset;
    asset.asset_id = "asset-logo";
    asset.original_uri = "/raw/source-logo.png";
    asset.local_path = "/resolved/cache/logo.png";
    asset.content_hash = "sha256:test";
    asset.mime = "image/png";
    ir.asset_manifest.assets.push_back(asset);

    std::vector<ImportDiagnostic> diagnostics;
    auto root = build_native_view_tree(ir, {}, {.diagnostics_out = &diagnostics});
    REQUIRE(root != nullptr);
    auto* image = dynamic_cast<ImageView*>(root.get());
    REQUIRE(image != nullptr);
    REQUIRE(image->image_source() == "file:///resolved/cache/logo.png");
    REQUIRE_FALSE(diagnostics_contain(diagnostics, "native-materialize-unresolved-asset"));
}

TEST_CASE("baked native materializer resolves figma-plugin asset_ref image sources",
          "[view][import][native-materializer][figma-plugin][asset-ref]") {
    DesignIR ir;
    ir.source = DesignSource::figma_plugin;
    ir.source_adapter = "figma-plugin";
    ir.root.type = "image";
    ir.root.name = "Imported Figma Image";
    ir.root.attributes["asset_ref"] = "3:43";
    ir.root.style.width = 64.0f;
    ir.root.style.height = 32.0f;

    IRAssetRef asset;
    asset.asset_id = "3:43";
    asset.original_uri = "figma://KCKIyZoWXjde6qVNCm4qPa/3:43";
    asset.local_path = "/resolved/import/assets/3_43.png";
    asset.mime = "image/png";
    ir.asset_manifest.assets.push_back(asset);

    std::vector<ImportDiagnostic> diagnostics;
    auto root = build_native_view_tree(ir, {}, {.diagnostics_out = &diagnostics});
    REQUIRE(root != nullptr);
    auto* image = dynamic_cast<ImageView*>(root.get());
    REQUIRE(image != nullptr);
    REQUIRE(image->image_source() == "file:///resolved/import/assets/3_43.png");
    REQUIRE_FALSE(diagnostics_contain(diagnostics, "native-materialize-unresolved-asset"));
}

TEST_CASE("baked native materializer preserves figma-plugin bleed sprite geometry",
          "[view][import][native-materializer][figma-plugin][fidelity]") {
    DesignIR ir;
    ir.source = DesignSource::figma_plugin;
    ir.source_adapter = "figma-plugin";
    ir.root.type = "image";
    ir.root.name = "Imported Bleed Sprite";
    ir.root.attributes["asset_ref"] = "sprite";
    ir.root.attributes["png_natural_w"] = "420";
    ir.root.attributes["png_natural_h"] = "484";
    ir.root.attributes["art_core_x"] = "148";
    ir.root.attributes["art_core_y"] = "0";
    ir.root.attributes["art_core_w"] = "115";
    ir.root.attributes["art_core_h"] = "129";
    ir.root.style.width = 62.0f;
    ir.root.style.height = 68.0f;
    ir.root.style.position = "absolute";
    ir.root.style.left = 20.0f;
    ir.root.style.top = 30.0f;
    ir.root.style.render_bounds = IRStyle::RenderBounds{210.0f, 116.0f, -74.0f, 0.0f};

    IRAssetRef asset;
    asset.asset_id = "sprite";
    asset.original_uri = "figma://fixture/sprite";
    asset.local_path = "/resolved/import/assets/sprite.png";
    asset.mime = "image/png";
    ir.asset_manifest.assets.push_back(asset);

    std::vector<ImportDiagnostic> diagnostics;
    auto root = build_native_view_tree(ir, ir.asset_manifest, {.diagnostics_out = &diagnostics});
    REQUIRE(root != nullptr);
    auto* image = dynamic_cast<ImageView*>(root.get());
    REQUIRE(image != nullptr);

    const float scale = 68.0f / 129.0f;
    const float expected_w = 420.0f * scale;
    const float expected_h = 484.0f * scale;
    const float expected_left =
        20.0f - 148.0f * scale + (62.0f - 115.0f * scale) * 0.5f;
    CHECK(image->flex().preferred_width == Catch::Approx(expected_w));
    CHECK(image->flex().preferred_height == Catch::Approx(expected_h));
    CHECK(image->flex().dim_width.value == Catch::Approx(expected_w));
    CHECK(image->flex().dim_height.value == Catch::Approx(expected_h));
    CHECK(image->left() == Catch::Approx(expected_left));
    CHECK(image->top() == Catch::Approx(30.0f));
    REQUIRE_FALSE(diagnostics_contain(diagnostics, "native-materialize-unresolved-asset"));
}

TEST_CASE("baked native materializer returns an unresolved asset placeholder with diagnostics",
          "[view][import][native-materializer][phase-4]") {
    DesignIR ir;
    ir.root.type = "image";
    ir.root.stable_anchor_id = "missing-logo";
    ir.root.attributes["srcAssetId"] = "asset-missing";
    ir.root.style.width = 64.0f;
    ir.root.style.height = 32.0f;

    std::vector<ImportDiagnostic> diagnostics;
    auto root = build_native_view_tree(ir, {}, {.diagnostics_out = &diagnostics});
    REQUIRE(root != nullptr);
    REQUIRE(dynamic_cast<ImageView*>(root.get()) == nullptr);
    REQUIRE(root->id() == "missing-logo");
    REQUIRE(root->flex().preferred_width == 64.0f);
    REQUIRE(root->flex().preferred_height == 32.0f);
    REQUIRE(diagnostics_contain(diagnostics, "native-missing-asset"));
    REQUIRE(diagnostics_count(diagnostics, "native-materialize-unresolved-asset") == 0);
}

TEST_CASE("baked native materializer applies token theme only to the detached root",
          "[view][import][native-materializer][phase-4]") {
    DesignIR ir;
    ir.root.type = "frame";
    ir.root.stable_anchor_id = "root";
    ir.root.children.push_back(label("child", "Voice", 40.0f, 20.0f));
    ir.tokens.colors["accent.primary"] = "#112233";

    auto root = build_native_view_tree(ir, {}, {});
    REQUIRE(root != nullptr);
    REQUIRE(root->theme().colors.count("accent.primary") == 1);
    REQUIRE(root->child_count() == 1);
    REQUIRE(root->child_at(0)->theme().colors.empty());
}

TEST_CASE("baked native materializer applies a CSS background gradient",
          "[view][import][native-materializer][gradient]") {
    // Real Figma imports paint their light "hero" panels and illustration fills
    // as CSS gradients (e.g. ELYSIUM's Rectangle 5). Dropping background_gradient
    // was the dominant dark/light parity gap; the materializer must route it
    // through the shared css_gradient helper. See css_gradient.cpp.
    DesignIR ir;
    ir.root.type = "frame";
    ir.root.stable_anchor_id = "panel";
    ir.root.style.width = 320.0f;
    ir.root.style.height = 200.0f;
    ir.root.style.background_color = "#1c1d1d";  // solid base, painted under
    ir.root.style.background_gradient =
        "linear-gradient(to bottom, #e4edf6, #b7c8db)";

    auto root = build_native_view_tree(ir, {}, {});
    REQUIRE(root != nullptr);
    REQUIRE(root->has_background_gradient());
    REQUIRE(root->background_gradient_type() == 1);  // 1 = linear
}

TEST_CASE("baked native materializer reproduces the design's captured knob pointer",
          "[view][import][native-materializer][knob][sprite]") {
    // hoist_captured_art_knobs stamps the design's own pointer geometry; the
    // materializer must forward it to the Knob (set_captured_indicator) so the
    // renderer draws THAT pointer over the disc instead of a synthetic notch —
    // killing the double line and aligning to the disc's baked reference ticks.
    DesignIR ir;
    ir.root.type = "frame";
    ir.root.stable_anchor_id = "root";
    ir.root.style.width = 100.0f;
    ir.root.style.height = 100.0f;

    IRNode knob;
    knob.type = "frame";
    knob.stable_anchor_id = "knob";
    knob.audio_widget = AudioWidgetType::knob;
    knob.style.width = 30.0f;
    knob.style.height = 32.0f;
    // Post-hoist + post-enrich attribute contract for a captured-art knob.
    knob.attributes["asset_ref"] = "disc";
    knob.attributes["asset_path"] = "/resolved/disc.png";
    knob.attributes["png_natural_w"] = "420";
    knob.attributes["png_natural_h"] = "484";
    knob.attributes["knob_ind_r_in"] = "0.604";
    knob.attributes["knob_ind_r_out"] = "0.936";
    knob.attributes["knob_ind_w"] = "0.05";
    knob.attributes["knob_ind_color"] = "#ffffff";
    ir.root.children.push_back(std::move(knob));

    auto root = build_native_view_tree(ir, {}, {});
    REQUIRE(root != nullptr);
    REQUIRE(root->child_count() == 1);
    auto* k = dynamic_cast<Knob*>(root->child_at(0));
    REQUIRE(k != nullptr);
    REQUIRE(k->has_captured_indicator());
    CHECK(k->captured_indicator_r_out() == Catch::Approx(0.936f));
    CHECK(k->captured_indicator_r_in() == Catch::Approx(0.604f));
}

TEST_CASE("imported text vertically centers in a slot taller than its line",
          "[view][import][native-materializer][text]") {
    // Figma centers text in a fixed-height frame, but the IR carries no
    // textAlignVertical. A single-line label given a slot taller than its font
    // (e.g. an 8px "SEARCH" in a 17px box) must center; a tight box stays top.
    // The web-compat codegen emits verticalAlign:middle under the SAME rule, so
    // the two render paths agree (screenshot-parity invariant). Label default top.
    SECTION("tall slot → center") {
        DesignIR ir;
        ir.root = label("search", "SEARCH", 80.0f, 17.0f);
        ir.root.style.font_size = 8.0f;
        auto root = build_native_view_tree(ir, {}, {});
        auto* lbl = dynamic_cast<Label*>(root.get());
        REQUIRE(lbl != nullptr);
        REQUIRE(lbl->vertical_align() == pulp::canvas::TextVerticalAlign::center);
    }
    SECTION("tight slot → top (unchanged)") {
        DesignIR ir;
        ir.root = label("tight", "Hz", 40.0f, 9.0f);
        ir.root.style.font_size = 8.0f;  // 9 <= 8 * 1.15 → not centered
        auto root = build_native_view_tree(ir, {}, {});
        auto* lbl = dynamic_cast<Label*>(root.get());
        REQUIRE(lbl != nullptr);
        REQUIRE(lbl->vertical_align() == pulp::canvas::TextVerticalAlign::top);
    }
}

TEST_CASE("rasterized-vector image does not redraw its baked stroke as a box border",
          "[view][import][native-materializer][image][fidelity]") {
    // A Figma vector exported as a PNG carries its stroke as border_color /
    // border_width, but the stroke is already in the raster. Drawing it again
    // paints a spurious outline — the visible bug was a purple rectangle around
    // the FILTER & EQ curve. Image views must suppress the CSS border.
    DesignIR ir;
    ir.source = DesignSource::figma_plugin;
    ir.root.type = "image";
    ir.root.stable_anchor_id = "curve";
    ir.root.attributes["asset_ref"] = "3:188";
    ir.root.style.width = 177.0f;
    ir.root.style.height = 26.0f;
    ir.root.style.border_color = "#7e6aff";
    ir.root.style.border_width = 1.0f;

    IRAssetRef asset;
    asset.asset_id = "3:188";
    asset.original_uri = "figma://fixture/3:188";
    asset.local_path = "/resolved/assets/3_188.png";
    asset.mime = "image/png";
    ir.asset_manifest.assets.push_back(asset);

    auto root = build_native_view_tree(ir, ir.asset_manifest, {});
    REQUIRE(root != nullptr);
    auto* image = dynamic_cast<ImageView*>(root.get());
    REQUIRE(image != nullptr);
    CHECK(image->border_width() == Catch::Approx(0.0f));
}

TEST_CASE("baked native materializer resolves an rgba() background color",
          "[view][import][native-materializer][color]") {
    // Figma demotes a hairline stroke (the FILTER & EQ grid Line images) to a
    // 1px frame whose fill is the stroke color — typically rgba(171,171,171,0.1).
    // parse_hex_color drops rgba(), so the grid painted nothing; apply_visual_style
    // now falls back to the shared CSS color parser. Without this the EQ grid
    // (and any rgba fill) is invisible.
    DesignIR ir;
    ir.root.type = "frame";
    ir.root.stable_anchor_id = "grid-line";
    ir.root.style.width = 177.0f;
    ir.root.style.height = 1.0f;
    ir.root.style.background_color = "rgba(171, 171, 171, 0.1)";

    auto root = build_native_view_tree(ir, {}, {});
    REQUIRE(root != nullptr);
    REQUIRE(root->has_background_color());
    const auto c = root->background_color();
    CHECK(c.r == Catch::Approx(171.0f / 255.0f).margin(0.01f));
    CHECK(c.a == Catch::Approx(0.1f).margin(0.01f));
}

TEST_CASE("baked native materializer maps a Dropdown frame to an interactive ComboBox",
          "[view][import][native-materializer][combo-box]") {
    // ELYSIUM's FX RACK ships explicit "Dropdown" frames (a selected-value text
    // + a chevron image). Recognized by layer name → a functional ComboBox whose
    // first item is the captured selection; the source has no option list, so
    // stub options demonstrate the popup. The text/chevron children are
    // suppressed (the ComboBox paints its own display).
    DesignIR ir;
    ir.root.type = "frame";
    ir.root.stable_anchor_id = "root";

    IRNode dropdown;
    dropdown.type = "frame";
    dropdown.name = "Dropdown";
    dropdown.stable_anchor_id = "fx-delay";
    IRNode value;
    value.type = "text";
    value.text_content = "1/4 Delay";
    value.stable_anchor_id = "fx-delay-text";
    IRNode chevron;                  // a SQUARE down-chevron marks a real dropdown
    chevron.type = "image";
    chevron.name = "expand_more";
    chevron.attributes["asset_ref"] = "chevron";
    chevron.style.width = 16.0f;
    chevron.style.height = 16.0f;
    chevron.stable_anchor_id = "fx-delay-chevron";
    dropdown.children.push_back(value);
    dropdown.children.push_back(chevron);
    ir.root.children.push_back(std::move(dropdown));

    auto root = build_native_view_tree(ir, {}, {});
    REQUIRE(root != nullptr);
    REQUIRE(root->child_count() == 1);
    auto* combo = dynamic_cast<ComboBox*>(root->child_at(0));
    REQUIRE(combo != nullptr);
    REQUIRE_FALSE(combo->items().empty());
    CHECK(combo->items().front() == "1/4 Delay");
    CHECK(combo->selected_text() == "1/4 Delay");
    CHECK(combo->items().size() >= 2);           // stub options for the popup
    CHECK(combo->child_count() == 0);            // text + chevron suppressed
}

TEST_CASE("baked native materializer leaves prev/next cyclers and templates alone",
          "[view][import][native-materializer][combo-box]") {
    // Not every "Dropdown"-named frame is a dropdown. A prev/next preset CYCLER
    // (ELYSIUM's ENVELOPE/FILTER/FX-RACK headers) uses a WIDE "< >" icon, and an
    // unconfigured design-system TEMPLATE shows the literal word "Dropdown".
    // Neither must become a ComboBox — they stay plain frames.
    auto make_dropdown = [](const std::string& value_text, float icon_w,
                            float icon_h) {
        IRNode dd;
        dd.type = "frame";
        dd.name = "Dropdown";
        dd.stable_anchor_id = "dd";
        IRNode v;
        v.type = "text";
        v.text_content = value_text;
        v.stable_anchor_id = "dd-text";
        IRNode icon;
        icon.type = "image";
        icon.attributes["asset_ref"] = "icon";
        icon.style.width = icon_w;
        icon.style.height = icon_h;
        icon.stable_anchor_id = "dd-icon";
        dd.children.push_back(v);
        dd.children.push_back(icon);
        return dd;
    };

    SECTION("prev/next cycler (wide < > icon) stays a frame") {
        DesignIR ir;
        ir.root = make_dropdown("Short Plucks", 42.0f, 16.0f);  // aspect 2.6
        auto root = build_native_view_tree(ir, {}, {});
        REQUIRE(root != nullptr);
        REQUIRE(dynamic_cast<ComboBox*>(root.get()) == nullptr);
    }
    SECTION("unconfigured template ('Dropdown' value) renders nothing") {
        DesignIR ir;
        ir.root = make_dropdown("Dropdown", 16.0f, 16.0f);  // square but template
        auto root = build_native_view_tree(ir, {}, {});
        REQUIRE(root != nullptr);
        // Not a ComboBox, and inert: a hidden placeholder with no children, so it
        // can't surface as a stray dropdown between panels.
        REQUIRE(dynamic_cast<ComboBox*>(root.get()) == nullptr);
        CHECK(root->child_count() == 0);
        CHECK_FALSE(root->hit_testable());
    }
}

TEST_CASE("baked native materializer maps a Search container to a full-box field",
          "[view][import][native-materializer][text-editor]") {
    // ELYSIUM's "Search" field is a CONTAINER (a background pill + a "Search"
    // placeholder text + a magnifier icon). The WHOLE box becomes a TAPPABLE
    // input (not just the tiny text cell): the placeholder is "SEARCH", the icon
    // is kept as an overlay, and the placeholder text + bg chrome are dropped.
    DesignIR ir;
    ir.root.type = "frame";          // the search container (e.g. "Group 59")
    ir.root.stable_anchor_id = "search-box";
    ir.root.style.width = 184.0f;
    ir.root.style.height = 26.0f;

    IRNode bg;                        // background pill — dropped
    bg.type = "frame";
    bg.name = "Rectangle 66";
    bg.stable_anchor_id = "search-bg";
    IRNode placeholder;              // the placeholder text — becomes the editor's
    placeholder.type = "text";
    placeholder.name = "Search";
    placeholder.text_content = "SEARCH";
    placeholder.style.font_size = 8.0f;
    placeholder.stable_anchor_id = "search-text";
    IRNode icon;                     // magnifier — kept as an overlay child
    icon.type = "image";
    icon.name = "ic:round-search";
    icon.attributes["asset_ref"] = "search-icon";
    icon.style.left = 6.0f;
    icon.style.width = 15.0f;
    icon.stable_anchor_id = "search-icon";
    ir.root.children.push_back(bg);
    ir.root.children.push_back(placeholder);
    ir.root.children.push_back(icon);

    auto root = build_native_view_tree(ir, {}, {});
    REQUIRE(root != nullptr);
    auto* editor = dynamic_cast<TextEditor*>(root.get());
    REQUIRE(editor != nullptr);
    CHECK(editor->placeholder == "SEARCH");
    CHECK(editor->content_inset_left() > 15.0f);   // text cleared past the icon
    CHECK(root->child_count() == 1);               // only the icon kept (no text/bg)

    // Tappable + editable: focus then type enters text over the placeholder.
    editor->on_focus_changed(true);
    TextInputEvent te;
    te.text = "drums";
    editor->on_text_input(te);
    CHECK(editor->text() == "drums");
}

TEST_CASE("hoist_captured_art_knobs promotes a body disc + pointer to an interactive skin",
          "[view][import][native-materializer][knob][sprite]") {
    // A captured-art knob ships a body disc image + a ~0-area pointer hairline
    // (the native notch replaces the pointer). Hoist the disc's asset_ref onto
    // the knob, drop the captured children, keep it a knob (interactive).
    DesignIR ir;
    ir.root.type = "frame";
    ir.root.stable_anchor_id = "root";

    IRNode knob;
    knob.type = "frame";
    knob.stable_anchor_id = "knob";
    knob.audio_widget = AudioWidgetType::knob;
    IRNode body;       // the disc, at (0,0), 30x32 → center (15,16), half-extent 15
    body.type = "image";
    body.stable_anchor_id = "body";
    body.attributes["asset_ref"] = "disc-asset";
    body.style.left = 0.0f;
    body.style.top = 0.0f;
    body.style.width = 30.0f;
    body.style.height = 32.0f;
    IRNode pointer;    // a ~0-width stroked pointer hairline near top-center
    pointer.type = "image";
    pointer.stable_anchor_id = "ptr";
    pointer.attributes["asset_ref"] = "ptr-asset";
    pointer.style.left = 14.0f;   // ~centered on the 30-wide disc
    pointer.style.top = 2.0f;
    pointer.style.width = 0.0f;
    pointer.style.height = 5.0f;  // spans y 2..7
    pointer.style.border_width = 1.5f;
    pointer.style.border_color = "#ffffff";
    knob.children.push_back(body);
    knob.children.push_back(pointer);
    ir.root.children.push_back(std::move(knob));

    hoist_captured_art_knobs(ir);

    const auto& k = ir.root.children.at(0);
    REQUIRE(k.audio_widget == AudioWidgetType::knob);          // stays interactive
    REQUIRE(k.attributes.at("asset_ref") == "disc-asset");     // disc hoisted
    REQUIRE(k.attributes.at("sprite_strip_frame_count") == "1");
    // Captured layers (disc + pointer) are gone — the design's pointer geometry
    // is stamped instead, so the renderer reproduces the real indicator.
    REQUIRE(k.children.empty());
    // pointer ends (14,2)/(14,7) from disc center (15,16): far ≈ 14.04, near ≈ 9.06;
    // half-extent 15 → r_out ≈ 0.936, r_in ≈ 0.604.
    REQUIRE(k.attributes.count("knob_ind_r_out") == 1);
    const float r_out = std::stof(k.attributes.at("knob_ind_r_out"));
    const float r_in = std::stof(k.attributes.at("knob_ind_r_in"));
    CHECK(r_out == Catch::Approx(0.936f).margin(0.02f));
    CHECK(r_in == Catch::Approx(0.604f).margin(0.02f));
    CHECK(r_out > r_in);
    REQUIRE(k.attributes.at("knob_ind_color") == "#ffffff");
}

TEST_CASE("hoist_captured_art_knobs demotes a multi-layer knob to a static container",
          "[view][import][native-materializer][knob][sprite]") {
    // Two SUBSTANTIAL captured layers (e.g. body + highlight) can't fit one
    // single-frame skin, so the knob demotes to a plain container — every layer
    // renders as an image (faithful but not turnable), no silent layer loss.
    DesignIR ir;
    ir.root.type = "frame";
    ir.root.stable_anchor_id = "root";

    IRNode knob;
    knob.type = "frame";
    knob.stable_anchor_id = "knob";
    knob.audio_widget = AudioWidgetType::knob;
    for (const char* id : {"body", "highlight"}) {
        IRNode layer;
        layer.type = "image";
        layer.stable_anchor_id = id;
        layer.attributes["asset_ref"] = std::string(id) + "-asset";
        layer.style.width = 40.0f;
        layer.style.height = 40.0f;
        knob.children.push_back(std::move(layer));
    }
    ir.root.children.push_back(std::move(knob));

    hoist_captured_art_knobs(ir);

    const auto& k = ir.root.children.at(0);
    REQUIRE(k.audio_widget == AudioWidgetType::none);   // demoted to container
    REQUIRE(k.children.size() == 2);                    // both layers preserved
    REQUIRE(k.attributes.count("asset_ref") == 0);
}

TEST_CASE("baked native materializer leaves background gradient unset without one",
          "[view][import][native-materializer][gradient]") {
    DesignIR ir;
    ir.root.type = "frame";
    ir.root.stable_anchor_id = "panel";
    ir.root.style.width = 100.0f;
    ir.root.style.height = 100.0f;
    ir.root.style.background_color = "#202020";

    auto root = build_native_view_tree(ir, {}, {});
    REQUIRE(root != nullptr);
    REQUIRE_FALSE(root->has_background_gradient());
}

TEST_CASE("standard meter snaps fill edge and suppresses duplicate peak line",
          "[view][meter][import][native-materializer]") {
    Meter meter;
    meter.set_bounds({0.0f, 0.0f, 8.0f, 56.0f});
    meter.set_level(0.72f, 0.72f);

    pulp::canvas::RecordingCanvas same_edge_canvas;
    meter.paint(same_edge_canvas);

    const auto* fill = first_meter_fill_rect(same_edge_canvas);
    REQUIRE(fill != nullptr);
    REQUIRE(fill->f[0] == 1.0f);
    REQUIRE(fill->f[1] == 16.0f);
    REQUIRE(fill->f[2] == 6.0f);
    REQUIRE(fill->f[3] == 40.0f);
    REQUIRE(same_edge_canvas.count(pulp::canvas::DrawCommand::Type::stroke_line) == 0);

    meter.set_level(0.50f, 0.75f);
    pulp::canvas::RecordingCanvas separate_peak_canvas;
    meter.paint(separate_peak_canvas);

    fill = first_meter_fill_rect(separate_peak_canvas);
    REQUIRE(fill != nullptr);
    REQUIRE(fill->f[1] == 28.0f);
    REQUIRE(fill->f[3] == 28.0f);
    REQUIRE(separate_peak_canvas.count(pulp::canvas::DrawCommand::Type::stroke_line) == 1);

    const pulp::canvas::DrawCommand* peak_line = nullptr;
    for (const auto& command : separate_peak_canvas.commands()) {
        if (command.type == pulp::canvas::DrawCommand::Type::stroke_line) {
            peak_line = &command;
            break;
        }
    }
    REQUIRE(peak_line != nullptr);
    REQUIRE(peak_line->f[0] == 1.0f);
    REQUIRE(peak_line->f[1] == 14.0f);
    REQUIRE(peak_line->f[2] == 7.0f);
    REQUIRE(peak_line->f[3] == 14.0f);
}

TEST_CASE("baked native materializer preserves waveform preview shape",
          "[view][waveform][import][native-materializer]") {
    DesignIR ir;
    ir.root = frame("waveform-root", 88.0f, 42.0f, LayoutDirection::column);

    auto waveform_node = frame("osc-waveform", 88.0f, 42.0f, LayoutDirection::column);
    waveform_node.audio_widget = AudioWidgetType::waveform;
    waveform_node.attributes["pulpWaveformShape"] = "saw";
    ir.root.children.push_back(std::move(waveform_node));

    auto root = build_native_view_tree(ir, {}, {});
    REQUIRE(root != nullptr);
    REQUIRE(root->child_count() == 1);

    auto* waveform = dynamic_cast<WaveformView*>(root->child_at(0));
    REQUIRE(waveform != nullptr);
    REQUIRE(waveform->preview_shape() == WaveformView::PreviewShape::saw);

    waveform->set_bounds({0.0f, 0.0f, 88.0f, 42.0f});
    pulp::canvas::RecordingCanvas canvas;
    waveform->paint(canvas);

    REQUIRE(canvas.count(pulp::canvas::DrawCommand::Type::stroke_line) == 1);
    REQUIRE(canvas.count(pulp::canvas::DrawCommand::Type::begin_path) == 1);
    REQUIRE(canvas.count(pulp::canvas::DrawCommand::Type::move_to) == 1);
    REQUIRE(canvas.count(pulp::canvas::DrawCommand::Type::line_to) == 7);
    REQUIRE(canvas.count(pulp::canvas::DrawCommand::Type::stroke_current_path) == 1);
}

TEST_CASE("baked native materializer maps fill sizing by parent axis",
          "[view][import][native-materializer][phase-4]") {
    DesignIR ir;
    ir.root = frame("row-root", 300.0f, 120.0f, LayoutDirection::row);

    auto main_axis_fill = frame("main-fill", 0.0f, 40.0f, LayoutDirection::column);
    main_axis_fill.style.width.reset();
    main_axis_fill.layout.width_mode = SizingMode::fill;

    auto cross_axis_fill = frame("cross-fill", 64.0f, 0.0f, LayoutDirection::column);
    cross_axis_fill.style.height.reset();
    cross_axis_fill.layout.height_mode = SizingMode::fill;

    ir.root.children.push_back(std::move(main_axis_fill));
    ir.root.children.push_back(std::move(cross_axis_fill));

    auto root = build_native_view_tree(ir, {}, {});
    REQUIRE(root != nullptr);
    REQUIRE(root->child_count() == 2);

    const auto& main_flex = root->child_at(0)->flex();
    const auto& cross_flex = root->child_at(1)->flex();
    REQUIRE(main_flex.flex_grow == 1.0f);
    REQUIRE(cross_flex.flex_grow == 0.0f);
    REQUIRE(cross_flex.align_self == FlexAlign::stretch);
}

TEST_CASE("baked native materializer preserves audio widget attributes",
          "[view][import][native-materializer][phase-4]") {
    DesignIR ir;
    ir.root = frame("audio-root", 320.0f, 120.0f, LayoutDirection::row);

    auto knob_node = frame("drive", 64.0f, 64.0f, LayoutDirection::column);
    knob_node.audio_widget = AudioWidgetType::knob;
    knob_node.audio_label = "Drive";
    knob_node.audio_min = -60.0f;
    knob_node.audio_max = 0.0f;
    knob_node.audio_default = -15.0f;
    knob_node.attributes["value"] = "0.2";

    auto fader_node = frame("mix", 96.0f, 32.0f, LayoutDirection::column);
    fader_node.audio_widget = AudioWidgetType::fader;
    fader_node.audio_label = "Mix";
    fader_node.attributes["value"] = "0.33";
    fader_node.attributes["orientation"] = "horizontal";
    fader_node.attributes["pulpThumbShape"] = "rectangle";
    fader_node.attributes["pulpThumbWidth"] = "17";
    fader_node.attributes["pulpThumbHeight"] = "5";
    fader_node.attributes["pulpThumbCornerRadius"] = "1";

    auto circle_fader_node = frame("trim", 32.0f, 96.0f, LayoutDirection::column);
    circle_fader_node.audio_widget = AudioWidgetType::fader;
    circle_fader_node.audio_label = "Trim";
    circle_fader_node.attributes["value"] = "0.5";
    circle_fader_node.attributes["pulpThumbShape"] = "circle";
    circle_fader_node.attributes["pulpThumbWidth"] = "12";
    circle_fader_node.attributes["pulpThumbHeight"] = "12";

    auto meter_node = frame("level", 96.0f, 24.0f, LayoutDirection::column);
    meter_node.audio_widget = AudioWidgetType::meter;
    meter_node.attributes["value"] = "0.25";
    meter_node.attributes["orientation"] = "horizontal";

    auto xy_node = frame("xy", 72.0f, 72.0f, LayoutDirection::column);
    xy_node.audio_widget = AudioWidgetType::xy_pad;
    xy_node.attributes["x"] = "0.2";
    xy_node.attributes["y"] = "0.8";

    auto choice_node = frame("waveform-choice", 21.0f, 13.0f, LayoutDirection::column);
    choice_node.type = "toggle_button";
    choice_node.text_content = "SAW";
    choice_node.attributes["checked"] = "true";
    choice_node.attributes["pulpOnBackgroundColor"] = "#1e1008";
    choice_node.attributes["pulpOffBackgroundColor"] = "#00000000";
    choice_node.attributes["pulpOnTextColor"] = "#ff6b35";
    choice_node.attributes["pulpOffTextColor"] = "#666666";
    choice_node.attributes["pulpOnBorderColor"] = "#ff6b35";
    choice_node.attributes["pulpOffBorderColor"] = "#1e1e24";
    choice_node.attributes["pulpCornerRadius"] = "2";
    choice_node.attributes["pulpFontSize"] = "7";

    auto editor_node = frame("preset-name", 96.0f, 24.0f, LayoutDirection::column);
    editor_node.type = "input";
    editor_node.attributes["type"] = "text";
    editor_node.attributes["pulpPlaceholder"] = "Preset";
    editor_node.attributes["pulpInitialValue"] = "Init";

    ir.root.children.push_back(std::move(knob_node));
    ir.root.children.push_back(std::move(fader_node));
    ir.root.children.push_back(std::move(circle_fader_node));
    ir.root.children.push_back(std::move(meter_node));
    ir.root.children.push_back(std::move(xy_node));
    ir.root.children.push_back(std::move(choice_node));
    ir.root.children.push_back(std::move(editor_node));

    auto root = build_native_view_tree(ir, {}, {.preview_mode = true});
    REQUIRE(root != nullptr);
    REQUIRE(root->child_count() == 7);

    auto* knob = dynamic_cast<Knob*>(root->child_at(0));
    REQUIRE(knob != nullptr);
    REQUIRE(knob->label() == "Drive");
    REQUIRE(knob->value() == 0.2f);
    REQUIRE(knob->default_value() == 0.75f);
    REQUIRE(knob->render_style() == WidgetRenderStyle::minimal);

    auto* fader = dynamic_cast<Fader*>(root->child_at(1));
    REQUIRE(fader != nullptr);
    REQUIRE(fader->label() == "Mix");
    REQUIRE(fader->value() == 0.33f);
    REQUIRE(fader->orientation() == Fader::Orientation::horizontal);
    REQUIRE(fader->thumb_shape() == Fader::ThumbShape::rectangle);
    REQUIRE(fader->thumb_width() == 17.0f);
    REQUIRE(fader->thumb_height() == 5.0f);
    REQUIRE(fader->thumb_corner_radius() == 1.0f);
    REQUIRE(fader->render_style() == WidgetRenderStyle::minimal);

    auto* circle_fader = dynamic_cast<Fader*>(root->child_at(2));
    REQUIRE(circle_fader != nullptr);
    REQUIRE(circle_fader->label() == "Trim");
    REQUIRE(circle_fader->thumb_shape() == Fader::ThumbShape::circle);
    REQUIRE(circle_fader->thumb_width() == 12.0f);
    REQUIRE(circle_fader->thumb_height() == 12.0f);

    auto* meter = dynamic_cast<Meter*>(root->child_at(3));
    REQUIRE(meter != nullptr);
    REQUIRE(meter->display_rms() == 0.25f);
    REQUIRE(meter->display_peak() == 0.25f);
    REQUIRE(meter->orientation() == Meter::Orientation::horizontal);
    REQUIRE(meter->render_style() == WidgetRenderStyle::minimal);

    auto* xy = dynamic_cast<XYPad*>(root->child_at(4));
    REQUIRE(xy != nullptr);
    REQUIRE(xy->x_value() == 0.2f);
    REQUIRE(xy->y_value() == 0.8f);

    auto* choice = dynamic_cast<ToggleButton*>(root->child_at(5));
    REQUIRE(choice != nullptr);
    REQUIRE(choice->label() == "SAW");
    REQUIRE(choice->is_on());
    REQUIRE(choice->on_background_color_override().has_value());
    REQUIRE(choice->on_background_color_override()->r8() == 0x1e);
    REQUIRE(choice->on_background_color_override()->g8() == 0x10);
    REQUIRE(choice->on_background_color_override()->b8() == 0x08);
    REQUIRE(choice->off_background_color_override().has_value());
    REQUIRE(choice->off_background_color_override()->a8() == 0x00);
    REQUIRE(choice->on_text_color_override().has_value());
    REQUIRE(choice->on_text_color_override()->r8() == 0xff);
    REQUIRE(choice->on_text_color_override()->g8() == 0x6b);
    REQUIRE(choice->on_text_color_override()->b8() == 0x35);
    REQUIRE(choice->off_text_color_override().has_value());
    REQUIRE(choice->off_text_color_override()->r8() == 0x66);
    REQUIRE(choice->off_border_color_override().has_value());
    REQUIRE(choice->off_border_color_override()->r8() == 0x1e);
    REQUIRE(choice->corner_radius_override() == 2.0f);
    REQUIRE(choice->font_size_override() == 7.0f);

    auto* editor = dynamic_cast<TextEditor*>(root->child_at(6));
    REQUIRE(editor != nullptr);
    REQUIRE(editor->placeholder == "Preset");
    REQUIRE(editor->text() == "Init");
}

TEST_CASE("baked native materializer routes promoted widget hits over decorative descendants",
          "[view][import][native-materializer][hit-test]") {
    DesignIR ir;
    ir.root = frame("root", 120.0f, 120.0f, LayoutDirection::column);

    auto knob_node = frame("gain-knob", 80.0f, 80.0f, LayoutDirection::column);
    knob_node.audio_widget = AudioWidgetType::knob;
    knob_node.audio_label = "Gain";
    knob_node.attributes["value"] = "0.5";

    auto decorative_art = frame("gain-knob-imported-art", 80.0f, 80.0f, LayoutDirection::column);
    decorative_art.style.background_color = "#ff00ff";
    knob_node.children.push_back(std::move(decorative_art));

    auto nested_button = frame("gain-knob-nested-button", 60.0f, 20.0f, LayoutDirection::column);
    nested_button.type = "button";
    nested_button.text_content = "Fine";
    knob_node.children.push_back(std::move(nested_button));
    ir.root.children.push_back(std::move(knob_node));

    auto root = build_native_view_tree(ir, {}, {.preview_mode = true});
    REQUIRE(root != nullptr);
    REQUIRE(root->child_count() == 1);

    root->set_bounds({0, 0, 120.0f, 120.0f});
    root->layout_children();

    auto* knob = dynamic_cast<Knob*>(root->child_at(0));
    REQUIRE(knob != nullptr);
    REQUIRE(knob->child_count() == 2);
    REQUIRE_FALSE(knob->child_at(0)->hit_testable());
    REQUIRE(knob->child_at(1)->hit_testable());

    auto* hit = root->hit_test({40.0f, 40.0f});
    REQUIRE(hit == knob);
}

TEST_CASE("binding-backed imported knob drag updates from the visible body",
          "[view][import][native-materializer][hit-test][binding]") {
    DesignIR ir;
    ir.root = frame("root", 160.0f, 120.0f, LayoutDirection::column);

    auto knob_node = frame("bound-gain-knob", 80.0f, 80.0f, LayoutDirection::column);
    knob_node.audio_widget = AudioWidgetType::knob;
    knob_node.audio_label = "Gain";
    knob_node.stable_anchor_id = "figma:bound-gain-knob";
    knob_node.attributes["value"] = "0.4";
    knob_node.attributes["pulpParamKey"] = "filter.gain";
    knob_node.attributes["pulpRouteId"] = "figma-plugin:filter.gain";
    knob_node.attributes["pulpBindingModule"] = "filter";
    knob_node.attributes["pulpBindingParam"] = "gain";
    knob_node.attributes["pulpEventContract"] = "onChange:set_param:filter.gain";
    knob_node.attributes["pulpGestureContract"] = "rotary_drag:begin/update/end";

    auto decorative_art = frame("imported-knob-art", 80.0f, 80.0f, LayoutDirection::column);
    decorative_art.style.background_color = "#ff00ff";
    auto value_label = label("imported-value-label", "80%", 80.0f, 20.0f);
    knob_node.children.push_back(std::move(decorative_art));
    knob_node.children.push_back(std::move(value_label));
    ir.root.children.push_back(std::move(knob_node));

    auto root = build_native_view_tree(ir, {}, {.preview_mode = true});
    REQUIRE(root != nullptr);
    REQUIRE(root->child_count() == 1);

    root->set_bounds({0, 0, 160.0f, 120.0f});
    root->layout_children();

    auto* knob = dynamic_cast<Knob*>(root->child_at(0));
    REQUIRE(knob != nullptr);
    REQUIRE(knob->child_count() == 2);
    REQUIRE_FALSE(knob->child_at(0)->hit_testable());
    REQUIRE_FALSE(knob->child_at(1)->hit_testable());

    auto* body_hit = root->hit_test({40.0f, 40.0f});
    REQUIRE(body_hit == knob);

    BindingBackedKnobContext binding;
    bind_native_view_tree(*root, ir, binding);
    REQUIRE(binding.bound_knobs.size() == 1);
    REQUIRE(binding.bound_knobs[0].route_id == "figma-plugin:filter.gain");
    REQUIRE(binding.bound_knobs[0].param_key == "filter.gain");
    REQUIRE(binding.bound_knobs[0].binding_module == "filter");
    REQUIRE(binding.bound_knobs[0].binding_param == "gain");
    REQUIRE(binding.bound_knobs[0].event_contract == "onChange:set_param:filter.gain");
    REQUIRE(binding.bound_knobs[0].gesture_contract == "rotary_drag:begin/update/end");

    const float before = knob->value();
    REQUIRE(before == 0.4f);
    REQUIRE(binding.normalized_value() == before);

    root->simulate_drag({40.0f, 40.0f}, {40.0f, 10.0f}, 3);

    REQUIRE(knob->value() > before);
    REQUIRE(binding.normalized_value() == knob->value());
    REQUIRE_FALSE(binding.changes.empty());
    REQUIRE(binding.changes.back() == knob->value());
    REQUIRE(binding.gesture_begin_count == 1);
    REQUIRE(binding.gesture_end_count == 1);
}

TEST_CASE("imported fader, button, and text input respond to interaction (Phase D)",
          "[view][import][native-materializer][interaction][phase-d]") {
    // Phase D — prove non-knob imported widgets are interactive, not just
    // rendered. The GRAINS knob already has a GPU-harness hit+drag proof; ELYSIUM
    // itself is knob-only (its "Search" is a static text label), so this covers
    // the remaining interactive widget classes — fader (drag), toggle button
    // (click), text input (type) — on a synthetic imported fixture through the
    // real materialize → hit-test → event path.
    DesignIR ir;
    ir.root.type = "frame";
    ir.root.stable_anchor_id = "root";
    ir.root.style.width = 200.0f;
    ir.root.style.height = 200.0f;
    ir.root.layout.direction = LayoutDirection::column;
    ir.root.layout.gap = 8.0f;

    auto fader = frame("mix", 160.0f, 24.0f, LayoutDirection::column);
    fader.audio_widget = AudioWidgetType::fader;
    fader.audio_label = "Mix";
    fader.attributes["value"] = "0.1";
    fader.attributes["orientation"] = "horizontal";

    auto toggle = frame("sel", 160.0f, 24.0f, LayoutDirection::column);
    toggle.type = "toggle_button";
    toggle.text_content = "ON";
    toggle.attributes["checked"] = "false";

    auto input = frame("name", 160.0f, 24.0f, LayoutDirection::column);
    input.type = "input";
    input.attributes["type"] = "text";
    input.attributes["pulpInitialValue"] = "ab";

    ir.root.children.push_back(std::move(fader));
    ir.root.children.push_back(std::move(toggle));
    ir.root.children.push_back(std::move(input));

    auto root = build_native_view_tree(ir, {}, {});
    REQUIRE(root != nullptr);
    REQUIRE(root->child_count() == 3);
    root->set_bounds({0.0f, 0.0f, 200.0f, 200.0f});
    root->layout_children();

    auto* f = dynamic_cast<Fader*>(root->child_at(0));
    auto* t = dynamic_cast<ToggleButton*>(root->child_at(1));
    auto* e = dynamic_cast<TextEditor*>(root->child_at(2));
    REQUIRE(f != nullptr);
    REQUIRE(t != nullptr);
    REQUIRE(e != nullptr);

    // Fader: dragging along its axis raises the value (routed through hit-test).
    const Rect fb = f->bounds();
    const float fader_before = f->value();
    root->simulate_drag({fb.x + fb.width * 0.15f, fb.y + fb.height * 0.5f},
                        {fb.x + fb.width * 0.85f, fb.y + fb.height * 0.5f}, 6);
    INFO("fader before=" << fader_before << " after=" << f->value());
    REQUIRE(f->value() > fader_before);

    // Toggle button: a click flips its state — it is a target, not a picture.
    const Rect tb = t->bounds();
    const bool toggle_before = t->is_on();
    root->simulate_click({tb.x + tb.width * 0.5f, tb.y + tb.height * 0.5f});
    INFO("toggle before=" << toggle_before << " after=" << t->is_on());
    REQUIRE(t->is_on() != toggle_before);

    // Text input: focusing and typing changes the committed text.
    const std::string text_before = e->text();
    e->on_focus_changed(true);
    TextInputEvent typed;
    typed.text = "c";
    e->on_text_input(typed);
    INFO("text before='" << text_before << "' after='" << e->text() << "'");
    REQUIRE(e->text() != text_before);
}

TEST_CASE("native materializer binding helper requires anchors and routes",
          "[view][import][native-materializer][binding]") {
    DesignIR ir;
    ir.root = frame("root", 320.0f, 260.0f, LayoutDirection::column);

    auto eligible = frame("eligible-knob", 80.0f, 80.0f, LayoutDirection::column);
    eligible.audio_widget = AudioWidgetType::knob;
    eligible.stable_anchor_id = "figma:eligible-knob";
    eligible.attributes["value"] = "0.25";
    eligible.attributes["pulpRouteId"] = "figma-plugin:eligible";
    eligible.attributes["pulpParamKey"] = "filter.eligible";
    eligible.attributes["pulpBindingModule"] = "filter";
    eligible.attributes["pulpBindingParam"] = "eligible";

    auto missing_anchor = frame("missing-anchor-knob", 80.0f, 80.0f, LayoutDirection::column);
    missing_anchor.audio_widget = AudioWidgetType::knob;
    missing_anchor.stable_anchor_id.reset();
    missing_anchor.attributes["value"] = "0.5";
    missing_anchor.attributes["pulpRouteId"] = "figma-plugin:missing-anchor";
    missing_anchor.attributes["pulpParamKey"] = "filter.missing_anchor";

    auto missing_route = frame("missing-route-knob", 80.0f, 80.0f, LayoutDirection::column);
    missing_route.audio_widget = AudioWidgetType::knob;
    missing_route.stable_anchor_id = "figma:missing-route-knob";
    missing_route.attributes["value"] = "0.75";
    missing_route.attributes["pulpParamKey"] = "filter.missing_route";

    auto wrong_type = label("wrong-type-label", "Wrong", 80.0f, 24.0f);
    wrong_type.attributes["pulpRouteId"] = "figma-plugin:wrong-type";
    wrong_type.attributes["pulpParamKey"] = "filter.wrong_type";

    ir.root.children.push_back(std::move(eligible));
    ir.root.children.push_back(std::move(missing_anchor));
    ir.root.children.push_back(std::move(missing_route));
    ir.root.children.push_back(std::move(wrong_type));

    auto root = build_native_view_tree(ir, {}, {.preview_mode = true});
    REQUIRE(root != nullptr);

    BindingBackedKnobContext binding;
    std::vector<ImportDiagnostic> diagnostics;
    bind_native_view_tree(*root, ir, binding, {.diagnostics_out = &diagnostics});

    REQUIRE(binding.bound_knobs.size() == 1);
    REQUIRE(binding.bound_knobs[0].route_id == "figma-plugin:eligible");
    REQUIRE(binding.bound_knobs[0].param_key == "filter.eligible");
    REQUIRE(diagnostics_count(diagnostics, "native-binding-missing-anchor") == 1);
    REQUIRE(diagnostics_count(diagnostics, "native-binding-missing-route") == 1);
    REQUIRE(diagnostics_count(diagnostics, "native-binding-not-applied") == 1);
    REQUIRE(diagnostics_count(diagnostics, "native-binding-anchor-not-found") == 0);
}

TEST_CASE("native materializer binding helper reports anchors missing from the view tree",
          "[view][import][native-materializer][binding]") {
    DesignIR ir;
    ir.root = frame("root", 120.0f, 120.0f, LayoutDirection::column);

    auto knob_node = frame("orphaned-knob", 80.0f, 80.0f, LayoutDirection::column);
    knob_node.audio_widget = AudioWidgetType::knob;
    knob_node.stable_anchor_id = "figma:orphaned-knob";
    knob_node.attributes["value"] = "0.5";
    knob_node.attributes["pulpRouteId"] = "figma-plugin:orphaned";
    knob_node.attributes["pulpParamKey"] = "filter.orphaned";
    ir.root.children.push_back(std::move(knob_node));

    auto root = build_native_view_tree(ir, {}, {.preview_mode = true});
    REQUIRE(root != nullptr);
    REQUIRE(root->child_count() == 1);
    root->child_at(0)->set_anchor_id("figma:different-view");

    BindingBackedKnobContext binding;
    std::vector<ImportDiagnostic> diagnostics;
    bind_native_view_tree(*root, ir, binding, {.diagnostics_out = &diagnostics});

    REQUIRE(binding.bound_knobs.empty());
    REQUIRE(diagnostics_count(diagnostics, "native-binding-anchor-not-found") == 1);
    REQUIRE(diagnostics[0].anchor_id == "figma:orphaned-knob");
}

TEST_CASE("native materializer binding helper refuses duplicate materialized anchors",
          "[view][import][native-materializer][binding]") {
    DesignIR ir;
    ir.root = frame("root", 220.0f, 120.0f, LayoutDirection::row);

    auto first = frame("first-duplicate-knob", 80.0f, 80.0f, LayoutDirection::column);
    first.audio_widget = AudioWidgetType::knob;
    first.stable_anchor_id = "figma:duplicate-knob";
    first.attributes["value"] = "0.25";
    first.attributes["pulpRouteId"] = "figma-plugin:first-duplicate";
    first.attributes["pulpParamKey"] = "filter.first";

    auto second = frame("second-duplicate-knob", 80.0f, 80.0f, LayoutDirection::column);
    second.audio_widget = AudioWidgetType::knob;
    second.stable_anchor_id = "figma:duplicate-knob";
    second.attributes["value"] = "0.75";
    second.attributes["pulpRouteId"] = "figma-plugin:second-duplicate";
    second.attributes["pulpParamKey"] = "filter.second";

    ir.root.children.push_back(std::move(first));
    ir.root.children.push_back(std::move(second));

    auto root = build_native_view_tree(ir, {}, {.preview_mode = true});
    REQUIRE(root != nullptr);

    BindingBackedKnobContext binding;
    std::vector<ImportDiagnostic> diagnostics;
    bind_native_view_tree(*root, ir, binding, {.diagnostics_out = &diagnostics});

    REQUIRE(binding.bound_knobs.empty());
    REQUIRE(diagnostics_count(diagnostics, "native-binding-duplicate-anchor") == 2);
    REQUIRE(diagnostics_count(diagnostics, "native-binding-not-applied") == 0);
    REQUIRE(diagnostics_count(diagnostics, "native-binding-anchor-not-found") == 0);
}

TEST_CASE("generated C++ binding helper requires a unique materialized anchor",
          "[view][import][native-materializer][binding][cpp-codegen]") {
    DesignIR ir;
    ir.root = frame("root", 220.0f, 120.0f, LayoutDirection::row);

    auto first = frame("figma:duplicate-knob", 80.0f, 80.0f, LayoutDirection::column);
    first.audio_widget = AudioWidgetType::knob;
    first.attributes["value"] = "0.25";
    first.attributes["pulpRouteId"] = "figma-plugin:first-duplicate";
    first.attributes["pulpParamKey"] = "filter.first";

    auto second = frame("figma:duplicate-knob", 80.0f, 80.0f, LayoutDirection::column);
    second.audio_widget = AudioWidgetType::knob;
    second.attributes["value"] = "0.75";
    second.attributes["pulpRouteId"] = "figma-plugin:second-duplicate";
    second.attributes["pulpParamKey"] = "filter.second";

    ir.root.children.push_back(std::move(first));
    ir.root.children.push_back(std::move(second));

    const auto result = generate_pulp_cpp(ir, ir.asset_manifest, {});

    REQUIRE(result.source.find(
        "find_imported_view_by_anchor(pulp::view::View& root, std::string_view anchor, int& matches)") !=
        std::string::npos);
    REQUIRE(result.source.find("route_0_match_count == 1") != std::string::npos);
    REQUIRE(result.source.find("route_1_match_count == 1") != std::string::npos);

    TempDir tmp("pulp-native-materializer-duplicate-anchor-codegen");
    const auto header = tmp.path / "imported_ui.hpp";
    const auto source = tmp.path / "imported_ui.cpp";
    const auto object = tmp.path / "imported_ui.o";
    write_text(header, result.header);
    write_text(source, result.source);

    std::string diagnostics;
    const bool compiled = compile_generated_source(source, object, &diagnostics);
    INFO(diagnostics);
    REQUIRE(compiled);
}

TEST_CASE("native materializer binding helper binds routed checkbox metadata",
          "[view][import][native-materializer][binding]") {
    DesignIR ir;
    ir.root = frame("root", 120.0f, 80.0f, LayoutDirection::column);

    auto checkbox_node = frame("bypass-checkbox", 32.0f, 32.0f, LayoutDirection::column);
    checkbox_node.type = "input";
    checkbox_node.stable_anchor_id = "figma:bypass-checkbox";
    checkbox_node.attributes["type"] = "checkbox";
    checkbox_node.attributes["checked"] = "true";
    checkbox_node.attributes["pulpRouteId"] = "figma-plugin:bypass";
    checkbox_node.attributes["pulpParamKey"] = "filter.bypass";
    checkbox_node.attributes["pulpBindingModule"] = "filter";
    checkbox_node.attributes["pulpBindingParam"] = "bypass";
    checkbox_node.attributes["pulpEventContract"] = "onChange:set_param:filter.bypass";
    checkbox_node.attributes["pulpGestureContract"] = "click:toggle";
    ir.root.children.push_back(std::move(checkbox_node));

    auto root = build_native_view_tree(ir, {}, {.preview_mode = true});
    REQUIRE(root != nullptr);
    REQUIRE(root->child_count() == 1);

    auto* checkbox = dynamic_cast<Checkbox*>(root->child_at(0));
    REQUIRE(checkbox != nullptr);
    REQUIRE(checkbox->is_checked());

    BindingBackedKnobContext binding;
    std::vector<ImportDiagnostic> diagnostics;
    bind_native_view_tree(*root, ir, binding, {.diagnostics_out = &diagnostics});

    REQUIRE(binding.bound_checkboxes.size() == 1);
    REQUIRE(binding.bound_checkboxes[0].route_id == "figma-plugin:bypass");
    REQUIRE(binding.bound_checkboxes[0].param_key == "filter.bypass");
    REQUIRE(binding.bound_checkboxes[0].binding_module == "filter");
    REQUIRE(binding.bound_checkboxes[0].binding_param == "bypass");
    REQUIRE(binding.bound_checkboxes[0].event_contract == "onChange:set_param:filter.bypass");
    REQUIRE(binding.bound_checkboxes[0].gesture_contract == "click:toggle");
    REQUIRE(diagnostics.empty());

    checkbox->on_mouse_down({16.0f, 16.0f});
    REQUIRE_FALSE(checkbox->is_checked());
    REQUIRE(binding.checkbox_changes.size() == 1);
    REQUIRE(binding.checkbox_changes[0].first == "filter.bypass");
    REQUIRE(binding.checkbox_changes[0].second == 0.0f);
}

TEST_CASE("native materializer binding helper skips repeated calls for the same context and view",
          "[view][import][native-materializer][binding]") {
    DesignIR ir;
    ir.root = frame("root", 160.0f, 80.0f, LayoutDirection::column);

    auto checkbox_node = frame("bypass-checkbox", 32.0f, 32.0f, LayoutDirection::column);
    checkbox_node.type = "input";
    checkbox_node.stable_anchor_id = "figma:bypass-checkbox";
    checkbox_node.attributes["type"] = "checkbox";
    checkbox_node.attributes["pulpRouteId"] = "figma-plugin:bypass";
    checkbox_node.attributes["pulpParamKey"] = "filter.bypass";
    ir.root.children.push_back(std::move(checkbox_node));

    auto root = build_native_view_tree(ir, {}, {.preview_mode = true});
    REQUIRE(root != nullptr);

    BindingBackedKnobContext binding;
    std::vector<ImportDiagnostic> first_diagnostics;
    bind_native_view_tree(*root, ir, binding, {.diagnostics_out = &first_diagnostics});
    REQUIRE(binding.bound_checkboxes.size() == 1);
    REQUIRE(first_diagnostics.empty());

    std::vector<ImportDiagnostic> second_diagnostics;
    bind_native_view_tree(*root, ir, binding, {.diagnostics_out = &second_diagnostics});
    REQUIRE(binding.bound_checkboxes.size() == 1);
    REQUIRE(diagnostics_count(second_diagnostics, "native-binding-already-applied") == 1);

    const auto first_checkbox_id = root->child_at(0)->import_binding_instance_id();
    root.reset();

    auto rebuilt_root = build_native_view_tree(ir, {}, {.preview_mode = true});
    REQUIRE(rebuilt_root != nullptr);
    REQUIRE(rebuilt_root->child_at(0)->import_binding_instance_id() != first_checkbox_id);

    std::vector<ImportDiagnostic> rebuilt_diagnostics;
    bind_native_view_tree(*rebuilt_root, ir, binding, {.diagnostics_out = &rebuilt_diagnostics});
    REQUIRE(binding.bound_checkboxes.size() == 2);
    REQUIRE(rebuilt_diagnostics.empty());

    BindingBackedKnobContext fresh_binding;
    std::vector<ImportDiagnostic> fresh_diagnostics;
    bind_native_view_tree(*rebuilt_root, ir, fresh_binding, {.diagnostics_out = &fresh_diagnostics});
    REQUIRE(fresh_binding.bound_checkboxes.size() == 1);
    REQUIRE(fresh_diagnostics.empty());
}

TEST_CASE("generated C++ binding helper emits routed checkbox bindings",
          "[view][import][native-materializer][binding][cpp-codegen]") {
    DesignIR ir;
    ir.root = frame("root", 120.0f, 80.0f, LayoutDirection::column);

    auto checkbox_node = frame("figma:bypass-checkbox", 32.0f, 32.0f, LayoutDirection::column);
    checkbox_node.type = "input";
    checkbox_node.attributes["type"] = "checkbox";
    checkbox_node.attributes["checked"] = "false";
    checkbox_node.attributes["pulpRouteId"] = "figma-plugin:bypass";
    checkbox_node.attributes["pulpParamKey"] = "filter.bypass";
    checkbox_node.attributes["pulpBindingModule"] = "filter";
    checkbox_node.attributes["pulpBindingParam"] = "bypass";
    ir.root.children.push_back(std::move(checkbox_node));

    const auto result = generate_pulp_cpp(ir, ir.asset_manifest, {});

    REQUIRE(result.source.find("std::make_unique<pulp::view::Checkbox>()") != std::string::npos);
    REQUIRE(result.source.find("ctx.bind_checkbox(*checkbox,") != std::string::npos);
    REQUIRE(result.source.find("ctx.claim_import_binding(*view, \"figma-plugin:bypass\")") !=
            std::string::npos);
    REQUIRE(result.source.find("route_0_match_count == 1") != std::string::npos);
    REQUIRE(result.binding_manifest.find("\"native_primitive\": \"checkbox\"") != std::string::npos);
    REQUIRE(result.binding_manifest.find("\"param_key\": \"filter.bypass\"") != std::string::npos);

    TempDir tmp("pulp-native-materializer-checkbox-codegen");
    const auto header = tmp.path / "imported_ui.hpp";
    const auto source = tmp.path / "imported_ui.cpp";
    const auto object = tmp.path / "imported_ui.o";
    write_text(header, result.header);
    write_text(source, result.source);

    std::string diagnostics;
    const bool compiled = compile_generated_source(source, object, &diagnostics);
    INFO(diagnostics);
    REQUIRE(compiled);
}

TEST_CASE("compiled generated C++ binding helper binds and fails closed at runtime",
          "[view][import][native-materializer][binding][cpp-codegen]") {
    auto root = pulp::test::generated_binding_runtime::build_generated_binding_runtime_ui();
    REQUIRE(root != nullptr);
    REQUIRE(root->child_count() == 1);

    auto* checkbox = dynamic_cast<Checkbox*>(root->child_at(0));
    REQUIRE(checkbox != nullptr);
    REQUIRE_FALSE(checkbox->is_checked());

    BindingBackedKnobContext binding;
    pulp::test::generated_binding_runtime::bind_generated_binding_runtime_ui(*root, binding);
    REQUIRE(binding.bound_checkboxes.size() == 1);
    REQUIRE(binding.bound_checkboxes[0].route_id == "figma-plugin:bypass");
    REQUIRE(binding.bound_checkboxes[0].param_key == "filter.bypass");
    REQUIRE(binding.bound_checkboxes[0].binding_module == "filter");
    REQUIRE(binding.bound_checkboxes[0].binding_param == "bypass");
    REQUIRE(binding.bound_checkboxes[0].event_contract == "onChange:set_param:filter.bypass");
    REQUIRE(binding.bound_checkboxes[0].gesture_contract == "click:toggle");

    checkbox->on_mouse_down({16.0f, 16.0f});
    REQUIRE(checkbox->is_checked());
    REQUIRE(binding.checkbox_changes.size() == 1);
    REQUIRE(binding.checkbox_changes[0].first == "filter.bypass");
    REQUIRE(binding.checkbox_changes[0].second == 1.0f);

    pulp::test::generated_binding_runtime::bind_generated_binding_runtime_ui(*root, binding);
    REQUIRE(binding.bound_checkboxes.size() == 1);

    BindingBackedKnobContext fresh_binding;
    pulp::test::generated_binding_runtime::bind_generated_binding_runtime_ui(*root, fresh_binding);
    REQUIRE(fresh_binding.bound_checkboxes.size() == 1);
}

TEST_CASE("native materializer binding helper ignores non-binding metadata",
          "[view][import][native-materializer][binding]") {
    DesignIR ir;
    ir.root = frame("root", 220.0f, 140.0f, LayoutDirection::column);

    auto route_only = frame("route-only", 80.0f, 40.0f, LayoutDirection::column);
    route_only.stable_anchor_id = "figma:route-only";
    route_only.attributes["pulpRouteId"] = "figma-plugin:route-only";
    route_only.attributes["pulpSourceFamily"] = "static-frame";

    auto initial_value_only = frame("initial-only-editor", 120.0f, 24.0f, LayoutDirection::column);
    initial_value_only.type = "input";
    initial_value_only.attributes["type"] = "text";
    initial_value_only.attributes["pulpInitialValue"] = "Init";

    ir.root.children.push_back(std::move(route_only));
    ir.root.children.push_back(std::move(initial_value_only));

    auto root = build_native_view_tree(ir, {}, {.preview_mode = true});
    REQUIRE(root != nullptr);

    BindingBackedKnobContext binding;
    std::vector<ImportDiagnostic> diagnostics;
    bind_native_view_tree(*root, ir, binding, {.diagnostics_out = &diagnostics});

    REQUIRE(binding.bound_knobs.empty());
    REQUIRE(diagnostics.empty());
}

TEST_CASE("native materializer binding helper binds routed initial-value text editors",
          "[view][import][native-materializer][binding]") {
    DesignIR ir;
    ir.root = frame("root", 160.0f, 80.0f, LayoutDirection::column);

    auto editor_node = frame("preset-name", 140.0f, 24.0f, LayoutDirection::column);
    editor_node.type = "input";
    editor_node.stable_anchor_id = "figma:preset-name";
    editor_node.attributes["type"] = "text";
    editor_node.attributes["pulpRouteId"] = "figma-plugin:preset-name";
    editor_node.attributes["pulpInitialValue"] = "Init";
    editor_node.attributes["pulpPlaceholder"] = "Preset";
    editor_node.attributes["pulpEventContract"] = "input:onChange:setState";
    editor_node.attributes["pulpFocusContract"] = "input:focus";
    ir.root.children.push_back(std::move(editor_node));

    auto root = build_native_view_tree(ir, {}, {.preview_mode = true});
    REQUIRE(root != nullptr);
    REQUIRE(root->child_count() == 1);

    auto* editor = dynamic_cast<TextEditor*>(root->child_at(0));
    REQUIRE(editor != nullptr);
    REQUIRE(editor->text() == "Init");

    BindingBackedKnobContext binding;
    std::vector<ImportDiagnostic> diagnostics;
    bind_native_view_tree(*root, ir, binding, {.diagnostics_out = &diagnostics});

    REQUIRE(binding.bound_text_editors.size() == 1);
    REQUIRE(binding.bound_text_editors[0].route_id == "figma-plugin:preset-name");
    REQUIRE(binding.bound_text_editors[0].value_key.empty());
    REQUIRE(binding.bound_text_editors[0].initial_value == "Init");
    REQUIRE(binding.bound_text_editors[0].placeholder == "Preset");
    REQUIRE(binding.bound_text_editors[0].event_contract == "input:onChange:setState");
    REQUIRE(binding.bound_text_editors[0].focus_contract == "input:focus");
    REQUIRE(diagnostics.empty());

    editor->set_text("Edited");
    REQUIRE(binding.text_changes.size() == 1);
    REQUIRE(binding.text_changes[0] == "Edited");
}

TEST_CASE("baked native materializer lets explicit hit-test metadata override promoted widget defaults",
          "[view][import][native-materializer][hit-test]") {
    DesignIR ir;
    ir.root = frame("root", 120.0f, 120.0f, LayoutDirection::column);

    auto knob_node = frame("gain-knob", 80.0f, 80.0f, LayoutDirection::column);
    knob_node.audio_widget = AudioWidgetType::knob;
    knob_node.audio_label = "Gain";

    auto child = frame("explicit-child", 80.0f, 80.0f, LayoutDirection::column);
    child.attributes["pulpHitTestable"] = "true";
    knob_node.children.push_back(std::move(child));
    ir.root.children.push_back(std::move(knob_node));

    auto root = build_native_view_tree(ir, {}, {.preview_mode = true});
    REQUIRE(root != nullptr);

    root->set_bounds({0, 0, 120.0f, 120.0f});
    root->layout_children();

    auto* knob = dynamic_cast<Knob*>(root->child_at(0));
    REQUIRE(knob != nullptr);
    REQUIRE(knob->child_count() == 1);
    REQUIRE(knob->child_at(0)->hit_testable());

    auto* hit = root->hit_test({40.0f, 40.0f});
    REQUIRE(hit == knob->child_at(0));
}

TEST_CASE("baked native materializer preserves interactive descendants under promoted widgets",
          "[view][import][native-materializer][hit-test]") {
    DesignIR ir;
    ir.root = frame("root", 120.0f, 120.0f, LayoutDirection::column);

    auto knob_node = frame("gain-knob", 80.0f, 80.0f, LayoutDirection::column);
    knob_node.audio_widget = AudioWidgetType::knob;
    knob_node.audio_label = "Gain";

    auto container = frame("interactive-container", 80.0f, 40.0f, LayoutDirection::column);
    auto button_node = frame("nested-fine-button", 60.0f, 20.0f, LayoutDirection::column);
    button_node.type = "button";
    button_node.text_content = "Fine";
    container.children.push_back(std::move(button_node));
    knob_node.children.push_back(std::move(container));
    ir.root.children.push_back(std::move(knob_node));

    auto root = build_native_view_tree(ir, {}, {.preview_mode = true});
    REQUIRE(root != nullptr);

    root->set_bounds({0, 0, 120.0f, 120.0f});
    root->layout_children();

    auto* knob = dynamic_cast<Knob*>(root->child_at(0));
    REQUIRE(knob != nullptr);
    REQUIRE(knob->child_count() == 1);
    auto* container_view = knob->child_at(0);
    REQUIRE(container_view != nullptr);
    REQUIRE(container_view->hit_testable());
    REQUIRE(container_view->pointer_events() == View::PointerEvents::box_none);
    REQUIRE(container_view->child_count() == 1);
    auto* button_view = dynamic_cast<TextButton*>(container_view->child_at(0));
    REQUIRE(button_view != nullptr);
    REQUIRE(button_view->hit_testable());

    auto* button_hit = root->hit_test({30.0f, 10.0f});
    REQUIRE(button_hit == button_view);

    auto* body_hit = root->hit_test({70.0f, 30.0f});
    REQUIRE(body_hit == knob);
}

TEST_CASE("baked native materializer honors explicit hit-test metadata",
          "[view][import][native-materializer][hit-test]") {
    DesignIR ir;
    ir.root = frame("root", 120.0f, 120.0f, LayoutDirection::column);

    auto decorative = frame("decorative-layer", 80.0f, 80.0f, LayoutDirection::column);
    decorative.attributes["pulpHitTestable"] = "false";
    ir.root.children.push_back(std::move(decorative));

    auto root = build_native_view_tree(ir, {}, {.preview_mode = true});
    REQUIRE(root != nullptr);
    REQUIRE(root->child_count() == 1);

    root->set_bounds({0, 0, 120.0f, 120.0f});
    root->layout_children();

    auto* child = root->child_at(0);
    REQUIRE(child != nullptr);
    REQUIRE_FALSE(child->hit_testable());

    auto* hit = root->hit_test({40.0f, 40.0f});
    REQUIRE(hit == root.get());
}

TEST_CASE("baked native materializer only treats display text as editor value for textarea (PR #3128 review)",
          "[view][import][native-materializer]") {
    // Regression: the text_value fallback used incidental node text for any
    // editor, so a label/heading captured as text_content was injected as the
    // editor's contents. Only a <textarea> body is genuinely the value.
    DesignIR ir;
    ir.root = frame("root", 200.0f, 120.0f, LayoutDirection::column);

    // <input type=text> with incidental label text, no explicit value.
    auto input_node = frame("name-input", 96.0f, 24.0f, LayoutDirection::column);
    input_node.type = "input";
    input_node.attributes["type"] = "text";
    input_node.text_content = "Preset Name";

    // <textarea> whose body IS the value.
    auto area_node = frame("notes", 120.0f, 60.0f, LayoutDirection::column);
    area_node.type = "textarea";
    area_node.attributes["pulpSourceFamily"] = "textarea";
    area_node.text_content = "hello world";

    ir.root.children.push_back(std::move(input_node));
    ir.root.children.push_back(std::move(area_node));

    auto root = build_native_view_tree(ir, {}, {.preview_mode = true});
    REQUIRE(root != nullptr);
    REQUIRE(root->child_count() == 2);

    auto* input_editor = dynamic_cast<TextEditor*>(root->child_at(0));
    REQUIRE(input_editor != nullptr);
    REQUIRE(input_editor->text().empty());

    auto* area_editor = dynamic_cast<TextEditor*>(root->child_at(1));
    REQUIRE(area_editor != nullptr);
    REQUIRE(area_editor->text() == "hello world");
}
