// A controlled Standalone product that declares vendor operations and binds
// handlers for all but one of them.

#include <pulp/format/processor.hpp>
#include <pulp/format/standalone.hpp>
#include <pulp/format/standalone_control_host.hpp>
#include <pulp/inspect/control_standalone_host.hpp>
#include <pulp/inspect/control_vendor_operation_executor.hpp>

#include <choc/text/choc_JSON.h>

#include <memory>
#include <string>

extern "C" const volatile char pulp_control_standalone_host_markers_v1[];

[[gnu::used]] const volatile char vendor_product_implementation_markers[] =
    "PULP_INSPECT_CAPABILITY_DEV_PULP_INSTANCE_READ_1_V1\0"
    "PULP_INSPECT_CAPABILITY_DEV_PULP_SESSION_CONTROL_1_V1\0"
    "PULP_INSPECT_CAPABILITY_DEV_PULP_VENDOR_READ_1_V1\0"
    "PULP_INSPECT_CAPABILITY_DEV_PULP_VENDOR_GESTURE_1_V1\0"
    "PULP_INSPECT_CAPABILITY_DEV_PULP_VENDOR_RENDER_1_V1\0"
    "PULP_INSPECT_CAPABILITY_DEV_PULP_VENDOR_DESTRUCTIVE_1_V1";

namespace {

constexpr pulp::state::ParamID kLevel = 1;

class VendorProductProcessor final : public pulp::format::Processor {
  public:
    pulp::format::PluginDescriptor descriptor() const override {
        pulp::format::PluginDescriptor d;
        d.name = "Vendor operations product fixture";
        d.manufacturer = "Example";
        d.bundle_id = "org.example.vendor-product";
        d.version = "1.0.0";
        d.output_buses = {{"Out", 1, false}};
        return d;
    }
    void define_parameters(pulp::state::StateStore& store) override {
        pulp::state::ParamInfo level;
        level.id = kLevel;
        level.name = "Level";
        level.range = {0.0f, 1.0f, 0.25f};
        store.add_parameter(level);
    }
    void prepare(const pulp::format::PrepareContext&) override {}
    void process(pulp::audio::BufferView<float>& output,
                 const pulp::audio::BufferView<const float>&, pulp::midi::MidiBuffer&,
                 pulp::midi::MidiBuffer&, const pulp::format::ProcessContext&) override {
        output.clear();
    }
    pulp::format::ViewSize view_size() const override {
        return {1, 1, 1, 1, 1, 1};
    }
};

std::string level_json(float level) {
    auto result = choc::value::createObject("");
    result.addMember("level", static_cast<double>(level));
    return choc::json::toString(result, false);
}

std::shared_ptr<const pulp::inspect::ControlVendorOperationHandlers>
vendor_operations(pulp::format::Processor&, pulp::state::StateStore& store) {
    using pulp::inspect::ControlVendorOperationCall;
    using pulp::inspect::ControlVendorOperationResult;
    auto handlers = std::make_shared<pulp::inspect::ControlVendorOperationHandlers>();
    handlers->add("dev.org.example/level.read@1", [&store](const ControlVendorOperationCall&) {
        return ControlVendorOperationResult::success(level_json(store.get_value(kLevel)));
    });
    handlers->add("dev.org.example/level.set@1", [&store](const ControlVendorOperationCall& call) {
        // The broker validated the schema; the value is still parsed as data.
        const auto arguments = choc::json::parse(call.arguments_json);
        store.set_value(kLevel, static_cast<float>(arguments["level"].getWithDefault<double>(0.0)));
        return ControlVendorOperationResult::success(level_json(store.get_value(kLevel)));
    });
    handlers->add("dev.org.example/render.ping@1", [](const ControlVendorOperationCall&) {
        return ControlVendorOperationResult::success(R"({"rendered":true})");
    });
    handlers->add("dev.org.example/preset.delete@1", [&store](const ControlVendorOperationCall&) {
        store.set_value(kLevel, 0.0f);
        return ControlVendorOperationResult::success(R"({"deleted":true})");
    });
    // Not declared, so the host drops it and the broker never routes to it.
    handlers->add("dev.org.example/level.reset@1", [&store](const ControlVendorOperationCall&) {
        store.set_value(kLevel, 0.0f);
        return ControlVendorOperationResult::success("{}");
    });
    return handlers;
}

std::unique_ptr<pulp::format::Processor> create_processor() {
    return std::make_unique<VendorProductProcessor>();
}

[[maybe_unused]] const bool control_factory_installed =
    pulp::format::detail::install_standalone_control_host_factory(
        &pulp::inspect::make_control_standalone_host);
[[maybe_unused]] const bool vendor_factory_installed =
    pulp::inspect::install_standalone_vendor_operations_factory(&vendor_operations);

} // namespace

int main() {
    if (!control_factory_installed || !vendor_factory_installed ||
        pulp_control_standalone_host_markers_v1[0] != 'P')
        return 64;
    pulp::format::StandaloneConfig config;
    config.input_channels = 0;
    config.output_channels = 1;
    config.persist_settings = false;
    config.show_settings_tab = false;
    pulp::format::StandaloneApp app(&create_processor);
    app.set_config(config);
    return app.run_with_editor(false) ? 0 : 65;
}
