#include <pulp/format/standalone.hpp>
#include <pulp/inspect/control_standalone_host.hpp>

#include <atomic>
#include <chrono>
#include <csignal>
#include <memory>
#include <thread>

namespace {

std::atomic<bool> stopping{false};

void request_stop(int) {
    stopping.store(true, std::memory_order_relaxed);
}

class InstalledControlProcessor final : public pulp::format::Processor {
  public:
    pulp::format::PluginDescriptor descriptor() const override {
        return {.name = "Pulp Control Standalone Host",
                .manufacturer = "Pulp",
                .bundle_id = "dev.pulp.control-standalone-host",
                .version = "1.0.0",
                .output_buses = {{"Output", 2}}};
    }

    void define_parameters(pulp::state::StateStore& store) override {
        store.add_parameter({.id = 1,
                             .name = "Level",
                             .range = {.min = 0.0f, .max = 1.0f, .default_value = 0.5f}});
    }

    void prepare(const pulp::format::PrepareContext&) override {}

    void process(pulp::audio::BufferView<float>& output,
                 const pulp::audio::BufferView<const float>&,
                 pulp::midi::MidiBuffer&, pulp::midi::MidiBuffer&,
                 const pulp::format::ProcessContext&) override {
        output.clear();
    }
};

std::unique_ptr<pulp::format::Processor> create_processor() {
    return std::make_unique<InstalledControlProcessor>();
}

const bool factory_installed = pulp::format::detail::install_standalone_control_host_factory(
    &pulp::inspect::make_control_standalone_host);

} // namespace

int main() {
    if (!factory_installed)
        return 64;
    std::signal(SIGINT, request_stop);
    std::signal(SIGTERM, request_stop);

    pulp::format::StandaloneApp app(&create_processor);
    pulp::format::StandaloneConfig config;
    config.persist_settings = false;
    config.headless = true;
    // The dedicated broker host has no hardware-audio product role. Reuse the
    // established no-device Standalone startup mode without opening an editor.
    config.screenshot_path = ".pulp-control-host-headless.png";
    app.set_config(config);
    if (!app.start())
        return 65;
    while (!stopping.load(std::memory_order_relaxed))
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    app.stop();
    return 0;
}
