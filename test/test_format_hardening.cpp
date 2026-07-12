#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <pulp/format/background_task_lane.hpp>
#include <pulp/format/headless.hpp>

#include <atomic>
#include <chrono>
#include <thread>
#include <stdexcept>

namespace {

class SidechainProbeProcessor final : public pulp::format::Processor {
public:
    pulp::format::PluginDescriptor descriptor() const override {
        return {
            .name = "SidechainProbe",
            .manufacturer = "PulpTest",
            .bundle_id = "com.pulp.test.sidechain-probe",
            .version = "1.0.0",
            .category = pulp::format::PluginCategory::Effect,
            .input_buses = {{"Main In", 1, false}, {"Sidechain", 1, true}},
            .output_buses = {{"Main Out", 1, false}},
        };
    }
    void define_parameters(pulp::state::StateStore&) override {}
    void prepare(const pulp::format::PrepareContext&) override {}
    void process(pulp::audio::BufferView<float>& output,
                 const pulp::audio::BufferView<const float>& input,
                 pulp::midi::MidiBuffer&, pulp::midi::MidiBuffer&,
                 const pulp::format::ProcessContext&) override {
        const auto* sidechain = sidechain_input();
        saw_sidechain = sidechain != nullptr;
        if (sidechain)
            buffers_distinct = sidechain->channel(0).data() != input.channel(0).data();
        for (std::size_t i = 0; i < output.num_samples(); ++i) {
            const float side = sidechain ? sidechain->channel(0)[i] : 0.0f;
            output.channel(0)[i] = input.channel(0)[i] + 10.0f * side;
        }
    }
    bool saw_sidechain = false;
    bool buffers_distinct = false;
};

std::unique_ptr<pulp::format::Processor> create_sidechain_probe() {
    return std::make_unique<SidechainProbeProcessor>();
}

} // namespace

using Catch::Matchers::WithinAbs;

TEST_CASE("HeadlessHost injects sidechain as an independent bus",
          "[format-hardening][sidechain]") {
    pulp::format::HeadlessHost host(create_sidechain_probe);
    host.prepare(48000.0, 8, 1, 1);
    pulp::audio::Buffer<float> main(1, 4), sidechain(1, 4), output(1, 4);
    for (std::size_t i = 0; i < 4; ++i) {
        main.channel(0)[i] = static_cast<float>(i + 1);
        sidechain.channel(0)[i] = 0.1f * static_cast<float>(i + 1);
    }
    const float* main_ptrs[] = {main.channel(0).data()};
    const float* side_ptrs[] = {sidechain.channel(0).data()};
    pulp::audio::BufferView<const float> main_view(main_ptrs, 1, 4);
    pulp::audio::BufferView<const float> side_view(side_ptrs, 1, 4);
    auto output_view = output.view();

    host.process_with_sidechain(output_view, main_view, side_view);
    auto* probe = host.processor_as<SidechainProbeProcessor>();
    REQUIRE(probe != nullptr);
    REQUIRE(probe->saw_sidechain);
    REQUIRE(probe->buffers_distinct);
    for (std::size_t i = 0; i < 4; ++i)
        REQUIRE_THAT(output.channel(0)[i], WithinAbs(2.0 * (i + 1), 0.0001));

    probe->saw_sidechain = true;
    host.process(output_view, main_view);
    REQUIRE_FALSE(probe->saw_sidechain);
}

TEST_CASE("HeadlessHost offline render drives an independent sidechain file",
          "[format-hardening][sidechain][offline]") {
    pulp::format::HeadlessHost host(create_sidechain_probe);
    host.prepare(48000.0, 3, 1, 1);
    pulp::audio::AudioFileData main{{{1.0f, 2.0f, 3.0f, 4.0f}}, 48000};
    pulp::audio::AudioFileData side{{{0.1f, 0.2f, 0.3f, 0.4f}}, 48000};
    pulp::audio::OfflineRenderOptions options;
    options.block_size_schedule = {3, 1};

    const auto rendered = host.render_offline_with_sidechain(main, side, options);
    REQUIRE(rendered.has_value());
    REQUIRE(rendered->num_frames() == 4);
    for (std::size_t i = 0; i < 4; ++i)
        REQUIRE_THAT(rendered->channels[0][i], WithinAbs(2.0 * (i + 1), 0.0001));

    side.sample_rate = 44100;
    REQUIRE_FALSE(host.render_offline_with_sidechain(main, side, options).has_value());
}

TEST_CASE("BackgroundTaskLane is bounded, prewarmed, and serialized",
          "[format-hardening][background-task]") {
    struct State { std::atomic<int> count{0}; std::atomic<int> last{0}; } state;
    const auto handler = [](void* context, const int& value) noexcept {
        auto& s = *static_cast<State*>(context);
        s.last.store(value, std::memory_order_release);
        s.count.fetch_add(1, std::memory_order_release);
    };
    pulp::format::BackgroundTaskLane<int, 4> lane;
    REQUIRE(lane.start(handler, &state));
    REQUIRE(lane.try_spawn(1));
    REQUIRE(lane.try_spawn(2));
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    while (state.count.load(std::memory_order_acquire) != 2 &&
           std::chrono::steady_clock::now() < deadline)
        std::this_thread::yield();
    lane.stop();
    REQUIRE(state.count.load() == 2);
    REQUIRE(state.last.load() == 2);
    REQUIRE_FALSE(lane.try_spawn(3));
}

TEST_CASE("BackgroundTaskLane latest policy coalesces safely",
          "[format-hardening][background-task]") {
    std::atomic<int> last{0};
    const auto handler = [](void* context, const int& value) noexcept {
        static_cast<std::atomic<int>*>(context)->store(value, std::memory_order_release);
    };
    pulp::format::BackgroundTaskLane<int> lane;
    REQUIRE(lane.start(handler, &last, pulp::format::BackgroundTaskPolicy::Latest));
    for (int value = 1; value <= 100; ++value) REQUIRE(lane.try_spawn(value));
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    while (last.load(std::memory_order_acquire) != 100 &&
           std::chrono::steady_clock::now() < deadline)
        std::this_thread::yield();
    lane.stop();
    REQUIRE(last.load() == 100);
}

#if defined(__cpp_exceptions) && __cpp_exceptions
TEST_CASE("BackgroundTaskLane contains a throwing author handler",
          "[format-hardening][background-task][abi-guard]") {
    std::atomic<int> completed{0};
    const auto handler = [](void* context, const int& value) {
        if (value == 1) throw std::runtime_error("author task failed");
        static_cast<std::atomic<int>*>(context)->store(value, std::memory_order_release);
    };
    pulp::format::BackgroundTaskLane<int> lane;
    REQUIRE(lane.start(handler, &completed));
    REQUIRE(lane.try_spawn(1));
    REQUIRE(lane.try_spawn(2));
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    while (completed.load(std::memory_order_acquire) != 2 &&
           std::chrono::steady_clock::now() < deadline)
        std::this_thread::yield();
    lane.stop();
    REQUIRE(completed.load() == 2);
}
#endif
