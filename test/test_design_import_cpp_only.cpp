#include "chainer-phase-f-hybrid.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <pulp/view/buttons.hpp>
#include <pulp/view/design_import.hpp>
#include <pulp/view/screenshot.hpp>
#include <pulp/view/screenshot_compare.hpp>
#include <pulp/view/text_editor.hpp>
#include <pulp/view/view.hpp>
#include <pulp/view/widgets.hpp>

#include <algorithm>
#include <cstdint>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#ifndef PULP_UI_RUNTIME_CPP_ONLY
#error "pulp-test-design-import-cpp-only must compile with PULP_UI_RUNTIME_CPP_ONLY"
#endif

#ifndef PULP_REPO_ROOT
#define PULP_REPO_ROOT ""
#endif

using namespace pulp::view;
namespace fs = std::filesystem;

namespace {

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

const std::vector<const char*>& required_param_keys() {
    static const std::vector<const char*> keys = {
        "osc_freq",
        "osc_detune",
        "osc_shape",
        "osc_waveform",
        "env_a",
        "env_d",
        "env_s",
        "env_r",
        "xover_lo",
        "xover_hi",
        "ms_mid_width",
        "ms_side_width",
        "filt_x",
        "filt_y",
        "send_level",
        "return_level",
        "master_out",
        "mid_bypass",
        "side_bypass",
        "selected_mod",
    };
    return keys;
}

class CppOnlyBindingContext final : public NativeImportBindingContext {
public:
    void bind_knob(Knob& knob, const NativeImportBindingDescriptor& descriptor) override {
        const auto key = std::string(descriptor.param_key);
        bound_params_.insert(key);
        knobs_.push_back({key, std::string(descriptor.route_id), knob.anchor_id(), &knob});
        values_[key] = knob.value();
        knob.on_gesture_begin = [this, key] { ++gesture_begin_count_[key]; };
        knob.on_change = [this, key](float value) { record_param_change(key, value); };
        knob.on_gesture_end = [this, key] { ++gesture_end_count_[key]; };
    }

    void bind_fader(Fader& fader, const NativeImportBindingDescriptor& descriptor) override {
        const auto key = std::string(descriptor.param_key);
        bound_params_.insert(key);
        faders_.push_back({key, std::string(descriptor.route_id), fader.anchor_id(), &fader});
        values_[key] = fader.value();
        fader.on_gesture_begin = [this, key] { ++gesture_begin_count_[key]; };
        fader.on_change = [this, key](float value) { record_param_change(key, value); };
        fader.on_gesture_end = [this, key] { ++gesture_end_count_[key]; };
    }

    void bind_xy_pad(XYPad& pad, const NativeImportXYPadBindingDescriptor& descriptor) override {
        const auto x_key = std::string(descriptor.x_param_key);
        const auto y_key = std::string(descriptor.y_param_key);
        bound_params_.insert(x_key);
        bound_params_.insert(y_key);
        xy_pads_.push_back({
            x_key,
            y_key,
            std::string(descriptor.route_id),
            pad.anchor_id(),
            &pad,
        });
        values_[x_key] = pad.x_value();
        values_[y_key] = pad.y_value();
        pad.on_gesture_begin = [this, x_key, y_key] {
            ++gesture_begin_count_[x_key];
            ++gesture_begin_count_[y_key];
        };
        pad.on_change = [this, x_key, y_key](float x, float y) {
            record_param_change(x_key, x);
            record_param_change(y_key, y);
        };
        pad.on_gesture_end = [this, x_key, y_key] {
            ++gesture_end_count_[x_key];
            ++gesture_end_count_[y_key];
        };
    }

    void bind_toggle_button(ToggleButton& button, const NativeImportBindingDescriptor& descriptor) override {
        const auto key = std::string(descriptor.param_key);
        bound_params_.insert(key);
        toggles_.push_back({key, std::string(descriptor.route_id), button.anchor_id(), &button});
        values_[key] = button.is_on() ? 1.0f : 0.0f;
        button.on_toggle = [this, key](bool on) {
            record_param_change(key, on ? 1.0f : 0.0f);
        };
    }

    void bind_choice_button(ToggleButton& button, const NativeImportChoiceBindingDescriptor& descriptor) override {
        const auto key = std::string(descriptor.param_key);
        bound_params_.insert(key);
        choices_.push_back({
            key,
            std::string(descriptor.choice_value),
            std::string(descriptor.route_id),
            button.anchor_id(),
            &button,
        });
        if (button.is_on())
            choice_values_[key] = std::string(descriptor.choice_value);
        button.on_toggle = [this, key, value = std::string(descriptor.choice_value)](bool) {
            select_choice(key, value);
        };
    }

    void bind_meter(Meter& meter, const NativeImportMeterBindingDescriptor& descriptor) override {
        meters_.push_back({
            std::string(descriptor.meter_source),
            std::string(descriptor.channel),
            std::string(descriptor.route_id),
            meter.anchor_id(),
            &meter,
        });
    }

    void bind_waveform_display(WaveformView& waveform,
                               const NativeImportWaveformBindingDescriptor& descriptor) override {
        bound_params_.insert(std::string(descriptor.param_key));
        waveform_displays_.push_back({
            std::string(descriptor.param_key),
            std::string(descriptor.route_id),
            waveform.anchor_id(),
            &waveform,
        });
    }

    void bind_text_editor(TextEditor& editor, const NativeImportTextBindingDescriptor& descriptor) override {
        text_inputs_.push_back({
            std::string(descriptor.value_key),
            std::string(descriptor.route_id),
            editor.anchor_id(),
            &editor,
        });
        editor.on_change = [this, key = std::string(descriptor.value_key)](const std::string& text) {
            text_values_[key] = text;
            ++text_change_count_;
        };
    }

    void bind_host_action(TextButton& button, const NativeImportHostActionDescriptor& descriptor) override {
        host_actions_.push_back({
            std::string(descriptor.action),
            std::string(descriptor.route_id),
            button.anchor_id(),
            &button,
        });
        button.on_click = [this, action = std::string(descriptor.action)] {
            host_action_events_.push_back(action);
        };
    }

    struct KnobBinding {
        std::string param_key;
        std::string route_id;
        std::string anchor;
        Knob* widget = nullptr;
    };
    struct FaderBinding {
        std::string param_key;
        std::string route_id;
        std::string anchor;
        Fader* widget = nullptr;
    };
    struct XYPadBinding {
        std::string x_param_key;
        std::string y_param_key;
        std::string route_id;
        std::string anchor;
        XYPad* widget = nullptr;
    };
    struct ToggleBinding {
        std::string param_key;
        std::string route_id;
        std::string anchor;
        ToggleButton* widget = nullptr;
    };
    struct ChoiceBinding {
        std::string param_key;
        std::string choice_value;
        std::string route_id;
        std::string anchor;
        ToggleButton* widget = nullptr;
    };
    struct MeterBinding {
        std::string meter_source;
        std::string channel;
        std::string route_id;
        std::string anchor;
        Meter* widget = nullptr;
    };
    struct WaveformBinding {
        std::string param_key;
        std::string route_id;
        std::string anchor;
        WaveformView* widget = nullptr;
    };
    struct TextBinding {
        std::string value_key;
        std::string route_id;
        std::string anchor;
        TextEditor* widget = nullptr;
    };
    struct HostActionBinding {
        std::string action;
        std::string route_id;
        std::string anchor;
        TextButton* widget = nullptr;
    };

    const std::vector<KnobBinding>& knobs() const { return knobs_; }
    const std::vector<FaderBinding>& faders() const { return faders_; }
    const std::vector<XYPadBinding>& xy_pads() const { return xy_pads_; }
    const std::vector<ToggleBinding>& toggles() const { return toggles_; }
    const std::vector<ChoiceBinding>& choices() const { return choices_; }
    const std::vector<MeterBinding>& meters() const { return meters_; }
    const std::vector<WaveformBinding>& waveform_displays() const { return waveform_displays_; }
    const std::vector<TextBinding>& text_inputs() const { return text_inputs_; }
    const std::vector<HostActionBinding>& host_actions() const { return host_actions_; }
    const std::set<std::string>& bound_params() const { return bound_params_; }

    int param_change_count(std::string_view key) const {
        auto found = param_change_count_.find(std::string(key));
        return found == param_change_count_.end() ? 0 : found->second;
    }

    bool has_gesture_pair(std::string_view key) const {
        const auto owned = std::string(key);
        const auto begin = gesture_begin_count_.find(owned);
        const auto end = gesture_end_count_.find(owned);
        return begin != gesture_begin_count_.end() &&
               end != gesture_end_count_.end() &&
               begin->second > 0 &&
               end->second > 0;
    }

    void set_meter_level(std::string_view meter_source, std::string_view channel, float rms, float peak) {
        for (const auto& meter : meters_) {
            if (meter.meter_source == meter_source && meter.channel == channel && meter.widget != nullptr) {
                meter.widget->set_level(rms, peak);
                ++meter_update_count_;
                return;
            }
        }
        FAIL("meter binding not found");
    }

    int text_change_count() const { return text_change_count_; }
    int host_action_event_count() const { return static_cast<int>(host_action_events_.size()); }
    int meter_update_count() const { return meter_update_count_; }
    int choice_change_count() const { return choice_change_count_; }
    int parameter_event_count() const { return parameter_event_count_; }

private:
    void record_param_change(std::string_view key, float value) {
        values_[std::string(key)] = value;
        ++param_change_count_[std::string(key)];
        ++parameter_event_count_;
    }

    void select_choice(std::string_view key, std::string_view value) {
        for (auto& choice : choices_) {
            if (choice.param_key == key && choice.widget != nullptr)
                choice.widget->set_on(choice.choice_value == value);
        }
        choice_values_[std::string(key)] = std::string(value);
        ++choice_change_count_;
        for (auto& display : waveform_displays_) {
            if (display.param_key == key && display.widget != nullptr)
                display.widget->set_preview_shape(value);
        }
    }

    std::vector<KnobBinding> knobs_;
    std::vector<FaderBinding> faders_;
    std::vector<XYPadBinding> xy_pads_;
    std::vector<ToggleBinding> toggles_;
    std::vector<ChoiceBinding> choices_;
    std::vector<MeterBinding> meters_;
    std::vector<WaveformBinding> waveform_displays_;
    std::vector<TextBinding> text_inputs_;
    std::vector<HostActionBinding> host_actions_;
    std::set<std::string> bound_params_;
    std::unordered_map<std::string, float> values_;
    std::unordered_map<std::string, int> param_change_count_;
    std::unordered_map<std::string, int> gesture_begin_count_;
    std::unordered_map<std::string, int> gesture_end_count_;
    std::unordered_map<std::string, std::string> choice_values_;
    std::unordered_map<std::string, std::string> text_values_;
    std::vector<std::string> host_action_events_;
    int parameter_event_count_ = 0;
    int choice_change_count_ = 0;
    int text_change_count_ = 0;
    int meter_update_count_ = 0;
};

std::vector<Rect> routed_bounds_for(const CppOnlyBindingContext& ctx) {
    std::vector<Rect> out;
    auto append = [&out](const auto& rows) {
        for (const auto& row : rows) {
            if (row.widget == nullptr)
                continue;
            const auto bounds = absolute_bounds(*row.widget);
            if (bounds.width > 0.0f && bounds.height > 0.0f)
                out.push_back(bounds);
        }
    };
    append(ctx.knobs());
    append(ctx.faders());
    append(ctx.xy_pads());
    append(ctx.toggles());
    append(ctx.choices());
    append(ctx.meters());
    append(ctx.waveform_displays());
    append(ctx.text_inputs());
    append(ctx.host_actions());
    return out;
}

int exercise_behavior(View& root, CppOnlyBindingContext& ctx) {
    int interactions = 0;

    for (const auto& row : ctx.knobs()) {
        REQUIRE(row.widget != nullptr);
        const auto bounds = absolute_bounds(*row.widget);
        const auto before = row.widget->value();
        const Point start{bounds.x + bounds.width * 0.5f, bounds.y + bounds.height * 0.5f};
        const Point end{start.x, start.y - 48.0f};
        root.simulate_drag(start, end, 6);
        REQUIRE(row.widget->value() != Catch::Approx(before));
        REQUIRE(ctx.param_change_count(row.param_key) > 0);
        REQUIRE(ctx.has_gesture_pair(row.param_key));
        ++interactions;
    }

    for (const auto& row : ctx.faders()) {
        REQUIRE(row.widget != nullptr);
        const auto bounds = absolute_bounds(*row.widget);
        const auto before = row.widget->value();
        const Point start{bounds.x + bounds.width * 0.5f, bounds.y + bounds.height - 4.0f};
        const Point end{start.x, bounds.y + 4.0f};
        root.simulate_drag(start, end, 6);
        REQUIRE(row.widget->value() != Catch::Approx(before));
        REQUIRE(ctx.param_change_count(row.param_key) > 0);
        REQUIRE(ctx.has_gesture_pair(row.param_key));
        ++interactions;
    }

    for (const auto& row : ctx.xy_pads()) {
        REQUIRE(row.widget != nullptr);
        const auto bounds = absolute_bounds(*row.widget);
        const auto before_x = row.widget->x_value();
        const auto before_y = row.widget->y_value();
        const Point start{bounds.x + bounds.width * 0.2f, bounds.y + bounds.height * 0.8f};
        const Point end{bounds.x + bounds.width * 0.82f, bounds.y + bounds.height * 0.24f};
        root.simulate_drag(start, end, 6);
        REQUIRE(row.widget->x_value() != Catch::Approx(before_x));
        REQUIRE(row.widget->y_value() != Catch::Approx(before_y));
        REQUIRE(ctx.param_change_count(row.x_param_key) > 0);
        REQUIRE(ctx.param_change_count(row.y_param_key) > 0);
        REQUIRE(ctx.has_gesture_pair(row.x_param_key));
        REQUIRE(ctx.has_gesture_pair(row.y_param_key));
        ++interactions;
    }

    for (const auto& row : ctx.toggles()) {
        REQUIRE(row.widget != nullptr);
        const auto bounds = absolute_bounds(*row.widget);
        root.simulate_click({bounds.x + bounds.width * 0.5f, bounds.y + bounds.height * 0.5f});
        REQUIRE(ctx.param_change_count(row.param_key) > 0);
        ++interactions;
    }

    for (const auto& row : ctx.choices()) {
        REQUIRE(row.widget != nullptr);
        const auto bounds = absolute_bounds(*row.widget);
        root.simulate_click({bounds.x + bounds.width * 0.5f, bounds.y + bounds.height * 0.5f});
        ++interactions;
    }
    REQUIRE(ctx.choice_change_count() >= static_cast<int>(ctx.choices().size()));

    ctx.set_meter_level("output", "L", 0.95f, 1.0f);
    ctx.set_meter_level("output", "R", 0.51f, 0.62f);
    REQUIRE(ctx.meter_update_count() == 2);
    interactions += ctx.meter_update_count();

    REQUIRE(ctx.text_inputs().size() == 1);
    REQUIRE(ctx.text_inputs()[0].widget != nullptr);
    ctx.text_inputs()[0].widget->set_text("polywave_ms_split_cpp_only");
    REQUIRE(ctx.text_change_count() == 1);
    ++interactions;

    for (const auto& row : ctx.host_actions()) {
        REQUIRE(row.widget != nullptr);
        const auto bounds = absolute_bounds(*row.widget);
        root.simulate_click({bounds.x + bounds.width * 0.5f, bounds.y + bounds.height * 0.5f});
        ++interactions;
    }
    REQUIRE(ctx.host_action_event_count() == static_cast<int>(ctx.host_actions().size()));

    return interactions;
}

}  // namespace

TEST_CASE("Chainer generated C++ runs as a cpp-only eligible fixture",
          "[view][import][cpp-codegen][native-cpp-phase-g][cpp-only]") {
    constexpr uint32_t kWidth = 1280;
    constexpr uint32_t kHeight = 800;
    constexpr float kThreshold = 0.90f;

    const fs::path live_png_path =
        fs::path(PULP_REPO_ROOT) / "planning/artifacts/native-ui/nv0/reports/screenshots/chainer-live-coregraphics-1280x800.png";
    REQUIRE(fs::exists(live_png_path));

    auto root = pulp::test::phase_f_chainer_hybrid::build_chainer_phase_f_hybrid_ui();
    REQUIRE(root != nullptr);

    CppOnlyBindingContext ctx;
    pulp::test::phase_f_chainer_hybrid::bind_chainer_phase_f_hybrid_ui(*root, ctx);

    REQUIRE(ctx.knobs().size() == 8);
    REQUIRE(ctx.faders().size() == 6);
    REQUIRE(ctx.xy_pads().size() == 1);
    REQUIRE(ctx.toggles().size() == 2);
    REQUIRE(ctx.choices().size() == 21);
    REQUIRE(ctx.meters().size() == 2);
    REQUIRE(ctx.waveform_displays().size() == 1);
    REQUIRE(ctx.text_inputs().size() == 1);
    REQUIRE(ctx.host_actions().size() == 2);
    REQUIRE(ctx.bound_params().size() == required_param_keys().size());
    for (const auto* key : required_param_keys())
        REQUIRE(ctx.bound_params().count(key) == 1);

    root->set_bounds({0.0f, 0.0f, static_cast<float>(kWidth), static_cast<float>(kHeight)});
    root->layout_children();

    const auto live_png = read_bytes(live_png_path);
    auto cpp_only_png = render_to_png(*root, kWidth, kHeight, 1.0f);
    if (cpp_only_png.empty())
        SKIP("native screenshot renderer unavailable for cpp-only Phase G visual gate");
    REQUIRE_FALSE(live_png.empty());

    auto full_result = compare_screenshots(live_png, cpp_only_png, 32);
    REQUIRE(full_result.valid);
    auto diff_png = generate_diff_image(live_png, cpp_only_png, 32);
    REQUIRE_FALSE(diff_png.empty());

    const auto routed_bounds = routed_bounds_for(ctx);
    REQUIRE_FALSE(routed_bounds.empty());
    const auto crop_rect = expanded_crop(union_bounds(routed_bounds), 18.0f, kWidth, kHeight);
    auto live_crop = crop_png(live_png, crop_rect.x, crop_rect.y, crop_rect.width, crop_rect.height);
    auto cpp_only_crop = crop_png(cpp_only_png, crop_rect.x, crop_rect.y, crop_rect.width, crop_rect.height);
    REQUIRE_FALSE(live_crop.empty());
    REQUIRE_FALSE(cpp_only_crop.empty());
    auto routed_result = compare_screenshots(live_crop, cpp_only_crop, 32);
    REQUIRE(routed_result.valid);
    auto routed_diff_png = generate_diff_image(live_crop, cpp_only_crop, 32);
    REQUIRE_FALSE(routed_diff_png.empty());

    const bool full_within_threshold = full_result.passes(kThreshold);
    const bool routed_within_threshold = routed_result.passes(kThreshold);
    REQUIRE(routed_within_threshold);

    const int interaction_count = exercise_behavior(*root, ctx);
    REQUIRE(interaction_count >= 43);
    REQUIRE(ctx.parameter_event_count() > 0);

    if (const char* artifact_dir = std::getenv("PULP_NATIVE_UI_PHASE_D_ARTIFACT_DIR")) {
        const fs::path dir(artifact_dir);
        write_bytes(dir / "reports" / "screenshots" / "chainer-phase-g-cpp-only.png", cpp_only_png);
        write_bytes(dir / "reports" / "screenshots" / "chainer-phase-g-cpp-only-full-diff.png", diff_png);
        write_bytes(dir / "reports" / "screenshots" / "chainer-phase-g-cpp-only-routed-region.png", cpp_only_crop);
        write_bytes(dir / "reports" / "screenshots" / "chainer-phase-g-cpp-only-routed-region-diff.png",
                    routed_diff_png);

        std::ostringstream report;
        report << "{\n"
               << "  \"schema\": \"pulp-native-ui-phase-g-cpp-only-behavior-visual-v1\",\n"
               << "  \"fixture\": \"chainer-phase-g-cpp-only-eligible\",\n"
               << "  \"scope\": \"view-core-only-generated-cpp-vs-live-runtime\",\n"
               << "  \"compiled_with_cpp_only_flag\": true,\n"
               << "  \"target_links_script_runtime\": false,\n"
               << "  \"threshold\": " << kThreshold << ",\n"
               << "  \"classification\": \""
               << (routed_within_threshold ? "within_threshold" : "classified_difference") << "\",\n"
               << "  \"within_threshold\": " << (routed_within_threshold ? "true" : "false") << ",\n"
               << "  \"full_within_threshold\": " << (full_within_threshold ? "true" : "false") << ",\n"
               << "  \"full_similarity\": " << full_result.similarity << ",\n"
               << "  \"full_mean_error\": " << full_result.mean_error << ",\n"
               << "  \"full_diff_pixels\": " << full_result.diff_pixels << ",\n"
               << "  \"routed_region_similarity\": " << routed_result.similarity << ",\n"
               << "  \"routed_region_mean_error\": " << routed_result.mean_error << ",\n"
               << "  \"routed_region_diff_pixels\": " << routed_result.diff_pixels << ",\n"
               << "  \"crop_rect\": {"
               << "\"x\": " << crop_rect.x << ", "
               << "\"y\": " << crop_rect.y << ", "
               << "\"width\": " << crop_rect.width << ", "
               << "\"height\": " << crop_rect.height << "},\n"
               << "  \"bound_counts\": {"
               << "\"knobs\": " << ctx.knobs().size() << ", "
               << "\"faders\": " << ctx.faders().size() << ", "
               << "\"xy_pads\": " << ctx.xy_pads().size() << ", "
               << "\"toggles\": " << ctx.toggles().size() << ", "
               << "\"choices\": " << ctx.choices().size() << ", "
               << "\"meters\": " << ctx.meters().size() << ", "
               << "\"waveform_displays\": " << ctx.waveform_displays().size() << ", "
               << "\"text_inputs\": " << ctx.text_inputs().size() << ", "
               << "\"host_actions\": " << ctx.host_actions().size() << ", "
               << "\"param_keys\": " << ctx.bound_params().size() << "},\n"
               << "  \"behavior\": {"
               << "\"passed\": true, "
               << "\"interaction_count\": " << interaction_count << ", "
               << "\"parameter_event_count\": " << ctx.parameter_event_count() << ", "
               << "\"choice_change_count\": " << ctx.choice_change_count() << ", "
               << "\"meter_update_count\": " << ctx.meter_update_count() << ", "
               << "\"text_change_count\": " << ctx.text_change_count() << ", "
               << "\"host_action_event_count\": " << ctx.host_action_event_count() << "},\n"
               << "  \"scope_boundaries\": [\n"
               << "    \"proves the checked-in generated C++ view tree can build, render, and handle routed interactions through a view-core-only target\",\n"
               << "    \"does not claim arbitrary imported designs are cpp-only eligible\",\n"
               << "    \"does not remove the live runtime safety net for unsupported imports\"\n"
               << "  ]\n"
               << "}\n";
        write_text(dir / "reports" / "chainer-phase-g-cpp-only-behavior-visual-report.json",
                   report.str());
    }
}
