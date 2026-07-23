#include "support/timeline_graph_binding_test_support.hpp"

TEST_CASE("timeline graph binding can churn the same ItemId without registry retention") {
    const auto map = tempo_map();
    ProgramHarness programs;
    programs.publish(audio_project(1.0f, 128), map, asset_pool(std::vector<float>(128, 1.0f)), 1);
    auto pinned = programs.store.read();
    SignalGraph graph;
    const auto output_node = graph.add_output_node(1);
    const std::array routes{TimelineTrackGraphRoute{{10}, output_node, 0, 0}};
    for (int iteration = 0; iteration < 8; ++iteration) {
        TimelineGraphPlaybackBinding binding(graph, programs.store);
        REQUIRE(binding.prepare(*pinned, routes, config(1), 48'000.0, 64));
        Buffer input(1, 32);
        Buffer output(1, 32);
        auto output_view = output.view();
        REQUIRE(binding.process(output_view, input.const_view(), snapshot(*pinned, 32)));
    }
    REQUIRE(graph.nodes().size() == 1);
}

TEST_CASE("timeline graph binding carries renderer and pending MIDI across reprepare") {
    const auto map = tempo_map();
    ProgramHarness programs;
    programs.publish(audio_project(1.0f, 128), map,
                     asset_pool(std::vector<float>(128, 1.0f)), 1);
    auto program = programs.store.read();

    SignalGraph graph;
    const auto output_node = graph.add_output_node(1);
    auto counter = std::make_unique<MidiCountingSlot>();
    auto* counter_ptr = counter.get();
    const auto midi_destination = graph.add_plugin_node(
        std::move(counter), 1, 1, "MIDI counter");
    REQUIRE(graph.prepare(48'000.0, 32));
    TimelineGraphPlaybackBinding binding(graph, programs.store);
    const std::array routes{
        TimelineTrackGraphRoute{{10}, output_node, 0, midi_destination}};
    REQUIRE(binding.prepare(*program, routes, config(1), 48'000.0, 32));

    Buffer input(1, 32);
    Buffer output(1, 32);
    auto output_view = output.view();
    REQUIRE(binding.process(output_view, input.const_view(), snapshot(*program, 32)));
    const auto stable_audio = binding.audio_node_for({10});
    const auto stable_midi = binding.midi_input_node_for({10});
    const auto carry = binding.renderer_state_for({10});
    REQUIRE(carry.valid);

    midi::MidiBuffer pending;
    pending.reserve(1);
    pending.add(midi::MidiEvent::note_on(0, 64, 100));
    REQUIRE(graph.inject_midi(stable_midi, pending));
    REQUIRE(binding.prepare(*program, routes, config(1), 48'000.0, 32));
    REQUIRE(binding.audio_node_for({10}) == stable_audio);
    REQUIRE(binding.midi_input_node_for({10}) == stable_midi);
    const auto carried = binding.renderer_state_for({10});
    REQUIRE(carried.valid == carry.valid);
    REQUIRE(carried.key == carry.key);
    REQUIRE(carried.event_cursor == carry.event_cursor);
    REQUIRE(carried.source_sample == carry.source_sample);
    REQUIRE(carried.timeline_tick == carry.timeline_tick);
    REQUIRE(carried.loop_iteration == carry.loop_iteration);

    // Drive the graph directly so the binding does not publish a newer empty
    // note batch over the deliberately pending ingress publication.
    graph.process(output_view, input.const_view(), 32);
    REQUIRE(counter_ptr->last_event_count == 1);
    graph.process(output_view, input.const_view(), 32);
    REQUIRE(counter_ptr->last_event_count == 0);
}

TEST_CASE("timeline graph binding transactionally adds and removes PDC tracks") {
    const auto map = tempo_map();
    auto assets = asset_pool(std::vector<float>(128, 1.0f));
    ProgramHarness programs;
    programs.publish(audio_project(1.0f, 128), map, assets, 1);

    SignalGraph graph;
    CustomNodeType unrelated;
    unrelated.type_id = "pulp.test.timeline-unrelated-unused";
    unrelated.num_output_ports = 1;
    unrelated.process = [](audio::BufferView<float>& output,
                           const audio::BufferView<const float>&, int) {
        output.clear();
    };
    REQUIRE(graph.register_custom_node_type(std::move(unrelated)));
    const auto input_node = graph.add_input_node(1);
    const auto latency_node = graph.add_plugin_node(
        std::make_unique<ReportedLatencySilence>(), 1, 1, "PDC anchor");
    const auto output_node = graph.add_output_node(1);
    REQUIRE(graph.connect(input_node, 0, latency_node, 0));
    REQUIRE(graph.connect(latency_node, 0, output_node, 0));
    REQUIRE(graph.prepare(48'000.0, 4));
    REQUIRE(graph.custom_node_type_count() == 1);

    std::shared_ptr<const void> pinned_old_snapshot;
    {
        TimelineGraphPlaybackBinding binding(graph, programs.store);
        const std::array one_route{
            TimelineTrackGraphRoute{{10}, output_node, 0, 0}};
        auto one = programs.store.read();
        REQUIRE(binding.prepare(*one, one_route, config(1), 48'000.0, 4));
        const auto stable_audio = binding.audio_node_for({10});
        const auto stable_midi = binding.midi_input_node_for({10});
        REQUIRE(graph.custom_node_type_count() == 2);

        Buffer input(1, 4);
        Buffer output(1, 4);
        auto output_view = output.view();
        REQUIRE(binding.process(output_view, input.const_view(), snapshot(*one, 4, 0)));
        REQUIRE(output.storage[0] == std::vector<float>{0.0f, 0.0f, 1.0f, 1.0f});

        programs.publish(parallel_audio_project(128), map, assets, 2);
        auto two = programs.store.read();
        const std::array two_routes{
            TimelineTrackGraphRoute{{10}, output_node, 0, 0},
            TimelineTrackGraphRoute{{11}, output_node, 0, 0}};
        REQUIRE(binding.prepare(*two, two_routes, config(1), 48'000.0, 4));
        REQUIRE(binding.audio_node_for({10}) == stable_audio);
        REQUIRE(binding.midi_input_node_for({10}) == stable_midi);
        REQUIRE(graph.custom_node_type_count() == 3);
        REQUIRE(binding.process(output_view, input.const_view(), snapshot(*two, 4, 4)));
        REQUIRE(output.storage[0] == std::vector<float>{1.0f, 1.0f, 1.5f, 1.5f});

        programs.publish(audio_project(1.0f, 128), map, assets, 3);
        auto one_again = programs.store.read();
        REQUIRE(binding.prepare(*one_again, one_route, config(1), 48'000.0, 4));
        REQUIRE(binding.audio_node_for({10}) == stable_audio);
        REQUIRE(binding.midi_input_node_for({10}) == stable_midi);
        REQUIRE(binding.audio_node_for({11}) == 0);
        REQUIRE(graph.custom_node_type_count() == 2);
        REQUIRE(binding.process(output_view, input.const_view(),
                                snapshot(*one_again, 4, 8)));
        REQUIRE(output.storage[0] == std::vector<float>{1.0f, 1.0f, 1.0f, 1.0f});
        pinned_old_snapshot = graph.live_snapshot_handle();
    }

    REQUIRE(graph.nodes().size() == 3);
    REQUIRE(graph.custom_node_type_count() == 1);
    pinned_old_snapshot.reset();
}


TEST_CASE("timeline graph binding quiescently reprepares plugin dimensions") {
    auto map48 = tempo_map({48'000, 1});
    auto assets = asset_pool(std::vector<float>(512, 1.0f));
    ProgramHarness programs48;
    programs48.publish(audio_project(1.0f, 512), map48, assets, 1);
    auto program48 = programs48.store.read();

    SignalGraph graph;
    auto plugin = std::make_unique<DimensionTrackingSlot>();
    auto* plugin_ptr = plugin.get();
    const auto plugin_node = graph.add_plugin_node(std::move(plugin), 1, 1,
                                                   "dimension tracker");
    const auto output_node = graph.add_output_node(1);
    REQUIRE(graph.connect(plugin_node, 0, output_node, 0));
    REQUIRE(graph.prepare(48'000.0, 64));
    TimelineGraphPlaybackBinding binding(graph, programs48.store);
    const std::array routes{
        TimelineTrackGraphRoute{{10}, plugin_node, 0, 0}};
    REQUIRE(binding.prepare(*program48, routes, config(1), 48'000.0, 64));

    auto map44 = tempo_map({44'100, 1});
    ProgramHarness programs44;
    programs44.publish(audio_project(1.0f, 512), map44, assets, 2);
    auto program44 = programs44.store.read();
    REQUIRE(binding.prepare_quiesced(*program44, routes, config(1), 44'100.0, 128));
    REQUIRE(plugin_ptr->prepared_sample_rate.load(std::memory_order_relaxed) == 44'100.0);
    REQUIRE(plugin_ptr->prepared_max_block.load(std::memory_order_relaxed) == 128);
    REQUIRE(binding.prepare_quiesced(*program44, routes, config(1), 44'100.0, 256));
    REQUIRE(plugin_ptr->prepared_max_block.load(std::memory_order_relaxed) == 256);

    Buffer input(1, 32);
    Buffer output(1, 32);
    auto output_view = output.view();
    REQUIRE(binding.process(output_view, input.const_view(), snapshot(*program44, 32)));
    REQUIRE(std::abs(output.storage[0][0] - 1.0f) < 1.0e-6f);

    auto map32 = tempo_map({32'000, 1});
    ProgramHarness programs32;
    programs32.publish(audio_project(1.0f, 512), map32, assets, 3);
    auto program32 = programs32.store.read();
    plugin_ptr->fail_sample_rate.store(32'000.0, std::memory_order_relaxed);
    REQUIRE(binding.prepare_quiesced(*program32, routes, config(1), 32'000.0, 256).code ==
            TimelineGraphAdmissionCode::GraphPrepareFailed);
    REQUIRE(binding.process(output_view, input.const_view(), snapshot(*program44, 32, 32)));
    REQUIRE(std::abs(output.storage[0][0] - 1.0f) < 1.0e-6f);
    REQUIRE(plugin_ptr->prepared_sample_rate.load(std::memory_order_relaxed) == 44'100.0);
    REQUIRE(plugin_ptr->prepared_max_block.load(std::memory_order_relaxed) == 256);
}

TEST_CASE("timeline graph binding restores shared lifecycles after quiesced failure") {
    auto map48 = tempo_map({48'000, 1});
    auto assets = asset_pool(std::vector<float>(512, 1.0f));
    ProgramHarness programs48;
    programs48.publish(audio_project(1.0f, 512), map48, assets, 1);
    auto program48 = programs48.store.read();

    SignalGraph graph;
    auto first = std::make_unique<DimensionTrackingSlot>();
    auto* first_ptr = first.get();
    const auto first_node = graph.add_plugin_node(std::move(first), 1, 1, "first tracker");
    auto second = std::make_unique<DimensionTrackingSlot>();
    auto* second_ptr = second.get();
    const auto second_node = graph.add_plugin_node(std::move(second), 1, 1, "second tracker");
    const auto output_node = graph.add_output_node(1);
    REQUIRE(graph.connect(first_node, 0, second_node, 0));
    REQUIRE(graph.connect(second_node, 0, output_node, 0));
    REQUIRE(graph.prepare(48'000.0, 64));
    TimelineGraphPlaybackBinding binding(graph, programs48.store);
    const std::array routes{TimelineTrackGraphRoute{{10}, first_node, 0, 0}};
    REQUIRE(binding.prepare(*program48, routes, config(1), 48'000.0, 64));

    auto map32 = tempo_map({32'000, 1});
    ProgramHarness programs32;
    programs32.publish(audio_project(1.0f, 512), map32, assets, 2);
    auto program32 = programs32.store.read();
    second_ptr->fail_sample_rate.store(32'000.0, std::memory_order_relaxed);
    REQUIRE(binding.prepare_quiesced(*program32, routes, config(1), 32'000.0, 128).code ==
            TimelineGraphAdmissionCode::GraphPrepareFailed);
    REQUIRE(first_ptr->prepared_sample_rate.load(std::memory_order_relaxed) == 48'000.0);
    REQUIRE(first_ptr->prepared_max_block.load(std::memory_order_relaxed) == 64);
    REQUIRE(second_ptr->prepared_sample_rate.load(std::memory_order_relaxed) == 48'000.0);
    REQUIRE(second_ptr->prepared_max_block.load(std::memory_order_relaxed) == 64);

    Buffer input(1, 32);
    Buffer output(1, 32);
    auto output_view = output.view();
    REQUIRE(binding.process(output_view, input.const_view(), snapshot(*program48, 32)));
    REQUIRE(output.storage[0][0] == 1.0f);

    // If restoring even one already-touched shared instance fails, the binding
    // revokes its publication instead of exposing a partially re-prepared graph.
    first_ptr->fail_sample_rate.store(48'000.0, std::memory_order_relaxed);
    REQUIRE(binding.prepare_quiesced(*program32, routes, config(1), 32'000.0, 128).code ==
            TimelineGraphAdmissionCode::GraphPrepareFailed);
    std::fill(output.storage[0].begin(), output.storage[0].end(), 7.0f);
    REQUIRE(binding.process(output_view, input.const_view(), snapshot(*program48, 32)).code ==
            TimelineGraphProcessCode::MissingProgram);
    REQUIRE(output.storage[0] == std::vector<float>(32, 0.0f));
}

TEST_CASE("timeline graph binding revokes publication when commit rollback fails") {
    auto map48 = tempo_map({48'000, 1});
    auto assets = asset_pool(std::vector<float>(512, 1.0f));
    ProgramHarness programs48;
    programs48.publish(audio_project(1.0f, 512), map48, assets, 1);
    auto program48 = programs48.store.read();

    SignalGraph graph;
    auto plugin = std::make_unique<DimensionTrackingSlot>();
    auto* plugin_ptr = plugin.get();
    const auto plugin_node =
        graph.add_plugin_node(std::move(plugin), 1, 1, "dimension tracker");
    const auto output_node = graph.add_output_node(1);
    REQUIRE(graph.connect(plugin_node, 0, output_node, 0));
    REQUIRE(graph.prepare(48'000.0, 64));
    TimelineGraphPlaybackBinding binding(graph, programs48.store);
    const std::array routes{TimelineTrackGraphRoute{{10}, plugin_node, 0, 0}};
    REQUIRE(binding.prepare(*program48, routes, config(1), 48'000.0, 64));

    auto map44 = tempo_map({44'100, 1});
    ProgramHarness programs44;
    programs44.publish(audio_project(1.0f, 512), map44, assets, 2);
    auto program44 = programs44.store.read();
    BindingCommitFailure failure{&graph, plugin_ptr, true};
    binding.set_before_graph_commit_hook_for_test(&BindingCommitFailure::hook, &failure);
    REQUIRE(binding.prepare_quiesced(*program44, routes, config(1), 44'100.0, 128).code ==
            TimelineGraphAdmissionCode::GraphPrepareFailed);
    REQUIRE_FALSE(graph.is_prepared());

    Buffer input(1, 32);
    Buffer output(1, 32);
    std::fill(output.storage[0].begin(), output.storage[0].end(), 7.0f);
    auto output_view = output.view();
    REQUIRE(binding.process(output_view, input.const_view(), snapshot(*program48, 32)).code ==
            TimelineGraphProcessCode::MissingProgram);
    REQUIRE(output.storage[0] == std::vector<float>(32, 0.0f));
}
