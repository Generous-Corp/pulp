#include <pulp/canvas/canvas.hpp>
#include <pulp/format/processor.hpp>
#include <pulp/format/standalone.hpp>
#include <pulp/format/standalone_control_host.hpp>
#include <pulp/inspect/control_standalone_host.hpp>
#include <pulp/view/view.hpp>

#include <cstdint>
#include <cstdlib>
#include <memory>

extern "C" const volatile char pulp_control_standalone_host_markers_v1[];

namespace {

class EvidenceView final : public pulp::view::View {
  public:
    void paint(pulp::canvas::Canvas& canvas) override {
        const auto bounds = this->bounds();
        canvas.set_fill_color(pulp::canvas::Color::rgba8(18, 24, 38));
        canvas.fill_rect(0.0f, 0.0f, bounds.width, bounds.height);
        canvas.set_fill_color(pulp::canvas::Color::rgba8(38, 198, 218));
        canvas.fill_rect(24.0f, 24.0f, bounds.width - 48.0f, 56.0f);
        canvas.set_fill_color(pulp::canvas::Color::rgba8(255, 183, 77));
        canvas.fill_rect(48.0f, 104.0f, bounds.width - 96.0f, 52.0f);
        for (std::uint8_t index = 0; index < 16; ++index) {
            canvas.set_fill_color(
                pulp::canvas::Color::rgba8(static_cast<std::uint8_t>(32 + index * 12),
                                           static_cast<std::uint8_t>(224 - index * 9),
                                           static_cast<std::uint8_t>(80 + index * 7)));
            canvas.fill_rect(52.0f + static_cast<float>(index) * 13.5f, 120.0f, 10.0f, 20.0f);
        }
    }
};

class EvidenceProcessor final : public pulp::format::Processor {
  public:
    pulp::format::PluginDescriptor descriptor() const override {
        return {.name = "Pulp GPU Health Standalone Product Fixture",
                .manufacturer = "Pulp",
                .bundle_id = "dev.pulp.test.gpu-health-standalone-product",
                .version = "1.0.0",
                .output_buses = {{"Output", 2}}};
    }

    void define_parameters(pulp::state::StateStore&) override {}
    void prepare(const pulp::format::PrepareContext&) override {}

    void process(pulp::audio::BufferView<float>& output,
                 const pulp::audio::BufferView<const float>&, pulp::midi::MidiBuffer&,
                 pulp::midi::MidiBuffer&, const pulp::format::ProcessContext&) override {
        output.clear();
    }

    pulp::format::ViewSize view_size() const override {
        return {320, 180, 320, 180, 320, 180};
    }

    std::unique_ptr<pulp::view::View> create_view() override {
        return std::make_unique<EvidenceView>();
    }
};

std::unique_ptr<pulp::format::Processor> create_processor() {
    return std::make_unique<EvidenceProcessor>();
}

[[maybe_unused]] const bool control_factory_installed =
    pulp::format::detail::install_standalone_control_host_factory(
        &pulp::inspect::make_control_standalone_host);

} // namespace

int main() {
    const auto* screenshot_path = std::getenv("PULP_A3_PRODUCT_SCREENSHOT");
    if (!screenshot_path || screenshot_path[0] == '\0' || !control_factory_installed ||
        pulp_control_standalone_host_markers_v1[0] != 'P')
        return 64;

    pulp::format::StandaloneConfig config;
    config.input_channels = 0;
    config.output_channels = 2;
    config.persist_settings = false;
    config.show_settings_tab = false;
    config.screenshot_path = screenshot_path;
    // The delayed capture keeps the product alive long enough for broker
    // enrollment and supplies a deterministic safety exit without audio I/O.
    config.screenshot_frame_delay = 300;

    pulp::format::StandaloneApp app(&create_processor);
    app.set_config(config);
    const bool use_gpu = std::getenv("PULP_A3_PRODUCT_DISABLE_GPU") == nullptr;
    return app.run_with_editor(use_gpu) ? 0 : 65;
}
