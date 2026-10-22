#include "detail/realtime_gpu_audio_path.hpp"
#include "detail/shared_io_stamped_bridge.hpp"
#include "detail/shared_io_trace.hpp"
#include "detail/wavenet_realtime_channel.hpp"
#include "harness/rt_allocation_probe.hpp"
#include "support/wavenet_program_session_channel.hpp"
#include <algorithm>
#include <array>
#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <pulp/gpu_audio/gpu_audio_program.hpp>
#include <chrono>
#include <thread>
#include <pulp/gpu_audio/gpu_audio_program.hpp>
#include <pulp/gpu_audio/gpu_audio_transport.hpp>
#include <pulp/gpu_audio/gpu_wavenet_realtime_node.hpp>
#include <string_view>

using namespace pulp::gpu_audio;
namespace {

TEST_CASE("realtime provider identity fails closed for an untyped node",
          "[gpu_audio][realtime][provider]") {
    const auto identity = detail::realtime_gpu_provider_identity(nullptr);
    CHECK_FALSE(identity.authenticated);
    CHECK_FALSE(identity.native_runtime_authenticated);
    CHECK(identity.provider_revision.empty());
    CHECK(identity.adapter_name.empty());
}

struct Control {
    bool complete = true, fail = false, reject = false, release = true;
    bool wrong_sequence = false, late = false;
    // lost: the device is gone; submits are refused, pending work completes
    // as ProviderFailed. hang: the provider accepts work and never completes it.
    bool lost = false, hang = false;
    // The device is lost while a submission is in progress.
    bool lose_on_submit = false;
    int submits = 0, services = 0, wait_services = 0, receives = 0, releases = 0;
    std::uint64_t last_wait_deadline = 0;
    std::array<std::uint64_t, 8> wait_deadlines{}, wait_starts{};
};
class Channel final : public detail::WaveNetRealtimeChannel {
  public:
    explicit Channel(Control& control) : control_(control) {}
    bool submit(std::span<const float> input, std::uint64_t sequence) noexcept override {
        ++control_.submits;
        if (control_.lose_on_submit)
            control_.lost = true;
        if (control_.reject || control_.lost)
            return false;
        auto pending = std::find_if(pending_.begin(), pending_.end(),
                                    [](const auto& value) { return !value.active; });
        if (pending == pending_.end())
            return false;
        std::copy(input.begin(), input.end(), pending->data.begin());
        pending->sequence = sequence;
        pending->active = true;
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
        if (!control_.complete || (control_.hang && !control_.lost))
            return {};
        auto pending = std::find_if(pending_.begin(), pending_.end(),
                                    [](const auto& value) { return value.active; });
        if (pending == pending_.end())
            return {};
        pending->active = false;
        const bool failed = control_.fail || control_.lost;
        if (!failed)
            std::copy_n(pending->data.data(), output.size(), output.data());
        const bool late = control_.late;
        return GpuWaveNetBlockResult{pending->sequence + (control_.wrong_sequence ? 1 : 0),
                                     failed ? GpuWaveNetBlockStatus::ProviderFailed
                                            : GpuWaveNetBlockStatus::GpuDelivered,
                                     late};
    }
    bool lost() const noexcept override {
        return control_.lost;
    }
    bool release() noexcept override {
        ++control_.releases;
        return control_.release;
    }

  private:
    struct Pending {
        std::array<float, 2> data{};
        std::uint64_t sequence = 0;
        bool active = false;
    };
    Control& control_;
    std::array<Pending, 16> pending_{};
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
                                          std::uint64_t wait_ns = 0,
                                          std::uint32_t max_inflight = 1) {
        return {.session = {.descriptor = {.block_size = 2,
                                           .sample_rate = 48000,
                                           .stream_instances = 1,
                                           .layers = {&layer, 1},
                                           .weight_count = weights.size()},
                            .weights = weights,
                            .slots = std::max(2u, max_inflight)},
                .channels = channels,
                .lead_blocks = lead,
                .capacity = lead + 3,
                .max_inflight = max_inflight,
                .completion_service_wait_ns = wait_ns};
    }
};
struct Tweak {
    std::uint32_t lead = 1, capacity = 0, channels = 1;
    std::uint32_t watchdog_blocks = 0;
    bool trace = false;
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
                     bool trace = false, std::uint32_t max_inflight = 1)
        : node(shape.config(lead, count, wait_ns, max_inflight)), channels(count) {
        start(trace);
    }
    explicit Harness(const Tweak& tweak) : node(tweaked(shape, tweak)), channels(tweak.channels) {
        start(tweak.trace);
    }
    static GpuWaveNetRealtimeNode::Config tweaked(Shape& shape, const Tweak& tweak) {
        auto config = shape.config(tweak.lead, tweak.channels);
        if (tweak.capacity != 0)
            config.capacity = tweak.capacity;
        config.watchdog_blocks = tweak.watchdog_blocks;
        return config;
    }
    void start(bool trace) {
        const auto count = channels;
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
    std::uint8_t callback(float value, std::uint64_t start_ns = 0) {
        a.fill(value);
        b.fill(value + 100);
        pulp::audio::BufferView<const float> in(inputs.data(), channels, 2);
        pulp::audio::BufferView<float> out(outputs.data(), channels, 2);
        auto status = path.process(path.context, in, out, 2, sequence, true, start_ns);
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

TEST_CASE("realtime provider identity is empty without an authenticated shared provider",
          "[gpu_audio][wavenet][realtime][identity]") {
    const auto identity = detail::realtime_gpu_provider_identity(nullptr);
    CHECK_FALSE(identity.authenticated);
    CHECK(identity.provider_revision.empty());
    CHECK(identity.adapter_name.empty());
    CHECK(identity.adapter_backend.empty());
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

TEST_CASE("WaveNet bounded multi-flight retires contiguous results in order",
          "[gpu_audio][wavenet][realtime]") {
    Harness h(3, 1, 0, false, 3);
    h.controls[0].complete = false;
    CHECK(h.callback(1) == detail::kRealtimeGpuPriming);
    h.service();
    CHECK(h.callback(2) == detail::kRealtimeGpuPriming);
    h.service();
    CHECK(h.callback(3) == detail::kRealtimeGpuPriming);
    h.service();
    CHECK(h.controls[0].submits == 3);

    h.controls[0].complete = true;
    CHECK(h.service() == 3);
    REQUIRE(h.callback(4) == detail::kRealtimeGpuReady);
    CHECK(h.outa[0] == 1);
    REQUIRE(h.callback(5) == detail::kRealtimeGpuReady);
    CHECK(h.outa[0] == 2);
    REQUIRE(h.callback(6) == detail::kRealtimeGpuReady);
    CHECK(h.outa[0] == 3);
}

TEST_CASE("WaveNet depth telemetry reports bounded multi-flight",
          "[gpu_audio][wavenet][realtime][depth]") {
    Shape shape;
    const auto run = [&](std::uint32_t capacity) {
        auto config = shape.config(1, 1, 0, capacity - 1);
        config.capacity = capacity;
        GpuWaveNetRealtimeNode node(config);
        REQUIRE(node.configure_trace({.enabled = true,
                                      .capture_admissions = true,
                                      .capture_callback_timing = true,
                                      .success_stride = 1}));
        Control control;
        std::vector<std::unique_ptr<detail::WaveNetRealtimeChannel>> providers;
        providers.push_back(std::make_unique<Channel>(control));
        REQUIRE(detail::WaveNetRealtimeTestAccess::prepare(node, std::move(providers)));
        auto path = detail::realtime_gpu_node_path(&node);
        std::array<float, 2> input{}, output{};
        const std::array<const float*, 1> inputs{input.data()};
        const std::array<float*, 1> outputs{output.data()};
        std::uint64_t sequence = 0;
        for (std::uint64_t block = 0; block < 6; ++block) {
            input[0] = static_cast<float>(block + 1);
            pulp::audio::BufferView<const float> in(inputs.data(), 1, 2);
            pulp::audio::BufferView<float> out(outputs.data(), 1, 2);
            const auto status = path.process(path.context, in, out, 2, sequence, true, 0);
            const auto disposition = status == detail::kRealtimeGpuReady
                                         ? detail::SharedIoDeliveryDisposition::GpuDelivered
                                     : status == detail::kRealtimeGpuPriming
                                         ? detail::SharedIoDeliveryDisposition::Priming
                                         : detail::SharedIoDeliveryDisposition::SilenceDelivered;
            path.delivered(path.context, sequence++, static_cast<std::uint8_t>(disposition), 0, 0);
            (void)path.service(path.context, 0);
        }
        REQUIRE(node.release());
        return detail::WaveNetRealtimeTestAccess::telemetry(node);
    };
    const auto depth_two = run(2);
    const auto depth_eight = run(8);
    CHECK(depth_two.submitted_blocks == 6);
    CHECK(depth_eight.submitted_blocks == 6);
    CHECK(depth_two.in_flight_high_water >= 1);
    CHECK(depth_eight.in_flight_high_water >= depth_two.in_flight_high_water);
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
        bool cpu_fallback_prepared() const noexcept override {
            return true;
        }
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

TEST_CASE("WaveNet fallback witness is checked after static configuration",
          "[gpu_audio][wavenet][realtime]") {
    class WitnessNode final : public GpuWaveNetRealtimeNode {
      public:
        using GpuWaveNetRealtimeNode::GpuWaveNetRealtimeNode;
        mutable int witness_calls = 0;
        bool witness_ready = false;
        bool cpu_fallback_prepared() const noexcept override {
            ++witness_calls;
            return witness_ready;
        }
    };

    Shape shape;
    auto config = shape.config();
    config.miss_policy = MissPolicy::CpuFallback;
    config.supports_cpu_fallback = true;
    Control control;
    WitnessNode node(config);

    auto channel = [&] {
        std::vector<std::unique_ptr<detail::WaveNetRealtimeChannel>> providers;
        providers.push_back(std::make_unique<Channel>(control));
        return providers;
    };
    REQUIRE_FALSE(detail::WaveNetRealtimeTestAccess::prepare(node, channel()));
    CHECK(node.witness_calls == 1);
    CHECK_FALSE(detail::realtime_gpu_node_path(&node).active());

    node.witness_ready = true;
    REQUIRE(detail::WaveNetRealtimeTestAccess::prepare(node, channel()));
    CHECK(node.witness_calls == 2);
    CHECK(detail::realtime_gpu_node_path(&node).active());
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
    CHECK_FALSE(first.gpu_elapsed_available);
    CHECK(first.has(detail::SharedIoTraceStage::WorkerEntry));
    CHECK(first.has(detail::SharedIoTraceStage::EncodeBegin));
    CHECK(first.has(detail::SharedIoTraceStage::EncodeEnd));
    CHECK(first.has(detail::SharedIoTraceStage::SubmitBegin));
    CHECK(first.has(detail::SharedIoTraceStage::SubmitEnd));
    CHECK(first.has(detail::SharedIoTraceStage::CompletionObserved));
    CHECK(first.has(detail::SharedIoTraceStage::RetirementObserved));
    CHECK(first.cpu_ns[static_cast<std::size_t>(detail::SharedIoTraceStage::WorkerEntry)] <=
          first.cpu_ns[static_cast<std::size_t>(detail::SharedIoTraceStage::EncodeBegin)]);
    CHECK(first.cpu_ns[static_cast<std::size_t>(detail::SharedIoTraceStage::EncodeEnd)] <=
          first.cpu_ns[static_cast<std::size_t>(detail::SharedIoTraceStage::SubmitBegin)]);
    CHECK(first.cpu_ns[static_cast<std::size_t>(detail::SharedIoTraceStage::SubmitEnd)] <=
          first.cpu_ns[static_cast<std::size_t>(detail::SharedIoTraceStage::CompletionObserved)]);
    CHECK(first.cpu_ns[static_cast<std::size_t>(detail::SharedIoTraceStage::CompletionObserved)] <=
          first.cpu_ns[static_cast<std::size_t>(detail::SharedIoTraceStage::RetirementObserved)]);
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

TEST_CASE("WaveNet live drains defer telemetry validation and quiescent release validates it",
          "[gpu_audio][wavenet][trace]") {
    Harness h(1, 2, 0, true);
    h.callback(1);
    h.service();
    h.service();
    h.callback(2);
    h.service();
    // Negative control: the worker drain may race the audio callback, so its
    // counters receipt must not claim validity.
    auto live = detail::WaveNetRealtimeTestAccess::last_trace_drain(h.node);
    CHECK(live.recording_enabled);
    CHECK_FALSE(live.telemetry_valid);
    CHECK(live.telemetry_error == detail::SharedIoTelemetryError::TelemetryValidationDeferred);
    // The same counters are coherent: a quiescent validation accepts them.
    CHECK(detail::validate_shared_io_telemetry(detail::WaveNetRealtimeTestAccess::telemetry(h.node))
              .accepted());

    REQUIRE(h.node.release());
    const auto closed = detail::WaveNetRealtimeTestAccess::last_trace_drain(h.node);
    CHECK(closed.recording_enabled);
    CHECK(closed.telemetry_valid);
    CHECK(closed.telemetry_error == detail::SharedIoTelemetryError::None);
}

TEST_CASE("WaveNet destructor drain validates telemetry after failed release",
          "[gpu_audio][wavenet][trace]") {
    struct Witness {
        GpuWaveNetRealtimeNode* node = nullptr;
        detail::SharedIoTraceDrainResult before_release;
        detail::SharedIoTraceDrainResult at_destruction;
        std::size_t ownership_count = 0;
    } witness;
    {
        Harness h(1, 1, 0, true);
        witness.node = &h.node;
        detail::WaveNetRealtimeTestAccess::observe_trace(
            h.node,
            {&witness, [](void*, std::uint64_t, const detail::SharedIoTraceRecord&) noexcept {},
             [](void*, std::uint64_t, const detail::SharedIoTraceAdmission&) noexcept {},
             [](void* context, const detail::SharedIoTraceOwnership&) noexcept {
                 // Called from the destructor body after its final drains.
                 auto& self = *static_cast<Witness*>(context);
                 ++self.ownership_count;
                 self.at_destruction =
                     detail::WaveNetRealtimeTestAccess::last_trace_drain(*self.node);
             }});
        h.controls[0].complete = false;
        h.callback(1);
        h.service();
        h.controls[0].release = false;
        // A failed release keeps the generation open and does not drain.
        REQUIRE_FALSE(h.node.release());
        witness.before_release = detail::WaveNetRealtimeTestAccess::last_trace_drain(h.node);
    }
    CHECK(witness.before_release.telemetry_error ==
          detail::SharedIoTelemetryError::TelemetryValidationDeferred);
    REQUIRE(witness.ownership_count == 1);
    CHECK(witness.at_destruction.telemetry_valid);
    CHECK(witness.at_destruction.telemetry_error == detail::SharedIoTelemetryError::None);
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

TEST_CASE("WaveNet rejects malformed trace observers before draining",
          "[gpu_audio][wavenet][trace][negative]") {
    Harness h(1, 1, 0, true);
    TraceCapture capture;
    const detail::SharedIoTraceDrainObserver malformed{
        .context = &capture,
        .record = [](void*, std::uint64_t, const detail::SharedIoTraceRecord&) noexcept {},
    };
    detail::WaveNetRealtimeTestAccess::observe_trace(h.node, malformed);
    h.callback(1);
    h.service();
    h.service();
    REQUIRE(h.node.release());
    CHECK(capture.admission_count == 0);
    CHECK(capture.record_count == 0);
    CHECK(detail::WaveNetRealtimeTestAccess::trace_stats(h.node).admissions_drained > 0);
    CHECK(detail::WaveNetRealtimeTestAccess::trace_stats(h.node).drained > 0);
}

TEST_CASE("WaveNet observer marks admitted terminal identity",
          "[gpu_audio][wavenet][trace][observer]") {
    Harness h(1, 1, 0, true);
    TraceCapture capture;
    capture.attach(h.node);
    h.callback(1);
    h.service();
    h.service();
    REQUIRE(h.node.release());
    capture.require_closed();
    for (std::size_t i = 0; i < capture.record_count; ++i) {
        const auto& record = capture.records[i].value;
        if (record.gpu_terminal != detail::SharedIoGpuTerminalDisposition::None)
            CHECK(record.admission_identity_matched);
    }
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

TEST_CASE("WaveNet recovery diagnostics latch the first reason and sequence",
          "[gpu_audio][wavenet][diagnostic]") {
    using Recovery = detail::SharedIoRecoveryReason;
    Harness h;
    detail::WaveNetRealtimeTestAccess::request_recovery(h.node, Recovery::ProviderFailure, 37);
    detail::WaveNetRealtimeTestAccess::request_recovery(h.node, Recovery::InputSaturated, 38);
    CHECK(detail::WaveNetRealtimeTestAccess::recovery_reason(h.node) == Recovery::ProviderFailure);
    CHECK(detail::WaveNetRealtimeTestAccess::recovery_sequence(h.node) == 37);
    CHECK(h.node.recovery_reason() == GpuAudioRecoveryReason::ProviderFailure);
    CHECK(h.node.recovery_sequence() == 37);
    const auto status = h.node.status_snapshot();
    CHECK(status.recovery_reason == GpuAudioRecoveryReason::ProviderFailure);
    CHECK(status.recovery_sequence == 37);
    CHECK(status.diagnostics_available);
    CHECK(status.provider_state == GpuAudioProviderState::Degraded);
    CHECK(status.selected_engine == GpuAudioEngine::Unknown);
    CHECK(status.sample_rate == 48000);
    CHECK(status.block_size == 2);
    CHECK(status.latency_samples == 2);
    CHECK(status.prepared_lead_blocks == 1);
    CHECK(status.fallback_policy == MissPolicy::Silence);
    CHECK_FALSE(status.fallback_available);
}

TEST_CASE("WaveNet status projection is truthful across prepare and release",
          "[gpu_audio][wavenet][diagnostic]") {
    GpuWaveNetRealtimeNode::Config config;
    config.session.descriptor.block_size = 2;
    config.session.descriptor.sample_rate = 48'000;
    config.channels = 1;
    config.lead_blocks = 1;
    GpuWaveNetRealtimeNode node(config);
    auto initial = node.status_snapshot();
    CHECK(initial.schema_version == GpuAudioStatus::kSchemaVersion);
    CHECK(initial.provider_state == GpuAudioProviderState::Uninitialized);
    CHECK_FALSE(initial.diagnostics_available);
    CHECK(initial.recovery_reason == GpuAudioRecoveryReason::None);
    CHECK(node.release());
    auto released = node.status_snapshot();
    CHECK(released.provider_state == GpuAudioProviderState::Uninitialized);
    CHECK_FALSE(released.diagnostics_available);
}

TEST_CASE("WaveNet status telemetry remains available without trace recording",
          "[gpu_audio][wavenet][diagnostic]") {
    Harness h;
    for (std::uint32_t block = 0; block < 4; ++block) {
        (void)h.callback(static_cast<float>(block + 1));
        (void)h.service();
    }
    const auto status = h.node.status_snapshot();
    CHECK(status.diagnostics_available);
    CHECK(status.produced_blocks + status.missed_blocks > 0);
}

TEST_CASE("WaveNet public recovery diagnostics preserve every recovery reason",
          "[gpu_audio][wavenet][diagnostic]") {
    using Recovery = detail::SharedIoRecoveryReason;
    using PublicRecovery = GpuAudioRecoveryReason;
    const std::array cases{
        std::pair{Recovery::None, PublicRecovery::None},
        std::pair{Recovery::SequenceGap, PublicRecovery::SequenceGap},
        std::pair{Recovery::InputSaturated, PublicRecovery::InputSaturated},
        std::pair{Recovery::ProviderFailure, PublicRecovery::ProviderFailure},
        std::pair{Recovery::ProviderLost, PublicRecovery::ProviderLost},
        std::pair{Recovery::OfflineFence, PublicRecovery::OfflineFence},
        std::pair{Recovery::InvalidCallback, PublicRecovery::InvalidCallback},
        std::pair{Recovery::ProviderUnresponsive, PublicRecovery::ProviderUnresponsive},
    };
    for (const auto& [recovery, expected] : cases) {
        Harness h;
        if (recovery != Recovery::None)
            detail::WaveNetRealtimeTestAccess::request_recovery(h.node, recovery, 91);
        CHECK(h.node.recovery_reason() == expected);
        // Loss and an unresponsive provider need a new provider; every other
        // fence is recoverable. The fake provider is never authenticated, so
        // the unfenced state is Degraded rather than Ready.
        const bool unavailable =
            recovery == Recovery::ProviderLost || recovery == Recovery::ProviderUnresponsive;
        CHECK(h.node.status_snapshot().provider_state ==
              (unavailable ? GpuAudioProviderState::Unavailable : GpuAudioProviderState::Degraded));
        CHECK(h.node.recovery_sequence() ==
              (recovery == Recovery::None ? std::numeric_limits<std::uint64_t>::max() : 91));
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

// Every status a health test observes must satisfy the v3 invariants.
pulp::gpu_audio::GpuAudioStatus validated(const pulp::gpu_audio::GpuAudioStatus& status) {
    const auto validation = pulp::gpu_audio::validate_gpu_audio_status(status);
    INFO("status error " << static_cast<int>(validation.error));
    CHECK(validation.accepted());
    return status;
}

TEST_CASE("WaveNet transport status reports the latched recovery cause after a fence",
          "[gpu_audio][wavenet][status][health]") {
    using Recovery = detail::SharedIoRecoveryReason;
    Harness h;
    REQUIRE(h.path.health != nullptr);
    GpuAudioTransport transport;
    REQUIRE(transport.prepare(&h.node, {8}));
    // Positive control: the unfenced test node has no authenticated provider,
    // so it is Degraded with no latched cause, never Unavailable.
    auto status = validated(transport.status_snapshot());
    CHECK(status.provider_state == GpuAudioProviderState::Degraded);
    CHECK(status.recovery_reason == GpuAudioRecoveryReason::None);
    CHECK_FALSE(h.node.fenced());

    // This tests the node's device-loss seam, not a real lost GPU.
    detail::WaveNetRealtimeTestAccess::request_recovery(h.node, Recovery::ProviderLost, 21);
    transport.pump();
    status = validated(transport.status_snapshot());
    CHECK(status.provider_state == GpuAudioProviderState::Unavailable);
    CHECK(status.recovery_reason == GpuAudioRecoveryReason::ProviderLost);
    CHECK(status.recovery_sequence == 21);
    transport.release();
}

TEST_CASE("WaveNet transport status reports a recoverable cause as Degraded",
          "[gpu_audio][wavenet][status][health]") {
    using Recovery = detail::SharedIoRecoveryReason;
    Harness h;
    GpuAudioTransport transport;
    REQUIRE(transport.prepare(&h.node, {8}));
    detail::WaveNetRealtimeTestAccess::request_recovery(h.node, Recovery::InputSaturated, 4);
    transport.pump();
    const auto status = validated(transport.status_snapshot());
    CHECK(status.provider_state == GpuAudioProviderState::Degraded);
    CHECK(status.recovery_reason == GpuAudioRecoveryReason::InputSaturated);
    CHECK(status.recovery_sequence == 4);
    transport.release();
}

// Hidden until GpuWaveNetRealtimeNode::status_snapshot() fills the v3 epoch,
// delivery-epoch and terminal-count fields that the transport republishes;
// remove the `[.]` tag once it does.
TEST_CASE("WaveNet transport status carries the node epoch and terminal counts",
          "[.][pending-wp-b][gpu_audio][wavenet][status][health]") {
    Harness h;
    GpuAudioTransport transport;
    REQUIRE(transport.prepare(&h.node, {8}));
    constexpr auto delivered = static_cast<std::size_t>(GpuAudioTerminalDisposition::GpuDelivered);
    // Drive the node the way its own tests do (unclocked callbacks), then let
    // one transport pump republish the node's status.
    for (int block = 0; block < 6; ++block) {
        h.callback(float(block + 1));
        h.service();
        h.service();
    }
    transport.pump();
    auto status = validated(transport.status_snapshot());
    CHECK(status.terminal_counts[delivered] > 0);
    CHECK(status.epoch != 0);
    CHECK(status.delivery_epoch_active);
    const auto epoch = status.epoch;

    detail::WaveNetRealtimeTestAccess::request_recovery(
        h.node, detail::SharedIoRecoveryReason::SequenceGap, 3);
    transport.pump();
    status = validated(transport.status_snapshot());
    CHECK(status.provider_state == GpuAudioProviderState::Degraded);
    CHECK(status.recovery_reason == GpuAudioRecoveryReason::SequenceGap);
    CHECK(status.epoch == epoch);
    CHECK_FALSE(status.delivery_epoch_active);
    transport.release();
}

TEST_CASE("WaveNet device loss outranks the completion failures it causes",
          "[gpu_audio][wavenet][realtime][loss]") {
    for (bool trace : {false, true}) {
        for (bool lose : {false, true}) {
            CAPTURE(trace, lose);
            TraceCapture capture;
            Harness h(1, 2, 0, trace);
            if (trace)
                capture.attach(h.node);
            h.callback(1);
            h.service();
            // Channel 0 reports an ordinary failure and is collected first.
            // When channel 1's device is lost in the same pump, loss is latched.
            h.controls[0].fail = true;
            h.controls[1].lost = lose;
            h.controls[1].fail = !lose;
            h.service();
            CHECK(h.node.fenced());
            CHECK(h.node.recovery_reason() == (lose ? GpuAudioRecoveryReason::ProviderLost
                                                    : GpuAudioRecoveryReason::ProviderFailure));
            CHECK(h.node.recovery_sequence() == 0);
            const auto status = h.node.status_snapshot();
            CHECK(status.provider_state ==
                  (lose ? GpuAudioProviderState::Unavailable : GpuAudioProviderState::Degraded));
            CHECK(h.callback(2) == detail::kRealtimeGpuMissed);
            if (trace) {
                REQUIRE(h.node.release());
                capture.require_closed();
                const auto& terminal = capture.only_terminal();
                CHECK(terminal.gpu_terminal ==
                      (lose ? detail::SharedIoGpuTerminalDisposition::DeviceLost
                            : detail::SharedIoGpuTerminalDisposition::ProviderFailed));
                CHECK(terminal.outcome == (lose ? detail::SharedIoTraceOutcome::DeviceLost
                                                : detail::SharedIoTraceOutcome::CompletionFailed));
                CHECK(terminal.gpu_reason ==
                      (lose ? detail::SharedIoFallbackReason::DeviceLost
                            : detail::SharedIoFallbackReason::CompletionFailed));
            }
        }
    }
}

TEST_CASE("WaveNet device loss latches at the oldest in-flight block without a completion",
          "[gpu_audio][wavenet][realtime][loss]") {
    Harness h(3);
    h.controls[0].complete = false;
    h.callback(1);
    h.service();
    h.callback(2);
    h.callback(3);
    h.service();
    CHECK(h.node.recovery_reason() == GpuAudioRecoveryReason::None);
    h.controls[0].lost = true;
    h.service();
    CHECK(h.node.recovery_reason() == GpuAudioRecoveryReason::ProviderLost);
    CHECK(h.node.recovery_sequence() == 0);
    CHECK(h.node.status_snapshot().provider_state == GpuAudioProviderState::Unavailable);
}

TEST_CASE("WaveNet submission refused by a lost device latches ProviderLost",
          "[gpu_audio][wavenet][realtime][loss]") {
    for (bool lose : {false, true}) {
        CAPTURE(lose);
        TraceCapture capture;
        Harness h(1, 1, 0, true);
        capture.attach(h.node);
        h.callback(1);
        h.service();
        h.callback(2);
        h.controls[0].lose_on_submit = lose;
        h.controls[0].reject = !lose;
        h.service();
        CHECK(h.node.recovery_reason() == (lose ? GpuAudioRecoveryReason::ProviderLost
                                                : GpuAudioRecoveryReason::ProviderFailure));
        CHECK(h.node.recovery_sequence() == 1);
        REQUIRE(h.node.release());
        capture.require_closed();
        const auto refused =
            std::find_if(capture.records.begin(), capture.records.begin() + capture.record_count,
                         [](const auto& record) {
                             return record.value.sequence == 1 && record.value.gpu_work_admitted;
                         });
        REQUIRE(refused != capture.records.begin() + capture.record_count);
        CHECK(refused->value.outcome == (lose ? detail::SharedIoTraceOutcome::DeviceLost
                                              : detail::SharedIoTraceOutcome::SubmissionRejected));
    }
}

namespace {
struct HangRun {
    GpuAudioRecoveryReason reason = GpuAudioRecoveryReason::None;
    std::uint64_t sequence = 0, hung_sequence = 0;
    std::uint32_t blocks_after_hang = 0;
    std::size_t allocations = 0;
    GpuAudioProviderState state = GpuAudioProviderState::Uninitialized;
};
// Healthy warmup, then a provider that stops completing. Each round is
// `callbacks` callbacks followed by `pumps` worker pumps.
HangRun run_hang(const Tweak& tweak, std::uint32_t callbacks, std::uint32_t pumps,
                 std::uint32_t resume_after = UINT32_MAX) {
    Harness h(tweak);
    for (int block = 0; block < 4; ++block) {
        h.callback(float(block));
        h.service();
    }
    HangRun run;
    // The block submitted by the last healthy pump is the first that hangs.
    run.hung_sequence = h.sequence - 1;
    h.controls[0].hang = true;
    for (std::uint32_t round = 0; round < 16; ++round) {
        for (std::uint32_t i = 0; i < callbacks; ++i) {
            pulp::test::RtAllocationProbe probe;
            (void)h.callback(float(h.sequence));
            run.allocations += probe.allocation_count();
            ++run.blocks_after_hang;
        }
        if (run.blocks_after_hang >= resume_after)
            h.controls[0].hang = false;
        for (std::uint32_t i = 0; i < pumps; ++i)
            h.service();
        if (h.node.recovery_reason() != GpuAudioRecoveryReason::None)
            break;
    }
    run.reason = h.node.recovery_reason();
    run.sequence = h.node.recovery_sequence();
    run.state = h.node.status_snapshot().provider_state;
    return run;
}
} // namespace

TEST_CASE("WaveNet hung provider is reported unresponsive before the input ring saturates",
          "[gpu_audio][wavenet][realtime][hang]") {
    struct Cadence {
        std::uint32_t callbacks, pumps;
    };
    // The watchdog counts callback blocks, so worker pump rate must not matter;
    // a worker that falls behind still reports the hang, not the saturation.
    for (const auto cadence :
         {Cadence{1, 1}, Cadence{1, 4}, Cadence{2, 1}, Cadence{3, 1}, Cadence{4, 1}}) {
        CAPTURE(cadence.callbacks, cadence.pumps);
        const auto run = run_hang({}, cadence.callbacks, cadence.pumps);
        CHECK(run.reason == GpuAudioRecoveryReason::ProviderUnresponsive);
        CHECK(run.sequence == run.hung_sequence);
        CHECK(run.state == GpuAudioProviderState::Unavailable);
        // Shape capacity is lead + 3 = 4 records; saturation needs 5 blocks.
        CHECK(run.blocks_after_hang <= 4);
        CHECK(run.allocations == 0);
    }
}

TEST_CASE("WaveNet watchdog honors its configured block budget",
          "[gpu_audio][wavenet][realtime][hang]") {
    // Wide ring, so only the configured budget can fire.
    const auto one = run_hang({.capacity = 16, .watchdog_blocks = 1}, 1, 1);
    CHECK(one.reason == GpuAudioRecoveryReason::ProviderUnresponsive);
    CHECK(one.blocks_after_hang == 1);
    const auto five = run_hang({.capacity = 16, .watchdog_blocks = 5}, 1, 1);
    CHECK(five.reason == GpuAudioRecoveryReason::ProviderUnresponsive);
    CHECK(five.blocks_after_hang == 5);
}

TEST_CASE("WaveNet provider that stalls inside the watchdog budget is not unresponsive",
          "[gpu_audio][wavenet][realtime][hang]") {
    const auto run = run_hang({.capacity = 16}, 1, 1, 3);
    CHECK(run.reason == GpuAudioRecoveryReason::None);
    CHECK(run.state != GpuAudioProviderState::Unavailable);
}

TEST_CASE("WaveNet results are late only once their consuming callback has finished",
          "[gpu_audio][wavenet][realtime][late]") {
    SECTION("bursty callbacks and a stalled worker never drop a result that can still land") {
        // Lead 2 at 2 frames / 48 kHz is an 83 us window. Callbacks arrive in
        // bursts and the worker sleeps far past that window between them; every
        // result still precedes its consumer, so none may be rejected.
        Harness h(Tweak{.lead = 2, .capacity = 8});
        std::uint32_t ready = 0, missed = 0;
        for (int round = 0; round < 12; ++round) {
            for (int burst = 0; burst < 2; ++burst) {
                const auto status = h.callback(float(h.sequence + 1));
                ready += status == detail::kRealtimeGpuReady ? 1 : 0;
                missed += status == detail::kRealtimeGpuMissed ? 1 : 0;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
            // One admission per pump: the third pump collects the burst's
            // second block before its consumer runs.
            h.service();
            h.service();
            h.service();
        }
        const auto status = h.node.status_snapshot();
        const auto late = static_cast<std::size_t>(GpuAudioTerminalDisposition::LateRejected);
        CHECK(missed == 0);
        CHECK(ready == 22);
        CHECK(status.terminal_counts[late] == 0);
        CHECK(h.node.recovery_reason() == GpuAudioRecoveryReason::None);
    }
    SECTION("a result whose consumer already ran is rejected as late") {
        TraceCapture capture;
        Harness h(1, 1, 0, true);
        capture.attach(h.node);
        h.controls[0].complete = false;
        h.callback(1);
        h.service();
        // The consumer of block 0 runs before the provider completes it.
        CHECK(h.callback(2) == detail::kRealtimeGpuMissed);
        h.controls[0].complete = true;
        h.service();
        const auto status = h.node.status_snapshot();
        CHECK(status.terminal_counts[static_cast<std::size_t>(
                  GpuAudioTerminalDisposition::LateRejected)] == 1);
        // Late is a miss, not a provider fault.
        CHECK(h.node.recovery_reason() == GpuAudioRecoveryReason::None);
        REQUIRE(h.node.release());
        const auto first =
            std::find_if(capture.records.begin(), capture.records.begin() + capture.record_count,
                         [](const auto& record) {
                             return record.value.sequence == 0 && record.value.gpu_work_admitted;
                         });
        REQUIRE(first != capture.records.begin() + capture.record_count);
        CHECK(first->value.gpu_terminal == detail::SharedIoGpuTerminalDisposition::LateRejected);
        CHECK(first->value.gpu_reason == detail::SharedIoFallbackReason::DeadlineExceeded);
    }
}

namespace {
// Minimal shared-I/O provider whose completions retire through the real
// SharedIoComputePlan. Each accepted submission becomes terminal after
// `delay_polls` provider polls, so a test can make the provider slow without
// making it silent.
class SlowProvider final : public detail::SharedIoArenaProvider {
    struct Slot {
        std::vector<std::byte> input, output;
        std::uint32_t index = 0;
        std::uint64_t generation = 1;
        bool retired = false;
    };
    struct Deferred {
        SlotToken token;
        std::shared_ptr<detail::SharedIoTerminalInbox> inbox;
        std::uint32_t polls_left = 0;
    };

  public:
    std::uint32_t delay_polls = 0;
    std::uint64_t completions = 0, polls = 0;

    bool create_slot(std::uint32_t index, std::size_t in, std::size_t out,
                     SlotResources& resources) noexcept override {
        auto slot = std::make_unique<Slot>();
        slot->input.resize(in);
        slot->output.resize(out);
        slot->index = index;
        resources.input = slot->input.data();
        resources.input_size = in;
        resources.output = slot->output.data();
        resources.output_size = out;
        resources.opaque = slot.get();
        resources.input_lifecycle = {
            .allocated = true, .import_attempted = true, .import_succeeded = true};
        resources.output_lifecycle = resources.input_lifecycle;
        resources.storage_kind = StorageKind::ImportedHostPointer;
        slots_.push_back(std::move(slot));
        return true;
    }
    void retire_slot(SlotResources& resources) noexcept override {
        if (auto* slot = static_cast<Slot*>(resources.opaque)) {
            slot->retired = true;
            ++slot->generation;
        }
        resources.input_lifecycle.dispose_observed = true;
        resources.output_lifecycle.dispose_observed = true;
    }
    void destroy_slot(SlotResources& resources) noexcept override {
        auto* slot = static_cast<Slot*>(resources.opaque);
        std::erase_if(slots_, [slot](const auto& owned) { return owned.get() == slot; });
        resources.input_lifecycle.host_freed = true;
        resources.output_lifecycle.host_freed = true;
        resources.opaque = nullptr;
    }
    bool acquire_slot_buffers(const SlotResources& resources,
                              SlotBufferHandle& handle) const noexcept override {
        auto* slot = static_cast<Slot*>(resources.opaque);
        if (!slot || slot->retired)
            return false;
        handle = {};
        handle.provider = this;
        handle.device = this;
        handle.input_buffer = slot->input.data();
        handle.output_buffer = slot->output.data();
        handle.slot = slot->index;
        handle.generation = slot->generation;
        handle.lifetime = lifetime_;
        return true;
    }
    bool validate_slot_buffers(const SlotBufferHandle& handle) const noexcept override {
        if (handle.provider != this || handle.lifetime.expired())
            return false;
        return std::any_of(slots_.begin(), slots_.end(), [&](const auto& slot) {
            return !slot->retired && handle.input_buffer == slot->input.data() &&
                   handle.generation == slot->generation;
        });
    }
    bool submit(const SlotResources& resources, SlotToken token,
                std::shared_ptr<detail::SharedIoTerminalInbox> inbox) noexcept override {
        std::copy_n(static_cast<const std::byte*>(resources.input),
                    std::min(resources.input_size, resources.output_size),
                    static_cast<std::byte*>(resources.output));
        deferred_[deferred_count_++] = {token, std::move(inbox), delay_polls};
        return true;
    }
    void poll() noexcept override {
        ++polls;
        std::size_t kept = 0;
        for (std::size_t i = 0; i < deferred_count_; ++i) {
            auto& entry = deferred_[i];
            if (entry.polls_left > 0 && --entry.polls_left > 0) {
                deferred_[kept++] = std::move(entry);
                continue;
            }
            (void)entry.inbox->push(entry.token, CompletionStatus::RetiredSuccess);
            ++completions;
        }
        deferred_count_ = kept;
    }
    bool drain() noexcept override {
        for (std::size_t i = 0; i < deferred_count_; ++i)
            (void)deferred_[i].inbox->push(deferred_[i].token, CompletionStatus::RetiredSuccess);
        deferred_count_ = 0;
        return true;
    }

  private:
    std::vector<std::unique_ptr<Slot>> slots_;
    std::array<Deferred, 8> deferred_{};
    std::size_t deferred_count_ = 0;
    std::shared_ptr<const void> lifetime_ = std::make_shared<int>(0);
};
class PassthroughProgram final : public detail::SharedIoPreparedProgram {
  public:
    bool prepare(detail::SharedIoArenaProvider&,
                 std::span<const SlotBufferHandle>) noexcept override {
        return true;
    }
    bool submit(detail::SharedIoArenaProvider& provider, const SlotResources& resources,
                SlotToken token,
                std::shared_ptr<detail::SharedIoTerminalInbox> inbox) noexcept override {
        return provider.submit(resources, token, std::move(inbox));
    }
    bool release() noexcept override {
        return true;
    }
};
} // namespace

TEST_CASE("WaveNet provider that completes late through the shared-I/O plan is slow, not "
          "unresponsive",
          "[gpu_audio][wavenet][realtime][hang]") {
    // About two provider polls per worker pump and two pumps per callback, so a
    // delay of 4 polls is roughly two callback blocks and 16 is about eight.
    for (const std::uint32_t delay : {0u, 4u, 16u}) {
        CAPTURE(delay);
        Shape shape;
        auto config = shape.config(1);
        // The bounded completion service drives SharedIoComputePlan::drain_until.
        config.completion_service_wait_ns = 50'000;
        GpuWaveNetRealtimeNode node(config);
        auto provider = std::make_unique<SlowProvider>();
        auto* slow = provider.get();
        slow->delay_polls = delay;
        auto session = std::make_unique<detail::SharedIoProgramSession>();
        REQUIRE(session->prepare({std::move(provider), std::make_unique<PassthroughProgram>()},
                                 {.slots = 2,
                                  .input_bytes_per_slot = 2 * sizeof(float),
                                  .output_bytes_per_slot = 2 * sizeof(float)}));
        std::vector<std::unique_ptr<detail::WaveNetRealtimeChannel>> channels;
        channels.push_back(std::make_unique<pulp::test::ProgramSessionChannel>(std::move(session)));
        REQUIRE(detail::WaveNetRealtimeTestAccess::prepare(node, std::move(channels)));
        auto path = detail::realtime_gpu_node_path(&node);
        std::array<float, 2> input{}, output{};
        const float* ip = input.data();
        float* op = output.data();
        pulp::audio::BufferView<const float> in(&ip, 1, 2);
        pulp::audio::BufferView<float> out(&op, 1, 2);
        for (std::uint64_t sequence = 0; sequence < 24; ++sequence) {
            input.fill(float(sequence + 1));
            const auto status = path.process(path.context, in, out, 2, sequence, true, 0);
            path.delivered(path.context, sequence,
                           static_cast<std::uint8_t>(
                               status == detail::kRealtimeGpuReady
                                   ? detail::SharedIoDeliveryDisposition::GpuDelivered
                                   : detail::SharedIoDeliveryDisposition::SilenceDelivered),
                           0, 0);
            (void)path.service(path.context, 0);
            (void)path.service(path.context, 0);
        }
        const auto status = node.status_snapshot();
        INFO("polls " << slow->polls << " completions " << slow->completions);
        CHECK(slow->completions > 0);
        const auto delivered = status.terminal_counts[static_cast<std::size_t>(
            GpuAudioTerminalDisposition::GpuDelivered)];
        const auto late = status.terminal_counts[static_cast<std::size_t>(
            GpuAudioTerminalDisposition::LateRejected)];
        if (delay == 0) {
            CHECK(node.recovery_reason() == GpuAudioRecoveryReason::None);
            CHECK(delivered > 0);
            CHECK(late == 0);
        } else if (delay == 4) {
            // Slow: it keeps completing, misses its consumers, and falls behind
            // until the input ring saturates. It is never unresponsive.
            CHECK(late > 0);
            CHECK(node.recovery_reason() == GpuAudioRecoveryReason::InputSaturated);
            CHECK(status.provider_state != GpuAudioProviderState::Unavailable);
        } else {
            // Silent for longer than the watchdog window with work in flight.
            CHECK(node.recovery_reason() == GpuAudioRecoveryReason::ProviderUnresponsive);
            CHECK(status.provider_state == GpuAudioProviderState::Unavailable);
        }
        REQUIRE(node.release());
    }
}

TEST_CASE("WaveNet device loss replaces an earlier symptom it caused",
          "[gpu_audio][wavenet][realtime][loss]") {
    SECTION("a failed completion one pump before the device reports loss") {
        Harness h;
        h.callback(1);
        h.service();
        h.controls[0].fail = true;
        h.service();
        REQUIRE(h.node.recovery_reason() == GpuAudioRecoveryReason::ProviderFailure);
        CHECK(h.node.status_snapshot().provider_state == GpuAudioProviderState::Degraded);
        h.controls[0].lost = true;
        h.service();
        CHECK(h.node.recovery_reason() == GpuAudioRecoveryReason::ProviderLost);
        CHECK(h.node.recovery_sequence() == 0);
        CHECK(h.node.status_snapshot().provider_state == GpuAudioProviderState::Unavailable);
    }
    SECTION("an unresponsive provider that later reports loss") {
        Harness h;
        h.callback(1);
        h.service();
        h.controls[0].hang = true;
        for (int i = 0; i < 4 && h.node.recovery_reason() == GpuAudioRecoveryReason::None; ++i) {
            h.callback(float(i));
            h.service();
        }
        REQUIRE(h.node.recovery_reason() == GpuAudioRecoveryReason::ProviderUnresponsive);
        h.controls[0].lost = true;
        h.service();
        CHECK(h.node.recovery_reason() == GpuAudioRecoveryReason::ProviderLost);
    }
    SECTION("an independent earlier cause stays first, but the provider is unavailable") {
        Harness h;
        for (int i = 0; i < 8; ++i)
            h.callback(float(i));
        h.service();
        REQUIRE(h.node.recovery_reason() == GpuAudioRecoveryReason::InputSaturated);
        CHECK(h.node.status_snapshot().provider_state == GpuAudioProviderState::Degraded);
        h.controls[0].lost = true;
        h.service();
        CHECK(h.node.recovery_reason() == GpuAudioRecoveryReason::InputSaturated);
        CHECK(h.node.status_snapshot().provider_state == GpuAudioProviderState::Unavailable);
    }
}

TEST_CASE("WaveNet status reports schema v3 epoch, terminals and provider facts",
          "[gpu_audio][wavenet][diagnostic]") {
    const auto count = [](const GpuAudioStatus& status, GpuAudioTerminalDisposition disposition) {
        return status.terminal_counts[static_cast<std::size_t>(disposition)];
    };
    Harness h;
    for (int block = 0; block < 4; ++block) {
        h.callback(float(block));
        h.service();
        h.service();
    }
    // One block in flight is the next callback's; collect it early enough.
    h.callback(4);
    h.service();
    // Delivering: blocks 0-3 retired as delivered, block 4 in flight.
    auto status = h.node.status_snapshot();
    CHECK(status.schema_version == 3);
    CHECK(status.epoch == 1);
    CHECK(status.delivery_epoch_active);
    CHECK(count(status, GpuAudioTerminalDisposition::GpuDelivered) == 4);
    std::uint64_t total = 0;
    for (const auto value : status.terminal_counts)
        total += value;
    CHECK(total == 4);
    CHECK(status.gpu_kernel_available);
    CHECK(status.completion_mode == GpuAudioCompletionMode::ProcessEvents);
    CHECK_FALSE(status.timestamps_available);

    // Fenced: the in-flight block is cancelled and delivery stops.
    REQUIRE(h.path.fence(h.path.context));
    status = h.node.status_snapshot();
    CHECK(status.epoch == 1);
    CHECK_FALSE(status.delivery_epoch_active);
    CHECK(count(status, GpuAudioTerminalDisposition::Cancelled) == 1);
    CHECK(count(status, GpuAudioTerminalDisposition::GpuDelivered) == 4);
    CHECK(status.recovery_reason == GpuAudioRecoveryReason::OfflineFence);
    CHECK(status.provider_state == GpuAudioProviderState::Degraded);

    // A new preparation opens a new epoch with fresh counts.
    std::vector<std::unique_ptr<detail::WaveNetRealtimeChannel>> providers;
    providers.push_back(std::make_unique<Channel>(h.controls[0]));
    REQUIRE(detail::WaveNetRealtimeTestAccess::prepare(h.node, std::move(providers), h.sequence));
    status = h.node.status_snapshot();
    CHECK(status.epoch == 2);
    CHECK(status.delivery_epoch_active);
    CHECK(count(status, GpuAudioTerminalDisposition::GpuDelivered) == 0);
}

TEST_CASE("WaveNet status counts a failed block under its miss policy",
          "[gpu_audio][wavenet][diagnostic]") {
    Harness h;
    h.callback(1);
    h.service();
    h.controls[0].fail = true;
    h.service();
    const auto status = h.node.status_snapshot();
    CHECK(status.terminal_counts[static_cast<std::size_t>(GpuAudioTerminalDisposition::Silence)] ==
          1);
    CHECK(status.terminal_counts[static_cast<std::size_t>(
              GpuAudioTerminalDisposition::CpuFallback)] == 0);
}

TEST_CASE("WaveNet status reports the configured completion mode",
          "[gpu_audio][wavenet][diagnostic]") {
    const std::array cases{
        std::pair{GpuWaveNetCompletionPolicy::ProcessEvents, GpuAudioCompletionMode::ProcessEvents},
        std::pair{GpuWaveNetCompletionPolicy::WaitAny, GpuAudioCompletionMode::WaitAny},
        std::pair{GpuWaveNetCompletionPolicy::TimedWaitAny, GpuAudioCompletionMode::TimedWaitAny},
    };
    for (const auto& [policy, mode] : cases) {
        Shape shape;
        auto config = shape.config();
        config.session.completion_policy = policy;
        GpuWaveNetRealtimeNode node(config);
        Control control;
        std::vector<std::unique_ptr<detail::WaveNetRealtimeChannel>> providers;
        providers.push_back(std::make_unique<Channel>(control));
        REQUIRE(detail::WaveNetRealtimeTestAccess::prepare(node, std::move(providers)));
        CHECK(node.status_snapshot().completion_mode == mode);
    }
}

TEST_CASE("WaveNet watchdog stays quiet while a concurrent worker admits mid-callback",
          "[gpu_audio][wavenet][realtime][hang]") {
    // The worker can admit a block after the callback publishes it but before
    // the callback advances its position, so the watchdog must not read that
    // gap as silence. A healthy provider on a concurrent worker never trips it.
    // A deep ring and a wide window absorb worker scheduling hiccups; an
    // unsigned wrap of the silence count would exceed any window.
    Harness h(Tweak{.lead = 2, .capacity = 64, .watchdog_blocks = 32});
    std::atomic<bool> done{false};
    // Completing on every other pump leaves pumps that see work in flight
    // and no completion, which is when the watchdog measures silence.
    std::thread worker([&] {
        for (std::uint64_t pump = 0; !done.load(std::memory_order_acquire); ++pump) {
            h.controls[0].complete = pump % 2 == 0;
            (void)h.path.service(h.path.context, 0);
        }
    });
    // Paced like a fast audio device so the spinning worker keeps up.
    for (int block = 0; block < 5'000 && h.node.recovery_reason() == GpuAudioRecoveryReason::None;
         ++block) {
        (void)h.callback(float(block));
        std::this_thread::sleep_for(std::chrono::microseconds(20));
    }
    done.store(true, std::memory_order_release);
    worker.join();
    INFO("blocks " << h.sequence << " reason " << int(h.node.recovery_reason()));
    CHECK(h.sequence >= 100);
    CHECK(h.node.recovery_reason() != GpuAudioRecoveryReason::ProviderUnresponsive);
}
