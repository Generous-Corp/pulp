#include "detail/realtime_gpu_audio_path.hpp"
#include "detail/shared_io_stamped_bridge.hpp"
#include "detail/shared_io_trace.hpp"
#include "detail/wavenet_realtime_channel.hpp"
#include "harness/rt_allocation_probe.hpp"
#include <algorithm>
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <pulp/gpu_audio/gpu_audio_transport.hpp>
#include <pulp/gpu_audio/gpu_wavenet_realtime_node.hpp>
#include <string_view>

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
// Observe the real sole-consumer drain, including release and reprepare drains.
// Fixed storage keeps the observer safe even in noexcept release paths.
struct TraceCapture {
    struct Admission {
        std::uint64_t engine;
        detail::SharedIoTraceAdmission value;
    };
    struct Record {
        std::uint64_t engine;
        detail::SharedIoTraceRecord value;
    };
    std::array<Admission, 64> admissions{};
    std::array<Record, 128> records{};
    std::size_t admission_count = 0, record_count = 0;
    bool overflow = false;
    std::size_t ownership_count = 0;
    detail::SharedIoTraceOwnership ownership;
    void attach(GpuWaveNetRealtimeNode& node) {
        detail::WaveNetRealtimeTestAccess::observe_trace(
            node, {this,
                   [](void* context, std::uint64_t engine,
                      const detail::SharedIoTraceRecord& record) noexcept {
                       auto& self = *static_cast<TraceCapture*>(context);
                       if (self.record_count == self.records.size()) {
                           self.overflow = true;
                           return;
                       }
                       self.records[self.record_count++] = {engine, record};
                   },
                   [](void* context, std::uint64_t engine,
                      const detail::SharedIoTraceAdmission& admission) noexcept {
                       auto& self = *static_cast<TraceCapture*>(context);
                       if (self.admission_count == self.admissions.size()) {
                           self.overflow = true;
                           return;
                       }
                       self.admissions[self.admission_count++] = {engine, admission};
                   },
                   [](void* context, const detail::SharedIoTraceOwnership& ownership) noexcept {
                       auto& self = *static_cast<TraceCapture*>(context);
                       ++self.ownership_count;
                       self.ownership = ownership;
                   }});
    }
    std::size_t terminal_count() const {
        return std::count_if(records.begin(), records.begin() + record_count, [](const auto& r) {
            return r.value.gpu_terminal != detail::SharedIoGpuTerminalDisposition::None;
        });
    }
    void require_closed() const {
        REQUIRE_FALSE(overflow);
        CHECK(ownership_count == 0);
        REQUIRE(admission_count > 0);
        CHECK(terminal_count() == admission_count);
        for (std::size_t i = 0; i < admission_count; ++i) {
            const auto& a = admissions[i];
            CHECK(std::count_if(admissions.begin(), admissions.begin() + admission_count,
                                [&](const auto& other) {
                                    return a.engine == other.engine &&
                                           a.value.generation == other.value.generation &&
                                           a.value.sequence == other.value.sequence;
                                }) == 1);
            CHECK(
                std::count_if(records.begin(), records.begin() + record_count, [&](const auto& r) {
                    return a.engine == r.engine && a.value.generation == r.value.generation &&
                           a.value.sequence == r.value.sequence &&
                           r.value.gpu_terminal != detail::SharedIoGpuTerminalDisposition::None;
                }) == 1);
        }
        for (std::size_t i = 0; i < record_count; ++i)
            CHECK(records[i].value.valid());
    }
    const detail::SharedIoTraceRecord& only_terminal() const {
        REQUIRE(terminal_count() == 1);
        return std::find_if(records.begin(), records.begin() + record_count,
                            [](const auto& r) {
                                return r.value.gpu_terminal !=
                                       detail::SharedIoGpuTerminalDisposition::None;
                            })
            ->value;
    }
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
    explicit Harness(std::uint32_t lead = 1, std::uint32_t count = 1, std::uint64_t wait_ns = 0,
                     bool trace = false)
        : node(shape.config(lead, count, wait_ns)), channels(count) {
        if (trace)
            REQUIRE(node.configure_trace(
                {.enabled = true, .capture_admissions = true, .capture_callback_timing = true}));
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
    for (auto trace_enabled : {false, true})
        for (auto wait_ns : {0ull, 500'000ull}) {
            Harness h(1, 1, wait_ns, trace_enabled);
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
    for (auto trace_enabled : {false, true})
        for (auto wait_ns : {0ull, 500'000ull}) {
            Harness h(1, 1, wait_ns, trace_enabled);
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
    for (auto trace_enabled : {false, true})
        for (auto wait_ns : {0ull, 500'000ull}) {
            Harness h(1, 1, wait_ns, trace_enabled);
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

TEST_CASE("WaveNet owned trace closes each worker admission without inventing GPU timing",
          "[gpu_audio][wavenet][trace]") {
    Harness h(1, 2, 0, true);
    REQUIRE_FALSE(h.node.configure_trace({.enabled = false}));
    h.callback(1);
    h.service();
    h.service();
    auto first = detail::WaveNetRealtimeTestAccess::last_terminal(h.node);
    REQUIRE(first.valid());
    CHECK(first.gpu_terminal == detail::SharedIoGpuTerminalDisposition::CompletedAccepted);
    CHECK(first.sequence == 0);
    CHECK_FALSE(first.has(detail::SharedIoTraceStage::Scheduled));
    CHECK_FALSE(first.has(detail::SharedIoTraceStage::SubmitBegin));
    CHECK_FALSE(first.gpu_elapsed_available);
    CHECK(first.has(detail::SharedIoTraceStage::WorkerEntry));
    CHECK(first.has(detail::SharedIoTraceStage::CompletionObserved));
    h.callback(2);
    h.service();
    REQUIRE(h.node.release());
    auto terminal = detail::WaveNetRealtimeTestAccess::last_terminal(h.node);
    CHECK(terminal.gpu_terminal == detail::SharedIoGpuTerminalDisposition::CancelledTeardown);
    CHECK(terminal.sequence == 1);
    auto stats = detail::WaveNetRealtimeTestAccess::trace_stats(h.node);
    CHECK(stats.admissions_enqueued == 2);
    CHECK(stats.admissions_drained == 2);
    CHECK(stats.invalid == 0);
    CHECK(stats.enqueued == stats.drained);
    CHECK(stats.dropped == 0);
    REQUIRE(h.node.release());
    CHECK(detail::WaveNetRealtimeTestAccess::trace_stats(h.node).attempted == stats.attempted);
}

TEST_CASE("WaveNet partial channel rejection produces one failed terminal after retirement",
          "[gpu_audio][wavenet][trace]") {
    Harness h(1, 2, 0, true);
    h.controls[1].reject = true;
    h.callback(1);
    h.service();
    h.service();
    const auto terminal = detail::WaveNetRealtimeTestAccess::last_terminal(h.node);
    CHECK(terminal.valid());
    CHECK(terminal.outcome == detail::SharedIoTraceOutcome::SubmissionRejected);
    CHECK(terminal.gpu_reason == detail::SharedIoFallbackReason::SubmissionRejected);
    auto stats = detail::WaveNetRealtimeTestAccess::trace_stats(h.node);
    CHECK(stats.admissions_attempted == 1);
    CHECK(stats.attempted == 1);
    h.callback(2);
    h.service();
    REQUIRE(h.node.release());
    stats = detail::WaveNetRealtimeTestAccess::trace_stats(h.node);
    CHECK(stats.admissions_attempted == 1);
    CHECK(stats.invalid == 0);
    CHECK(stats.enqueued == stats.drained);
}

TEST_CASE("WaveNet owned trace discloses callback queue loss and preserves failed release",
          "[gpu_audio][wavenet][trace]") {
    Harness h(1, 1, 0, true);
    h.controls[0].complete = false;
    h.callback(1);
    h.service();
    for (unsigned i = 0; i < detail::SharedIoTraceRecorder::capacity + 20; ++i)
        h.callback(2);
    h.controls[0].release = false;
    CHECK_FALSE(h.node.release());
    CHECK(detail::WaveNetRealtimeTestAccess::trace_stats(h.node).dropped > 0);
    h.controls[0].release = true;
    REQUIRE(h.node.release());
    const auto stats = detail::WaveNetRealtimeTestAccess::trace_stats(h.node);
    CHECK(stats.dropped > 0);
    CHECK(stats.enqueued == stats.drained);
    CHECK(stats.admissions_enqueued == stats.admissions_drained);
    CHECK(detail::WaveNetRealtimeTestAccess::last_terminal(h.node).gpu_terminal ==
          detail::SharedIoGpuTerminalDisposition::CancelledTeardown);
}

TEST_CASE("Stamped ingress timestamps survive reuse without changing stream identity",
          "[gpu_audio][wavenet][trace]") {
    using Bridge = detail::SharedIoStampedBridge;
    Bridge bridge;
    REQUIRE(bridge.prepare({.capacity = 3,
                            .channels = 1,
                            .block_size = 2,
                            .lead_blocks = 1,
                            .capture_callback_timing = true},
                           1));
    std::array<float, 2> input{}, output{};
    for (std::uint64_t sequence = 0; sequence < 12; ++sequence) {
        const std::uint64_t observed = sequence % 2 ? 100 + sequence : 0;
        auto callback = bridge.begin_callback(input, sequence, observed);
        (void)bridge.consume_output(callback, output);
        REQUIRE(bridge.begin_worker_admission());
        auto lease = bridge.acquire_input();
        REQUIRE(lease);
        CHECK(lease->stamp() == Bridge::Stamp{1, sequence});
        CHECK(lease->callback_ingress_ns() == observed);
        REQUIRE(bridge.release_input(*lease));
        bridge.end_worker_admission();
    }
    bridge.suspend_delivery();
    REQUIRE(bridge.activate_epoch(2));
    auto callback = bridge.begin_callback(input, 12, 0);
    (void)bridge.consume_output(callback, output);
    REQUIRE(bridge.begin_worker_admission());
    auto lease = bridge.acquire_input();
    REQUIRE(lease);
    CHECK(lease->stamp() == Bridge::Stamp{2, 12});
    CHECK(lease->callback_ingress_ns() == 0);
    REQUIRE(bridge.release_input(*lease));
    bridge.end_worker_admission();
}

TEST_CASE("WaveNet trace is off by default and reprepare keeps engine but advances epoch",
          "[gpu_audio][wavenet][trace]") {
    Harness off;
    off.callback(1);
    off.service();
    off.service();
    CHECK(detail::WaveNetRealtimeTestAccess::trace_engine(off.node) == 0);
    CHECK(detail::WaveNetRealtimeTestAccess::trace_stats(off.node).attempted == 0);
    Harness h(1, 1, 0, true);
    h.callback(1);
    h.service();
    h.service();
    const auto generation = detail::WaveNetRealtimeTestAccess::last_terminal(h.node).generation;
    const auto engine = detail::WaveNetRealtimeTestAccess::trace_engine(h.node);
    REQUIRE(engine != 0);
    REQUIRE(h.node.release());
    std::vector<std::unique_ptr<detail::WaveNetRealtimeChannel>> providers;
    providers.push_back(std::make_unique<Channel>(h.controls[0]));
    REQUIRE(detail::WaveNetRealtimeTestAccess::prepare(h.node, std::move(providers), h.sequence));
    h.callback(2);
    h.service();
    h.service();
    CHECK(detail::WaveNetRealtimeTestAccess::last_terminal(h.node).generation > generation);
    CHECK(detail::WaveNetRealtimeTestAccess::trace_engine(h.node) == engine);
    CHECK(detail::next_shared_io_trace_engine_id() != engine);
}

TEST_CASE("WaveNet teardown preserves drained partial submission failure",
          "[gpu_audio][wavenet][trace]") {
    for (bool fence_first : {false, true}) {
        TraceCapture capture;
        Harness h(1, 2, 0, true);
        capture.attach(h.node);
        h.controls[0].complete = false;
        h.controls[1].reject = true;
        h.callback(1);
        h.service();
        REQUIRE(capture.admission_count == 1);
        REQUIRE(capture.terminal_count() == 0);
        h.controls[0].release = false;
        if (fence_first)
            CHECK_FALSE(h.path.fence(h.path.context));
        else
            CHECK_FALSE(h.node.release());
        CHECK(capture.terminal_count() == 0);
        h.controls[0].release = true;
        if (fence_first)
            REQUIRE(h.path.fence(h.path.context));
        REQUIRE(h.node.release());
        REQUIRE(h.node.release());
        capture.require_closed();
        const auto& terminal = capture.only_terminal();
        CHECK(terminal.outcome == detail::SharedIoTraceOutcome::SubmissionRejected);
        CHECK(terminal.gpu_terminal == detail::SharedIoGpuTerminalDisposition::ProviderFailed);
        CHECK(terminal.gpu_reason == detail::SharedIoFallbackReason::SubmissionRejected);
        CHECK_FALSE(terminal.has(detail::SharedIoTraceStage::CompletionObserved));
    }
}

TEST_CASE("WaveNet drained records close reused sequences in separate epochs",
          "[gpu_audio][wavenet][trace]") {
    TraceCapture capture;
    Harness h(1, 1, 0, true);
    capture.attach(h.node);
    h.callback(1);
    h.service();
    h.service();
    h.callback(2);
    h.service();
    REQUIRE(h.path.fence(h.path.context));
    REQUIRE(h.node.release());
    const auto first_generation = capture.admissions[0].value.generation;
    std::vector<std::unique_ptr<detail::WaveNetRealtimeChannel>> providers;
    providers.push_back(std::make_unique<Channel>(h.controls[0]));
    REQUIRE(detail::WaveNetRealtimeTestAccess::prepare(h.node, std::move(providers), 0));
    h.sequence = 0;
    h.callback(3);
    h.service();
    h.service();
    REQUIRE(h.node.release());
    REQUIRE(capture.admission_count == 3);
    CHECK(capture.admissions[2].value.sequence == 0);
    CHECK(capture.admissions[2].value.generation != first_generation);
    CHECK(capture.admissions[2].engine == capture.admissions[0].engine);
    capture.require_closed();
}

TEST_CASE("WaveNet drained provider failure remains terminal across repeated fence",
          "[gpu_audio][wavenet][trace]") {
    TraceCapture capture;
    Harness h(1, 1, 0, true);
    capture.attach(h.node);
    h.controls[0].fail = true;
    h.callback(1);
    h.service();
    h.service();
    REQUIRE(h.path.fence(h.path.context));
    REQUIRE(h.path.fence(h.path.context));
    REQUIRE(h.node.release());
    capture.require_closed();
    CHECK(capture.only_terminal().gpu_reason == detail::SharedIoFallbackReason::CompletionFailed);
}

TEST_CASE("WaveNet stale terminal preserves the bridge recovery cause",
          "[gpu_audio][wavenet][trace]") {
    using Recovery = detail::SharedIoRecoveryReason;
    using Reason = detail::SharedIoFallbackReason;
    const std::array cases{std::pair{Recovery::InputSaturated, Reason::InputSaturated},
                           std::pair{Recovery::ProviderFailure, Reason::CompletionFailed},
                           std::pair{Recovery::ProviderLost, Reason::DeviceLost},
                           std::pair{Recovery::OfflineFence, Reason::Teardown},
                           std::pair{Recovery::SequenceGap, Reason::SequenceGap},
                           std::pair{Recovery::InvalidCallback, Reason::InvalidCallback}};
    for (const auto& [recovery, reason] : cases) {
        TraceCapture capture;
        Harness h(1, 1, 0, true);
        capture.attach(h.node);
        h.controls[0].complete = false;
        h.callback(1);
        h.service();
        // This tests the node's device-loss/recovery seam, not a real lost GPU.
        detail::WaveNetRealtimeTestAccess::request_recovery(h.node, recovery);
        h.callback(2);
        h.controls[0].complete = true;
        h.service();
        REQUIRE(h.node.release());
        capture.require_closed();
        CHECK(capture.only_terminal().gpu_terminal ==
              detail::SharedIoGpuTerminalDisposition::StaleRejected);
        CHECK(capture.only_terminal().gpu_reason == reason);
        const auto delivery =
            std::find_if(capture.records.begin(), capture.records.begin() + capture.record_count,
                         [](const auto& record) { return record.value.output_eligible; });
        REQUIRE(delivery != capture.records.begin() + capture.record_count);
        CHECK(delivery->value.delivery_reason == reason);
        if (recovery == Recovery::InvalidCallback)
            CHECK(std::string_view(detail::shared_io_fallback_reason_name(reason)) ==
                  "invalid_callback");
    }
}

TEST_CASE("WaveNet failed destructor release discloses unresolved physical ownership",
          "[gpu_audio][wavenet][trace]") {
    for (bool fail_release : {false, true}) {
        TraceCapture capture;
        {
            Harness h(1, 2, 0, true);
            capture.attach(h.node);
            h.controls[0].complete = false;
            h.controls[1].complete = false;
            h.callback(1);
            h.service();
            REQUIRE(capture.admission_count == 1);
            h.controls[1].release = !fail_release;
            // Two pending outputs, but only one unresolved physical release.
        }
        if (fail_release) {
            CHECK(capture.ownership_count == 1);
            CHECK_FALSE(capture.ownership.physical_release_complete);
            CHECK(capture.ownership.unresolved_channel_count == 1);
            CHECK(capture.ownership.engine_id == capture.admissions[0].engine);
            CHECK(capture.ownership.generation == capture.admissions[0].value.generation);
            CHECK(capture.terminal_count() == 0);
        } else {
            capture.require_closed();
        }
    }
}
