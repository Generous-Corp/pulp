#include <pulp/format/headless.hpp>
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

} // namespace

int main() {
    pulp::format::HeadlessHost app(&create_processor);
    if (!app.valid() || app.processor() == nullptr)
        return 64;
    std::signal(SIGINT, request_stop);
    std::signal(SIGTERM, request_stop);
    auto control = pulp::inspect::make_control_standalone_host();
    if (!control || !control->start(*app.processor(), app.state()))
        return 65;
    while (!stopping.load(std::memory_order_relaxed) && control->ready())
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    control->stop();
    return 0;
}
