#include "chainer-phase-f-hybrid.hpp"

#include <pulp/view/buttons.hpp>
#include <pulp/view/design_import.hpp>
#include <pulp/view/text_editor.hpp>
#include <pulp/view/view.hpp>
#include <pulp/view/widgets.hpp>
#include <pulp/view/window_host.hpp>

#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace {

class PreviewBindingContext final : public pulp::view::NativeImportBindingContext {
public:
    void reset() {
        choices_.clear();
        waveform_displays_.clear();
        meters_.clear();
        values_.clear();
        text_values_.clear();
        host_actions_.clear();
    }

    void bind_knob(pulp::view::Knob& knob,
                   const pulp::view::NativeImportBindingDescriptor& descriptor) override {
        const auto key = std::string(descriptor.param_key);
        values_[key] = knob.value();
        knob.on_change = [this, key](float value) {
            values_[key] = value;
        };
    }

    void bind_fader(pulp::view::Fader& fader,
                    const pulp::view::NativeImportBindingDescriptor& descriptor) override {
        const auto key = std::string(descriptor.param_key);
        values_[key] = fader.value();
        fader.on_change = [this, key](float value) {
            values_[key] = value;
        };
    }

    void bind_xy_pad(pulp::view::XYPad& pad,
                     const pulp::view::NativeImportXYPadBindingDescriptor& descriptor) override {
        const auto x_key = std::string(descriptor.x_param_key);
        const auto y_key = std::string(descriptor.y_param_key);
        values_[x_key] = pad.x_value();
        values_[y_key] = pad.y_value();
        pad.on_change = [this, x_key, y_key](float x, float y) {
            values_[x_key] = x;
            values_[y_key] = y;
        };
    }

    void bind_toggle_button(pulp::view::ToggleButton& button,
                            const pulp::view::NativeImportBindingDescriptor& descriptor) override {
        const auto key = std::string(descriptor.param_key);
        values_[key] = button.is_on() ? 1.0f : 0.0f;
        button.on_toggle = [this, key](bool on) {
            values_[key] = on ? 1.0f : 0.0f;
        };
    }

    void bind_choice_button(pulp::view::ToggleButton& button,
                            const pulp::view::NativeImportChoiceBindingDescriptor& descriptor) override {
        choices_.push_back({
            std::string(descriptor.param_key),
            std::string(descriptor.choice_value),
            &button,
        });
        if (button.is_on())
            choice_values_[std::string(descriptor.param_key)] = std::string(descriptor.choice_value);

        const auto key = std::string(descriptor.param_key);
        const auto value = std::string(descriptor.choice_value);
        button.on_toggle = [this, key, value](bool) {
            select_choice(key, value);
        };
    }

    void bind_meter(pulp::view::Meter& meter,
                    const pulp::view::NativeImportMeterBindingDescriptor& descriptor) override {
        meters_.push_back({std::string(descriptor.channel), &meter});
    }

    void bind_waveform_display(
        pulp::view::WaveformView& waveform,
        const pulp::view::NativeImportWaveformBindingDescriptor& descriptor) override {
        waveform_displays_.push_back({std::string(descriptor.param_key), &waveform});
    }

    void bind_text_editor(pulp::view::TextEditor& editor,
                          const pulp::view::NativeImportTextBindingDescriptor& descriptor) override {
        const auto key = std::string(descriptor.value_key);
        if (!key.empty())
            text_values_[key] = editor.text();
        editor.on_change = [this, key](const std::string& text) {
            if (!key.empty())
                text_values_[key] = text;
        };
    }

    void bind_host_action(pulp::view::TextButton& button,
                          const pulp::view::NativeImportHostActionDescriptor& descriptor) override {
        const auto action = std::string(descriptor.action);
        const auto payload = std::string(descriptor.payload_contract);
        host_actions_.push_back(action);
        button.on_click = [action, payload] {
            std::cerr << "native host action: " << action;
            if (!payload.empty())
                std::cerr << " payload=" << payload;
            std::cerr << "\\n";
        };
    }

    void prime_dynamic_surfaces() {
        for (auto& meter : meters_) {
            if (meter.widget == nullptr)
                continue;
            if (meter.channel == "R")
                meter.widget->set_level(0.42f, 0.50f);
            else
                meter.widget->set_level(0.78f, 0.88f);
        }
    }

private:
    struct Choice {
        std::string key;
        std::string value;
        pulp::view::ToggleButton* button = nullptr;
    };

    struct Meter {
        std::string channel;
        pulp::view::Meter* widget = nullptr;
    };

    struct WaveformDisplay {
        std::string key;
        pulp::view::WaveformView* widget = nullptr;
    };

    void select_choice(std::string_view key, std::string_view value) {
        for (auto& choice : choices_) {
            if (choice.key != key || choice.button == nullptr)
                continue;
            choice.button->set_on(choice.value == value);
        }
        choice_values_[std::string(key)] = std::string(value);
        for (auto& display : waveform_displays_) {
            if (display.key == key && display.widget != nullptr)
                display.widget->set_preview_shape(value);
        }
    }

    std::vector<Choice> choices_;
    std::vector<Meter> meters_;
    std::vector<WaveformDisplay> waveform_displays_;
    std::unordered_map<std::string, float> values_;
    std::unordered_map<std::string, std::string> choice_values_;
    std::unordered_map<std::string, std::string> text_values_;
    std::vector<std::string> host_actions_;
};

}  // namespace

int main(int argc, char** argv) {
    std::string screenshot_path;
    for (int i = 1; i < argc; ++i) {
        const std::string arg(argv[i]);
        constexpr std::string_view prefix = "--screenshot=";
        if (arg.rfind(prefix, 0) == 0) {
            screenshot_path = arg.substr(prefix.size());
        }
    }

    PreviewBindingContext binding_context;
    auto root = pulp::test::phase_f_chainer_hybrid::build_chainer_phase_f_hybrid_ui();
    if (!root) {
        std::cerr << "failed to build generated native UI\n";
        return 1;
    }

    pulp::test::phase_f_chainer_hybrid::bind_chainer_phase_f_hybrid_ui(*root, binding_context);
    binding_context.prime_dynamic_surfaces();
    root->set_requires_gpu_host(true);

    pulp::view::WindowOptions options;
    options.title = "Pulp Native UI Phase F Preview";
    options.width = 1280.0f;
    options.height = 800.0f;
    options.min_width = 900.0f;
    options.min_height = 560.0f;
    options.resizable = true;
    options.use_gpu = true;
    options.initially_hidden = !screenshot_path.empty();

    auto window = pulp::view::WindowHost::create(*root, options);
    if (!window) {
        std::cerr << "failed to create native GPU window host\n";
        return 1;
    }

    window->set_design_viewport(900.0f, 520.0f);
    window->set_fixed_aspect_ratio(900.0f / 520.0f);
    window->set_close_callback([] {});

    if (!screenshot_path.empty()) {
        int frame_count = 0;
        window->set_idle_callback([&] {
            if (++frame_count < 6)
                return;

            auto png = window->capture_back_buffer_png();
            std::ofstream out(screenshot_path, std::ios::binary);
            out.write(reinterpret_cast<const char*>(png.data()),
                      static_cast<std::streamsize>(png.size()));
            window->request_close();
        });
    }

    window->run_event_loop();
    return 0;
}
