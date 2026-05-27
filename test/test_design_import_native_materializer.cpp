#include <pulp/canvas/canvas.hpp>
#include <pulp/state/store.hpp>
#include <pulp/view/design_import.hpp>
#include <pulp/view/layout_snapshot.hpp>
#include <pulp/view/script_engine.hpp>
#include <pulp/view/view.hpp>
#include <pulp/view/widget_bridge.hpp>
#include <pulp/view/widgets.hpp>

#include <catch2/catch_test_macros.hpp>

#include <memory>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

using namespace pulp::view;

namespace {

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

    ir.root.children.push_back(std::move(knob_node));
    ir.root.children.push_back(std::move(fader_node));
    ir.root.children.push_back(std::move(circle_fader_node));
    ir.root.children.push_back(std::move(meter_node));
    ir.root.children.push_back(std::move(xy_node));
    ir.root.children.push_back(std::move(choice_node));

    auto root = build_native_view_tree(ir, {}, {.preview_mode = true});
    REQUIRE(root != nullptr);
    REQUIRE(root->child_count() == 6);

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
}
