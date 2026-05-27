#include "fixtures/design_import_generated_cpp_fixture.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include <choc/text/choc_JSON.h>
#include <pulp/platform/child_process.hpp>
#include <pulp/state/store.hpp>
#include <pulp/view/design_import.hpp>
#include <pulp/view/layout_snapshot.hpp>
#include <pulp/view/screenshot.hpp>
#include <pulp/view/screenshot_compare.hpp>
#include <pulp/view/view.hpp>
#include <pulp/view/widgets.hpp>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <map>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

using namespace pulp::view;
namespace fs = std::filesystem;

#ifndef PULP_TEST_CXX_COMPILER
#define PULP_TEST_CXX_COMPILER ""
#endif

#ifndef PULP_REPO_ROOT
#define PULP_REPO_ROOT ""
#endif

std::unique_ptr<pulp::view::View> build_imported_ui();
void bind_imported_ui(pulp::view::View& root, pulp::view::NativeImportBindingContext& ctx);

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

void write_bytes(const fs::path& path, const std::vector<uint8_t>& bytes) {
    fs::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary);
    REQUIRE(out.is_open());
    out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    REQUIRE(out.good());
}

std::string read_text(const fs::path& path) {
    std::ifstream in(path);
    REQUIRE(in.is_open());
    std::ostringstream ss;
    ss << in.rdbuf();
    REQUIRE((in.good() || in.eof()));
    return ss.str();
}

View* find_anchor(View& root, std::string_view anchor) {
    if (root.anchor_id() == anchor)
        return &root;
    for (std::size_t i = 0; i < root.child_count(); ++i) {
        if (auto* found = find_anchor(*root.child_at(i), anchor))
            return found;
    }
    return nullptr;
}

struct PhaseDParamEvent {
    std::string param_key;
    float value = 0.0f;
};

struct PhaseDGestureEvent {
    std::string param_key;
    std::string phase;
};

class PhaseDKnobBindingContext final : public NativeImportBindingContext {
public:
    PhaseDKnobBindingContext() {
        store_.set_gesture_callbacks(
            [this](pulp::state::ParamID id) { record_gesture(id, "begin"); },
            [this](pulp::state::ParamID id) { record_gesture(id, "end"); });
    }

    void bind_knob(Knob& knob, const NativeImportBindingDescriptor& descriptor) override {
        const auto param_key = std::string(descriptor.param_key);
        const auto route_id = std::string(descriptor.route_id);

        auto id = static_cast<pulp::state::ParamID>(param_ids_.size() + 1u);
        pulp::state::ParamInfo info;
        info.id = id;
        info.name = param_key;
        info.range = {0.0f, 1.0f, knob.value()};
        store_.add_parameter(info);
        store_.set_normalized(id, knob.value());

        param_ids_[param_key] = id;
        param_keys_by_id_[id] = param_key;
        route_ids_[param_key] = route_id;
        bound_params_.push_back(param_key);
        knob.on_gesture_begin = [this, id] {
            store_.begin_gesture(id);
        };
        knob.on_change = [this, id, param_key](float normalized) {
            store_.set_normalized(id, normalized);
            events_.push_back({param_key, normalized});
        };
        knob.on_gesture_end = [this, id] {
            store_.end_gesture(id);
        };
    }

    float normalized(std::string_view param_key) const {
        auto found = param_ids_.find(std::string(param_key));
        REQUIRE(found != param_ids_.end());
        return store_.get_normalized(found->second);
    }

    std::size_t change_count(std::string_view param_key) const {
        std::size_t count = 0;
        for (const auto& event : events_) {
            if (event.param_key == param_key)
                ++count;
        }
        return count;
    }

    const std::vector<std::string>& bound_params() const { return bound_params_; }
    const std::vector<PhaseDParamEvent>& events() const { return events_; }
    const std::vector<PhaseDGestureEvent>& gestures() const { return gestures_; }

    bool has_ordered_gesture(std::string_view param_key) const {
        bool saw_begin = false;
        for (const auto& event : gestures_) {
            if (event.param_key != param_key)
                continue;
            if (event.phase == "begin")
                saw_begin = true;
            if (event.phase == "end")
                return saw_begin;
        }
        return false;
    }

private:
    void record_gesture(pulp::state::ParamID id, std::string phase) {
        auto found = param_keys_by_id_.find(id);
        REQUIRE(found != param_keys_by_id_.end());
        gestures_.push_back({found->second, std::move(phase)});
    }

    pulp::state::StateStore store_;
    std::unordered_map<std::string, pulp::state::ParamID> param_ids_;
    std::unordered_map<pulp::state::ParamID, std::string> param_keys_by_id_;
    std::unordered_map<std::string, std::string> route_ids_;
    std::vector<std::string> bound_params_;
    std::vector<PhaseDParamEvent> events_;
    std::vector<PhaseDGestureEvent> gestures_;
};

IRNode label_node(std::string id,
                  std::string text,
                  float width,
                  float height) {
    IRNode node;
    node.type = "text";
    node.name = id;
    node.stable_anchor_id = std::move(id);
    node.text_content = std::move(text);
    node.style.width = width;
    node.style.height = height;
    return node;
}

IRNode frame_node(std::string id,
                  std::string name,
                  float width,
                  float height,
                  LayoutDirection direction) {
    IRNode node;
    node.type = "frame";
    node.name = std::move(name);
    node.stable_anchor_id = std::move(id);
    node.layout.direction = direction;
    node.style.width = width;
    node.style.height = height;
    return node;
}

std::size_t count_occurrences(std::string_view text, std::string_view needle) {
    std::size_t count = 0;
    std::size_t pos = 0;
    while ((pos = text.find(needle, pos)) != std::string_view::npos) {
        ++count;
        pos += needle.size();
    }
    return count;
}

DesignIR build_codegen_fixture_ir() {
    DesignIR ir;
    ir.source = DesignSource::stitch;
    ir.capture_method = "adapter_parse";
    ir.source_adapter = "phase7-codegen-test";
    ir.source_version = "1";
    ir.tokens.colors["bg.primary"] = "#112233";
    ir.tokens.colors["bg.alias"] = "#112233";
    ir.tokens.colors["semantic.surface"] = "surface-token";
    ir.tokens.dimensions["panel.width"] = 320.0f;
    ir.tokens.dimensions["panel.width.alias"] = 320.0f;
    ir.tokens.dimensions["panel.height"] = 140.0f;
    ir.tokens.strings["label.drive"] = "Drive";

    ir.root = frame_node("root", "Panel", 320.0f, 140.0f, LayoutDirection::column);
    ir.root.style.background_color = "#112233";
    ir.root.layout.gap = 10.0f;

    auto header = frame_node("header", "Header", 320.0f, 32.0f, LayoutDirection::row);
    header.layout.gap = 8.0f;
    header.children.push_back(label_node("title", "Cloud Chorus", 144.0f, 24.0f));
    header.children.push_back(label_node("badge", "Generated C++", 120.0f, 24.0f));
    ir.root.children.push_back(std::move(header));

    auto drive = frame_node("drive", "Drive", 72.0f, 72.0f, LayoutDirection::column);
    drive.audio_widget = AudioWidgetType::knob;
    drive.audio_label = "Drive";
    drive.audio_min = -60.0f;
    drive.audio_max = 0.0f;
    drive.audio_default = -15.0f;
    drive.attributes["value"] = "0.2";
    ir.root.children.push_back(std::move(drive));

    IRAssetRef asset;
    asset.asset_id = "logo";
    asset.original_uri = "logo.svg";
    asset.content_hash = "sha256:fixture";
    asset.mime = "image/svg+xml";
    ir.asset_manifest.assets.push_back(std::move(asset));
    return ir;
}

DesignIR build_phase_a_typed_control_ir() {
    DesignIR ir;
    ir.source = DesignSource::stitch;
    ir.capture_method = "phase-a-typed-ir-smoke";
    ir.source_adapter = "native-cpp-import-execution-validation";
    ir.source_version = "phase-a";
    ir.root = frame_node("phase-a-root", "Typed Control Smoke", 360.0f, 140.0f, LayoutDirection::row);
    ir.root.layout.gap = 12.0f;

    auto drive = frame_node("drive", "Drive", 72.0f, 72.0f, LayoutDirection::column);
    drive.audio_widget = AudioWidgetType::knob;
    drive.audio_label = "Drive";
    drive.audio_min = 0.0f;
    drive.audio_max = 1.0f;
    drive.audio_default = 0.4f;
    drive.attributes["value"] = "0.7";
    ir.root.children.push_back(std::move(drive));

    auto mix = frame_node("mix", "Mix", 48.0f, 96.0f, LayoutDirection::column);
    mix.audio_widget = AudioWidgetType::fader;
    mix.audio_label = "Mix";
    mix.audio_min = 0.0f;
    mix.audio_max = 1.0f;
    mix.audio_default = 0.5f;
    mix.attributes["value"] = "0.25";
    ir.root.children.push_back(std::move(mix));

    auto level = frame_node("level", "Level", 96.0f, 24.0f, LayoutDirection::column);
    level.audio_widget = AudioWidgetType::meter;
    level.audio_label = "Level";
    level.attributes["value"] = "0.62";
    level.attributes["orientation"] = "horizontal";
    ir.root.children.push_back(std::move(level));

    auto shape = frame_node("shape", "Shape", 72.0f, 72.0f, LayoutDirection::column);
    shape.audio_widget = AudioWidgetType::xy_pad;
    shape.audio_label = "Shape";
    shape.attributes["x"] = "0.3";
    shape.attributes["y"] = "0.8";
    shape.attributes["xLabel"] = "Cutoff";
    shape.attributes["yLabel"] = "Resonance";
    ir.root.children.push_back(std::move(shape));

    return ir;
}

DesignIR build_untyped_named_control_ir() {
    DesignIR ir;
    ir.source = DesignSource::stitch;
    ir.capture_method = "phase-a-negative-typed-ir-smoke";
    ir.root = frame_node("root", "Untyped Smoke", 120.0f, 80.0f, LayoutDirection::column);

    auto untyped = frame_node("gain-knob-looking-frame",
                              "GainKnob",
                              64.0f,
                              64.0f,
                              LayoutDirection::column);
    untyped.attributes["value"] = "0.5";
    ir.root.children.push_back(std::move(untyped));
    return ir;
}

std::string json_string(choc::value::ValueView value) {
    return std::string(value.getString());
}

float json_float(choc::value::ValueView value) {
    if (value.isFloat64()) return static_cast<float>(value.getFloat64());
    if (value.isFloat32()) return value.getFloat32();
    if (value.isInt64()) return static_cast<float>(value.getInt64());
    FAIL("expected numeric JSON value");
    return 0.0f;
}

float json_float_or(choc::value::ValueView value, float fallback) {
    if (value.isVoid()) return fallback;
    return json_float(value);
}

std::string float_attr(float value) {
    std::ostringstream out;
    out << std::setprecision(7) << value;
    return out.str();
}

IRNode* node_at_ir_path(IRNode& root, std::string_view path) {
    const std::string text(path);
    REQUIRE(text.rfind("root", 0) == 0);
    IRNode* node = &root;
    std::size_t pos = 4;
    while (pos < text.size()) {
        REQUIRE(text[pos] == '/');
        ++pos;
        const auto slash = text.find('/', pos);
        const auto part = text.substr(pos, slash == std::string::npos ? std::string::npos : slash - pos);
        REQUIRE_FALSE(part.empty());
        const auto index = static_cast<std::size_t>(std::stoul(part));
        REQUIRE(index < node->children.size());
        node = &node->children[index];
        pos = slash == std::string::npos ? text.size() : slash;
    }
    return node;
}

std::string event_contract_string(choc::value::ValueView route) {
    const auto event = route["event_contracts"][0];
    return json_string(event["prop"]) + ":" + json_string(event["kind"]) + ":" +
           json_string(event["param_key"]);
}

std::string gesture_contract_string(choc::value::ValueView route) {
    const auto gesture = route["gesture_contracts"][0];
    std::string out = json_string(gesture["kind"]) + ":";
    const auto boundaries = gesture["boundaries"];
    for (uint32_t i = 0; i < boundaries.size(); ++i) {
        if (i != 0) out += "/";
        out += json_string(boundaries[i]);
    }
    return out;
}

std::string style_token_string(choc::value::ValueView route) {
    const auto tokens = route["style_token_references"];
    if (!tokens.isArray() || tokens.size() == 0)
        return {};
    std::string out;
    for (uint32_t i = 0; i < tokens.size(); ++i) {
        if (i != 0) out += ",";
        out += json_string(tokens[i]);
    }
    return out;
}

std::string color_for_style_tokens(std::string_view tokens) {
    if (tokens.find("C.orange") != std::string_view::npos) return "#ff6b35";
    if (tokens.find("C.blue") != std::string_view::npos) return "#5b8af0";
    if (tokens.find("C.purple") != std::string_view::npos) return "#9b59ff";
    if (tokens.find("C.green") != std::string_view::npos) return "#3ddc84";
    if (tokens.find("C.amber") != std::string_view::npos) return "#f0a030";
    return "#ff6b35";
}

void add_chainer_token_colors(DesignIR& ir) {
    ir.tokens.colors["chainer.orange"] = "#ff6b35";
    ir.tokens.colors["chainer.blue"] = "#5b8af0";
    ir.tokens.colors["chainer.purple"] = "#9b59ff";
    ir.tokens.colors["chainer.green"] = "#3ddc84";
    ir.tokens.colors["chainer.amber"] = "#f0a030";
}

IRNode lower_chainer_knob_route_to_node(IRNode& materialized_root,
                                        choc::value::ValueView route) {
    const auto materialized_path = json_string(route["materialized_ir_path"]);
    auto* materialized_node = node_at_ir_path(materialized_root, materialized_path);
    REQUIRE(materialized_node != nullptr);
    REQUIRE(materialized_node->stable_anchor_id.has_value());
    REQUIRE(*materialized_node->stable_anchor_id == json_string(route["materialized_ir_anchor"]));

    const auto binding = route["parameter_bindings"][0];
    const auto size = json_float(route["size"]);
    const auto value = json_float(route["value"]);
    const auto default_value = json_float_or(route["default_value"], value);
    const auto label = json_string(route["label"]);
    const auto style_tokens = style_token_string(route);

    auto knob = *materialized_node;
    knob.children.clear();
    knob.name = label;
    knob.text_content.clear();
    knob.style.width = size;
    knob.style.height = size;
    knob.style.border_color = color_for_style_tokens(style_tokens);
    knob.audio_widget = AudioWidgetType::knob;
    knob.audio_label = label;
    knob.audio_min = 0.0f;
    knob.audio_max = 1.0f;
    knob.audio_default = default_value;
    knob.attributes["value"] = float_attr(value);
    knob.attributes["pulpRouteId"] = json_string(route["id"]);
    knob.attributes["pulpRouteType"] = json_string(route["route_type"]);
    knob.attributes["pulpSourceFamily"] = json_string(route["source_component_family"]);
    knob.attributes["pulpSourcePath"] = json_string(route["stable_source_path"]);
    knob.attributes["pulpParamKey"] = json_string(binding["param_key"]);
    knob.attributes["pulpBindingModule"] = json_string(binding["module"]);
    knob.attributes["pulpBindingParam"] = json_string(binding["param"]);
    knob.attributes["pulpEventContract"] = event_contract_string(route);
    knob.attributes["pulpGestureContract"] = gesture_contract_string(route);
    knob.attributes["pulpStyleTokens"] = style_tokens;
    knob.attributes["pulpDefaultValueSource"] =
        route["default_value"].isVoid() ? "phase_c_initial_value_fallback" : "source_default";
    return knob;
}

DesignIR lower_chainer_knob_route_to_phase_c_ir(DesignIR materialized_ir,
                                                choc::value::ValueView route) {
    const auto size = json_float(route["size"]);

    DesignIR ir;
    ir.source = DesignSource::jsx;
    ir.capture_method = "phase-c-chainer-one-knob-route-overlay";
    ir.source_adapter = "native-cpp-import-execution-validation";
    ir.source_version = "phase-c";
    add_chainer_token_colors(ir);
    ir.root = frame_node("phase-c-root", "Chainer One Knob", size + 28.0f, size + 38.0f, LayoutDirection::column);
    ir.root.children.push_back(lower_chainer_knob_route_to_node(materialized_ir.root, route));
    return ir;
}

DesignIR lower_chainer_knob_routes_to_phase_d_ir(DesignIR materialized_ir,
                                                 choc::value::ValueView route_rows) {
    DesignIR ir;
    ir.source = DesignSource::jsx;
    ir.capture_method = "phase-d-chainer-all-knobs-route-overlay";
    ir.source_adapter = "native-cpp-import-execution-validation";
    ir.source_version = "phase-d";
    add_chainer_token_colors(ir);
    ir.root = frame_node("phase-d-root", "Chainer All Knobs", 520.0f, 96.0f, LayoutDirection::row);
    ir.root.layout.gap = 12.0f;

    for (uint32_t i = 0; i < route_rows.size(); ++i) {
        const auto route = route_rows[i];
        if (json_string(route["source_component_family"]) != "Knob")
            continue;
        ir.root.children.push_back(lower_chainer_knob_route_to_node(materialized_ir.root, route));
    }
    REQUIRE(ir.root.children.size() == 8);

    return ir;
}

std::string diff_messages(const LayoutTreeDiff& diff) {
    std::ostringstream out;
    for (const auto& message : diff.messages)
        out << message << '\n';
    return out.str();
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

}  // namespace

TEST_CASE("typed DesignIR smoke emits typed baked C++ controls",
          "[view][import][cpp-codegen][native-cpp-phase-a]") {
    const auto ir = build_phase_a_typed_control_ir();
    CppExportOptions opts;
    opts.header_filename = "phase_a_typed_controls.hpp";
    opts.namespace_name = "pulp::test::phase_a";

    const auto result = generate_pulp_cpp(ir, ir.asset_manifest, opts);

    REQUIRE(count_occurrences(result.source, "std::make_unique<pulp::view::Knob>()") == 1);
    REQUIRE(count_occurrences(result.source, "std::make_unique<pulp::view::Fader>()") == 1);
    REQUIRE(count_occurrences(result.source, "std::make_unique<pulp::view::Meter>()") == 1);
    REQUIRE(count_occurrences(result.source, "std::make_unique<pulp::view::XYPad>()") == 1);
    REQUIRE(result.source.find("std::make_unique<pulp::view::View>()") != std::string::npos);

    REQUIRE(result.source.find("->set_label(\"Drive\");") != std::string::npos);
    REQUIRE(result.source.find("->set_value(/* TODO: bind to param */ 0.7f);") != std::string::npos);
    REQUIRE(result.source.find("->set_default_value(0.4f);") != std::string::npos);
    REQUIRE(result.source.find("->set_label(\"Mix\");") != std::string::npos);
    REQUIRE(result.source.find("->set_value(/* TODO: bind to param */ 0.25f);") != std::string::npos);
    REQUIRE(result.source.find("->set_level(/* TODO: bind to meter */ 0.62f, 0.62f);") != std::string::npos);
    REQUIRE(result.source.find("->set_orientation(pulp::view::Meter::Orientation::horizontal);") != std::string::npos);
    REQUIRE(result.source.find("->set_x(0.3f);") != std::string::npos);
    REQUIRE(result.source.find("->set_y(0.8f);") != std::string::npos);
    REQUIRE(result.source.find("->set_x_label(\"Cutoff\");") != std::string::npos);
    REQUIRE(result.source.find("->set_y_label(\"Resonance\");") != std::string::npos);

    TempDir tmp("pulp-phase-a-typed-cpp-codegen");
    const auto header = tmp.path / "phase_a_typed_controls.hpp";
    const auto source = tmp.path / "phase_a_typed_controls.cpp";
    const auto object = tmp.path / "phase_a_typed_controls.o";
    write_text(header, result.header);
    write_text(source, result.source);

    std::string diagnostics;
    const bool compiled = compile_generated_source(source, object, &diagnostics);
    INFO(diagnostics);
    REQUIRE(compiled);
}

TEST_CASE("untyped named control-looking IR remains generic baked C++",
          "[view][import][cpp-codegen][native-cpp-phase-a]") {
    const auto ir = build_untyped_named_control_ir();
    const auto result = generate_pulp_cpp(ir, ir.asset_manifest, {});

    REQUIRE(result.source.find("set_id(\"gain-knob-looking-frame\")") != std::string::npos);
    REQUIRE(result.source.find("std::make_unique<pulp::view::Knob>()") == std::string::npos);
    REQUIRE(result.source.find("std::make_unique<pulp::view::Fader>()") == std::string::npos);
    REQUIRE(result.source.find("std::make_unique<pulp::view::Meter>()") == std::string::npos);
    REQUIRE(result.source.find("std::make_unique<pulp::view::XYPad>()") == std::string::npos);
    REQUIRE(count_occurrences(result.source, "std::make_unique<pulp::view::View>()") >= 2);
}

TEST_CASE("binding manifest preserves fallback-only route diagnostics",
          "[view][import][cpp-codegen][native-cpp-phase-c]") {
    DesignIR ir;
    ir.source = DesignSource::jsx;
    ir.root = frame_node("fallback-root", "Fallback Root", 120.0f, 80.0f, LayoutDirection::column);
    auto control = frame_node("fallback-control", "Unavailable Control", 48.0f, 48.0f, LayoutDirection::column);
    control.attributes["pulpRouteId"] = "chainer.unmatched.0";
    control.attributes["pulpFallbackReason"] = "Missing native event contract.";
    ir.root.children.push_back(std::move(control));

    const auto result = generate_pulp_cpp(ir, ir.asset_manifest, {});
    auto binding_manifest = choc::json::parse(result.binding_manifest);
    REQUIRE(binding_manifest["entries"].size() == 1);
    const auto& entry = binding_manifest["entries"][0];
    REQUIRE(entry["id"].getString() == std::string("chainer.unmatched.0"));
    REQUIRE(entry["native_primitive"].getString() == std::string("view"));
    REQUIRE(entry["fallback_reason"].getString() == std::string("Missing native event contract."));
}

TEST_CASE("Chainer route overlay can lower one knob to typed C++ with binding sidecar",
          "[view][import][cpp-codegen][native-cpp-phase-c]") {
    const fs::path manifest_path =
        fs::path(PULP_REPO_ROOT) / "planning/artifacts/native-ui/nv0/reports/chainer-route-manifest.json";
    REQUIRE(fs::exists(manifest_path));
    const auto overlay = read_text(manifest_path);
    REQUIRE(overlay.find("\"schema\": \"pulp-native-ui-route-overlay-v1\"") != std::string::npos);
    REQUIRE(overlay.find("\"id\": \"chainer.knob.0.osc_freq\"") != std::string::npos);
    REQUIRE(overlay.find("\"materialized_ir_path\": \"root/1/2/0/1/0\"") != std::string::npos);
    REQUIRE(overlay.find("\"unique_knob_ir_paths\": 8") != std::string::npos);
    auto route_manifest = choc::json::parse(overlay);
    const auto route_rows = route_manifest["source_contract_overlay"]["node_route_rows"];
    uint32_t route_index = route_rows.size();
    for (uint32_t i = 0; i < route_rows.size(); ++i) {
        if (route_rows[i]["id"].getString() == std::string("chainer.knob.0.osc_freq")) {
            route_index = i;
            break;
        }
    }
    REQUIRE(route_index < route_rows.size());
    const auto route = route_rows[route_index];
    REQUIRE(route["id"].getString() == std::string("chainer.knob.0.osc_freq"));
    REQUIRE(route["materialized_ir_path"].getString() == std::string("root/1/2/0/1/0"));
    REQUIRE(json_float(route["value"]) == 0.35f);
    REQUIRE(route["default_value"].isVoid());
    REQUIRE(route["default_value_source"].getString() == std::string("not_captured"));

    const fs::path chainer_ir_path =
        fs::path(PULP_REPO_ROOT) / "planning/artifacts/native-ui/nv0/reports/generated/chainer-ir.json";
    REQUIRE(fs::exists(chainer_ir_path));
    auto materialized_ir = parse_design_ir_json(read_text(chainer_ir_path));

    const auto ir = lower_chainer_knob_route_to_phase_c_ir(std::move(materialized_ir), route);
    CppExportOptions opts;
    opts.header_filename = "phase_c_chainer_one_knob.hpp";
    opts.namespace_name = "pulp::test::phase_c";

    const auto result = generate_pulp_cpp(ir, ir.asset_manifest, opts);

    REQUIRE(count_occurrences(result.source, "std::make_unique<pulp::view::Knob>()") == 1);
    REQUIRE(result.source.find("->set_anchor_id(\"pr_2c\");") != std::string::npos);
    REQUIRE(result.source.find("->set_label(\"freq\");") != std::string::npos);
    REQUIRE(result.source.find("->set_value(/* TODO: bind to param */ 0.35f);") != std::string::npos);
    REQUIRE(result.source.find("->set_default_value(0.35f);") != std::string::npos);
    REQUIRE(result.source.find("kChainerOrange = pulp::view::Color::rgba8(255, 107, 53, 255)") != std::string::npos);
    REQUIRE(result.source.find("tokens::kChainerOrange") != std::string::npos);

    REQUIRE(result.binding_manifest.find("\"schema\": \"pulp-native-cpp-binding-manifest-v1\"") != std::string::npos);
    REQUIRE(result.binding_manifest.find("\"id\": \"chainer.knob.0.osc_freq\"") != std::string::npos);
    REQUIRE(result.binding_manifest.find("\"native_primitive\": \"knob\"") != std::string::npos);
    REQUIRE(result.binding_manifest.find("\"source_family\": \"Knob\"") != std::string::npos);
    REQUIRE(result.binding_manifest.find("\"param_key\": \"osc_freq\"") != std::string::npos);
    REQUIRE(result.binding_manifest.find("\"binding_module\": \"OSC\"") != std::string::npos);
    REQUIRE(result.binding_manifest.find("\"binding_param\": \"freq\"") != std::string::npos);
    REQUIRE(result.binding_manifest.find("\"event_contract\": \"onChange:set_param:osc_freq\"") != std::string::npos);
    REQUIRE(result.binding_manifest.find("\"gesture_contract\": \"rotary_drag:begin/update/end\"") != std::string::npos);
    REQUIRE(result.binding_manifest.find("\"style_tokens\": \"C.orange\"") != std::string::npos);
    REQUIRE(result.binding_manifest.find("\"default_value_source\": \"phase_c_initial_value_fallback\"") != std::string::npos);
    REQUIRE(result.header.find("bind_imported_ui(pulp::view::View& root, pulp::view::NativeImportBindingContext& ctx)") != std::string::npos);
    REQUIRE(count_occurrences(result.source, "ctx.bind_knob(") == 1);
    auto binding_manifest = choc::json::parse(result.binding_manifest);
    REQUIRE(binding_manifest["schema"].getString() == std::string("pulp-native-cpp-binding-manifest-v1"));
    REQUIRE(binding_manifest["entries"].size() == 1);
    const auto& entry = binding_manifest["entries"][0];
    REQUIRE(entry["id"].getString() == std::string("chainer.knob.0.osc_freq"));
    REQUIRE(entry["ir_path"].getString() == std::string("root/0"));
    REQUIRE(entry["native_primitive"].getString() == std::string("knob"));
    REQUIRE(entry["param_key"].getString() == std::string("osc_freq"));
    REQUIRE(entry["style_tokens"].getString() == std::string("C.orange"));
    REQUIRE(entry["default_value_source"].getString() == std::string("phase_c_initial_value_fallback"));

    TempDir tmp("pulp-phase-c-chainer-one-knob-cpp-codegen");
    const auto header = tmp.path / "phase_c_chainer_one_knob.hpp";
    const auto source = tmp.path / "phase_c_chainer_one_knob.cpp";
    const auto object = tmp.path / "phase_c_chainer_one_knob.o";
    write_text(header, result.header);
    write_text(source, result.source);

    std::string diagnostics;
    const bool compiled = compile_generated_source(source, object, &diagnostics);
    INFO(diagnostics);
    REQUIRE(compiled);
}

TEST_CASE("Chainer route overlay can lower all knobs to typed C++ with binding sidecar",
          "[view][import][cpp-codegen][native-cpp-phase-d]") {
    const fs::path manifest_path =
        fs::path(PULP_REPO_ROOT) / "planning/artifacts/native-ui/nv0/reports/chainer-route-manifest.json";
    REQUIRE(fs::exists(manifest_path));
    auto route_manifest = choc::json::parse(read_text(manifest_path));
    const auto route_rows = route_manifest["source_contract_overlay"]["node_route_rows"];
    REQUIRE(route_manifest["source_contract_overlay"]["validation"]["actual"]["knob_routes"].getInt64() == 8);

    const fs::path chainer_ir_path =
        fs::path(PULP_REPO_ROOT) / "planning/artifacts/native-ui/nv0/reports/generated/chainer-ir.json";
    REQUIRE(fs::exists(chainer_ir_path));
    auto materialized_ir = parse_design_ir_json(read_text(chainer_ir_path));

    const auto ir = lower_chainer_knob_routes_to_phase_d_ir(std::move(materialized_ir), route_rows);
    CppExportOptions opts;
    opts.header_filename = "phase_d_chainer_all_knobs.hpp";
    opts.namespace_name = "pulp::test::phase_d";

    const auto result = generate_pulp_cpp(ir, ir.asset_manifest, opts);
    REQUIRE(count_occurrences(result.source, "std::make_unique<pulp::view::Knob>()") == 8);
    REQUIRE(result.header.find("bind_imported_ui(pulp::view::View& root, pulp::view::NativeImportBindingContext& ctx)") != std::string::npos);
    REQUIRE(count_occurrences(result.source, "ctx.bind_knob(") == 8);
    REQUIRE(result.source.find("tokens::kChainerOrange") != std::string::npos);
    REQUIRE(result.source.find("tokens::kChainerBlue") != std::string::npos);
    REQUIRE(result.source.find("tokens::kChainerPurple") != std::string::npos);
    REQUIRE(result.source.find("tokens::kChainerAmber") != std::string::npos);
    REQUIRE(result.source.find("tokens::kChainerGreen") != std::string::npos);

    auto binding_manifest = choc::json::parse(result.binding_manifest);
    REQUIRE(binding_manifest["schema"].getString() == std::string("pulp-native-cpp-binding-manifest-v1"));
    REQUIRE(binding_manifest["entries"].size() == 8);

    struct ExpectedKnob {
        const char* id;
        const char* anchor;
        const char* label;
        const char* param_key;
        const char* binding_module;
        const char* binding_param;
        const char* style_tokens;
    };
    const std::vector<ExpectedKnob> expected = {
        {"chainer.knob.0.osc_freq", "pr_2c", "freq", "osc_freq", "OSC", "freq", "C.orange"},
        {"chainer.knob.1.osc_detune", "pr_2l", "detune", "osc_detune", "OSC", "detune", "C.blue"},
        {"chainer.knob.2.osc_shape", "pr_2u", "shape", "osc_shape", "OSC", "shape", "C.purple"},
        {"chainer.knob.3.xover_lo", "pr_49", "lo", "xover_lo", "XOVER", "lo_freq", "C.amber"},
        {"chainer.knob.4.xover_hi", "pr_4i", "hi", "xover_hi", "XOVER", "hi_freq", "C.amber"},
        {"chainer.knob.5.ms_mid_width", "pr_4y", "mid wid", "ms_mid_width", "MS", "mid_width", "C.green"},
        {"chainer.knob.6.ms_side_width", "pr_57", "side wid", "ms_side_width", "MS", "side_width", "C.green"},
        {"chainer.knob.7.master_out", "pr_6p", "output", "master_out", "LIMIT", "output_gain", "C.green"},
    };

    for (const auto& knob : expected) {
        REQUIRE(result.source.find(std::string("->set_anchor_id(\"") + knob.anchor + "\");") != std::string::npos);
        REQUIRE(result.source.find(std::string("->set_label(\"") + knob.label + "\");") != std::string::npos);

        bool found = false;
        for (uint32_t i = 0; i < binding_manifest["entries"].size(); ++i) {
            const auto entry = binding_manifest["entries"][i];
            if (entry["id"].getString() != std::string(knob.id))
                continue;
            found = true;
            REQUIRE(entry["anchor_id"].getString() == std::string(knob.anchor));
            REQUIRE(entry["native_primitive"].getString() == std::string("knob"));
            REQUIRE(entry["route_type"].getString() == std::string("native_cpp"));
            REQUIRE(entry["source_family"].getString() == std::string("Knob"));
            REQUIRE(entry["param_key"].getString() == std::string(knob.param_key));
            REQUIRE(entry["binding_module"].getString() == std::string(knob.binding_module));
            REQUIRE(entry["binding_param"].getString() == std::string(knob.binding_param));
            REQUIRE(entry["event_contract"].getString() == std::string("onChange:set_param:") + knob.param_key);
            REQUIRE(entry["gesture_contract"].getString() == std::string("rotary_drag:begin/update/end"));
            REQUIRE(entry["style_tokens"].getString() == std::string(knob.style_tokens));
            REQUIRE(entry["default_value_source"].getString() == std::string("phase_c_initial_value_fallback"));
            break;
        }
        REQUIRE(found);
    }

    TempDir tmp("pulp-phase-d-chainer-all-knobs-cpp-codegen");
    const auto header = tmp.path / "phase_d_chainer_all_knobs.hpp";
    const auto source = tmp.path / "phase_d_chainer_all_knobs.cpp";
    const auto object = tmp.path / "phase_d_chainer_all_knobs.o";
    write_text(header, result.header);
    write_text(source, result.source);

    std::string diagnostics;
    const bool compiled = compile_generated_source(source, object, &diagnostics);
    INFO(diagnostics);
    REQUIRE(compiled);
}

TEST_CASE("generated Chainer all-knob C++ can bind and drag every knob",
          "[view][import][cpp-codegen][native-cpp-phase-d][behavior]") {
    auto root = ::build_imported_ui();
    REQUIRE(root != nullptr);

    PhaseDKnobBindingContext ctx;
    ::bind_imported_ui(*root, ctx);
    REQUIRE(ctx.bound_params().size() == 8);

    root->set_bounds({0.0f, 0.0f, 520.0f, 96.0f});
    root->layout_children();

    struct ExpectedKnob {
        const char* anchor;
        const char* param_key;
    };
    const std::vector<ExpectedKnob> expected = {
        {"pr_2c", "osc_freq"},
        {"pr_2l", "osc_detune"},
        {"pr_2u", "osc_shape"},
        {"pr_49", "xover_lo"},
        {"pr_4i", "xover_hi"},
        {"pr_4y", "ms_mid_width"},
        {"pr_57", "ms_side_width"},
        {"pr_6p", "master_out"},
    };

    auto before_png = render_to_png(*root, 520, 96, 1.0f);
    std::map<std::string, float> before_values;
    std::map<std::string, float> after_values;

    for (const auto& item : expected) {
        auto* view = find_anchor(*root, item.anchor);
        REQUIRE(view != nullptr);
        auto* knob = dynamic_cast<Knob*>(view);
        REQUIRE(knob != nullptr);

        const auto bounds = knob->bounds();
        REQUIRE(bounds.width > 0.0f);
        REQUIRE(bounds.height > 0.0f);

        const auto before = knob->value();
        before_values[item.param_key] = before;
        const Point start{bounds.x + bounds.width * 0.5f, bounds.y + bounds.height * 0.5f};
        const Point end{start.x, start.y - 48.0f};
        root->simulate_drag(start, end, 6);

        const auto after = knob->value();
        after_values[item.param_key] = after;
        REQUIRE(after > before);
        REQUIRE(ctx.normalized(item.param_key) == Catch::Approx(after));
        REQUIRE(ctx.change_count(item.param_key) > 0);
        REQUIRE(ctx.has_ordered_gesture(item.param_key));
    }

    auto after_png = render_to_png(*root, 520, 96, 1.0f);
    bool visual_smoke_valid = false;
    CompareResult visual_smoke;
    if (!before_png.empty() && !after_png.empty()) {
        visual_smoke = compare_screenshots(before_png, after_png, 8);
        REQUIRE(visual_smoke.valid);
        REQUIRE(visual_smoke.similarity < 0.999f);
        visual_smoke_valid = true;
    }

    if (const char* artifact_dir = std::getenv("PULP_NATIVE_UI_PHASE_D_ARTIFACT_DIR")) {
        const fs::path dir(artifact_dir);
        if (!before_png.empty())
            write_bytes(dir / "reports" / "screenshots" / "chainer-phase-d-all-knobs-before.png", before_png);
        if (!after_png.empty())
            write_bytes(dir / "reports" / "screenshots" / "chainer-phase-d-all-knobs-after.png", after_png);

        std::ostringstream report;
        report << "{\n"
               << "  \"schema\": \"pulp-native-ui-phase-d-behavior-v1\",\n"
               << "  \"fixture\": \"chainer-phase-d-all-knobs\",\n"
               << "  \"scope\": \"generated-native-cpp-widget-and-binding-helper\",\n"
               << "  \"headless_drag_tests\": " << expected.size() << ",\n"
               << "  \"bound_knobs\": " << ctx.bound_params().size() << ",\n"
               << "  \"parameter_updates\": " << ctx.events().size() << ",\n"
               << "  \"gesture_events\": " << ctx.gestures().size() << ",\n"
               << "  \"gesture_begin_events\": "
               << std::count_if(ctx.gestures().begin(), ctx.gestures().end(), [](const PhaseDGestureEvent& event) {
                      return event.phase == "begin";
                  }) << ",\n"
               << "  \"gesture_end_events\": "
               << std::count_if(ctx.gestures().begin(), ctx.gestures().end(), [](const PhaseDGestureEvent& event) {
                      return event.phase == "end";
                  }) << ",\n"
               << "  \"visual_smoke_valid\": " << (visual_smoke_valid ? "true" : "false") << ",\n"
               << "  \"visual_smoke_similarity\": " << std::setprecision(7) << visual_smoke.similarity << ",\n"
               << "  \"knobs\": [";
        for (std::size_t i = 0; i < expected.size(); ++i) {
            if (i != 0)
                report << ",";
            const auto& item = expected[i];
            report << "\n    {"
                   << "\"anchor\": \"" << item.anchor << "\", "
                   << "\"param_key\": \"" << item.param_key << "\", "
                   << "\"before\": " << before_values[item.param_key] << ", "
                   << "\"after\": " << after_values[item.param_key] << ", "
                   << "\"change_count\": " << ctx.change_count(item.param_key)
                   << "}";
        }
        report << "\n  ]\n"
               << "}\n";
        write_text(dir / "reports" / "chainer-phase-d-behavior-report.json", report.str());
    }
}

TEST_CASE("baked C++ exporter emits ownable C++ source artifacts",
          "[view][import][cpp-codegen][phase-7]") {
    auto ir = build_codegen_fixture_ir();
    ir.root.children.front().children.push_back(
        label_node("escape",
                   std::string("A") + static_cast<char>(0x01) + "B" + static_cast<char>(0) + "9",
                   48.0f,
                   24.0f));
    CppExportOptions opts;
    opts.namespace_name = "pulp::test::generated_design";
    opts.header_filename = "generated_design.hpp";

    const auto result = generate_pulp_cpp(ir, ir.asset_manifest, opts);
    REQUIRE(result.header.find("build_imported_ui") != std::string::npos);
    REQUIRE(result.header.find("bake_asset_manifest") != std::string::npos);
    REQUIRE(result.source.find("namespace tokens") != std::string::npos);
    REQUIRE(result.source.find("set_background_color(tokens::") != std::string::npos);
    REQUIRE(result.source.find("kBgPrimary = pulp::view::Color::rgba8(17, 34, 51, 255)") != std::string::npos);
    REQUIRE(result.source.find("kBgAlias = pulp::view::Color::rgba8(17, 34, 51, 255)") != std::string::npos);
    REQUIRE(result.source.find("kSemanticSurface = \"surface-token\"") != std::string::npos);
    REQUIRE(result.source.find("kPanelWidthAlias = 320.0f") != std::string::npos);
    REQUIRE(result.source.find("A\\001B\\0009") != std::string::npos);
    REQUIRE(result.source.find("namespace assets") != std::string::npos);
    REQUIRE(result.source.find("assets::kLogo") != std::string::npos);
    REQUIRE(result.source.find("// auto-extracted: structural name \"Header\"") != std::string::npos);
    REQUIRE(result.source.find("std::unique_ptr<pulp::view::View> build_header()") != std::string::npos);
    REQUIRE(result.source.find("/* TODO: bind to param */") != std::string::npos);
    REQUIRE(result.source.find("->set_value(/* TODO: bind to param */ 0.2f);") != std::string::npos);
    REQUIRE(result.source.find("->set_default_value(0.75f);") != std::string::npos);
    REQUIRE(result.source.find("std::make_unique<pulp::view::Knob>()") != std::string::npos);
    REQUIRE(result.source.find("build_native_view_tree") == std::string::npos);
    REQUIRE(result.source.find("serialize_design_ir") == std::string::npos);

    TempDir tmp("pulp-design-import-cpp-codegen");
    const auto header = tmp.path / "generated_design.hpp";
    const auto source = tmp.path / "generated_design.cpp";
    const auto object = tmp.path / "generated_design.o";
    write_text(header, result.header);
    write_text(source, result.source);

    std::string diagnostics;
    const bool compiled = compile_generated_source(source, object, &diagnostics);
    INFO(diagnostics);
    REQUIRE(compiled);
}

TEST_CASE("generated C++ fixture renders layout-equivalent native tree",
          "[view][import][cpp-codegen][phase-7]") {
    auto ir = build_codegen_fixture_ir();
    auto baked_native = build_native_view_tree(ir, ir.asset_manifest);
    auto generated_cpp = pulp::test::design_import_cpp_fixture::build_imported_ui();
    REQUIRE(baked_native != nullptr);
    REQUIRE(generated_cpp != nullptr);

    baked_native->set_bounds({0, 0, 320, 140});
    generated_cpp->set_bounds({0, 0, 320, 140});
    baked_native->layout_children();
    generated_cpp->layout_children();

    const LayoutTreeSnapshotOptions options{
        .surface = "phase7-baked-cpp",
        .fixture = "generated-cpp-vs-baked-native",
        .viewport_width = 320,
        .viewport_height = 140,
    };
    const auto baked_json = dump_layout_tree(*baked_native, options);
    const auto generated_json = dump_layout_tree(*generated_cpp, options);

    LayoutTreeDiff diff;
    const bool equivalent = layout_tree_snapshots_equivalent(
        baked_json,
        generated_json,
        {},
        &diff);
    INFO(diff_messages(diff));
    INFO("baked native:\n" << baked_json);
    INFO("generated cpp:\n" << generated_json);
    REQUIRE(equivalent);

    const auto manifest = pulp::test::design_import_cpp_fixture::bake_asset_manifest();
    REQUIRE(manifest.assets.size() == 1);
    REQUIRE(manifest.assets.front().asset_id == "logo");
    REQUIRE(manifest.assets.front().content_hash == "sha256:fixture");
}
