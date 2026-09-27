#include "detail/realtime_gpu_audio_path.hpp"
#include "detail/shared_io_trace.hpp"
#include "detail/wavenet_realtime_channel.hpp"
#include "harness/rt_allocation_probe.hpp"
#include <algorithm>
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <pulp/gpu_audio/gpu_audio_transport.hpp>
#include <pulp/gpu_audio/gpu_wavenet_realtime_node.hpp>

using namespace pulp::gpu_audio;
namespace {
struct Control {
    bool complete = true, fail = false, reject = false, release = true;
    bool wrong_sequence = false, late = false;
    int submits = 0, services = 0, wait_services = 0, receives = 0, releases = 0;
    std::uint64_t last_wait_deadline = 0;
    std::array<std::uint64_t, 8> wait_deadlines{}, wait_starts{};
};
class Channel final : public detail::WaveNetRealtimeChannel {
  public:
    explicit Channel(Control& control) : control_(control) {}
    bool submit(std::span<const float> input, std::uint64_t sequence) noexcept override {
        ++control_.submits;
        if (control_.reject)
            return false;
        std::copy(input.begin(), input.end(), data_.begin());
        sequence_ = sequence;
        pending_ = true;
        return true;
    }
    void service(std::uint64_t) noexcept override {
        ++control_.services;
    }
    void service_until(std::uint64_t now, std::uint64_t deadline) noexcept override {
        if (control_.wait_services < static_cast<int>(control_.wait_deadlines.size())) {
            control_.wait_deadlines[control_.wait_services] = deadline;
            control_.wait_starts[control_.wait_services] = now;
        }
        ++control_.wait_services;
        control_.last_wait_deadline = deadline;
        ++control_.services;
    }
    std::optional<GpuWaveNetBlockResult> receive(std::span<float> output) noexcept override {
        ++control_.receives;
        if (!pending_ || !control_.complete)
            return {};
        pending_ = false;
        if (!control_.fail)
            std::copy_n(data_.data(), output.size(), output.data());
        return GpuWaveNetBlockResult{sequence_ + (control_.wrong_sequence ? 1 : 0),
                                     control_.fail ? GpuWaveNetBlockStatus::ProviderFailed
                                                   : GpuWaveNetBlockStatus::GpuDelivered,
                                     control_.late};
    }
    bool release() noexcept override {
        ++control_.releases;
        return control_.release;
    }

  private:
    Control& control_;
    std::array<float, 2> data_{};
    std::uint64_t sequence_ = 0;
    bool pending_ = false;
};
struct Shape {
    GpuWaveNetLayerDescriptor layer{.input_size = 1,
                                    .condition_size = 1,
                                    .channels = 1,
                                    .kernel = 2,
                                    .head_size = 1,
                                    .dilation = 1,
                                    .tanh_activation = true};
    std::array<float, 9> weights{};
    GpuWaveNetRealtimeNode::Config config(std::uint32_t lead = 1, std::uint32_t channels = 1,
                                          std::uint64_t wait_ns = 0) {
        return {.session = {.descriptor = {.block_size = 2,
                                           .sample_rate = 48000,
                                           .stream_instances = 1,
                                           .layers = {&layer, 1},
                                           .weight_count = weights.size()},
                            .weights = weights},
                .channels = channels,
                .lead_blocks = lead,
                .capacity = lead + 3,
                .completion_service_wait_ns = wait_ns};
    }
};
struct Harness {
    Shape shape;
    std::array<Control, 2> controls{};
    GpuWaveNetRealtimeNode node;
    detail::RealtimeGpuNodePath path;
    std::uint64_t sequence = 0;
    std::uint32_t channels;
    std::array<float, 2> a{}, b{}, outa{}, outb{};
    std::array<const float*, 2> inputs{a.data(), b.data()};
    std::array<float*, 2> outputs{outa.data(), outb.data()};
    explicit Harness(std::uint32_t lead = 1, std::uint32_t count = 1, std::uint64_t wait_ns = 0)
        : node(shape.config(lead, count, wait_ns)), channels(count) {
        std::vector<std::unique_ptr<detail::WaveNetRealtimeChannel>> providers;
        for (std::uint32_t ch = 0; ch < count; ++ch)
            providers.push_back(std::make_unique<Channel>(controls[ch]));
        REQUIRE(detail::WaveNetRealtimeTestAccess::prepare(node, std::move(providers)));
        path = detail::realtime_gpu_node_path(&node);
        REQUIRE(path.active());
        CHECK_FALSE(node.fenced());
    }
    std::uint8_t callback(float value) {
        a.fill(value);
        b.fill(value + 100);
        pulp::audio::BufferView<const float> in(inputs.data(), channels, 2);
        pulp::audio::BufferView<float> out(outputs.data(), channels, 2);
        auto status = path.process(path.context, in, out, 2, sequence, true, 0);
        const auto disposition = status == detail::kRealtimeGpuReady
                                     ? detail::SharedIoDeliveryDisposition::GpuDelivered
                                 : status == detail::kRealtimeGpuPriming
                                     ? detail::SharedIoDeliveryDisposition::Priming
                                     : detail::SharedIoDeliveryDisposition::SilenceDelivered;
        path.delivered(path.context, sequence++, static_cast<std::uint8_t>(disposition), 0, 0);
        return status;
    }
    std::uint32_t service() {
        return path.service(path.context, 0);
    }
};
} // namespace

TEST_CASE("WaveNet stamped node never calls providers from callback and honors lead",
          "[gpu_audio][wavenet][realtime]") {
    for (auto lead : {1u, 2u, 4u, 8u}) {
        Harness h(lead, 2);
        for (std::uint32_t block = 0; block < lead + 6; ++block) {
            const auto calls = h.controls[0].services;
            const auto submits = h.controls[0].submits;
            auto status = h.callback(float(block + 1));
            CHECK(h.controls[0].services == calls);
            CHECK(h.controls[0].submits == submits);
            if (block < lead)
                CHECK(status == detail::kRealtimeGpuPriming);
            else {
                REQUIRE(status == detail::kRealtimeGpuReady);
                CHECK(h.outa[0] == float(block - lead + 1));
                CHECK(h.outb[0] == float(block - lead + 101));
            }
            h.service();
            h.service();
        }
        CHECK(detail::realtime_gpu_provider(&h.node) == GpuAudioProvider::Unknown);
    }
}

TEST_CASE("WaveNet timed service publishes new work with one shared pump deadline",
          "[gpu_audio][wavenet][realtime]") {
    Harness h(1, 2, 500'000);
    CHECK(h.callback(1) == detail::kRealtimeGpuPriming);
    CHECK(h.controls[0].services == 0);
    CHECK(h.controls[1].services == 0);
    CHECK(h.service() == 1);
    for (const auto& control : h.controls) {
        CHECK(control.wait_services == 2);
        CHECK(control.wait_deadlines[0] == control.wait_starts[0] + 500'000);
        CHECK(control.wait_deadlines[0] == control.wait_deadlines[1]);
        CHECK(control.wait_starts[1] >= control.wait_starts[0]);
        CHECK(control.wait_deadlines[0] == h.controls[0].wait_deadlines[0]);
        CHECK(control.submits == 1);
    }
    REQUIRE(h.callback(2) == detail::kRealtimeGpuReady);
    CHECK(h.outa[0] == 1);
    CHECK(h.outb[0] == 101);
    CHECK(h.controls[0].wait_services == 2);
    CHECK(h.service() == 1);
    CHECK(h.controls[0].wait_services == 4);
    CHECK(h.controls[0].wait_deadlines[2] >= h.controls[0].wait_deadlines[0]);
    CHECK(h.controls[0].wait_deadlines[2] == h.controls[0].wait_deadlines[3]);
    CHECK(h.controls[0].wait_deadlines[2] == h.controls[1].wait_deadlines[3]);
    CHECK(h.service() == 0);
    CHECK(h.controls[0].wait_services == 5);
}

TEST_CASE("WaveNet zero wait retains completion on the following pump",
          "[gpu_audio][wavenet][realtime]") {
    Harness h;
    h.callback(1);
    CHECK(h.service() == 0);
    CHECK(h.controls[0].services == 1);
    CHECK(h.controls[0].receives == 0);
    CHECK(h.service() == 1);
    CHECK(h.controls[0].services == 2);
    CHECK(h.controls[0].wait_services == 0);
    REQUIRE(h.callback(2) == detail::kRealtimeGpuReady);
    CHECK(h.outa[0] == 1);
}

TEST_CASE("WaveNet timed service counts both publications without admitting a third block",
          "[gpu_audio][wavenet][realtime]") {
    Harness h(3, 2, 500'000);
    h.controls[0].complete = h.controls[1].complete = false;
    h.callback(1);
    CHECK(h.service() == 0);
    h.callback(2);
    h.callback(3);
    h.controls[0].complete = h.controls[1].complete = true;
    CHECK(h.service() == 2);
    for (const auto& control : h.controls) {
        CHECK(control.submits == 2);
        CHECK(control.wait_services == 4);
        CHECK(control.wait_deadlines[2] == control.wait_deadlines[3]);
    }
    REQUIRE(h.callback(4) == detail::kRealtimeGpuReady);
    CHECK(h.outa[0] == 1);
    REQUIRE(h.callback(5) == detail::kRealtimeGpuReady);
    CHECK(h.outa[0] == 2);
}

TEST_CASE("WaveNet incomplete timed submission stops after two service passes",
          "[gpu_audio][wavenet][realtime]") {
    Harness h(1, 2, 500'000);
    h.controls[1].complete = false;
    h.callback(1);
    CHECK(h.service() == 0);
    CHECK(h.controls[0].wait_services == 2);
    CHECK(h.controls[1].wait_services == 2);
    CHECK(h.service() == 0);
    CHECK(h.controls[0].wait_services == 3);
    CHECK(h.controls[1].wait_services == 3);
    CHECK(h.controls[0].submits == 1);
    CHECK(h.controls[1].submits == 1);
    CHECK(h.callback(2) == detail::kRealtimeGpuMissed);
}

TEST_CASE("WaveNet delayed completion cannot fill a later block with stale audio",
          "[gpu_audio][wavenet][realtime]") {
    Harness h;
    h.controls[0].complete = false;
    h.callback(1);
    h.service();
    CHECK(h.callback(2) == detail::kRealtimeGpuMissed);
    h.controls[0].complete = true;
    h.service(); // publishes sequence0 too late and submits sequence1
    CHECK(h.callback(3) == detail::kRealtimeGpuMissed); // stale0 cannot fill due1
    h.service();
    h.service(); // complete1 (late), complete2
    REQUIRE(h.callback(4) == detail::kRealtimeGpuReady);
    CHECK(h.outa[0] == 3);
}
TEST_CASE("WaveNet channel failure fences all channel output and future admission",
          "[gpu_audio][wavenet][realtime]") {
    for (auto wait_ns : {0ull, 500'000ull}) {
        for (auto mode : {0, 1, 2}) {
            Harness h(1, 2, wait_ns);
            h.controls[1].fail = mode == 0;
            h.controls[1].reject = mode == 1;
            h.controls[1].wrong_sequence = mode == 2;
            h.callback(1);
            CHECK(h.service() == 0);
            if (wait_ns != 0) {
                CHECK(h.controls[0].wait_services == (mode == 1 ? 1 : 2));
                CHECK(h.controls[1].wait_services == (mode == 1 ? 1 : 2));
            }
            CHECK(h.service() == 0);
            CHECK(h.callback(2) == detail::kRealtimeGpuMissed);
            h.controls[1].fail = false;
            h.controls[1].reject = false;
            h.controls[1].wrong_sequence = false;
            h.service();
            h.service();
            CHECK(h.callback(3) == detail::kRealtimeGpuMissed);
            CHECK(h.controls[0].submits == 1);
            CHECK(h.node.fenced());
        }
    }
}
TEST_CASE("WaveNet ingress loss fences history rather than skipping an input",
          "[gpu_audio][wavenet][realtime]") {
    Harness h;
    for (int i = 0; i < 8; ++i)
        h.callback(float(i));
    h.service();
    CHECK(h.controls[0].submits == 0);
    CHECK(h.callback(9) == detail::kRealtimeGpuMissed);
}
TEST_CASE("WaveNet provider release failure retains owner for physical drain retry",
          "[gpu_audio][wavenet][realtime]") {
    Harness h;
    h.callback(1);
    h.service();
    h.controls[0].release = false;
    CHECK_FALSE(h.node.release());
    CHECK_FALSE(detail::realtime_gpu_node_path(&h.node).active());
    CHECK(h.controls[0].releases == 1);
    h.controls[0].release = true;
    CHECK(h.node.release());
    CHECK(h.controls[0].releases == 2);
}
TEST_CASE("WaveNet transport rejects legacy route when realtime node is unprepared",
          "[gpu_audio][wavenet][realtime]") {
    Shape shape;
    GpuWaveNetRealtimeNode node(shape.config());
    GpuAudioTransport transport;
    CHECK_FALSE(transport.prepare(&node, {}));
}
TEST_CASE("WaveNet fence leaves later callbacks on fallback until preparation",
          "[gpu_audio][wavenet][realtime]") {
    for (auto wait_ns : {0ull, 500'000ull}) {
        Harness h(1, 1, wait_ns);
        h.callback(1);
        h.service();
        h.service();
        CHECK(h.path.fence(h.path.context));
        CHECK(h.callback(2) == detail::kRealtimeGpuMissed);
        h.service();
        CHECK(h.controls[0].submits == 1);
    }
}

TEST_CASE("WaveNet callback performs no C++ allocations", "[gpu_audio][wavenet][realtime]") {
    for (auto wait_ns : {0ull, 500'000ull}) {
        Harness h(1, 1, wait_ns);
        h.callback(1);
        h.service();
        h.service();
        std::size_t allocations = 0;
        std::uint8_t status;
        {
            pulp::test::RtAllocationProbe probe;
            status = h.callback(2);
            allocations = probe.allocation_count();
        }
        CHECK(status == detail::kRealtimeGpuReady);
        CHECK(allocations == 0);
    }
}
TEST_CASE("WaveNet invalid config and prewarm overflow are rejected before provider calls",
          "[gpu_audio][wavenet][realtime]") {
    for (auto mode : {0, 1, 2, 3}) {
        Shape shape;
        auto config = shape.config();
        std::uint64_t first = 0;
        if (mode == 0)
            config.capacity = config.lead_blocks;
        if (mode == 1)
            config.miss_policy = MissPolicy::PassthroughDry;
        if (mode == 2)
            config.session.slots = 1;
        if (mode == 3) {
            first = (std::uint64_t{1} << 63) - 2;
            config.prewarm_blocks = 2;
        }
        Control control;
        GpuWaveNetRealtimeNode node(config);
        std::vector<std::unique_ptr<detail::WaveNetRealtimeChannel>> providers;
        providers.push_back(std::make_unique<Channel>(control));
        CHECK_FALSE(detail::WaveNetRealtimeTestAccess::prepare(node, std::move(providers), first));
        CHECK(control.submits == 0);
        CHECK(control.services == 0);
        CHECK_FALSE(detail::realtime_gpu_node_path(&node).active());
    }
}
TEST_CASE("WaveNet reprepare discards previous epoch output", "[gpu_audio][wavenet][realtime]") {
    Harness h;
    h.callback(10);
    h.service();
    h.service();
    std::vector<std::unique_ptr<detail::WaveNetRealtimeChannel>> providers;
    providers.push_back(std::make_unique<Channel>(h.controls[0]));
    REQUIRE(detail::WaveNetRealtimeTestAccess::prepare(h.node, std::move(providers), h.sequence));
    h.path = detail::realtime_gpu_node_path(&h.node);
    CHECK(h.callback(20) == detail::kRealtimeGpuPriming);
    h.service();
    h.service();
    REQUIRE(h.callback(30) == detail::kRealtimeGpuReady);
    CHECK(h.outa[0] == 20);
}

TEST_CASE("WaveNet transport primes one callback shadow on hits and misses",
          "[gpu_audio][wavenet][realtime]") {
    class ShadowNode final : public GpuWaveNetRealtimeNode {
      public:
        using GpuWaveNetRealtimeNode::GpuWaveNetRealtimeNode;
        int primes = 0, fallbacks = 0;
        std::array<float, 2> previous{}, due{};
        void prime_fallback(const pulp::audio::BufferView<const float>& in,
                            std::uint32_t n) noexcept override {
            ++primes;
            due = previous;
            std::copy_n(in.channel_ptr(0), n, previous.data());
        }
        void process_cpu_fallback(const pulp::audio::BufferView<const float>&,
                                  pulp::audio::BufferView<float>& out,
                                  std::uint32_t n) noexcept override {
            ++fallbacks;
            std::copy_n(due.data(), n, out.channel_ptr(0));
        }
    };
    Shape shape;
    auto config = shape.config();
    config.miss_policy = MissPolicy::CpuFallback;
    config.supports_cpu_fallback = true;
    Control control;
    ShadowNode node(config);
    std::vector<std::unique_ptr<detail::WaveNetRealtimeChannel>> providers;
    providers.push_back(std::make_unique<Channel>(control));
    REQUIRE(detail::WaveNetRealtimeTestAccess::prepare(node, std::move(providers)));
    GpuAudioTransport transport;
    REQUIRE(transport.prepare(&node, {}));
    std::array<float, 2> input{1, 1}, output{};
    const float* ip = input.data();
    float* op = output.data();
    pulp::audio::BufferView<const float> in(&ip, 1, 2);
    pulp::audio::BufferView<float> out(&op, 1, 2);
    transport.process(in, out, 2);
    transport.pump();
    transport.pump();
    input.fill(2);
    transport.process(in, out, 2);
    CHECK(output[0] == 1);
    CHECK(node.primes == 2);
    CHECK(node.fallbacks == 0);
    input.fill(3);
    transport.process(in, out, 2); // No worker servicing sequence1.
    CHECK(output[0] == 2);
    CHECK(node.primes == 3);
    CHECK(node.fallbacks == 1);
    CHECK(control.submits == 1); // No second CPU worker implementation exists.
    input.fill(4);
    transport.process_offline(in, out, 2);
    CHECK(output[0] == 3);
    CHECK(node.primes == 4);
    CHECK(node.fallbacks == 2);
}

TEST_CASE("WaveNet late retirement leaves a sequence hole without resetting valid history",
          "[gpu_audio][wavenet][realtime]") {
    for (auto wait_ns : {0ull, 500'000ull}) {
        Harness h(1, 1, wait_ns);
        h.controls[0].late = true;
        h.callback(1);
        h.service();
        h.service();
        CHECK(h.callback(2) == detail::kRealtimeGpuMissed);
        h.controls[0].late = false;
        h.service();
        h.service();
        REQUIRE(h.callback(3) == detail::kRealtimeGpuReady);
        CHECK(h.outa[0] == 2);
    }
}
