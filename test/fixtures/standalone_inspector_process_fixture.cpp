#include <pulp/format/standalone.hpp>
#include <pulp/view/theme.hpp>
#include <pulp/view/widgets.hpp>

#include <cstdlib>
#include <memory>
#include <string>
#include <string_view>

namespace {

class InspectorProcessProcessor final : public pulp::format::Processor {
public:
    pulp::format::PluginDescriptor descriptor() const override {
        return {
            .name = "Inspector Process Fixture",
            .manufacturer = "Pulp",
            .bundle_id = "com.pulp.test.inspector-process-fixture",
            .version = "1.0.0",
            .category = pulp::format::PluginCategory::Effect,
            .input_buses = {},
            .output_buses = {{"Output", 2}},
        };
    }

    void define_parameters(pulp::state::StateStore& store) override {
        store.add_parameter({
            .id = 1,
            .name = "Gain",
            .unit = "dB",
            .range = {-18.0f, 18.0f, 0.0f},
        });
    }

    void prepare(const pulp::format::PrepareContext&) override {}

    void process(pulp::audio::BufferView<float>& output,
                 const pulp::audio::BufferView<const float>&,
                 pulp::midi::MidiBuffer&, pulp::midi::MidiBuffer&,
                 const pulp::format::ProcessContext&) override {
        output.clear();
    }

    pulp::format::ViewSize view_size() const override {
        return {320, 180, 320, 180, 320, 180};
    }

    std::unique_ptr<pulp::view::View> create_view() override {
        using namespace pulp::view;
        auto root = std::make_unique<View>();
        root->set_theme(Theme::dark());
        root->set_background_color(pulp::canvas::Color::rgba8(16, 19, 29, 255));
        root->flex().direction = FlexDirection::column;
        root->flex().padding = 16.0f;
        root->flex().gap = 12.0f;

        auto title = std::make_unique<Label>("LIVE STANDALONE INSPECTOR");
        title->set_font_size(18.0f);
        title->set_font_weight(700);
        title->flex().preferred_height = 30.0f;
        root->add_child(std::move(title));

        auto panel = std::make_unique<View>();
        panel->set_background_color(pulp::canvas::Color::rgba8(74, 126, 255, 255));
        panel->flex().preferred_height = 64.0f;
        root->add_child(std::move(panel));

        auto status = std::make_unique<Label>("Authenticated compositor capture");
        status->set_font_size(13.0f);
        status->flex().preferred_height = 24.0f;
        root->add_child(std::move(status));
        return root;
    }
};

std::unique_ptr<pulp::format::Processor> create_processor() {
    return std::make_unique<InspectorProcessProcessor>();
}

} // namespace

int main(int argc, char** argv) {
    bool dsp_only = false;
    std::string exit_screenshot;
    int frames = 180;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--dsp-only") {
            dsp_only = true;
        } else if (arg == "--exit-screenshot" && i + 1 < argc) {
            exit_screenshot = argv[++i];
        } else if (arg == "--frames" && i + 1 < argc) {
            frames = std::atoi(argv[++i]);
        }
    }

    pulp::format::StandaloneApp app(create_processor);
    pulp::format::StandaloneConfig config;
    config.sample_rate = 48'000.0;
    config.buffer_size = 256;
    config.output_channels = 2;
    config.input_channels = 0;
    config.persist_settings = false;
    app.set_config(config);

    if (dsp_only) {
        if (!app.start()) return 2;
        app.stop();
        return 0;
    }

    if (exit_screenshot.empty() || frames <= 0) return 3;
    config.screenshot_path = std::move(exit_screenshot);
    config.screenshot_frame_delay = frames;
    app.set_config(config);
    return app.run_with_editor(/*use_gpu=*/true) ? 0 : 4;
}
