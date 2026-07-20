#include "support/timeline_graph_binding_test_support.hpp"

#ifdef PULP_TEST_CLAP_PATH
#include <filesystem>
#endif

TEST_CASE("timeline graph binding delivers attached automation to mapped plugins") {
    const auto map = tempo_map();
    ProgramHarness programs;
    programs.publish(automation_project(*map), map,
                     take(DecodedAudioAssetPool::create({})), 1);
    auto program = programs.store.read();
    REQUIRE(program);
    SignalGraph graph;
    graph.set_anticipation_enabled(true);
    const auto output_node = graph.add_output_node(1);
    auto recorder = std::make_unique<AutomationRecordingSlot>();
    auto* observed = recorder.get();
    const auto plugin = graph.add_plugin_node(std::move(recorder), 1, 1);
    REQUIRE(graph.connect(plugin, 0, output_node, 0));
    REQUIRE(graph.prepare(48'000.0, 64));
    const std::array devices{TimelineDeviceGraphRoute{{20}, plugin}};
    const std::array routes{TimelineTrackGraphRoute{
        {10}, output_node, 0, 0, devices}};
    TimelineGraphPlaybackBinding binding(graph, programs.store);
    const auto admission = binding.prepare_quiesced(
        *program, routes, config(1), 48'000.0, 64);
    INFO("admission code " << static_cast<int>(admission.code)
         << " actual " << admission.actual);
    REQUIRE(admission);
    REQUIRE_FALSE(graph.inject_parameter_events(plugin, ParameterEventQueue{}));

    Buffer input(1, 32);
    Buffer output(1, 32, 1.0f);
    auto output_view = output.view();
    const auto result = binding.process(
        output_view, input.const_view(), snapshot(*program, 32));
    REQUIRE(result);
    REQUIRE(result.emitted_automation_events > 0);
    REQUIRE(observed->process_count == 1);
    REQUIRE(observed->event_count == result.emitted_automation_events);
    REQUIRE(observed->received[0].param_id == 7);
    REQUIRE(observed->received[0].sample_offset == 0);
    REQUIRE(observed->received[0].value == 0.25f);
    REQUIRE(std::all_of(
        observed->received.begin(), observed->received.begin() + observed->event_count,
        [](const auto& event) {
            return event.ramp_duration_sample_frames == 0;
        }));
    REQUIRE(std::any_of(
        observed->received.begin(), observed->received.begin() + observed->event_count,
        [](const auto& event) {
            return event.sample_offset == 16 && event.value == 0.75f;
        }));
}
#ifdef PULP_TEST_CLAP_PATH
TEST_CASE("timeline graph binding automates the hosted PulpGain CLAP") {
    namespace fs = std::filesystem;
    const fs::path path = PULP_TEST_CLAP_PATH;
    if (!fs::exists(path)) {
        WARN("CLAP test plugin not built at " << path << " - skipping");
        return;
    }
    PluginInfo info;
    info.name = "PulpGain";
    info.path = path.string();
    info.format = PluginFormat::CLAP;
    auto slot = PluginSlot::load(info);
    REQUIRE(slot);
    REQUIRE(slot->is_loaded());
    auto* gain = slot.get();

    const auto map = tempo_map();
    ProgramHarness programs;
    programs.publish(automation_project(*map, 0.25f, 0.75f, 2), map,
                     take(DecodedAudioAssetPool::create({})), 1);
    auto program = programs.store.read();
    SignalGraph graph;
    const auto input_node = graph.add_input_node(2);
    const auto output = graph.add_output_node(2);
    const auto plugin = graph.add_plugin_node(std::move(slot), 2, 2);
    REQUIRE(graph.connect(input_node, 0, plugin, 0));
    REQUIRE(graph.connect(input_node, 1, plugin, 1));
    REQUIRE(graph.connect(plugin, 0, output, 0));
    REQUIRE(graph.connect(plugin, 1, output, 1));
    REQUIRE(graph.prepare(48'000.0, 64));
    const std::array devices{TimelineDeviceGraphRoute{{20}, plugin}};
    const std::array routes{TimelineTrackGraphRoute{{10}, output, 0, 0, devices}};
    TimelineGraphPlaybackBinding binding(graph, programs.store);
    REQUIRE(binding.prepare(*program, routes, config(2), 48'000.0, 64));
    Buffer input(2, 32, 1.0f);
    Buffer output_buffer(2, 32);
    auto output_view = output_buffer.view();
    REQUIRE(binding.process(output_view, input.const_view(),
                            snapshot(*program, 32)));
    REQUIRE(std::abs(output_buffer.storage[0][8]
                     - std::pow(10.0f, 0.75f / 20.0f)) < 1.0e-4f);
    REQUIRE(std::abs(output_buffer.storage[0][24]
                     - std::pow(10.0f, 0.75f / 20.0f)) < 1.0e-4f);
    REQUIRE(std::abs(gain->get_parameter(2) - 0.75f) < 1.0e-4f);
}
#endif

TEST_CASE("timeline graph binding rejects invalid automation routes and parameters") {
    const auto map = tempo_map();
    ProgramHarness programs;
    programs.publish(automation_project(*map), map,
                     take(DecodedAudioAssetPool::create({})), 1);
    auto program = programs.store.read();
    REQUIRE(program);
    SignalGraph graph;
    const auto output_node = graph.add_output_node(1);
    auto recorder = std::make_unique<AutomationRecordingSlot>();
    const auto plugin = graph.add_plugin_node(std::move(recorder), 1, 1);
    REQUIRE(graph.prepare(48'000.0, 64));
    TimelineGraphPlaybackBinding binding(graph, programs.store);

    SECTION("missing placement") {
        const std::array routes{TimelineTrackGraphRoute{{10}, output_node, 0, 0}};
        REQUIRE(binding.preflight(*program, routes, config(1), 64).code ==
                TimelineGraphAdmissionCode::MissingDevicePlacement);
    }
    SECTION("missing node") {
        const std::array devices{TimelineDeviceGraphRoute{{20}, 999'999}};
        const std::array routes{TimelineTrackGraphRoute{
            {10}, output_node, 0, 0, devices}};
        REQUIRE(binding.preflight(*program, routes, config(1), 64).code ==
                TimelineGraphAdmissionCode::MissingDeviceNode);
    }
    SECTION("non plugin node") {
        const std::array devices{TimelineDeviceGraphRoute{{20}, output_node}};
        const std::array routes{TimelineTrackGraphRoute{
            {10}, output_node, 0, 0, devices}};
        REQUIRE(binding.preflight(*program, routes, config(1), 64).code ==
                TimelineGraphAdmissionCode::DeviceNodeNotPlugin);
    }
    SECTION("unknown parameter") {
        auto other = std::make_unique<AutomationRecordingSlot>(8);
        const auto other_node = graph.add_plugin_node(std::move(other), 1, 1);
        const std::array devices{TimelineDeviceGraphRoute{{20}, other_node}};
        const std::array routes{TimelineTrackGraphRoute{
            {10}, output_node, 0, 0, devices}};
        REQUIRE(binding.preflight(*program, routes, config(1), 64).code ==
                TimelineGraphAdmissionCode::UnknownAutomationParameter);
    }
    SECTION("read only parameter") {
        ParamFlags flags;
        flags.read_only = true;
        auto other = std::make_unique<AutomationRecordingSlot>(7, flags);
        const auto other_node = graph.add_plugin_node(std::move(other), 1, 1);
        const std::array devices{TimelineDeviceGraphRoute{{20}, other_node}};
        const std::array routes{TimelineTrackGraphRoute{
            {10}, output_node, 0, 0, devices}};
        REQUIRE(binding.preflight(*program, routes, config(1), 64).code ==
                TimelineGraphAdmissionCode::ReadOnlyAutomationParameter);
    }
    SECTION("non automatable parameter") {
        ParamFlags flags;
        flags.automatable = false;
        auto other = std::make_unique<AutomationRecordingSlot>(7, flags);
        const auto other_node = graph.add_plugin_node(std::move(other), 1, 1);
        const std::array devices{TimelineDeviceGraphRoute{{20}, other_node}};
        const std::array routes{TimelineTrackGraphRoute{
            {10}, output_node, 0, 0, devices}};
        REQUIRE(binding.preflight(*program, routes, config(1), 64).code ==
                TimelineGraphAdmissionCode::NonAutomatableAutomationParameter);
    }
    (void)plugin;
}

TEST_CASE("timeline automation claims are owner scoped and released") {
    const auto map = tempo_map();
    ProgramHarness programs;
    programs.publish(automation_project(*map), map,
                     take(DecodedAudioAssetPool::create({})), 1);
    auto program = programs.store.read();
    REQUIRE(program);
    SignalGraph graph;
    const auto output = graph.add_output_node(1);
    auto recorder = std::make_unique<AutomationRecordingSlot>();
    auto* observed = recorder.get();
    const auto plugin = graph.add_plugin_node(std::move(recorder), 1, 1);
    REQUIRE(graph.prepare(48'000.0, 64));
    ParameterEventQueue pending_live;
    REQUIRE(pending_live.push({7, 0, 0.99f, 0}));
    REQUIRE(graph.inject_parameter_events(plugin, pending_live));
    const std::array devices{TimelineDeviceGraphRoute{{20}, plugin}};
    const std::array routes{TimelineTrackGraphRoute{{10}, output, 0, 0, devices}};

    {
        TimelineGraphPlaybackBinding first(graph, programs.store);
        REQUIRE(first.prepare(*program, routes, config(1), 48'000.0, 64));
        REQUIRE_FALSE(graph.inject_parameter_events(plugin, pending_live));
        TimelineGraphPlaybackBinding competing(graph, programs.store);
        REQUIRE(competing.prepare(*program, routes, config(1), 48'000.0, 64).code ==
                TimelineGraphAdmissionCode::DuplicateDeviceNodeOwnership);
        Buffer input(1, 32);
        Buffer output_buffer(1, 32);
        auto output_view = output_buffer.view();
        REQUIRE(first.process(output_view, input.const_view(),
                              snapshot(*program, 32)));
        REQUIRE(std::none_of(
            observed->received.begin(),
            observed->received.begin() + observed->event_count,
            [](const auto& event) { return event.value == 0.99f; }));
    }
    REQUIRE(graph.inject_parameter_events(plugin, pending_live));
}

TEST_CASE("timeline automation claims coexist on disjoint plugin nodes") {
    const auto map = tempo_map();
    ProgramHarness programs;
    programs.publish(automation_project(*map), map,
                     take(DecodedAudioAssetPool::create({})), 1);
    auto program = programs.store.read();
    SignalGraph graph;
    const auto output = graph.add_output_node(1);
    const auto first_plugin = graph.add_plugin_node(
        std::make_unique<AutomationRecordingSlot>(), 1, 1);
    const auto second_plugin = graph.add_plugin_node(
        std::make_unique<AutomationRecordingSlot>(), 1, 1);
    REQUIRE(graph.prepare(48'000.0, 64));
    const std::array first_devices{TimelineDeviceGraphRoute{{20}, first_plugin}};
    const std::array second_devices{TimelineDeviceGraphRoute{{20}, second_plugin}};
    const std::array first_routes{TimelineTrackGraphRoute{
        {10}, output, 0, 0, first_devices}};
    const std::array second_routes{TimelineTrackGraphRoute{
        {10}, output, 0, 0, second_devices}};
    TimelineGraphPlaybackBinding first(graph, programs.store);
    TimelineGraphPlaybackBinding second(graph, programs.store);
    REQUIRE(first.prepare(*program, first_routes, config(1), 48'000.0, 64));
    REQUIRE(second.prepare(*program, second_routes, config(1), 48'000.0, 64));
}

TEST_CASE("device mappings without lanes do not claim parameter ingress") {
    const auto map = tempo_map();
    ProgramHarness programs;
    programs.publish(device_project(), map,
                     take(DecodedAudioAssetPool::create({})), 1);
    auto program = programs.store.read();
    SignalGraph graph;
    const auto output = graph.add_output_node(1);
    const auto plugin = graph.add_plugin_node(
        std::make_unique<AutomationRecordingSlot>(), 1, 1);
    REQUIRE(graph.prepare(48'000.0, 64));
    const std::array devices{TimelineDeviceGraphRoute{{20}, plugin}};
    const std::array routes{TimelineTrackGraphRoute{{10}, output, 0, 0, devices}};
    TimelineGraphPlaybackBinding binding(graph, programs.store);
    REQUIRE(binding.prepare(*program, routes, config(1), 48'000.0, 64));
    ParameterEventQueue live;
    REQUIRE(live.push({7, 0, 0.5f, 0}));
    REQUIRE(graph.inject_parameter_events(plugin, live));
}

TEST_CASE("automated device placements cannot share one plugin node") {
    const auto map = tempo_map();
    ProgramHarness programs;
    programs.publish(two_device_automation_project(*map), map,
                     take(DecodedAudioAssetPool::create({})), 1);
    auto program = programs.store.read();
    SignalGraph graph;
    const auto output = graph.add_output_node(1);
    const auto plugin = graph.add_plugin_node(
        std::make_unique<AutomationRecordingSlot>(), 1, 1);
    REQUIRE(graph.prepare(48'000.0, 64));
    const std::array devices{
        TimelineDeviceGraphRoute{{20}, plugin},
        TimelineDeviceGraphRoute{{21}, plugin},
    };
    const std::array routes{TimelineTrackGraphRoute{{10}, output, 0, 0, devices}};
    TimelineGraphPlaybackBinding binding(graph, programs.store);
    REQUIRE(binding.preflight(*program, routes, config(1), 64).code ==
            TimelineGraphAdmissionCode::DuplicateDeviceNodeOwnership);
}

TEST_CASE("timeline automation claims reject graph automation on the same node") {
    const auto map = tempo_map();
    ProgramHarness programs;
    programs.publish(automation_project(*map), map,
                     take(DecodedAudioAssetPool::create({})), 1);
    auto program = programs.store.read();
    SignalGraph graph;
    const auto source = graph.add_gain_node();
    const auto output = graph.add_output_node(1);
    const auto plugin = graph.add_plugin_node(
        std::make_unique<AutomationRecordingSlot>(), 1, 1);
    REQUIRE(graph.connect_automation(source, 0, plugin, 7, 0.0f, 1.0f));
    REQUIRE(graph.prepare(48'000.0, 64));
    const std::array devices{TimelineDeviceGraphRoute{{20}, plugin}};
    const std::array routes{TimelineTrackGraphRoute{{10}, output, 0, 0, devices}};
    TimelineGraphPlaybackBinding binding(graph, programs.store);
    const auto admission = binding.preflight(*program, routes, config(1), 64);
    REQUIRE(admission.code ==
            TimelineGraphAdmissionCode::DeviceNodeAutomationConflict);
    REQUIRE(admission.node == plugin);
}
