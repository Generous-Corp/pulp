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
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iterator>
#include <map>
#include <regex>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
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

std::vector<uint8_t> read_bytes(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    REQUIRE(in.is_open());
    return {std::istreambuf_iterator<char>(in), {}};
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

Rect absolute_bounds(const View& view) {
    auto out = view.bounds();
    for (auto* parent = view.parent(); parent != nullptr; parent = parent->parent()) {
        out.x += parent->bounds().x;
        out.y += parent->bounds().y;
    }
    return out;
}

Rect union_bounds(const std::vector<Rect>& rects) {
    REQUIRE_FALSE(rects.empty());
    float left = rects.front().x;
    float top = rects.front().y;
    float right = rects.front().right();
    float bottom = rects.front().bottom();
    for (const auto& rect : rects) {
        left = std::min(left, rect.x);
        top = std::min(top, rect.y);
        right = std::max(right, rect.right());
        bottom = std::max(bottom, rect.bottom());
    }
    return {left, top, right - left, bottom - top};
}

struct PixelCropRect {
    uint32_t x = 0;
    uint32_t y = 0;
    uint32_t width = 0;
    uint32_t height = 0;
};

PixelCropRect expanded_crop(Rect rect, float pad, uint32_t max_width, uint32_t max_height) {
    const auto left = std::max(0.0f, std::floor(rect.x - pad));
    const auto top = std::max(0.0f, std::floor(rect.y - pad));
    const auto right = std::min(static_cast<float>(max_width), std::ceil(rect.right() + pad));
    const auto bottom = std::min(static_cast<float>(max_height), std::ceil(rect.bottom() + pad));
    REQUIRE(right > left);
    REQUIRE(bottom > top);
    return {
        static_cast<uint32_t>(left),
        static_cast<uint32_t>(top),
        static_cast<uint32_t>(right - left),
        static_cast<uint32_t>(bottom - top),
    };
}

bool crop_intersects_diff(PixelCropRect crop, const DiffBounds& bounds) {
    if (!bounds.valid)
        return true;
    const auto crop_right = crop.x + crop.width;
    const auto crop_bottom = crop.y + crop.height;
    const auto bounds_right = bounds.x + bounds.width;
    const auto bounds_bottom = bounds.y + bounds.height;
    return crop.x < bounds_right && crop_right > bounds.x &&
           crop.y < bounds_bottom && crop_bottom > bounds.y;
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

std::unordered_map<std::string, Rect> read_runtime_native_bounds(const fs::path& path) {
    REQUIRE(fs::exists(path));
    auto trace = choc::json::parse(read_text(path));
    REQUIRE(trace.isObject());
    const auto entries = trace["native_bounds"];
    REQUIRE(entries.isArray());

    std::unordered_map<std::string, Rect> out;
    for (uint32_t i = 0; i < entries.size(); ++i) {
        const auto entry = entries[i];
        if (!entry.isObject())
            continue;
        const auto id = json_string(entry["id"]);
        const auto bounds = entry["bounds"];
        if (id.empty() || !bounds.isObject())
            continue;
        out[id] = Rect{
            json_float(bounds["x"]),
            json_float(bounds["y"]),
            json_float(bounds["width"]),
            json_float(bounds["height"])
        };
    }
    return out;
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

struct ChainerKnobSourceFormula {
    float source_start_angle = -135.0f;
    float source_sweep_angle = 270.0f;
    float radius_base_inset = 4.0f;
    float track_radius_inset = 1.0f;
    float body_radius_inset = 3.0f;
    float pointer_radius_inset = 6.0f;
    float center_dot_radius = 2.0f;
    float stroke_width = 1.5f;
    float body_stroke_width = 0.5f;
    float fill_opacity = 0.45f;
    std::string track_dash_array = "72 28";
    std::string track_dash_offset = "36";
    bool source_guard_passed = false;
};

float regex_float_or_fail(const std::string& text,
                          const std::string& pattern,
                          const char* label) {
    std::smatch match;
    INFO(label);
    INFO(pattern);
    REQUIRE(std::regex_search(text, match, std::regex(pattern)));
    REQUIRE(match.size() >= 2);
    return std::stof(match[1].str());
}

std::string regex_string_or_fail(const std::string& text,
                                 const std::string& pattern,
                                 const char* label) {
    std::smatch match;
    INFO(label);
    INFO(pattern);
    REQUIRE(std::regex_search(text, match, std::regex(pattern)));
    REQUIRE(match.size() >= 2);
    return match[1].str();
}

ChainerKnobSourceFormula read_chainer_knob_source_formula(const fs::path& source_path) {
    REQUIRE(fs::exists(source_path));
    const auto source = read_text(source_path);
    const auto knob_start = source.find("function Knob({");
    REQUIRE(knob_start != std::string::npos);
    const auto fader_start = source.find("function Fader({", knob_start);
    REQUIRE(fader_start != std::string::npos);
    const auto body = source.substr(knob_start, fader_start - knob_start);

    ChainerKnobSourceFormula formula;
    formula.source_start_angle = regex_float_or_fail(
        body, R"(const angle = (-?[0-9.]+) \+ value \* [0-9.]+;)", "source knob start angle");
    formula.source_sweep_angle = regex_float_or_fail(
        body, R"(const angle = -?[0-9.]+ \+ value \* ([0-9.]+);)", "source knob sweep angle");
    formula.radius_base_inset = regex_float_or_fail(
        body, R"(r = size / 2 - ([0-9.]+);)", "source knob base radius inset");
    formula.pointer_radius_inset = regex_float_or_fail(
        body, R"(Math\.sin\(rad\) \* \(r - ([0-9.]+)\);)", "source knob pointer radius inset");
    formula.track_radius_inset = regex_float_or_fail(
        body, R"(Math\.sin\(arcStart\) \* \(r - ([0-9.]+)\);)", "source knob track radius inset");
    formula.body_radius_inset = regex_float_or_fail(
        body, R"(r=\{r - ([0-9.]+)\} fill=\{C\.bgMod\})", "source knob body radius inset");
    formula.center_dot_radius = regex_float_or_fail(
        body, R"(r=\{([0-9.]+)\} fill=\{color\})", "source knob center dot radius");
    formula.stroke_width = regex_float_or_fail(
        body, R"(strokeWidth=\{([0-9.]+)\} opacity=\{0\.45\})", "source knob stroke width");
    formula.body_stroke_width = regex_float_or_fail(
        body, R"(r=\{r - [0-9.]+\} fill=\{C\.bgMod\} stroke=\{C\.borderMid\} strokeWidth=\{([0-9.]+)\})",
        "source knob body stroke width");
    formula.fill_opacity = regex_float_or_fail(
        body, R"(fill="none" stroke=\{color\} strokeWidth=\{[0-9.]+\} opacity=\{([0-9.]+)\})",
        "source knob fill opacity");
    formula.track_dash_array = regex_string_or_fail(
        body, R"re(strokeDasharray="([^"]+)")re", "source knob track dash array");
    formula.track_dash_offset = regex_string_or_fail(
        body, R"re(strokeDashoffset="([^"]+)")re", "source knob track dash offset");

    REQUIRE(formula.source_start_angle == Catch::Approx(-135.0f));
    REQUIRE(formula.source_sweep_angle == Catch::Approx(270.0f));
    REQUIRE(formula.radius_base_inset == Catch::Approx(4.0f));
    REQUIRE(formula.track_radius_inset == Catch::Approx(1.0f));
    REQUIRE(formula.body_radius_inset == Catch::Approx(3.0f));
    REQUIRE(formula.pointer_radius_inset == Catch::Approx(6.0f));
    REQUIRE(formula.center_dot_radius == Catch::Approx(2.0f));
    REQUIRE(formula.stroke_width == Catch::Approx(1.5f));
    REQUIRE(formula.body_stroke_width == Catch::Approx(0.5f));
    REQUIRE(formula.fill_opacity == Catch::Approx(0.45f));
    REQUIRE(formula.track_dash_array == "72 28");
    REQUIRE(formula.track_dash_offset == "36");
    formula.source_guard_passed = true;
    return formula;
}

fs::path source_jsx_path_from_route_manifest(choc::value::ValueView route_manifest) {
    const auto relative = json_string(route_manifest["inputs"]["sourceJsx"]["path"]);
    REQUIRE_FALSE(relative.empty());
    fs::path path(relative);
    if (path.is_relative())
        path = fs::path(PULP_REPO_ROOT) / path;
    REQUIRE(fs::exists(path));
    return path;
}

std::string hex_alpha(float alpha) {
    const auto clamped = std::clamp(alpha, 0.0f, 1.0f);
    const auto byte = static_cast<int>(std::round(clamped * 255.0f));
    std::ostringstream out;
    out << std::hex << std::nouppercase << std::setw(2) << std::setfill('0') << byte;
    return out.str();
}

float chainer_source_track_radius(float size, const ChainerKnobSourceFormula& formula) {
    return std::max(0.0f, size * 0.5f - formula.radius_base_inset - formula.track_radius_inset);
}

float chainer_source_body_radius(float size, const ChainerKnobSourceFormula& formula) {
    return std::max(0.0f, size * 0.5f - formula.radius_base_inset - formula.body_radius_inset);
}

float chainer_source_pointer_radius(float size, const ChainerKnobSourceFormula& formula) {
    return std::max(0.0f, size * 0.5f - formula.radius_base_inset - formula.pointer_radius_inset);
}

std::string chainer_knob_schema_for_style_tokens(std::string_view tokens,
                                                 float size,
                                                 const ChainerKnobSourceFormula& formula) {
    const auto color = color_for_style_tokens(tokens);
    const auto fill_color = color + hex_alpha(formula.fill_opacity);
    const auto track_radius = chainer_source_track_radius(size, formula);
    const auto body_radius = chainer_source_body_radius(size, formula);
    const auto pointer_outer_radius = chainer_source_pointer_radius(size, formula);
    const auto schema_start_angle = -formula.source_start_angle;
    const auto schema_end_angle = schema_start_angle + formula.source_sweep_angle;
    std::ostringstream out;
    out << "{\"elements\":["
        << "{\"type\":\"arc\",\"color\":\"#2a2a34\",\"radius\":\"" << float_attr(track_radius)
        << "\",\"startAngle\":" << float_attr(schema_start_angle)
        << ",\"sweepAngle\":" << float_attr(formula.source_sweep_angle)
        << ",\"width\":" << float_attr(formula.stroke_width) << "},"
        << "{\"type\":\"arc\",\"color\":\"" << fill_color << "\",\"radius\":\"" << float_attr(track_radius)
        << "\",\"startAngle\":" << float_attr(schema_start_angle)
        << ",\"sweepAngle\":{\"bind\":\"value\",\"range\":[0," << float_attr(formula.source_sweep_angle)
        << "]},\"width\":" << float_attr(formula.stroke_width) << "},"
        << "{\"type\":\"circle\",\"color\":\"#14141a\",\"radius\":\"" << float_attr(body_radius)
        << "\",\"strokeColor\":\"#2a2a34\",\"strokeWidth\":" << float_attr(formula.body_stroke_width) << "},"
        << "{\"type\":\"line\",\"color\":\"" << color << "\",\"angle\":{\"bind\":\"value\",\"range\":["
        << float_attr(schema_start_angle) << "," << float_attr(schema_end_angle)
        << "]},\"innerRadius\":\"0\",\"outerRadius\":\"" << float_attr(pointer_outer_radius)
        << "\",\"width\":" << float_attr(formula.stroke_width) << "},"
        << "{\"type\":\"circle\",\"color\":\"" << color << "\",\"radius\":\""
        << float_attr(formula.center_dot_radius) << "\"}"
        << "]}";
    return out.str();
}

void add_chainer_token_colors(DesignIR& ir) {
    ir.tokens.colors["chainer.bgMod"] = "#14141a";
    ir.tokens.colors["chainer.borderMid"] = "#2a2a34";
    ir.tokens.colors["chainer.orange"] = "#ff6b35";
    ir.tokens.colors["chainer.orange.fill"] = "#ff6b3573";
    ir.tokens.colors["chainer.blue"] = "#5b8af0";
    ir.tokens.colors["chainer.blue.fill"] = "#5b8af073";
    ir.tokens.colors["chainer.purple"] = "#9b59ff";
    ir.tokens.colors["chainer.purple.fill"] = "#9b59ff73";
    ir.tokens.colors["chainer.green"] = "#3ddc84";
    ir.tokens.colors["chainer.green.fill"] = "#3ddc8473";
    ir.tokens.colors["chainer.amber"] = "#f0a030";
    ir.tokens.colors["chainer.amber.fill"] = "#f0a03073";
}

IRNode lower_chainer_knob_route_to_node(IRNode& materialized_root,
                                        choc::value::ValueView route,
                                        const ChainerKnobSourceFormula& formula) {
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

    auto wrapper = *materialized_node;
    REQUIRE_FALSE(wrapper.children.empty());
    wrapper.name = label + " wrapper";
    wrapper.audio_widget = AudioWidgetType::none;
    wrapper.audio_label.clear();
    wrapper.layout.flex_shrink = 0.0f;
    wrapper.attributes.clear();
    wrapper.stable_anchor_id.reset();
    wrapper.anchor_strategy.reset();

    auto knob = wrapper.children.front();
    knob.children.clear();
    knob.type = "knob";
    knob.name = label;
    knob.text_content.clear();
    knob.style.width = size;
    knob.style.height = size;
    knob.style.border_color = color_for_style_tokens(style_tokens);
    knob.layout.flex_shrink = 0.0f;
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
    knob.attributes["pulpWidgetSchema"] = chainer_knob_schema_for_style_tokens(style_tokens, size, formula);
    knob.attributes["pulpShowInternalLabel"] = "false";
    knob.stable_anchor_id = json_string(route["materialized_ir_anchor"]);
    knob.anchor_strategy = "adapter";

    wrapper.children.front() = std::move(knob);
    return wrapper;
}

DesignIR lower_chainer_knob_route_to_phase_c_ir(DesignIR materialized_ir,
                                                choc::value::ValueView route,
                                                const ChainerKnobSourceFormula& formula) {
    const auto size = json_float(route["size"]);

    DesignIR ir;
    ir.source = DesignSource::jsx;
    ir.capture_method = "phase-c-chainer-one-knob-route-overlay";
    ir.source_adapter = "native-cpp-import-execution-validation";
    ir.source_version = "phase-c";
    add_chainer_token_colors(ir);
    ir.root = frame_node("phase-c-root", "Chainer One Knob", size + 28.0f, size + 38.0f, LayoutDirection::column);
    ir.root.children.push_back(lower_chainer_knob_route_to_node(materialized_ir.root, route, formula));
    return ir;
}

DesignIR lower_chainer_knob_routes_to_phase_d_ir(DesignIR materialized_ir,
                                                 choc::value::ValueView route_rows,
                                                 const ChainerKnobSourceFormula& formula) {
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
        ir.root.children.push_back(lower_chainer_knob_route_to_node(materialized_ir.root, route, formula));
    }
    REQUIRE(ir.root.children.size() == 8);

    return ir;
}

DesignIR lower_chainer_knob_routes_to_phase_d_original_layout_ir(DesignIR materialized_ir,
                                                                 choc::value::ValueView route_rows,
                                                                 const ChainerKnobSourceFormula& formula) {
    materialized_ir.capture_method = "phase-d-chainer-original-layout-hybrid-route-overlay";
    materialized_ir.source_adapter = "native-cpp-import-execution-validation";
    materialized_ir.source_version = "phase-d-original-layout-hybrid";
    add_chainer_token_colors(materialized_ir);

    std::size_t lowered = 0;
    for (uint32_t i = 0; i < route_rows.size(); ++i) {
        const auto route = route_rows[i];
        if (json_string(route["source_component_family"]) != "Knob")
            continue;
        const auto materialized_path = json_string(route["materialized_ir_path"]);
        auto knob = lower_chainer_knob_route_to_node(materialized_ir.root, route, formula);
        *node_at_ir_path(materialized_ir.root, materialized_path) = std::move(knob);
        ++lowered;
    }
    REQUIRE(lowered == 8);
    return materialized_ir;
}

std::string json_escape(std::string_view text) {
    std::ostringstream out;
    for (unsigned char c : text) {
        switch (c) {
            case '"': out << "\\\""; break;
            case '\\': out << "\\\\"; break;
            case '\b': out << "\\b"; break;
            case '\f': out << "\\f"; break;
            case '\n': out << "\\n"; break;
            case '\r': out << "\\r"; break;
            case '\t': out << "\\t"; break;
            default:
                if (c < 0x20) {
                    out << "\\u" << std::hex << std::setw(4) << std::setfill('0')
                        << static_cast<int>(c) << std::dec << std::setfill(' ');
                } else {
                    out << static_cast<char>(c);
                }
                break;
        }
    }
    return out.str();
}

Color hex_color_or(std::string_view value, Color fallback) {
    auto hex_digit = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    auto pair = [&](std::size_t offset) -> int {
        if (offset + 1 >= value.size()) return -1;
        const int high = hex_digit(value[offset]);
        const int low = hex_digit(value[offset + 1]);
        if (high < 0 || low < 0) return -1;
        return (high << 4) | low;
    };
    if (value.size() != 7 && value.size() != 9) return fallback;
    if (value.front() != '#') return fallback;
    const int r = pair(1);
    const int g = pair(3);
    const int b = pair(5);
    const int a = value.size() == 9 ? pair(7) : 255;
    if (r < 0 || g < 0 || b < 0 || a < 0) return fallback;
    return Color::rgba8(static_cast<uint8_t>(r),
                        static_cast<uint8_t>(g),
                        static_cast<uint8_t>(b),
                        static_cast<uint8_t>(a));
}

struct PhaseDKnobSurfaceCase {
    std::string id;
    std::string anchor;
    std::string label;
    std::string param_key;
    std::string style_tokens;
    std::string source_path;
    float size = 0.0f;
    float value = 0.0f;
};

struct PhaseDKnobLayoutCase {
    std::string id;
    std::string anchor;
    std::string source_visual_anchor;
    std::string source_visual_ir_path;
    std::string param_key;
    float expected_size = 0.0f;
};

std::vector<PhaseDKnobSurfaceCase> chainer_knob_surface_cases(choc::value::ValueView route_rows) {
    std::vector<PhaseDKnobSurfaceCase> cases;
    for (uint32_t i = 0; i < route_rows.size(); ++i) {
        const auto route = route_rows[i];
        if (json_string(route["source_component_family"]) != "Knob")
            continue;
        const auto binding = route["parameter_bindings"][0];
        PhaseDKnobSurfaceCase item;
        item.id = json_string(route["id"]);
        item.anchor = json_string(route["materialized_ir_anchor"]);
        item.label = json_string(route["label"]);
        item.param_key = json_string(binding["param_key"]);
        item.style_tokens = style_token_string(route);
        item.source_path = json_string(route["stable_source_path"]);
        item.size = json_float(route["size"]);
        item.value = json_float(route["value"]);
        cases.push_back(std::move(item));
    }
    REQUIRE(cases.size() == 8);
    return cases;
}

std::vector<PhaseDKnobLayoutCase> chainer_knob_layout_cases(IRNode& materialized_root,
                                                            choc::value::ValueView route_rows) {
    std::vector<PhaseDKnobLayoutCase> cases;
    for (uint32_t i = 0; i < route_rows.size(); ++i) {
        const auto route = route_rows[i];
        if (json_string(route["source_component_family"]) != "Knob")
            continue;
        const auto materialized_path = json_string(route["materialized_ir_path"]);
        auto* wrapper = node_at_ir_path(materialized_root, materialized_path);
        REQUIRE(wrapper != nullptr);
        REQUIRE_FALSE(wrapper->children.empty());
        auto& source_visual = wrapper->children.front();
        REQUIRE(source_visual.stable_anchor_id.has_value());

        const auto binding = route["parameter_bindings"][0];
        PhaseDKnobLayoutCase item;
        item.id = json_string(route["id"]);
        item.anchor = json_string(route["materialized_ir_anchor"]);
        item.source_visual_anchor = *source_visual.stable_anchor_id;
        item.source_visual_ir_path = materialized_path + "/0";
        item.param_key = json_string(binding["param_key"]);
        item.expected_size = json_float(route["size"]);

        // This checks the source-declared SVG/control size. The layout
        // report later measures whether the laid-out bounds preserve it.
        REQUIRE(source_visual.style.width.has_value());
        REQUIRE(source_visual.style.height.has_value());
        REQUIRE(*source_visual.style.width == Catch::Approx(item.expected_size));
        REQUIRE(*source_visual.style.height == Catch::Approx(item.expected_size));
        cases.push_back(std::move(item));
    }
    REQUIRE(cases.size() == 8);
    return cases;
}

void set_fixed_surface_size(View& view, float size) {
    auto& flex = view.flex();
    flex.direction = FlexDirection::column;
    flex.justify_content = FlexJustify::start;
    flex.align_items = FlexAlign::stretch;
    flex.preferred_width = size;
    flex.preferred_height = size;
    flex.dim_width = {size, DimensionUnit::px};
    flex.dim_height = {size, DimensionUnit::px};
}

class FixedSurfaceRowView final : public View {
public:
    FixedSurfaceRowView(std::vector<float> widths, float gap)
        : widths_(std::move(widths)), gap_(gap) {}

    void layout_children() override {
        REQUIRE(child_count() == widths_.size());
        const auto b = local_bounds();
        float x = 0.0f;
        for (std::size_t i = 0; i < child_count(); ++i) {
            const auto size = widths_[i];
            auto* child = child_at(i);
            child->set_bounds({x, (b.height - size) * 0.5f, size, size});
            child->layout_children();
            x += size + gap_;
        }
    }

private:
    std::vector<float> widths_;
    float gap_ = 0.0f;
};

class ChainerKnobSourceShapeView final : public View {
public:
    ChainerKnobSourceShapeView(float value, Color color, ChainerKnobSourceFormula formula)
        : value_(std::clamp(value, 0.0f, 1.0f)), color_(color), formula_(std::move(formula)) {}

    void paint(pulp::canvas::Canvas& canvas) override {
        const auto b = local_bounds();
        const float size = std::min(b.width, b.height);
        const float offset_x = (b.width - size) * 0.5f;
        const float offset_y = (b.height - size) * 0.5f;
        const float cx = offset_x + size * 0.5f;
        const float cy = offset_y + size * 0.5f;
        const float track_radius = chainer_source_track_radius(size, formula_);
        const float body_radius = chainer_source_body_radius(size, formula_);
        const float pointer_radius = chainer_source_pointer_radius(size, formula_);
        constexpr float kPi = 3.14159f;
        const float schema_start_angle = -formula_.source_start_angle;
        const float start = schema_start_angle * kPi / 180.0f;
        const float end = (schema_start_angle + value_ * formula_.source_sweep_angle) * kPi / 180.0f;

        canvas.set_line_cap(pulp::canvas::LineCap::round);
        canvas.set_stroke_color(Color::rgba8(42, 42, 52, 255));
        canvas.set_line_width(formula_.stroke_width);
        canvas.stroke_arc(cx, cy, track_radius, start,
                          (schema_start_angle + formula_.source_sweep_angle) * kPi / 180.0f);

        if (value_ > 0.001f) {
            canvas.set_stroke_color(Color::rgba(color_.r, color_.g, color_.b, formula_.fill_opacity));
            canvas.stroke_arc(cx, cy, track_radius, start, end);
        }

        canvas.set_fill_color(Color::rgba8(20, 20, 26, 255));
        canvas.fill_circle(cx, cy, body_radius);
        canvas.set_stroke_color(Color::rgba8(42, 42, 52, 255));
        canvas.set_line_width(formula_.body_stroke_width);
        canvas.stroke_circle(cx, cy, body_radius);

        const float source_angle = formula_.source_start_angle + value_ * formula_.source_sweep_angle;
        const float source_rad = source_angle * kPi / 180.0f;
        const float x2 = cx + std::sin(source_rad) * pointer_radius;
        const float y2 = cy - std::cos(source_rad) * pointer_radius;
        canvas.set_stroke_color(color_);
        canvas.set_line_width(formula_.stroke_width);
        canvas.set_line_cap(pulp::canvas::LineCap::round);
        canvas.stroke_line(cx, cy, x2, y2);

        canvas.set_fill_color(color_);
        canvas.fill_circle(cx, cy, formula_.center_dot_radius);
    }

private:
    float value_ = 0.0f;
    Color color_;
    ChainerKnobSourceFormula formula_;
};

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
    const auto source_formula = read_chainer_knob_source_formula(
        source_jsx_path_from_route_manifest(route_manifest));
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

    const auto ir = lower_chainer_knob_route_to_phase_c_ir(
        std::move(materialized_ir), route, source_formula);
    CppExportOptions opts;
    opts.header_filename = "phase_c_chainer_one_knob.hpp";
    opts.namespace_name = "pulp::test::phase_c";

    const auto result = generate_pulp_cpp(ir, ir.asset_manifest, opts);

    REQUIRE(count_occurrences(result.source, "std::make_unique<pulp::view::Knob>()") == 1);
    REQUIRE(result.source.find("->set_anchor_id(\"pr_2c\");") != std::string::npos);
    REQUIRE(result.source.find("->set_label(\"freq\");") != std::string::npos);
    REQUIRE(result.source.find("->set_value(/* TODO: bind to param */ 0.35f);") != std::string::npos);
    REQUIRE(result.source.find("->set_default_value(0.35f);") != std::string::npos);
    REQUIRE(result.source.find("->set_widget_schema(") != std::string::npos);
    REQUIRE(result.source.find("->set_show_label(false);") != std::string::npos);
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
    REQUIRE(entry["ir_path"].getString() == std::string("root/0/0"));
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
    const auto source_formula = read_chainer_knob_source_formula(
        source_jsx_path_from_route_manifest(route_manifest));
    const auto route_rows = route_manifest["source_contract_overlay"]["node_route_rows"];
    REQUIRE(route_manifest["source_contract_overlay"]["validation"]["actual"]["knob_routes"].getInt64() == 8);

    const fs::path chainer_ir_path =
        fs::path(PULP_REPO_ROOT) / "planning/artifacts/native-ui/nv0/reports/generated/chainer-ir.json";
    REQUIRE(fs::exists(chainer_ir_path));
    auto materialized_ir = parse_design_ir_json(read_text(chainer_ir_path));

    const auto ir = lower_chainer_knob_routes_to_phase_d_ir(
        std::move(materialized_ir), route_rows, source_formula);
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
    REQUIRE(count_occurrences(result.source, "->set_widget_schema(") == 8);
    REQUIRE(count_occurrences(result.source, "->set_show_label(false);") == 8);

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

        const auto bounds = absolute_bounds(*knob);
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

TEST_CASE("generated Chainer knob schemas match source-shape paint oracle",
          "[view][import][cpp-codegen][native-cpp-phase-d][visual]") {
    const fs::path manifest_path =
        fs::path(PULP_REPO_ROOT) / "planning/artifacts/native-ui/nv0/reports/chainer-route-manifest.json";
    REQUIRE(fs::exists(manifest_path));
    auto route_manifest = choc::json::parse(read_text(manifest_path));
    const auto source_formula = read_chainer_knob_source_formula(
        source_jsx_path_from_route_manifest(route_manifest));
    const auto cases = chainer_knob_surface_cases(
        route_manifest["source_contract_overlay"]["node_route_rows"]);

    auto generated = ::build_imported_ui();
    REQUIRE(generated != nullptr);
    generated->set_bounds({0.0f, 0.0f, 520.0f, 96.0f});
    generated->layout_children();

    std::vector<float> widths;
    widths.reserve(cases.size());
    float atlas_width = 0.0f;
    float atlas_height = 0.0f;
    constexpr float kGap = 8.0f;
    for (const auto& item : cases) {
        widths.push_back(item.size);
        atlas_width += item.size;
        atlas_height = std::max(atlas_height, item.size);
    }
    atlas_width += kGap * static_cast<float>(cases.size() - 1);

    FixedSurfaceRowView source_root(widths, kGap);
    FixedSurfaceRowView native_root(widths, kGap);

    for (const auto& item : cases) {
        const auto accent = hex_color_or(color_for_style_tokens(item.style_tokens),
                                         Color::rgba8(255, 107, 53, 255));
        auto source = std::make_unique<ChainerKnobSourceShapeView>(item.value, accent, source_formula);
        set_fixed_surface_size(*source, item.size);
        source_root.add_child(std::move(source));

        auto* generated_view = find_anchor(*generated, item.anchor);
        REQUIRE(generated_view != nullptr);
        auto* generated_knob = dynamic_cast<Knob*>(generated_view);
        REQUIRE(generated_knob != nullptr);
        REQUIRE_FALSE(generated_knob->widget_schema().empty());
        REQUIRE_FALSE(generated_knob->show_label());
        REQUIRE(generated_knob->value() == Catch::Approx(item.value));

        auto native = std::make_unique<Knob>();
        native->set_anchor_id(item.anchor);
        native->set_label(generated_knob->label());
        native->set_value(generated_knob->value());
        native->set_default_value(generated_knob->default_value());
        native->set_widget_schema(generated_knob->widget_schema());
        native->set_show_label(generated_knob->show_label());
        set_fixed_surface_size(*native, item.size);
        native_root.add_child(std::move(native));
    }

    const auto width = static_cast<uint32_t>(std::ceil(atlas_width));
    const auto height = static_cast<uint32_t>(std::ceil(atlas_height));
    auto source_png = render_to_png(source_root, width, height, 1.0f);
    auto native_png = render_to_png(native_root, width, height, 1.0f);
    if (source_png.empty() || native_png.empty())
        SKIP("native screenshot renderer unavailable for source-shape knob surface oracle");

    auto diff_png = generate_diff_image(source_png, native_png, 8);
    REQUIRE_FALSE(diff_png.empty());
    auto atlas_result = compare_screenshots(source_png, native_png, 8);
    REQUIRE(atlas_result.valid);

    struct SurfaceComparison {
        const PhaseDKnobSurfaceCase* item = nullptr;
        CompareResult result;
    };
    std::vector<SurfaceComparison> per_knob;
    per_knob.reserve(cases.size());
    float x = 0.0f;
    for (const auto& item : cases) {
        const auto crop_x = static_cast<uint32_t>(std::round(x));
        const auto crop_y = static_cast<uint32_t>(std::round((atlas_height - item.size) * 0.5f));
        const auto crop_size = static_cast<uint32_t>(std::round(item.size));
        auto source_crop = crop_png(source_png, crop_x, crop_y, crop_size, crop_size);
        auto native_crop = crop_png(native_png, crop_x, crop_y, crop_size, crop_size);
        REQUIRE_FALSE(source_crop.empty());
        REQUIRE_FALSE(native_crop.empty());
        auto result = compare_screenshots(source_crop, native_crop, 8);
        REQUIRE(result.valid);
        per_knob.push_back({&item, result});
        x += item.size + kGap;
    }

    float min_similarity = 1.0f;
    float similarity_sum = 0.0f;
    uint32_t diff_pixels = 0;
    uint32_t total_pixels = 0;
    for (const auto& row : per_knob) {
        min_similarity = std::min(min_similarity, row.result.similarity);
        similarity_sum += row.result.similarity;
        diff_pixels += row.result.diff_pixels;
        total_pixels += row.result.total_pixels;
    }
    const float average_similarity = similarity_sum / static_cast<float>(per_knob.size());
    constexpr float kSurfaceThreshold = 0.995f;
    const bool within_threshold = atlas_result.passes(kSurfaceThreshold) &&
        min_similarity >= kSurfaceThreshold;

    if (const char* artifact_dir = std::getenv("PULP_NATIVE_UI_PHASE_D_ARTIFACT_DIR")) {
        const fs::path dir(artifact_dir);
        write_bytes(dir / "reports" / "screenshots" / "chainer-phase-d-knob-surface-source-atlas.png", source_png);
        write_bytes(dir / "reports" / "screenshots" / "chainer-phase-d-knob-surface-native-atlas.png", native_png);
        write_bytes(dir / "reports" / "screenshots" / "chainer-phase-d-knob-surface-diff-atlas.png", diff_png);

        std::ostringstream report;
        report << "{\n"
               << "  \"schema\": \"pulp-native-ui-phase-d-knob-surface-visual-v1\",\n"
               << "  \"fixture\": \"chainer-phase-d-all-knobs\",\n"
               << "  \"scope\": \"generated-native-cpp-knob-schema-vs-source-shape-oracle\",\n"
               << "  \"native_surface\": \"generated-cpp-widget-schema\",\n"
               << "  \"source_shape_oracle\": \"transcribed-fixture-knob-formula\",\n"
               << "  \"source_formula_guard\": {"
               << "\"passed\": " << (source_formula.source_guard_passed ? "true" : "false") << ", "
               << "\"source_start_angle\": " << source_formula.source_start_angle << ", "
               << "\"source_sweep_angle\": " << source_formula.source_sweep_angle << ", "
               << "\"radius_base_inset\": " << source_formula.radius_base_inset << ", "
               << "\"track_radius_inset\": " << source_formula.track_radius_inset << ", "
               << "\"body_radius_inset\": " << source_formula.body_radius_inset << ", "
               << "\"pointer_radius_inset\": " << source_formula.pointer_radius_inset << ", "
               << "\"center_dot_radius\": " << source_formula.center_dot_radius << ", "
               << "\"stroke_width\": " << source_formula.stroke_width << ", "
               << "\"body_stroke_width\": " << source_formula.body_stroke_width << ", "
               << "\"fill_opacity\": " << source_formula.fill_opacity << ", "
               << "\"track_dash_array\": \"" << json_escape(source_formula.track_dash_array) << "\", "
               << "\"track_dash_offset\": \"" << json_escape(source_formula.track_dash_offset) << "\"},\n"
               << "  \"source_track_ring\": \"approximated_as_importable_sweep_arc\",\n"
               << "  \"threshold\": " << kSurfaceThreshold << ",\n"
               << "  \"classification\": \"" << (within_threshold ? "within_threshold" : "failed_visual_threshold") << "\",\n"
               << "  \"within_threshold\": " << (within_threshold ? "true" : "false") << ",\n"
               << "  \"knob_count\": " << per_knob.size() << ",\n"
               << "  \"atlas_width\": " << width << ",\n"
               << "  \"atlas_height\": " << height << ",\n"
               << "  \"atlas_similarity\": " << std::setprecision(7) << atlas_result.similarity << ",\n"
               << "  \"atlas_mean_error\": " << atlas_result.mean_error << ",\n"
               << "  \"atlas_diff_pixels\": " << atlas_result.diff_pixels << ",\n"
               << "  \"atlas_total_pixels\": " << atlas_result.total_pixels << ",\n"
               << "  \"min_knob_similarity\": " << min_similarity << ",\n"
               << "  \"average_knob_similarity\": " << average_similarity << ",\n"
               << "  \"knob_diff_pixels\": " << diff_pixels << ",\n"
               << "  \"knob_total_pixels\": " << total_pixels << ",\n"
               << "  \"knobs\": [";
        for (std::size_t i = 0; i < per_knob.size(); ++i) {
            if (i != 0)
                report << ",";
            const auto& row = per_knob[i];
            report << "\n    {"
                   << "\"id\": \"" << json_escape(row.item->id) << "\", "
                   << "\"anchor\": \"" << json_escape(row.item->anchor) << "\", "
                   << "\"source_path\": \"" << json_escape(row.item->source_path) << "\", "
                   << "\"param_key\": \"" << json_escape(row.item->param_key) << "\", "
                   << "\"style_tokens\": \"" << json_escape(row.item->style_tokens) << "\", "
                   << "\"size\": " << row.item->size << ", "
                   << "\"value\": " << row.item->value << ", "
                   << "\"similarity\": " << row.result.similarity << ", "
                   << "\"mean_error\": " << row.result.mean_error << ", "
                   << "\"diff_pixels\": " << row.result.diff_pixels << ", "
                   << "\"total_pixels\": " << row.result.total_pixels
                   << "}";
        }
        report << "\n  ]\n"
               << "}\n";
        write_text(dir / "reports" / "chainer-phase-d-knob-surface-visual-report.json", report.str());
    }

    REQUIRE(within_threshold);
}

TEST_CASE("Chainer original-layout hybrid classifies knob replacement bounds against source materialization",
          "[view][import][cpp-codegen][native-cpp-phase-d][layout]") {
    const fs::path manifest_path =
        fs::path(PULP_REPO_ROOT) / "planning/artifacts/native-ui/nv0/reports/chainer-route-manifest.json";
    const fs::path chainer_ir_path =
        fs::path(PULP_REPO_ROOT) / "planning/artifacts/native-ui/nv0/reports/generated/chainer-ir.json";
    const fs::path runtime_trace_path =
        fs::path(PULP_REPO_ROOT) / "planning/artifacts/native-ui/nv0/reports/traces/chainer-live-runtime-trace.json";
    REQUIRE(fs::exists(manifest_path));
    REQUIRE(fs::exists(chainer_ir_path));
    REQUIRE(fs::exists(runtime_trace_path));

    auto route_manifest = choc::json::parse(read_text(manifest_path));
    const auto source_formula = read_chainer_knob_source_formula(
        source_jsx_path_from_route_manifest(route_manifest));
    const auto route_rows = route_manifest["source_contract_overlay"]["node_route_rows"];
    auto materialized_ir = parse_design_ir_json(read_text(chainer_ir_path));
    const auto live_native_bounds = read_runtime_native_bounds(runtime_trace_path);
    const auto cases = chainer_knob_layout_cases(materialized_ir.root, route_rows);

    std::vector<ImportDiagnostic> source_diagnostics;
    auto source_view = build_native_view_tree(
        materialized_ir, materialized_ir.asset_manifest, {.diagnostics_out = &source_diagnostics});
    REQUIRE(source_view != nullptr);

    auto hybrid_ir = lower_chainer_knob_routes_to_phase_d_original_layout_ir(
        std::move(materialized_ir), route_rows, source_formula);
    std::vector<ImportDiagnostic> hybrid_diagnostics;
    auto hybrid_view = build_native_view_tree(
        hybrid_ir, hybrid_ir.asset_manifest, {.diagnostics_out = &hybrid_diagnostics});
    REQUIRE(hybrid_view != nullptr);

    constexpr float kWidth = 1280.0f;
    constexpr float kHeight = 800.0f;
    source_view->set_bounds({0.0f, 0.0f, kWidth, kHeight});
    hybrid_view->set_bounds({0.0f, 0.0f, kWidth, kHeight});
    source_view->layout_children();
    hybrid_view->layout_children();

    struct LayoutComparison {
        const PhaseDKnobLayoutCase* item = nullptr;
        Rect source_bounds;
        Rect live_source_bounds;
        Rect native_bounds;
        float center_delta_px = 0.0f;
        float size_delta_px = 0.0f;
        float source_expected_size_delta_px = 0.0f;
        float live_center_delta_px = 0.0f;
        float live_size_delta_px = 0.0f;
        float live_expected_size_delta_px = 0.0f;
        float native_expected_size_delta_px = 0.0f;
    };

    std::vector<LayoutComparison> comparisons;
    comparisons.reserve(cases.size());
    float max_center_delta = 0.0f;
    float max_size_delta = 0.0f;
    float max_source_expected_size_delta = 0.0f;
    float max_live_center_delta = 0.0f;
    float max_live_size_delta = 0.0f;
    float max_live_expected_size_delta = 0.0f;
    float max_native_expected_size_delta = 0.0f;
    constexpr float kBoundsTolerancePx = 0.5f;

    for (const auto& item : cases) {
        auto* source_visual = find_anchor(*source_view, item.source_visual_anchor);
        REQUIRE(source_visual != nullptr);
        auto* native_knob = find_anchor(*hybrid_view, item.anchor);
        REQUIRE(native_knob != nullptr);
        REQUIRE(dynamic_cast<Knob*>(native_knob) != nullptr);
        const auto live_it = live_native_bounds.find(item.source_visual_anchor);
        REQUIRE(live_it != live_native_bounds.end());

        const auto source_bounds = absolute_bounds(*source_visual);
        const auto live_source_bounds = live_it->second;
        const auto native_bounds = absolute_bounds(*native_knob);

        const auto source_cx = source_bounds.x + source_bounds.width * 0.5f;
        const auto source_cy = source_bounds.y + source_bounds.height * 0.5f;
        const auto live_source_cx = live_source_bounds.x + live_source_bounds.width * 0.5f;
        const auto live_source_cy = live_source_bounds.y + live_source_bounds.height * 0.5f;
        const auto native_cx = native_bounds.x + native_bounds.width * 0.5f;
        const auto native_cy = native_bounds.y + native_bounds.height * 0.5f;
        const auto center_delta = std::max(std::abs(source_cx - native_cx), std::abs(source_cy - native_cy));
        const auto size_delta = std::max(std::abs(source_bounds.width - native_bounds.width),
                                         std::abs(source_bounds.height - native_bounds.height));
        const auto source_expected_size_delta = std::max(std::abs(source_bounds.width - item.expected_size),
                                                         std::abs(source_bounds.height - item.expected_size));
        const auto live_center_delta = std::max(std::abs(live_source_cx - native_cx),
                                                std::abs(live_source_cy - native_cy));
        const auto live_size_delta = std::max(std::abs(live_source_bounds.width - native_bounds.width),
                                              std::abs(live_source_bounds.height - native_bounds.height));
        const auto live_expected_size_delta = std::max(std::abs(live_source_bounds.width - item.expected_size),
                                                       std::abs(live_source_bounds.height - item.expected_size));
        const auto native_expected_size_delta = std::max(std::abs(native_bounds.width - item.expected_size),
                                                         std::abs(native_bounds.height - item.expected_size));
        max_center_delta = std::max(max_center_delta, center_delta);
        max_size_delta = std::max(max_size_delta, size_delta);
        max_source_expected_size_delta = std::max(max_source_expected_size_delta, source_expected_size_delta);
        max_live_center_delta = std::max(max_live_center_delta, live_center_delta);
        max_live_size_delta = std::max(max_live_size_delta, live_size_delta);
        max_live_expected_size_delta = std::max(max_live_expected_size_delta, live_expected_size_delta);
        max_native_expected_size_delta = std::max(max_native_expected_size_delta, native_expected_size_delta);
        comparisons.push_back({&item,
                               source_bounds,
                               live_source_bounds,
                               native_bounds,
                               center_delta,
                               size_delta,
                               source_expected_size_delta,
                               live_center_delta,
                               live_size_delta,
                               live_expected_size_delta,
                               native_expected_size_delta});
    }

    const bool within_threshold = max_center_delta <= kBoundsTolerancePx &&
        max_size_delta <= kBoundsTolerancePx &&
        max_source_expected_size_delta <= kBoundsTolerancePx &&
        max_native_expected_size_delta <= kBoundsTolerancePx;
    const bool live_runtime_within_threshold = max_live_center_delta <= kBoundsTolerancePx &&
        max_live_size_delta <= kBoundsTolerancePx &&
        max_live_expected_size_delta <= kBoundsTolerancePx &&
        max_native_expected_size_delta <= kBoundsTolerancePx;
    // Prefer reporting native mismatch first if both sides are bad. Current
    // Chainer evidence is source-only collapse while native expected size holds.
    const char* coordinate_uncertainty_source = within_threshold
        ? "none"
        : max_source_expected_size_delta > kBoundsTolerancePx &&
              max_native_expected_size_delta <= kBoundsTolerancePx
            ? "source_materialized_bounds_collapsed"
            : max_native_expected_size_delta > kBoundsTolerancePx
                ? "native_layout_mismatch"
                : "source_native_bounds_delta";

    if (const char* artifact_dir = std::getenv("PULP_NATIVE_UI_PHASE_D_ARTIFACT_DIR")) {
        const fs::path dir(artifact_dir);
        std::ostringstream report;
        report << "{\n"
               << "  \"schema\": \"pulp-native-ui-phase-d-layout-bounds-v1\",\n"
               << "  \"fixture\": \"chainer-original-layout-hybrid\",\n"
               << "  \"scope\": \"source-materialized-knob-visual-bounds-vs-native-replacement-bounds\",\n"
               << "  \"source_bounds_basis\": \"materialized-ir-first-visual-child\",\n"
               << "  \"live_bounds_basis\": \"runtime-trace-getLayoutRect-source-visual-anchor\",\n"
               << "  \"native_bounds_basis\": \"original-layout-hybrid-native-knob\",\n"
               << "  \"threshold_px\": " << kBoundsTolerancePx << ",\n"
               << "  \"classification\": \"" << (within_threshold ? "within_threshold" : "failed_bounds_threshold") << "\",\n"
               << "  \"live_runtime_classification\": \""
               << (live_runtime_within_threshold ? "within_threshold" : "failed_bounds_threshold") << "\",\n"
               << "  \"coordinate_uncertainty_source\": \"" << coordinate_uncertainty_source << "\",\n"
               << "  \"within_threshold\": " << (within_threshold ? "true" : "false") << ",\n"
               << "  \"live_runtime_within_threshold\": "
               << (live_runtime_within_threshold ? "true" : "false") << ",\n"
               << "  \"knob_count\": " << comparisons.size() << ",\n"
               << "  \"live_runtime_matched_knob_count\": " << comparisons.size() << ",\n"
               << "  \"live_runtime_bounds_count\": " << live_native_bounds.size() << ",\n"
               << "  \"max_center_delta_px\": " << std::setprecision(7) << max_center_delta << ",\n"
               << "  \"max_size_delta_px\": " << max_size_delta << ",\n"
               << "  \"max_source_expected_size_delta_px\": " << max_source_expected_size_delta << ",\n"
               << "  \"max_live_center_delta_px\": " << max_live_center_delta << ",\n"
               << "  \"max_live_size_delta_px\": " << max_live_size_delta << ",\n"
               << "  \"max_live_expected_size_delta_px\": " << max_live_expected_size_delta << ",\n"
               << "  \"max_native_expected_size_delta_px\": " << max_native_expected_size_delta << ",\n"
               << "  \"knobs\": [";
        for (std::size_t i = 0; i < comparisons.size(); ++i) {
            if (i != 0)
                report << ",";
            const auto& row = comparisons[i];
            report << "\n    {"
                   << "\"id\": \"" << json_escape(row.item->id) << "\", "
                   << "\"anchor\": \"" << json_escape(row.item->anchor) << "\", "
                   << "\"source_visual_anchor\": \"" << json_escape(row.item->source_visual_anchor) << "\", "
                   << "\"source_visual_ir_path\": \"" << json_escape(row.item->source_visual_ir_path) << "\", "
                   << "\"param_key\": \"" << json_escape(row.item->param_key) << "\", "
                   << "\"expected_size\": " << row.item->expected_size << ", "
                   << "\"source_bounds\": {"
                   << "\"x\": " << row.source_bounds.x << ", "
                   << "\"y\": " << row.source_bounds.y << ", "
                   << "\"width\": " << row.source_bounds.width << ", "
                   << "\"height\": " << row.source_bounds.height << "}, "
                   << "\"live_source_bounds\": {"
                   << "\"x\": " << row.live_source_bounds.x << ", "
                   << "\"y\": " << row.live_source_bounds.y << ", "
                   << "\"width\": " << row.live_source_bounds.width << ", "
                   << "\"height\": " << row.live_source_bounds.height << "}, "
                   << "\"native_bounds\": {"
                   << "\"x\": " << row.native_bounds.x << ", "
                   << "\"y\": " << row.native_bounds.y << ", "
                   << "\"width\": " << row.native_bounds.width << ", "
                   << "\"height\": " << row.native_bounds.height << "}, "
                   << "\"center_delta_px\": " << row.center_delta_px << ", "
                   << "\"size_delta_px\": " << row.size_delta_px << ", "
                   << "\"source_expected_size_delta_px\": " << row.source_expected_size_delta_px << ", "
                   << "\"live_center_delta_px\": " << row.live_center_delta_px << ", "
                   << "\"live_size_delta_px\": " << row.live_size_delta_px << ", "
                   << "\"live_expected_size_delta_px\": " << row.live_expected_size_delta_px << ", "
                   << "\"native_expected_size_delta_px\": " << row.native_expected_size_delta_px
                   << "}";
        }
        report << "\n  ]\n"
               << "}\n";
        write_text(dir / "reports" / "chainer-phase-d-layout-report.json", report.str());
    }
}

TEST_CASE("Chainer original-layout hybrid classifies knob region visual diff",
          "[view][import][cpp-codegen][native-cpp-phase-d][visual]") {
    const fs::path manifest_path =
        fs::path(PULP_REPO_ROOT) / "planning/artifacts/native-ui/nv0/reports/chainer-route-manifest.json";
    const fs::path chainer_ir_path =
        fs::path(PULP_REPO_ROOT) / "planning/artifacts/native-ui/nv0/reports/generated/chainer-ir.json";
    const fs::path live_png_path =
        fs::path(PULP_REPO_ROOT) / "planning/artifacts/native-ui/nv0/reports/screenshots/chainer-live-coregraphics-1280x800.png";
    REQUIRE(fs::exists(manifest_path));
    REQUIRE(fs::exists(chainer_ir_path));
    REQUIRE(fs::exists(live_png_path));

    auto route_manifest = choc::json::parse(read_text(manifest_path));
    const auto source_formula = read_chainer_knob_source_formula(
        source_jsx_path_from_route_manifest(route_manifest));
    const auto route_rows = route_manifest["source_contract_overlay"]["node_route_rows"];
    auto materialized_ir = parse_design_ir_json(read_text(chainer_ir_path));
    auto hybrid_ir = lower_chainer_knob_routes_to_phase_d_original_layout_ir(
        std::move(materialized_ir), route_rows, source_formula);

    std::vector<ImportDiagnostic> diagnostics;
    auto hybrid_view = build_native_view_tree(hybrid_ir, hybrid_ir.asset_manifest, {.diagnostics_out = &diagnostics});
    REQUIRE(hybrid_view != nullptr);

    constexpr uint32_t kWidth = 1280;
    constexpr uint32_t kHeight = 800;
    auto hybrid_png = render_to_png(*hybrid_view, kWidth, kHeight, 1.0f);
    if (hybrid_png.empty())
        SKIP("native screenshot renderer unavailable for original-layout hybrid visual gate");
    const auto live_png = read_bytes(live_png_path);
    REQUIRE_FALSE(live_png.empty());

    auto full_result = compare_screenshots(live_png, hybrid_png, 32);
    REQUIRE(full_result.valid);
    auto full_changed = diff_bounds(live_png, hybrid_png, 32);

    const std::vector<std::string_view> anchors = {
        "pr_2c", "pr_2l", "pr_2u", "pr_49", "pr_4i", "pr_4y", "pr_57", "pr_6p",
    };
    std::vector<Rect> knob_bounds;
    for (auto anchor : anchors) {
        auto* view = find_anchor(*hybrid_view, anchor);
        REQUIRE(view != nullptr);
        REQUIRE(dynamic_cast<Knob*>(view) != nullptr);
        auto bounds = absolute_bounds(*view);
        REQUIRE(bounds.width > 0.0f);
        REQUIRE(bounds.height > 0.0f);
        knob_bounds.push_back(bounds);
    }

    const auto crop_rect = expanded_crop(union_bounds(knob_bounds), 18.0f, kWidth, kHeight);
    const bool full_diff_overlaps_crop = crop_intersects_diff(crop_rect, full_changed);
    REQUIRE(full_diff_overlaps_crop);
    auto live_crop = crop_png(live_png, crop_rect.x, crop_rect.y, crop_rect.width, crop_rect.height);
    auto hybrid_crop = crop_png(hybrid_png, crop_rect.x, crop_rect.y, crop_rect.width, crop_rect.height);
    REQUIRE_FALSE(live_crop.empty());
    REQUIRE_FALSE(hybrid_crop.empty());

    auto knob_result = compare_screenshots(live_crop, hybrid_crop, 32);
    REQUIRE(knob_result.valid);
    auto diff = generate_diff_image(live_crop, hybrid_crop, 32);
    REQUIRE_FALSE(diff.empty());
    auto changed = diff_bounds(live_crop, hybrid_crop, 32);

    constexpr float kKnobRegionThreshold = 0.90f;
    const bool within_threshold = knob_result.passes(kKnobRegionThreshold);
    const char* classification = within_threshold ? "within_threshold" : "classified_difference";
    const char* reason = within_threshold
        ? "native_hybrid_knob_region_matches_live_runtime_threshold"
        : "native_hybrid_layout_and_paint_still_differs_from_live_runtime_region";

    if (const char* artifact_dir = std::getenv("PULP_NATIVE_UI_PHASE_D_ARTIFACT_DIR")) {
        const fs::path dir(artifact_dir);
        write_bytes(dir / "reports" / "screenshots" / "chainer-phase-d-original-layout-hybrid.png", hybrid_png);
        write_bytes(dir / "reports" / "screenshots" / "chainer-phase-d-live-knob-region.png", live_crop);
        write_bytes(dir / "reports" / "screenshots" / "chainer-phase-d-hybrid-knob-region.png", hybrid_crop);
        write_bytes(dir / "reports" / "screenshots" / "chainer-phase-d-knob-region-diff.png", diff);

        std::ostringstream report;
        report << "{\n"
               << "  \"schema\": \"pulp-native-ui-phase-d-live-visual-v1\",\n"
               << "  \"fixture\": \"chainer-original-layout-hybrid\",\n"
               << "  \"scope\": \"native-hybrid-knob-region-vs-live-runtime\",\n"
               << "  \"threshold\": " << kKnobRegionThreshold << ",\n"
               << "  \"classification\": \"" << classification << "\",\n"
               << "  \"classification_reason\": \"" << reason << "\",\n"
               << "  \"within_threshold\": " << (within_threshold ? "true" : "false") << ",\n"
               << "  \"full_similarity\": " << std::setprecision(7) << full_result.similarity << ",\n"
               << "  \"full_mean_error\": " << full_result.mean_error << ",\n"
               << "  \"knob_region_similarity\": " << knob_result.similarity << ",\n"
               << "  \"knob_region_mean_error\": " << knob_result.mean_error << ",\n"
               << "  \"knob_region_diff_pixels\": " << knob_result.diff_pixels << ",\n"
               << "  \"knob_region_total_pixels\": " << knob_result.total_pixels << ",\n"
               << "  \"crop_rect\": {"
               << "\"x\": " << crop_rect.x << ", "
               << "\"y\": " << crop_rect.y << ", "
               << "\"width\": " << crop_rect.width << ", "
               << "\"height\": " << crop_rect.height << "},\n"
               << "  \"full_diff_bounds\": {"
               << "\"valid\": " << (full_changed.valid ? "true" : "false") << ", "
               << "\"x\": " << full_changed.x << ", "
               << "\"y\": " << full_changed.y << ", "
               << "\"width\": " << full_changed.width << ", "
               << "\"height\": " << full_changed.height << ", "
               << "\"diff_pixels\": " << full_changed.diff_pixels << "},\n"
               << "  \"full_diff_overlaps_crop\": " << (full_diff_overlaps_crop ? "true" : "false") << ",\n"
               << "  \"diff_bounds\": {"
               << "\"valid\": " << (changed.valid ? "true" : "false") << ", "
               << "\"x\": " << changed.x << ", "
               << "\"y\": " << changed.y << ", "
               << "\"width\": " << changed.width << ", "
               << "\"height\": " << changed.height << ", "
               << "\"diff_pixels\": " << changed.diff_pixels << "}\n"
               << "}\n";
        write_text(dir / "reports" / "chainer-phase-d-live-visual-report.json", report.str());
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
