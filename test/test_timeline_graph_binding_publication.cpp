#include "support/timeline_graph_binding_test_support.hpp"

TEST_CASE("timeline automation exact ingress stays pinned across stable plugin nodes") {
    const auto map = tempo_map();
    auto empty_assets = take(DecodedAudioAssetPool::create({}));
    ProgramHarness automated_programs;
    automated_programs.publish(automation_project(*map), map, empty_assets, 1);
    auto automated = automated_programs.store.read();
    ProgramHarness next_programs;
    next_programs.publish(
        automation_project(*map, 0.5f, 0.9f), map, empty_assets, 2);
    auto next = next_programs.store.read();
    REQUIRE(automated);
    REQUIRE(next);

    SignalGraph graph;
    const auto output = graph.add_output_node(1);
    auto recorder = std::make_unique<AutomationRecordingSlot>();
    auto* observed = recorder.get();
    const auto plugin = graph.add_plugin_node(std::move(recorder), 1, 1);
    REQUIRE(graph.prepare(48'000.0, 64));
    const std::array devices{TimelineDeviceGraphRoute{{20}, plugin}};
    const std::array routes{TimelineTrackGraphRoute{
        {10}, output, 0, 0, devices}};
    TimelineGraphPlaybackBinding binding(graph, automated_programs.store);
    const auto admission = binding.prepare(*automated, routes, config(1), 48'000.0, 64);
    INFO("admission code " << static_cast<int>(admission.code)
         << " actual " << admission.actual);
    REQUIRE(admission);

    BindingPublishPause pause;
    binding.set_before_binding_publish_hook_for_test(
        &BindingPublishPause::hook, &pause);
    std::atomic<TimelineGraphAdmissionCode> code{
        TimelineGraphAdmissionCode::GraphPrepareFailed};
    std::thread preparer([&] {
        code.store(binding.prepare(*next, routes, config(1), 48'000.0, 64).code,
                   std::memory_order_release);
    });
    REQUIRE(pause.wait_until_entered());
    Buffer input(1, 32);
    Buffer output_buffer(1, 32);
    auto output_view = output_buffer.view();
    REQUIRE(binding.process(
        output_view, input.const_view(), snapshot(*automated, 32)));
    REQUIRE(std::any_of(
        observed->received.begin(),
        observed->received.begin() + observed->event_count,
        [](const auto& event) { return event.value == 0.75f; }));
    REQUIRE(std::none_of(
        observed->received.begin(),
        observed->received.begin() + observed->event_count,
        [](const auto& event) { return event.value == 0.9f; }));
    pause.released.store(true, std::memory_order_release);
    preparer.join();
    REQUIRE(code.load(std::memory_order_acquire) ==
            TimelineGraphAdmissionCode::Accepted);
    REQUIRE(binding.process(
        output_view, input.const_view(), snapshot(*next, 32)));
    REQUIRE(std::any_of(
        observed->received.begin(),
        observed->received.begin() + observed->event_count,
        [](const auto& event) { return event.value == 0.9f; }));
}

TEST_CASE("timeline graph binding publishes coherent state during live reprepare") {
    const auto map = tempo_map();
    constexpr std::size_t kFrames = 128;
    ProgramHarness programs;
    programs.publish(audio_project(1.0f, kFrames), map,
                     asset_pool(std::vector<float>(kFrames, 1.0f)), 1);
    auto program = programs.store.read();

    SignalGraph graph;
    const auto gain_one = graph.add_gain_node("one");
    const auto gain_two = graph.add_gain_node("two");
    const auto output_node = graph.add_output_node(1);
    REQUIRE(graph.set_node_gain(gain_one, 1.0f));
    REQUIRE(graph.set_node_gain(gain_two, 2.0f));
    REQUIRE(graph.connect(gain_one, 0, output_node, 0));
    REQUIRE(graph.connect(gain_two, 0, output_node, 0));
    TimelineGraphPlaybackBinding binding(graph, programs.store);
    const std::array route_one{
        TimelineTrackGraphRoute{{10}, gain_one, 0, 0}};
    const std::array route_two{
        TimelineTrackGraphRoute{{10}, gain_two, 0, 0}};
    REQUIRE(binding.prepare(*program, route_one, config(1), 48'000.0, 32));

    std::atomic<bool> stop{false};
    std::atomic<std::uint64_t> one_blocks{0};
    std::atomic<std::uint64_t> two_blocks{0};
    std::atomic<std::uint64_t> invalid_blocks{0};
    std::atomic<std::size_t> allocations{1};
    std::thread audio_thread([&] {
        Buffer input(1, 32);
        Buffer output(1, 32);
        auto output_view = output.view();
        test::ScopedRtProcessProbe probe;
        while (!stop.load(std::memory_order_acquire)) {
            auto transport = snapshot(*program, 32, 0);
            transport.ranges[0].discontinuity = true;
            const auto result = binding.process(output_view, input.const_view(),
                                                transport);
            if (!result) {
                invalid_blocks.fetch_add(1, std::memory_order_relaxed);
                continue;
            }
            const float first = output.storage[0][0];
            const bool coherent = std::all_of(
                output.storage[0].begin(), output.storage[0].end(),
                [first](float sample) { return sample == first; });
            if (!coherent) {
                invalid_blocks.fetch_add(1, std::memory_order_relaxed);
            } else if (first == 1.0f) {
                one_blocks.fetch_add(1, std::memory_order_relaxed);
            } else if (first == 2.0f) {
                two_blocks.fetch_add(1, std::memory_order_relaxed);
            } else {
                invalid_blocks.fetch_add(1, std::memory_order_relaxed);
            }
        }
        allocations.store(probe.allocation_count(), std::memory_order_relaxed);
    });

    for (int iteration = 0; iteration < 64; ++iteration) {
        const auto& route = (iteration & 1) == 0 ? route_two : route_one;
        REQUIRE(binding.prepare(*program, route, config(1), 48'000.0, 32));
    }
    for (int spin = 0;
         spin < 10'000 && two_blocks.load(std::memory_order_relaxed) == 0; ++spin) {
        std::this_thread::yield();
    }
    stop.store(true, std::memory_order_release);
    audio_thread.join();

    REQUIRE(one_blocks.load(std::memory_order_relaxed) > 0);
    REQUIRE(two_blocks.load(std::memory_order_relaxed) > 0);
    REQUIRE(invalid_blocks.load(std::memory_order_relaxed) == 0);
    REQUIRE(allocations.load(std::memory_order_relaxed) == 0);
}
TEST_CASE("timeline graph binding publishes node-set generations atomically") {
    const auto map = tempo_map();
    auto assets = asset_pool(std::vector<float>(128, 1.0f));
    ProgramHarness two_programs;
    two_programs.publish(parallel_audio_project(128), map, assets, 1);
    auto two = two_programs.store.read();
    ProgramHarness one_programs;
    one_programs.publish(audio_project(1.0f, 128), map, assets, 2);
    auto one = one_programs.store.read();

    SignalGraph graph;
    const auto output_node = graph.add_output_node(1);
    TimelineGraphPlaybackBinding binding(graph, two_programs.store);
    const std::array two_routes{
        TimelineTrackGraphRoute{{10}, output_node, 0, 0},
        TimelineTrackGraphRoute{{11}, output_node, 0, 0}};
    const std::array one_route{
        TimelineTrackGraphRoute{{10}, output_node, 0, 0}};
    REQUIRE(binding.prepare(*two, two_routes, config(1), 48'000.0, 64));

    Buffer input(1, 32);
    Buffer output(1, 32);
    auto output_view = output.view();
    REQUIRE(binding.process(output_view, input.const_view(), snapshot(*two, 32)));
    REQUIRE(output.storage[0][0] == 1.5f);

    BindingPublishPause remove_pause;
    binding.set_before_binding_publish_hook_for_test(&BindingPublishPause::hook,
                                                     &remove_pause);
    std::atomic<TimelineGraphAdmissionCode> remove_code{
        TimelineGraphAdmissionCode::GraphPrepareFailed};
    std::thread remover([&] {
        remove_code.store(binding.prepare(*one, one_route, config(1), 48'000.0, 64).code,
                          std::memory_order_release);
    });
    REQUIRE(remove_pause.wait_until_entered());
    REQUIRE(binding.process(output_view, input.const_view(), snapshot(*two, 32)));
    REQUIRE(output.storage[0][0] == 1.5f);
    remove_pause.released.store(true, std::memory_order_release);
    remover.join();
    REQUIRE(remove_code.load(std::memory_order_acquire) ==
            TimelineGraphAdmissionCode::Accepted);
    REQUIRE(binding.process(output_view, input.const_view(), snapshot(*one, 32)));
    REQUIRE(output.storage[0][0] == 1.0f);

    BindingPublishPause add_pause;
    binding.set_before_binding_publish_hook_for_test(&BindingPublishPause::hook,
                                                     &add_pause);
    std::atomic<TimelineGraphAdmissionCode> add_code{
        TimelineGraphAdmissionCode::GraphPrepareFailed};
    std::thread adder([&] {
        add_code.store(binding.prepare(*two, two_routes, config(1), 48'000.0, 64).code,
                       std::memory_order_release);
    });
    REQUIRE(add_pause.wait_until_entered());
    REQUIRE(binding.process(output_view, input.const_view(), snapshot(*one, 32)));
    REQUIRE(output.storage[0][0] == 1.0f);
    add_pause.released.store(true, std::memory_order_release);
    adder.join();
    REQUIRE(add_code.load(std::memory_order_acquire) ==
            TimelineGraphAdmissionCode::Accepted);
    REQUIRE(binding.process(output_view, input.const_view(), snapshot(*two, 32)));
    REQUIRE(output.storage[0][0] == 1.5f);
    binding.set_before_binding_publish_hook_for_test(nullptr);
}

TEST_CASE("timeline graph binding publishes content generations atomically") {
    const auto map = tempo_map();
    auto assets = asset_pool(std::vector<float>(128, 1.0f));
    ProgramHarness programs;
    programs.publish(audio_project(1.0f, 128), map, assets, 1);
    auto first = programs.store.read();
    SignalGraph graph;
    const auto output_node = graph.add_output_node(1);
    TimelineGraphPlaybackBinding binding(graph, programs.store);
    const std::array routes{TimelineTrackGraphRoute{{10}, output_node, 0, 0}};
    REQUIRE(binding.prepare(*first, routes, config(1), 48'000.0, 64));

    Buffer input(1, 32);
    Buffer output(1, 32);
    auto output_view = output.view();
    REQUIRE(binding.process(output_view, input.const_view(), snapshot(*first, 32)));
    REQUIRE(output.storage[0][0] == 1.0f);

    programs.publish(audio_project(0.5f, 128), map, assets, 2);
    auto second = programs.store.read();
    BindingPublishPause pause;
    binding.set_before_binding_publish_hook_for_test(&BindingPublishPause::hook, &pause);
    std::atomic<TimelineGraphAdmissionCode> adoption_code{
        TimelineGraphAdmissionCode::GraphPrepareFailed};
    std::thread adopter([&] {
        adoption_code.store(binding.adopt_program(*second).code,
                            std::memory_order_release);
    });
    REQUIRE(pause.wait_until_entered());
    REQUIRE(binding.process(output_view, input.const_view(), snapshot(*first, 32, 32)));
    REQUIRE(output.storage[0][0] == 1.0f);
    pause.released.store(true, std::memory_order_release);
    adopter.join();
    REQUIRE(adoption_code.load(std::memory_order_acquire) ==
            TimelineGraphAdmissionCode::Accepted);
    REQUIRE(binding.process(output_view, input.const_view(), snapshot(*second, 32, 64)));
    REQUIRE(output.storage[0][0] == 0.5f);
    binding.set_before_binding_publish_hook_for_test(nullptr);
}

// Render continuity across a structural edit. The transport runs unbroken while
// a track is added to the sequence, so the edit changes the routed node set
// underneath a stream that is already sounding. PDC is active: the anchor
// reports two samples of latency, which delays the onset but must never punch a
// hole into the middle of the stream. A gap here would be either a dropped
// block (process returning false) or a silent sample once the compensation
// delay has passed.
TEST_CASE("timeline graph binding renders gap-free across a structural edit with PDC active",
          "[timeline][pdc][render-continuity]") {
    constexpr std::uint32_t kFrames = 4;
    constexpr int kBlocks = 32;
    constexpr int kEditBlock = 16;
    constexpr std::uint64_t kClipFrames = 512;
    // The PDC anchor's reported latency. The first samples of the stream are
    // the compensation delay, not a dropout, so continuity is asserted after
    // them.
    constexpr std::size_t kLatencySamples = 2;

    const auto map = tempo_map();
    auto assets = asset_pool(std::vector<float>(kClipFrames, 1.0f));
    ProgramHarness programs;
    programs.publish(audio_project(1.0f, kClipFrames), map, assets, 1);

    SignalGraph graph;
    const auto input_node = graph.add_input_node(1);
    const auto latency_node = graph.add_plugin_node(
        std::make_unique<ReportedLatencySilence>(), 1, 1, "PDC anchor");
    const auto output_node = graph.add_output_node(1);
    REQUIRE(graph.connect(input_node, 0, latency_node, 0));
    REQUIRE(graph.connect(latency_node, 0, output_node, 0));
    REQUIRE(graph.prepare(48'000.0, kFrames));

    TimelineGraphPlaybackBinding binding(graph, programs.store);
    const std::array one_route{TimelineTrackGraphRoute{{10}, output_node, 0, 0}};
    const std::array two_routes{TimelineTrackGraphRoute{{10}, output_node, 0, 0},
                                TimelineTrackGraphRoute{{11}, output_node, 0, 0}};
    auto one = programs.store.read();
    REQUIRE(binding.prepare(*one, one_route, config(1), 48'000.0, kFrames));
    const auto stable_audio = binding.audio_node_for({10});
    const auto node_types_before = graph.custom_node_type_count();

    Buffer input(1, kFrames);
    Buffer output(1, kFrames);
    auto output_view = output.view();

    std::vector<float> stream;
    stream.reserve(static_cast<std::size_t>(kBlocks) * kFrames);
    std::int64_t position = 0;

    // The transport never stops: both halves of the stream advance the same
    // running sample position, so the edit lands inside continuous playback.
    const auto render = [&](const PlaybackProgram& program, int blocks) {
        for (int block = 0; block < blocks; ++block) {
            INFO("sample position " << position);
            REQUIRE(binding.process(output_view, input.const_view(),
                                    snapshot(program, kFrames, position)));
            const auto& channel = output.storage[0];
            stream.insert(stream.end(), channel.begin(), channel.end());
            position += kFrames;
        }
    };

    render(*one, kEditBlock);

    // The structural edit lands mid-stream: a second track joins the sequence,
    // which grows the routed node set while audio is running.
    programs.publish(parallel_audio_project(kClipFrames), map, assets, 2);
    auto two = programs.store.read();
    REQUIRE(binding.prepare(*two, two_routes, config(1), 48'000.0, kFrames));
    REQUIRE(binding.audio_node_for({11}) != 0);
    REQUIRE(graph.custom_node_type_count() > node_types_before);
    // The pre-existing track keeps its node identity, so the edit is a
    // transactional grow rather than a teardown and rebuild.
    REQUIRE(binding.audio_node_for({10}) == stable_audio);

    render(*two, kBlocks - kEditBlock);

    REQUIRE(stream.size() == static_cast<std::size_t>(kBlocks) * kFrames);

    // Continuity: past the compensation delay every sample is sounding. A
    // dropped or zero-filled block during the edit shows up here as a zero.
    for (std::size_t index = kLatencySamples; index < stream.size(); ++index) {
        INFO("sample " << index);
        REQUIRE(stream[index] > 0.0f);
    }

    // The edit must actually have taken effect, otherwise the continuity
    // assertion above would pass vacuously against the pre-edit program. Track
    // 10 alone renders 1.0; both tracks sum to 1.5.
    const auto edit_sample = static_cast<std::size_t>(kEditBlock) * kFrames;
    REQUIRE(stream[edit_sample - 1] == 1.0f);
    REQUIRE(stream.back() == 1.5f);
}
