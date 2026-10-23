#include "detail/gpu_audio_provider_pool_test_access.hpp"
#include "detail/provider_pool_key.hpp"
#include "detail/realtime_gpu_audio_path.hpp"
#include "detail/realtime_gpu_health_mirror.hpp"
#include "detail/shared_io_stamped_bridge.hpp"
#include "detail/shared_io_trace.hpp"
#include "detail/shared_io_watchdog.hpp"
#include "detail/wavenet_realtime_channel.hpp"
#include "harness/rt_allocation_probe.hpp"
#include "support/wavenet_program_session_channel.hpp"
#include <algorithm>
#include <array>
#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <climits>
#include <pulp/gpu_audio/gpu_audio_program.hpp>
#include <pulp/gpu_audio/gpu_audio_provider_pool.hpp>
#include <pulp/gpu_audio/gpu_audio_transport.hpp>
#include <pulp/gpu_audio/gpu_wavenet_realtime_node.hpp>
#include <string_view>
#include <thread>

using namespace pulp::gpu_audio;
namespace {

// Every status a test reads must pass the public validator.
GpuAudioStatus checked_status(const GpuWaveNetRealtimeNode& node) {
    const auto status = node.status_snapshot();
    const auto validation = validate_gpu_audio_status(status);
    INFO("validate_gpu_audio_status error " << static_cast<int>(validation.error));
    CHECK(validation.accepted());
    return status;
}

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
    // Return each completion under the generation before the one that
    // admitted it, as a completion surviving from an earlier epoch would.
    bool stale_generation = false;
    // lost: the device is gone; submits are refused, pending work completes
    // as ProviderFailed. hang: the provider accepts work and never completes it.
    bool lost = false, hang = false;
    // The device is lost while a submission is in progress.
    bool lose_on_submit = false;
    // tail_only: complete only in the second, post-admission service pass of a
    // bounded-wait pump (both passes share one deadline). slow_every: every
    // block whose sequence is a multiple of it completes only once
    // completion_lag later blocks were submitted; the others complete at once,
    // ahead of it.
    bool tail_only = false, in_tail = false;
    int slow_every = 0, completion_lag = 0;
    int first_pass_completions = 0, tail_pass_completions = 0;
    // Completion events: whether the channel has a source, and what is set.
    bool notifier_supported = false;
    detail::SharedIoCompletionNotifier notifier{};
    int notifier_installs = 0, notifier_clears = 0;
    int submits = 0, services = 0, wait_services = 0, receives = 0, releases = 0;
    // Provider pool: a recyclable channel parks under `pool_key`. The last
    // bounded release budget is recorded.
    bool recyclable = false;
    std::string pool_key;
    int recycles = 0;
    std::uint64_t last_release_budget = 0;
    bool hang_release = false;
    std::uint64_t last_wait_deadline = 0;
    std::array<std::uint64_t, 8> wait_deadlines{}, wait_starts{};
};
class Channel final : public detail::WaveNetRealtimeChannel {
  public:
    explicit Channel(Control& control) : control_(control) {}
    bool submit(std::span<const float> input,
                const detail::SharedIoSubmissionTicket& ticket) noexcept override {
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
        pending->sequence = ticket.sequence;
        pending->ticket = ticket;
        pending->submit_index = control_.submits;
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
        control_.in_tail = deadline == control_.last_wait_deadline;
        control_.last_wait_deadline = deadline;
        ++control_.services;
    }
    std::optional<detail::WaveNetChannelCompletion>
    receive(std::span<float> output) noexcept override {
        ++control_.receives;
        if (!control_.complete || (control_.hang && !control_.lost))
            return {};
        if (control_.tail_only && !control_.in_tail)
            return {};
        auto pending = std::find_if(pending_.begin(), pending_.end(),
                                    [](const auto& value) { return value.active; });
        if (control_.tail_only || control_.slow_every != 0) {
            const auto eligible = [&](const Pending& value) {
                const bool slow =
                    control_.slow_every != 0 && value.sequence % control_.slow_every == 0;
                return value.active && control_.submits - value.submit_index >=
                                           (slow ? control_.completion_lag : 0);
            };
            pending = std::min_element(
                pending_.begin(), pending_.end(), [&](const auto& a, const auto& b) {
                    return eligible(a) && (!eligible(b) || a.sequence < b.sequence);
                });
            if (!eligible(*pending))
                return {};
            ++(control_.in_tail ? control_.tail_pass_completions : control_.first_pass_completions);
        }
        if (pending == pending_.end())
            return {};
        pending->active = false;
        const bool failed = control_.fail || control_.lost;
        if (!failed)
            std::copy_n(pending->data.data(), output.size(), output.data());
        const bool late = control_.late;
        auto ticket = pending->ticket;
        if (control_.stale_generation)
            --ticket.generation;
        return detail::WaveNetChannelCompletion{
            GpuWaveNetBlockResult{pending->sequence + (control_.wrong_sequence ? 1 : 0),
                                  failed ? GpuWaveNetBlockStatus::ProviderFailed
                                         : GpuWaveNetBlockStatus::GpuDelivered,
                                  late},
            ticket};
    }
    bool lost() const noexcept override {
        return control_.lost;
    }
    bool set_completion_notifier(detail::SharedIoCompletionNotifier notifier) noexcept override {
        if (!control_.notifier_supported)
            return false;
        (notifier.notify != nullptr ? control_.notifier_installs : control_.notifier_clears) += 1;
        control_.notifier = notifier;
        return true;
    }
    bool release() noexcept override {
        ++control_.releases;
        return control_.release;
    }
    bool release_bounded(std::uint64_t budget_ns) noexcept override {
        control_.last_release_budget = budget_ns;
        if (control_.hang_release) {
            // A provider that never drains: it uses its whole budget and fails.
            std::this_thread::sleep_for(std::chrono::nanoseconds(budget_ns));
            return false;
        }
        return release();
    }
    bool recycle(std::uint64_t) noexcept override {
        ++control_.recycles;
        return control_.recyclable;
    }
    std::string provider_key() const override {
        return control_.pool_key;
    }

  private:
    struct Pending {
        std::array<float, 2> data{};
        std::uint64_t sequence = 0;
        int submit_index = 0;
        detail::SharedIoSubmissionTicket ticket{};
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
                .completion_service_wait_ns = wait_ns,
                // The mechanics tests pin the window to the ring (capacity - 1);
                // the time-based default has its own tests.
                .watchdog_blocks = lead + 2};
    }
};
struct Tweak {
    std::uint32_t lead = 1, capacity = 0, channels = 1;
    std::uint32_t watchdog_blocks = 0;
    bool trace = false;
    // Leave watchdog_blocks at zero: the node's time-based default.
    bool default_watchdog = false;
    // Zero selects the node's default depth.
    std::uint32_t max_inflight = 1;
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
        auto config = shape.config(tweak.lead, tweak.channels, 0, tweak.max_inflight);
        if (tweak.capacity != 0)
            config.capacity = tweak.capacity;
        config.watchdog_blocks = tweak.default_watchdog       ? 0
                                 : tweak.watchdog_blocks != 0 ? tweak.watchdog_blocks
                                                              : config.capacity - 1;
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

TEST_CASE("WaveNet default depth is two in flight when the slots and ring allow it",
          "[gpu_audio][wavenet][realtime][depth]") {
    // max_inflight = 0 resolves to two with the session's two slots and a ring
    // that holds two blocks past the lead, else one; an explicit value is
    // kept. The lead, and so the reported latency, does not change.
    struct Case {
        std::uint32_t max_inflight, capacity;
        int expected_depth;
    };
    for (const auto c : {Case{0, 4, 2}, Case{0, 2, 1}, Case{1, 4, 1}}) {
        CAPTURE(c.max_inflight, c.capacity);
        Harness h(Tweak{.lead = 1, .capacity = c.capacity, .max_inflight = c.max_inflight});
        // Nothing completes, so the worker admits up to its depth and stops.
        h.controls[0].hang = true;
        for (std::uint32_t block = 0; block + 1 < c.capacity; ++block)
            h.callback(float(block));
        h.service();
        h.service();
        CHECK(h.controls[0].submits == c.expected_depth);
        const auto status = checked_status(h.node);
        CHECK(status.prepared_lead_blocks == 1);
        CHECK(status.latency_samples == 2);
    }
}

TEST_CASE("WaveNet depth telemetry reports bounded multi-flight",
          "[gpu_audio][wavenet][realtime][depth]") {
    Shape shape;
    const auto run = [&](std::uint32_t capacity) {
        auto config = shape.config(1, 1, 0, capacity - 1);
        config.capacity = capacity;
        config.watchdog_blocks = capacity - 1;
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

TEST_CASE("WaveNet node whose GPU results all arrive late reads as not GPU-delivering",
          "[gpu_audio][wavenet][realtime][status]") {
    Harness h;
    detail::WaveNetRealtimeTestAccess::assume_authenticated(h.node);
    const auto run = [&h](int blocks) {
        for (int i = 0; i < blocks; ++i) {
            h.callback(float(i + 1));
            h.service();
            h.service();
        }
    };
    // Positive control: a provider that keeps up delivers every block.
    run(40);
    CHECK_FALSE(detail::WaveNetRealtimeTestAccess::gpu_lagging(h.node));
    auto healthy = checked_status(h.node);
    CHECK(healthy.provider_state == GpuAudioProviderState::Ready);
    CHECK(healthy.selected_engine == GpuAudioEngine::Gpu);
    // Stuck late: it keeps completing, too late for every consumer, so the
    // substitute carries the stream although no recovery cause latches.
    h.controls[0].late = true;
    run(40);
    CHECK(h.node.recovery_reason() == GpuAudioRecoveryReason::None);
    CHECK(detail::WaveNetRealtimeTestAccess::gpu_lagging(h.node));
    auto late = checked_status(h.node);
    CHECK(late.provider_state == GpuAudioProviderState::Degraded);
    // The harness delivers silence for a miss, and the node declares no fallback.
    CHECK(late.selected_engine != GpuAudioEngine::Gpu);
    // Recovery: once the GPU delivers most recent blocks again, it is no
    // longer lagging.
    h.controls[0].late = false;
    run(40);
    CHECK_FALSE(detail::WaveNetRealtimeTestAccess::gpu_lagging(h.node));
    CHECK(h.node.recovery_reason() == GpuAudioRecoveryReason::None);
    CHECK(checked_status(h.node).provider_state == GpuAudioProviderState::Ready);
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

TEST_CASE("WaveNet node registers its realtime path through the registry",
          "[gpu_audio][wavenet][registry]") {
    // Negative: an unprepared node requires the realtime path but has none.
    Shape shape;
    GpuWaveNetRealtimeNode unprepared(shape.config());
    CHECK(detail::requires_realtime_gpu_path(&unprepared));
    CHECK_FALSE(detail::realtime_gpu_node_path(&unprepared).active());
    CHECK(detail::realtime_gpu_provider(&unprepared) == GpuAudioProvider::Unknown);
    // Positive: prepared, it binds an active path. Injected test channels carry
    // no provider identity, so no GPU provider is claimed.
    Harness h(1);
    CHECK(detail::requires_realtime_gpu_path(&h.node));
    CHECK(detail::realtime_gpu_node_path(&h.node).active());
    CHECK(detail::realtime_gpu_provider(&h.node) == GpuAudioProvider::Unknown);
}

TEST_CASE("WaveNet rejects a completion carrying another generation's admission ticket",
          "[gpu_audio][wavenet][realtime][ticket]") {
    // Production path: the node's own collection, fed by a channel that hands
    // back each completion under the generation before the one that admitted
    // it, as a completion surviving an earlier epoch would.
    Harness h(1);
    h.controls[0].stale_generation = true;
    CHECK(h.callback(1) == detail::kRealtimeGpuPriming);
    h.service();
    h.service();
    CHECK(detail::WaveNetRealtimeTestAccess::stale_completions(h.node) >= 1);
    // The stale completion retires nothing: it is not delivered, and it is not
    // taken as a provider failure either.
    CHECK(h.node.recovery_reason() == GpuAudioRecoveryReason::None);
    CHECK(h.callback(2) != detail::kRealtimeGpuReady);

    // Positive control: the same channel returning the admitted ticket
    // completes the block, which the next callback delivers.
    Harness control(1);
    CHECK(control.callback(1) == detail::kRealtimeGpuPriming);
    control.service();
    control.service();
    CHECK(detail::WaveNetRealtimeTestAccess::stale_completions(control.node) == 0);
    CHECK(control.callback(2) == detail::kRealtimeGpuReady);
    CHECK(control.node.recovery_reason() == GpuAudioRecoveryReason::None);
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
    const auto status = checked_status(h.node);
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
    auto initial = checked_status(node);
    CHECK(initial.schema_version == GpuAudioStatus::kSchemaVersion);
    CHECK(initial.provider_state == GpuAudioProviderState::Uninitialized);
    CHECK_FALSE(initial.diagnostics_available);
    CHECK(initial.recovery_reason == GpuAudioRecoveryReason::None);
    CHECK(node.release());
    auto released = checked_status(node);
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
    const auto status = checked_status(h.node);
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
        CHECK(checked_status(h.node).provider_state ==
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

TEST_CASE("WaveNet transport status carries the node epoch and terminal counts",
          "[gpu_audio][wavenet][status][health]") {
    // Driven directly, outside any transport, the node's own status must
    // already be coherent: its terminal counts stay within its own delivery
    // counters. Only a transport that was bypassed would lag them.
    {
        Harness direct;
        for (int block = 0; block < 6; ++block) {
            direct.callback(float(block + 1));
            direct.service();
            direct.service();
        }
        const auto node_status = direct.node.status_snapshot();
        INFO(
            "node status error " << static_cast<int>(validate_gpu_audio_status(node_status).error));
        CHECK(validate_gpu_audio_status(node_status).accepted());
        CHECK(node_status.terminal_counts[static_cast<std::size_t>(
                  GpuAudioTerminalDisposition::GpuDelivered)] > 0);
    }
    Harness h;
    GpuAudioTransport transport;
    REQUIRE(transport.prepare(&h.node, {8}));
    constexpr auto delivered = static_cast<std::size_t>(GpuAudioTerminalDisposition::GpuDelivered);
    // Drive the blocks through the transport, so its delivery counters and the
    // node's terminal counts describe the same blocks; driving the node
    // directly would leave produced_blocks at zero and fail the validator's
    // terminal-count bound.
    std::array<float, 2> input{};
    std::array<float, 2> output{};
    const std::array<const float*, 1> input_channels{input.data()};
    const std::array<float*, 1> output_channels{output.data()};
    for (int block = 0; block < 6; ++block) {
        input.fill(float(block + 1));
        const pulp::audio::BufferView<const float> in_view(input_channels.data(), 1, 2);
        pulp::audio::BufferView<float> out_view(output_channels.data(), 1, 2);
        transport.process(in_view, out_view, 2);
        transport.pump();
        transport.pump();
    }
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
            const auto status = checked_status(h.node);
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
    CHECK(checked_status(h.node).provider_state == GpuAudioProviderState::Unavailable);
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
                 std::uint32_t resume_after = UINT32_MAX, std::uint32_t rounds = 16) {
    Harness h(tweak);
    for (int block = 0; block < 4; ++block) {
        h.callback(float(block));
        h.service();
    }
    HangRun run;
    // The block submitted by the last healthy pump is the first that hangs.
    run.hung_sequence = h.sequence - 1;
    h.controls[0].hang = true;
    for (std::uint32_t round = 0; round < rounds; ++round) {
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
    run.state = checked_status(h.node).provider_state;
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

TEST_CASE("WaveNet default watchdog window is 200 ms converted to callback blocks",
          "[gpu_audio][wavenet][realtime][hang]") {
    // 200 ms in blocks of 64, 128 and 256 frames: 138/69/35 at 44.1 kHz,
    // 150/75/38 at 48 kHz, 300/150/75 at 96 kHz. The ring grows to hold the
    // window, the lead and one more block; an explicit window leaves the
    // configured ring alone, and a larger ring keeps its capacity - 1 window.
    using detail::size_provider_watchdog;
    struct Rate {
        std::uint32_t sample_rate, blocks_64, blocks_128, blocks_256;
    };
    for (const auto rate :
         {Rate{44100, 138, 69, 35}, Rate{48000, 150, 75, 38}, Rate{96000, 300, 150, 75}}) {
        CAPTURE(rate.sample_rate);
        CHECK(size_provider_watchdog(8, 2, 0, 64, rate.sample_rate).window_blocks ==
              rate.blocks_64);
        CHECK(size_provider_watchdog(8, 2, 0, 128, rate.sample_rate).window_blocks ==
              rate.blocks_128);
        CHECK(size_provider_watchdog(8, 2, 0, 256, rate.sample_rate).window_blocks ==
              rate.blocks_256);
        CHECK(size_provider_watchdog(8, 2, 0, 64, rate.sample_rate).ring_blocks ==
              rate.blocks_64 + 3);
    }
    CHECK(size_provider_watchdog(8, 2, 3, 64, 48000).window_blocks == 3);
    CHECK(size_provider_watchdog(8, 2, 3, 64, 48000).ring_blocks == 8);
    CHECK(size_provider_watchdog(400, 2, 0, 256, 48000).window_blocks == 399);
    CHECK(size_provider_watchdog(400, 2, 0, 256, 48000).ring_blocks == 402);

    // The harness's 2-frame blocks make the default 4800 callback blocks. A
    // hang trips it at exactly that silence, as ProviderUnresponsive at the
    // hung block, and the grown ring never saturates first.
    const auto hang = run_hang({.default_watchdog = true}, 1, 1, UINT32_MAX, 6000);
    CHECK(hang.reason == GpuAudioRecoveryReason::ProviderUnresponsive);
    CHECK(hang.sequence == hang.hung_sequence);
    CHECK(hang.blocks_after_hang == 4800);
    CHECK(hang.state == GpuAudioProviderState::Unavailable);
    CHECK(hang.allocations == 0);
    // Positive control: the same harness trips an explicit 5-block window at
    // exactly 5 blocks, so only the default's size is under test above.
    const auto control = run_hang({.capacity = 16, .watchdog_blocks = 5}, 1, 1);
    CHECK(control.reason == GpuAudioRecoveryReason::ProviderUnresponsive);
    CHECK(control.blocks_after_hang == 5);
    // A stall that ends one block short of the window latches nothing.
    const auto stall = run_hang({.default_watchdog = true}, 1, 1, 4799, 6000);
    CHECK(stall.reason == GpuAudioRecoveryReason::None);
    CHECK(stall.state != GpuAudioProviderState::Unavailable);
}

TEST_CASE("WaveNet ring saturated behind a hung provider is reported as the hang",
          "[gpu_audio][wavenet][realtime][hang]") {
    // A window wider than the ring: the input ring fills first. The oldest
    // in-flight block has seen no completion since its admission, so the
    // saturation is the hang's symptom and the node reports the hang.
    const auto run = run_hang({.capacity = 4, .watchdog_blocks = 16}, 1, 1);
    CHECK(run.reason == GpuAudioRecoveryReason::ProviderUnresponsive);
    CHECK(run.sequence == run.hung_sequence);
    CHECK(run.state == GpuAudioProviderState::Unavailable);
    CHECK(run.blocks_after_hang < 16);
    CHECK(run.allocations == 0);
    // Positive control: the same hang with the window inside the ring trips
    // the watchdog itself, at the same block.
    const auto control = run_hang({.capacity = 4, .watchdog_blocks = 2}, 1, 1);
    CHECK(control.reason == GpuAudioRecoveryReason::ProviderUnresponsive);
    CHECK(control.sequence == control.hung_sequence);
}

TEST_CASE("WaveNet ring saturated by a provider slower than the callback stays saturation",
          "[gpu_audio][wavenet][realtime][hang]") {
    // Two callbacks per pump: the provider completes every block, but the
    // backlog grows by one each round until the ring saturates. The block in
    // flight at that moment has not completed yet, and its own silence is a
    // couple of callbacks, far short of the ring: saturation, not a hang.
    Harness h(Tweak{.lead = 1, .capacity = 8, .watchdog_blocks = 16});
    for (int block = 0; block < 4; ++block) {
        h.callback(float(block));
        h.service();
    }
    bool saturated = false;
    for (int round = 0; round < 64 && !saturated; ++round) {
        h.callback(float(h.sequence));
        h.callback(float(h.sequence));
        saturated = h.node.recovery_reason() == GpuAudioRecoveryReason::InputSaturated;
        // The pump that classifies the saturation finds its block in flight.
        h.controls[0].hang = saturated;
        h.service();
        h.controls[0].hang = false;
    }
    REQUIRE(saturated);
    for (int pump = 0; pump < 4; ++pump)
        h.service();
    CHECK(h.node.recovery_reason() == GpuAudioRecoveryReason::InputSaturated);
    CHECK(checked_status(h.node).provider_state != GpuAudioProviderState::Unavailable);

    // Positive control: the same ring and window behind a true hang. The ring
    // fills during the hung block's silence, so the saturation is the hang.
    const auto hang = run_hang({.capacity = 8, .watchdog_blocks = 16}, 1, 1);
    CHECK(hang.reason == GpuAudioRecoveryReason::ProviderUnresponsive);
    CHECK(hang.sequence == hang.hung_sequence);
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
        const auto status = checked_status(h.node);
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
        const auto status = checked_status(h.node);
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
        ++drains;
        if (hang_drain) {
            // A provider whose work never retires: wait out the budget, then
            // fail closed so the caller keeps ownership (quarantine).
            const auto budget =
                std::chrono::nanoseconds(drain_budget_ns != 0 ? drain_budget_ns : 2'000'000'000);
            const auto until = std::chrono::steady_clock::now() + budget;
            while (std::chrono::steady_clock::now() < until)
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            return false;
        }
        for (std::size_t i = 0; i < deferred_count_; ++i)
            (void)deferred_[i].inbox->push(deferred_[i].token, CompletionStatus::RetiredSuccess);
        deferred_count_ = 0;
        return true;
    }
    void set_drain_budget_ns(std::uint64_t budget_ns) noexcept override {
        drain_budget_ns = budget_ns;
    }
    bool hang_drain = false;
    std::uint64_t drain_budget_ns = 0;
    int drains = 0;

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
        const auto status = checked_status(node);
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
        CHECK(checked_status(h.node).provider_state == GpuAudioProviderState::Degraded);
        h.controls[0].lost = true;
        h.service();
        CHECK(h.node.recovery_reason() == GpuAudioRecoveryReason::ProviderLost);
        CHECK(h.node.recovery_sequence() == 0);
        CHECK(checked_status(h.node).provider_state == GpuAudioProviderState::Unavailable);
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
        CHECK(checked_status(h.node).provider_state == GpuAudioProviderState::Degraded);
        h.controls[0].lost = true;
        h.service();
        CHECK(h.node.recovery_reason() == GpuAudioRecoveryReason::InputSaturated);
        CHECK(checked_status(h.node).provider_state == GpuAudioProviderState::Unavailable);
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
    auto status = checked_status(h.node);
    CHECK(status.schema_version == GpuAudioStatus::kSchemaVersion);
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
    status = checked_status(h.node);
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
    status = checked_status(h.node);
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
    const auto status = checked_status(h.node);
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
        // The channel reads `control` when the node releases it, so `control`
        // must outlive `node`.
        Control control;
        GpuWaveNetRealtimeNode node(config);
        std::vector<std::unique_ptr<detail::WaveNetRealtimeChannel>> providers;
        providers.push_back(std::make_unique<Channel>(control));
        REQUIRE(detail::WaveNetRealtimeTestAccess::prepare(node, std::move(providers)));
        CHECK(checked_status(node).completion_mode == mode);
    }
}

// Bounds for the concurrent-worker smoke test below. The race it samples is
// pinned deterministically by "WaveNet watchdog reads a position behind the
// oldest admission as no silence".
constexpr int kWatchdogRaceBlocks = 4000;
constexpr int kWatchdogRaceMinBlocks = 1000;

TEST_CASE("WaveNet watchdog reads a position behind the oldest admission as no silence",
          "[gpu_audio][wavenet][realtime][hang]") {
    // The worker can admit a block after the callback publishes it but before
    // the callback advances its position. Hold the position there, behind the
    // oldest admission: the silence is zero, never an unsigned wrap.
    for (const bool held : {true, false}) {
        CAPTURE(held);
        Harness h(Tweak{.lead = 1, .capacity = 16, .watchdog_blocks = 3});
        for (int block = 0; block < 4; ++block) {
            h.callback(float(block));
            h.service();
        }
        // Block 3, admitted by the last healthy pump, never completes.
        h.controls[0].hang = true;
        h.callback(4.0f);
        h.service();
        if (held) {
            // As if the callback that published block 3 were preempted before
            // advancing its position: the position is the oldest admission's
            // own sequence, one short of where its silence starts.
            detail::WaveNetRealtimeTestAccess::hold_callback_position(h.node, 3);
            for (int pump = 0; pump < 8; ++pump)
                h.service();
            CHECK(h.node.recovery_reason() == GpuAudioRecoveryReason::None);
        } else {
            // Positive control: the same hang, with the callback advancing,
            // trips the watchdog once three blocks pass in silence.
            for (int block = 0; block < 4; ++block) {
                h.callback(float(5 + block));
                h.service();
            }
            CHECK(h.node.recovery_reason() == GpuAudioRecoveryReason::ProviderUnresponsive);
        }
    }
}

TEST_CASE("WaveNet watchdog stays quiet while a concurrent worker admits mid-callback",
          "[gpu_audio][wavenet][realtime][hang]") {
    // The worker can admit a block after the callback publishes it but before
    // the callback advances its position, so the watchdog must not read that
    // gap as silence. A healthy provider on a concurrent worker never trips it.
    // An unsigned wrap of the silence count would exceed any window.
    Harness h(Tweak{.lead = 2, .capacity = 64, .watchdog_blocks = 32});
    std::atomic<bool> done{false};
    std::atomic<std::uint64_t> pumps{0};
    // The worker spins concurrently with the callback, so it still admits
    // blocks mid-callback. Completing on every other pump leaves pumps that see
    // work in flight and no completion, which is when the watchdog measures
    // silence.
    std::thread worker([&] {
        for (std::uint64_t pump = 0; !done.load(std::memory_order_acquire); ++pump) {
            h.controls[0].complete = pump % 2 == 0;
            (void)h.path.service(h.path.context, 0);
            pumps.store(pump + 1, std::memory_order_release);
        }
    });
    // Paced on the worker's acknowledgement rather than wall-clock time: each
    // callback waits until the worker has pumped twice since the previous one,
    // so at least one completing pump follows every block. A descheduled worker
    // then delays the callback instead of letting silence build up behind it.
    // The run is bounded by blocks and by one overall deadline, so a slow
    // sanitizer build ends with a verdict rather than a ctest timeout.
    constexpr int kBlocks = kWatchdogRaceBlocks;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    int acknowledged = 0;
    while (acknowledged < kBlocks && h.node.recovery_reason() == GpuAudioRecoveryReason::None &&
           std::chrono::steady_clock::now() < deadline) {
        const auto seen = pumps.load(std::memory_order_acquire);
        (void)h.callback(float(acknowledged));
        while (pumps.load(std::memory_order_acquire) < seen + 2 &&
               std::chrono::steady_clock::now() < deadline)
            std::this_thread::yield();
        if (pumps.load(std::memory_order_acquire) >= seen + 2)
            ++acknowledged;
    }
    done.store(true, std::memory_order_release);
    worker.join();
    INFO("acknowledged blocks " << acknowledged << " reason " << int(h.node.recovery_reason()));
    // Enough acknowledged blocks for the mid-callback race to have happened
    // many times; the watchdog must not have tripped on any of them.
    CHECK(acknowledged >= kWatchdogRaceMinBlocks);
    CHECK(h.node.recovery_reason() != GpuAudioRecoveryReason::ProviderUnresponsive);
}

TEST_CASE("WaveNet status names the engine producing audio and counts fallback once delivered",
          "[gpu_audio][wavenet][diagnostic]") {
    class ShadowNode final : public GpuWaveNetRealtimeNode {
      public:
        using GpuWaveNetRealtimeNode::GpuWaveNetRealtimeNode;
        bool cpu_fallback_prepared() const noexcept override {
            return true;
        }
        void prime_fallback(const pulp::audio::BufferView<const float>&,
                            std::uint32_t) noexcept override {}
        void process_cpu_fallback(const pulp::audio::BufferView<const float>&,
                                  pulp::audio::BufferView<float>& out,
                                  std::uint32_t n) noexcept override {
            std::fill_n(out.channel_ptr(0), n, 0.5f);
        }
    };
    const auto count = [](const GpuAudioStatus& status, GpuAudioTerminalDisposition disposition) {
        return status.terminal_counts[static_cast<std::size_t>(disposition)];
    };
    Shape shape;
    auto config = shape.config(2);
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
    const auto block = [&] {
        transport.process(in, out, 2);
        transport.pump();
        transport.pump();
    };
    for (int i = 0; i < 4; ++i)
        block();
    auto status = checked_status(node);
    CHECK(status.provider_state == GpuAudioProviderState::Degraded); // fake, unauthenticated
    CHECK(status.selected_engine == GpuAudioEngine::Cpu);
    CHECK(count(status, GpuAudioTerminalDisposition::GpuDelivered) == status.produced_blocks);

    // The next submitted block fails. Its consumer is two callbacks away.
    control.fail = true;
    transport.process(in, out, 2);
    transport.pump();
    transport.pump();
    status = checked_status(node);
    REQUIRE(status.recovery_reason == GpuAudioRecoveryReason::ProviderFailure);
    CHECK(status.selected_engine == GpuAudioEngine::Cpu);
    CHECK(count(status, GpuAudioTerminalDisposition::CpuFallback) == 0);
    for (int i = 0; i < 3; ++i)
        block();
    status = checked_status(node);
    CHECK(count(status, GpuAudioTerminalDisposition::CpuFallback) == 1);
    CHECK(status.fallback_blocks >= 1);
}

TEST_CASE("WaveNet completions seen only in the bounded-wait tail pass keep the watchdog quiet",
          "[gpu_audio][wavenet][realtime][hang]") {
    // Every completion lands in the second service pass of a pump, after
    // admission. Every fourth block is slow and finishes three blocks later,
    // while the blocks behind it complete at once, so the provider completes
    // work on every pump and is responsive. The slow block holds the oldest
    // in-flight position, so only the tail-pass completions show progress.
    Shape shape;
    auto config = shape.config(3, 1, 500'000, 4);
    config.capacity = 8;
    config.watchdog_blocks = 3;
    // The channel reads `control` when the node releases it, so `control`
    // must outlive `node`.
    Control control;
    GpuWaveNetRealtimeNode node(config);
    control.tail_only = true;
    control.slow_every = 4;
    control.completion_lag = 3;
    std::vector<std::unique_ptr<detail::WaveNetRealtimeChannel>> providers;
    providers.push_back(std::make_unique<Channel>(control));
    REQUIRE(detail::WaveNetRealtimeTestAccess::prepare(node, std::move(providers)));
    auto path = detail::realtime_gpu_node_path(&node);
    std::array<float, 2> input{}, output{};
    const float* ip = input.data();
    float* op = output.data();
    pulp::audio::BufferView<const float> in(&ip, 1, 2);
    pulp::audio::BufferView<float> out(&op, 1, 2);
    for (std::uint64_t sequence = 0; sequence < 32; ++sequence) {
        input.fill(float(sequence + 1));
        const auto status = path.process(path.context, in, out, 2, sequence, true, 0);
        path.delivered(
            path.context, sequence,
            static_cast<std::uint8_t>(status == detail::kRealtimeGpuReady
                                          ? detail::SharedIoDeliveryDisposition::GpuDelivered
                                      : status == detail::kRealtimeGpuPriming
                                          ? detail::SharedIoDeliveryDisposition::Priming
                                          : detail::SharedIoDeliveryDisposition::SilenceDelivered),
            0, 0);
        (void)path.service(path.context, 0);
    }
    CHECK(control.first_pass_completions == 0);
    CHECK(control.tail_pass_completions >= 20);
    CHECK(node.recovery_reason() == GpuAudioRecoveryReason::None);
    CHECK(checked_status(node).provider_state != GpuAudioProviderState::Unavailable);
}

TEST_CASE("WaveNet starved worker is not mistaken for an unresponsive provider",
          "[gpu_audio][wavenet][realtime][hang]") {
    // The fake behaves like a healthy provider: work finishes right away and
    // is reported on the next service. The worker is then descheduled for far
    // longer than the watchdog window. The first service after the stall must
    // collect what finished before it judges silence.
    struct Case {
        std::uint32_t capacity, watchdog_blocks;
        GpuAudioRecoveryReason expected;
    };
    for (const auto c : {Case{128, 8, GpuAudioRecoveryReason::None},
                         // A ring shorter than the stall overflows first, which
                         // is the true cause.
                         Case{4, 3, GpuAudioRecoveryReason::InputSaturated}}) {
        CAPTURE(c.capacity, c.watchdog_blocks);
        Harness h(Tweak{.lead = 1, .capacity = c.capacity, .watchdog_blocks = c.watchdog_blocks});
        for (int block = 0; block < 4; ++block) {
            h.callback(float(block));
            h.service();
        }
        // One block is in flight and has already finished on the provider.
        for (int block = 0; block < 100; ++block)
            h.callback(float(block));
        for (int pump = 0; pump < 200; ++pump)
            h.service();
        CHECK(h.node.recovery_reason() == c.expected);
        CHECK(h.node.recovery_reason() != GpuAudioRecoveryReason::ProviderUnresponsive);
        CHECK(checked_status(h.node).provider_state != GpuAudioProviderState::Unavailable);
    }
}

namespace {
// A re-acquire's replacement channels: every one shares `control`.
struct ChannelFactory {
    Control* control = nullptr;
    int created = 0;
    bool fail = false;
    // Optional: the first `first_count` replacements use `first_control`, and
    // the first creation waits `first_delay` (inside the re-acquire).
    Control* first_control = nullptr;
    int first_count = 0;
    std::chrono::milliseconds first_delay{0};
    static std::unique_ptr<detail::WaveNetRealtimeChannel> make(void* context) noexcept {
        auto& self = *static_cast<ChannelFactory*>(context);
        if (self.fail)
            return nullptr;
        if (self.created == 0 && self.first_delay.count() > 0)
            std::this_thread::sleep_for(self.first_delay);
        const bool first = self.first_control != nullptr && self.created < self.first_count;
        ++self.created;
        return std::make_unique<Channel>(first ? *self.first_control : *self.control);
    }
    void install(GpuWaveNetRealtimeNode& node) {
        detail::WaveNetRealtimeTestAccess::set_channel_factory(node, &ChannelFactory::make, this);
    }
};
} // namespace

TEST_CASE("WaveNet re-acquire replaces a lost provider in a new epoch on the same stream",
          "[gpu_audio][wavenet][realtime][reacquire]") {
    Harness h;
    ChannelFactory factory{.control = &h.controls[1]};
    factory.install(h.node);
    REQUIRE(detail::WaveNetRealtimeTestAccess::warmup_blocks(h.node) == 1);
    for (int block = 0; block < 3; ++block) {
        h.callback(float(h.sequence + 1));
        h.service();
        h.service();
    }
    h.controls[0].lost = true;
    h.service();
    REQUIRE(h.node.recovery_reason() == GpuAudioRecoveryReason::ProviderLost);
    CHECK(h.callback(float(h.sequence + 1)) == detail::kRealtimeGpuMissed);
    const auto old_receives = h.controls[0].receives;

    REQUIRE(h.path.reprepare(h.path.context, 1'000'000'000));
    CHECK(factory.created == 1);
    // The callback adopts the new epoch at its next block.
    const auto adopted = h.sequence;
    std::vector<std::uint8_t> statuses;
    for (int block = 0; block < 6; ++block) {
        const auto status = h.callback(float(h.sequence + 1));
        statuses.push_back(status);
        if (status == detail::kRealtimeGpuReady)
            CHECK(h.outa[0] == float(h.sequence - 1 - 1 + 1)); // the block one lead earlier
        h.service();
        h.service();
    }
    // Adoption block (due: old epoch) and the warmup block miss; then the GPU.
    CHECK(statuses[0] == detail::kRealtimeGpuMissed);
    CHECK(statuses[1] == detail::kRealtimeGpuMissed);
    for (std::size_t i = 2; i < statuses.size(); ++i)
        CHECK(statuses[i] == detail::kRealtimeGpuReady);
    CHECK(h.controls[0].receives == old_receives); // the old provider is never serviced again
    CHECK(h.controls[1].submits >= 4);
    const auto status = checked_status(h.node);
    CHECK(status.epoch == 2);
    CHECK(status.delivery_epoch_active);
    CHECK(status.recovery_reason == GpuAudioRecoveryReason::ProviderReacquired);
    CHECK(status.recovery_sequence == adopted + 1);
    CHECK(status.provider_state != GpuAudioProviderState::Unavailable);
    CHECK(
        status.terminal_counts[static_cast<std::size_t>(GpuAudioTerminalDisposition::Cancelled)] >=
        1);
}

TEST_CASE("WaveNet re-acquire that delivers again clears the stuck-late state",
          "[gpu_audio][wavenet][realtime][reacquire][status]") {
    Harness h;
    detail::WaveNetRealtimeTestAccess::assume_authenticated(h.node);
    ChannelFactory factory{.control = &h.controls[1]};
    factory.install(h.node);
    const auto run = [&h](int blocks) {
        for (int i = 0; i < blocks; ++i) {
            h.callback(float(h.sequence + 1));
            h.service();
            h.service();
        }
    };
    run(10);
    CHECK(checked_status(h.node).provider_state == GpuAudioProviderState::Ready);
    // Stuck late, then the provider is lost.
    h.controls[0].late = true;
    run(40);
    REQUIRE(detail::WaveNetRealtimeTestAccess::gpu_lagging(h.node));
    CHECK(checked_status(h.node).provider_state == GpuAudioProviderState::Degraded);
    h.controls[0].lost = true;
    h.service();
    REQUIRE(h.node.recovery_reason() == GpuAudioRecoveryReason::ProviderLost);
    REQUIRE(h.path.reprepare(h.path.context, 1'000'000'000));
    // Adoption and warmup miss, then the new provider's GPU delivers: a few
    // of its blocks read Ready, though most of the last 32 were not GPU.
    run(8);
    CHECK_FALSE(detail::WaveNetRealtimeTestAccess::gpu_lagging(h.node));
    const auto status = checked_status(h.node);
    CHECK(status.recovery_reason == GpuAudioRecoveryReason::ProviderReacquired);
    CHECK(status.provider_state == GpuAudioProviderState::Ready);
    CHECK(status.selected_engine == GpuAudioEngine::Gpu);
}

TEST_CASE("WaveNet re-acquire routes completion events to the replacement provider",
          "[gpu_audio][wavenet][realtime][reacquire]") {
    Harness h;
    h.controls[0].notifier_supported = true;
    h.controls[1].notifier_supported = true;
    ChannelFactory factory{.control = &h.controls[1]};
    factory.install(h.node);
    int wakes = 0;
    const detail::SharedIoCompletionNotifier notifier{
        [](void* context) noexcept { ++*static_cast<int*>(context); }, &wakes};
    REQUIRE(h.path.set_completion_notifier(h.path.context, notifier));
    CHECK(h.controls[0].notifier_installs == 1);
    for (int block = 0; block < 3; ++block) {
        h.callback(float(h.sequence + 1));
        h.service();
    }
    h.controls[0].lost = true;
    h.service();
    REQUIRE(h.path.reprepare(h.path.context, 1'000'000'000));
    // The replaced provider can no longer wake the transport; the new one can.
    CHECK(h.controls[0].notifier_clears == 1);
    CHECK(h.controls[0].notifier.notify == nullptr);
    CHECK(h.controls[1].notifier_installs == 1);
    REQUIRE(h.controls[1].notifier.notify != nullptr);
    h.controls[1].notifier.notify(h.controls[1].notifier.context);
    CHECK(wakes == 1);
    // Positive control for the clear: the transport's release clears it too.
    REQUIRE(h.path.set_completion_notifier(h.path.context, {}));
    CHECK(h.controls[1].notifier.notify == nullptr);
}

TEST_CASE("WaveNet re-acquire quarantines a provider that cannot drain",
          "[gpu_audio][wavenet][realtime][reacquire]") {
    Harness h;
    ChannelFactory factory{.control = &h.controls[1]};
    factory.install(h.node);
    h.callback(1);
    h.service();
    h.controls[0].hang = true;
    h.controls[0].release = false;
    for (int block = 0; block < 4 && h.node.recovery_reason() == GpuAudioRecoveryReason::None;
         ++block) {
        h.callback(float(block));
        h.service();
    }
    REQUIRE(h.node.recovery_reason() == GpuAudioRecoveryReason::ProviderUnresponsive);
    REQUIRE(h.path.reprepare(h.path.context, 1'000'000));
    CHECK(detail::WaveNetRealtimeTestAccess::quarantined_channels(h.node) == 1);
    for (int block = 0; block < 4; ++block) {
        h.callback(float(block));
        h.service();
        h.service();
    }
    CHECK(checked_status(h.node).recovery_reason == GpuAudioRecoveryReason::ProviderReacquired);
    // Teardown keeps retrying the quarantined provider; ownership is kept until
    // it drains.
    CHECK_FALSE(h.node.release());
    h.controls[0].release = true;
    CHECK(h.node.release());
    CHECK(detail::WaveNetRealtimeTestAccess::quarantined_channels(h.node) == 0);
}

TEST_CASE("WaveNet re-acquire is refused unless a live provider fault fenced the node",
          "[gpu_audio][wavenet][realtime][reacquire]") {
    {
        Harness h;
        ChannelFactory factory{.control = &h.controls[1]};
        factory.install(h.node);
        h.callback(1);
        h.service();
        CHECK_FALSE(h.path.reprepare(h.path.context, 1'000'000)); // not fenced
        REQUIRE(h.path.fence(h.path.context));
        CHECK_FALSE(h.path.reprepare(h.path.context, 1'000'000)); // offline fence
        CHECK(factory.created == 0);
    }
    {
        Harness h;
        ChannelFactory factory{.control = &h.controls[1], .fail = true};
        factory.install(h.node);
        h.callback(1);
        h.service();
        h.controls[0].lost = true;
        h.service();
        CHECK_FALSE(h.path.reprepare(h.path.context, 1'000'000)); // no new provider
        CHECK(h.node.fenced());
        factory.fail = false;
        CHECK(h.path.reprepare(h.path.context, 1'000'000)); // a later attempt succeeds
    }
}

TEST_CASE("Stamped bridge live epoch handoff keeps the stream and refuses stale work",
          "[gpu_audio][wavenet][reacquire]") {
    using Bridge = detail::SharedIoStampedBridge;
    const auto drive = [](Bridge& bridge, std::uint64_t sequence) {
        std::array<float, 2> input{float(sequence), float(sequence)}, output{};
        auto callback = bridge.begin_callback(input, sequence, 0);
        (void)bridge.consume_output(callback, output);
        return callback;
    };
    SECTION("the callback adopts the requested epoch at its next block") {
        Bridge bridge;
        REQUIRE(
            bridge.prepare({.capacity = 4, .channels = 1, .block_size = 2, .lead_blocks = 1}, 1));
        CHECK(drive(bridge, 0).stamp == Bridge::Stamp{1, 0});
        bridge.request_recovery(detail::SharedIoRecoveryReason::ProviderLost, 0);
        CHECK(drive(bridge, 1).stamp.epoch == 0);
        // A completion from the old epoch can never be published.
        const std::array<float, 2> stale{9, 9};
        CHECK(bridge.publish_output({1, 0}, stale) == Bridge::Publication::Invalid);
        REQUIRE(bridge.request_epoch(2));
        CHECK(bridge.recovery_reason() == detail::SharedIoRecoveryReason::None);
        CHECK(bridge.delivery_epoch() == 0); // nothing opens until the callback adopts
        CHECK(drive(bridge, 2).stamp == Bridge::Stamp{2, 2});
        CHECK(bridge.delivery_epoch() == 2);
        CHECK(bridge.publish_output({1, 1}, stale) == Bridge::Publication::Invalid);
        CHECK_FALSE(bridge.request_epoch(3)); // only while delivery is suspended
    }
    SECTION("a recovery request before adoption wins") {
        Bridge bridge;
        REQUIRE(
            bridge.prepare({.capacity = 4, .channels = 1, .block_size = 2, .lead_blocks = 1}, 1));
        bridge.request_recovery(detail::SharedIoRecoveryReason::ProviderLost, 0);
        REQUIRE(bridge.request_epoch(2));
        bridge.request_recovery(detail::SharedIoRecoveryReason::ProviderFailure, 1);
        CHECK(drive(bridge, 0).stamp.epoch == 0);
        CHECK(bridge.delivery_epoch() == 0);
        CHECK(bridge.recovery_reason() == detail::SharedIoRecoveryReason::ProviderFailure);
    }
}

TEST_CASE("WaveNet bounded release returns within budget against a provider that never drains",
          "[gpu_audio][wavenet][realtime][teardown]") {
    Shape shape;
    GpuWaveNetRealtimeNode node(shape.config(1));
    auto provider = std::make_unique<SlowProvider>();
    auto* hung = provider.get();
    auto session = std::make_unique<detail::SharedIoProgramSession>();
    REQUIRE(session->prepare({std::move(provider), std::make_unique<PassthroughProgram>()},
                             {.slots = 2,
                              .input_bytes_per_slot = 2 * sizeof(float),
                              .output_bytes_per_slot = 2 * sizeof(float)}));
    std::vector<std::unique_ptr<detail::WaveNetRealtimeChannel>> channels;
    channels.push_back(std::make_unique<pulp::test::ProgramSessionChannel>(std::move(session)));
    REQUIRE(detail::WaveNetRealtimeTestAccess::prepare(node, std::move(channels)));
    hung->hang_drain = true;
    const auto started = std::chrono::steady_clock::now();
    CHECK_FALSE(node.release_bounded(250'000'000));
    const auto elapsed = std::chrono::steady_clock::now() - started;
    CHECK(elapsed <= std::chrono::milliseconds(500));
    CHECK(elapsed >= std::chrono::milliseconds(200));
    // Ownership is kept; once the provider can drain, release completes.
    hung->hang_drain = false;
    CHECK(node.release());
}

namespace {
// Identity model on both sides: the fake GPU channel returns its input, and
// the CPU shadow returns the input one block late, as the GPU path delivers it.
class IdentityShadowNode final : public GpuWaveNetRealtimeNode {
  public:
    using GpuWaveNetRealtimeNode::GpuWaveNetRealtimeNode;
    bool cpu_fallback_prepared() const noexcept override {
        return true;
    }
    void prime_fallback(const pulp::audio::BufferView<const float>& in,
                        std::uint32_t n) noexcept override {
        due = previous;
        std::copy_n(in.channel_ptr(0), n, previous.data());
    }
    void process_cpu_fallback(const pulp::audio::BufferView<const float>&,
                              pulp::audio::BufferView<float>& out,
                              std::uint32_t n) noexcept override {
        std::copy_n(due.data(), n, out.channel_ptr(0));
    }
    std::array<float, 2> previous{}, due{};
};
enum class ReacquireFault { Lose, Hang };
struct ReacquireRun {
    GpuAudioStatus status;
    GpuAudioTransport::DeliverySnapshot delivery;
    std::size_t discontinuities = 0, allocations = 0;
    int created = 0;
};
struct ReacquireOptions {
    bool factory_fails = false;
    // Replacements that are lost from the start: their epochs fail before
    // delivering. `lost_replacements` < 0 means every replacement.
    int lost_replacements = 0;
    std::uint64_t backoff_ns = 0;
    std::chrono::milliseconds first_replacement_delay{0};
};
ReacquireRun run_reacquire(ReacquireFault fault, GpuAudioRecoveryPolicy policy,
                           bool factory_fails = false, ReacquireOptions options = {}) {
    options.factory_fails = options.factory_fails || factory_fails;
    Shape shape;
    auto config = shape.config(1);
    config.miss_policy = MissPolicy::CpuFallback;
    config.supports_cpu_fallback = true;
    // Declared first: the node's destructor releases channels that use them.
    std::array<Control, 3> controls{};
    controls[2].lost = true;
    IdentityShadowNode node(config);
    std::vector<std::unique_ptr<detail::WaveNetRealtimeChannel>> providers;
    providers.push_back(std::make_unique<Channel>(controls[0]));
    REQUIRE(detail::WaveNetRealtimeTestAccess::prepare(node, std::move(providers)));
    ChannelFactory factory{.control = &controls[1],
                           .fail = options.factory_fails,
                           .first_control = &controls[2],
                           .first_count =
                               options.lost_replacements < 0 ? INT_MAX : options.lost_replacements,
                           .first_delay = options.first_replacement_delay};
    factory.install(node);
    GpuAudioTransport transport;
    REQUIRE(transport.prepare(
        &node, {.recovery_policy = policy, .reacquire_backoff_ns = options.backoff_ns}));
    std::array<float, 2> input{}, output{};
    const float* ip = input.data();
    float* op = output.data();
    pulp::audio::BufferView<const float> in(&ip, 1, 2);
    pulp::audio::BufferView<float> out(&op, 1, 2);
    ReacquireRun run;
    for (int block = 0; block < 40; ++block) {
        if (block == 10) {
            controls[0].lost = fault == ReacquireFault::Lose;
            controls[0].hang = fault == ReacquireFault::Hang;
        }
        input.fill(float(block + 1));
        {
            pulp::test::RtAllocationProbe probe;
            transport.process(in, out, 2);
            run.allocations += probe.allocation_count();
        }
        // Continuity null: every block after priming is the input one block
        // earlier, whether the GPU, the fallback, or the re-acquired GPU made it.
        if (block > 0 && output[0] != float(block))
            ++run.discontinuities;
        transport.pump();
        transport.pump();
    }
    run.status = transport.status_snapshot();
    INFO("validate_gpu_audio_status error "
         << static_cast<int>(validate_gpu_audio_status(run.status).error));
    CHECK(validate_gpu_audio_status(run.status).accepted());
    run.delivery = transport.delivery_snapshot();
    run.created = factory.created;
    transport.release();
    return run;
}
} // namespace

TEST_CASE("Transport AutoReacquire returns a lost or hung provider to GPU delivery",
          "[gpu_audio][wavenet][realtime][reacquire]") {
    for (const auto fault : {ReacquireFault::Lose, ReacquireFault::Hang}) {
        CAPTURE(static_cast<int>(fault));
        const auto run = run_reacquire(fault, GpuAudioRecoveryPolicy::AutoReacquire);
        CHECK(run.discontinuities == 0);
        CHECK(run.allocations == 0);
        CHECK(run.created == 1);
        CHECK(run.status.reacquire_attempts == 1);
        CHECK(run.status.reacquire_successes == 1);
        CHECK(run.status.last_reacquire_to_gpu_ns > 0);
        CHECK(run.status.recovery_reason == GpuAudioRecoveryReason::ProviderReacquired);
        CHECK(run.status.epoch == 2);
        CHECK(run.status.delivery_epoch_active);
        CHECK(run.delivery.silence_blocks == 0);
        CHECK(run.delivery.cpu_fallback_blocks > 0);
        // GPU blocks after the outage: the run ends well past the warmup.
        CHECK(run.delivery.gpu_blocks > 20);
    }
}

TEST_CASE("Transport recovery stays on the fallback under Manual or a failing re-acquire",
          "[gpu_audio][wavenet][realtime][reacquire]") {
    const auto manual = run_reacquire(ReacquireFault::Lose, GpuAudioRecoveryPolicy::Manual);
    CHECK(manual.discontinuities == 0);
    CHECK(manual.status.reacquire_attempts == 0);
    CHECK(manual.status.recovery_reason == GpuAudioRecoveryReason::ProviderLost);
    CHECK(manual.status.provider_state == GpuAudioProviderState::Unavailable);

    const auto failing =
        run_reacquire(ReacquireFault::Lose, GpuAudioRecoveryPolicy::AutoReacquire, true);
    CHECK(failing.discontinuities == 0);
    CHECK(failing.status.reacquire_attempts == 3); // the configured maximum
    CHECK(failing.status.reacquire_successes == 0);
    CHECK(failing.status.recovery_reason == GpuAudioRecoveryReason::ProviderLost);
    CHECK(failing.delivery.silence_blocks == 0);
}

TEST_CASE("A re-acquire is informational in node health", "[gpu_audio][wavenet][reacquire]") {
    GpuAudioStatus status;
    status.provider_state = GpuAudioProviderState::Ready;
    status.recovery_reason = GpuAudioRecoveryReason::ProviderReacquired;
    status.recovery_sequence = 12;
    status.epoch = 2;
    status.delivery_epoch_active = true;
    const auto health = detail::health_from_status(status);
    CHECK_FALSE(health.fenced);
    CHECK_FALSE(health.device_lost);
    CHECK(detail::provider_state_for_health(health) == GpuAudioProviderState::Ready);
    status.recovery_reason = GpuAudioRecoveryReason::ProviderFailure;
    CHECK(detail::provider_state_for_health(detail::health_from_status(status)) ==
          GpuAudioProviderState::Degraded);
}

TEST_CASE("Provider pool keys come from provider identity and never cross provider kinds",
          "[gpu_audio][wavenet][pool]") {
    const auto identity = [](GpuAudioProvider kind, const char* backend, const char* revision) {
        detail::SharedIoProviderIdentity id;
        id.provider = kind;
        id.authenticated = true;
        id.adapter_backend = backend;
        id.provider_revision = revision;
        return id;
    };
    const auto dawn_key =
        detail::provider_pool_key(identity(GpuAudioProvider::Dawn, "metal", "ada56770"));
    // Same backend and revision, different provider kind.
    const auto cpu_key =
        detail::provider_pool_key(identity(GpuAudioProvider::Cpu, "metal", "ada56770"));
    const auto dawn_unpinned =
        detail::provider_pool_key(identity(GpuAudioProvider::Dawn, "metal", ""));
    const auto dawn_other =
        detail::provider_pool_key(identity(GpuAudioProvider::Dawn, "metal", "0000000"));
    CHECK(detail::provider_pool_key_matches(dawn_key, dawn_key));
    // A different kind never matches, pinned or not.
    CHECK_FALSE(detail::provider_pool_key_matches(cpu_key, dawn_key));
    CHECK_FALSE(detail::provider_pool_key_matches(cpu_key, dawn_unpinned));
    // A pinned revision must match exactly; an unpinned request takes any.
    CHECK_FALSE(detail::provider_pool_key_matches(dawn_key, dawn_other));
    CHECK(detail::provider_pool_key_matches(dawn_key, dawn_unpinned));
    // Unauthenticated or unknown-kind providers have no key and are never lent.
    auto unauthenticated = identity(GpuAudioProvider::Dawn, "metal", "ada56770");
    unauthenticated.authenticated = false;
    CHECK(detail::provider_pool_key(unauthenticated).empty());
    CHECK(detail::provider_pool_key(identity(GpuAudioProvider::Unknown, "metal", "x")).empty());
    CHECK_FALSE(detail::provider_pool_key_matches("", dawn_unpinned));
}

TEST_CASE("Transport capability reports the recovery policy in effect, not the configured one",
          "[gpu_audio][wavenet][reacquire]") {
    struct StagedNode final : GpuAudioNode {
        GpuAudioNodeDescriptor descriptor() const override {
            return {.name = "staged",
                    .input_channels = 1,
                    .output_channels = 1,
                    .block_size = 2,
                    .sample_rate = 48000,
                    .latency_blocks = 1};
        }
        bool prepare() override {
            return true;
        }
        void process_block(const pulp::audio::BufferView<const float>&,
                           pulp::audio::BufferView<float>& out, std::uint32_t) override {
            out.clear();
        }
    };
    const GpuAudioTransport::Config automatic{.recovery_policy =
                                                  GpuAudioRecoveryPolicy::AutoReacquire};
    {
        // No re-acquire hook: AutoReacquire is configured but not in effect.
        StagedNode node;
        GpuAudioTransport transport;
        REQUIRE(transport.prepare(&node, automatic));
        CHECK(transport.capability_report().recovery_policy == GpuAudioRecoveryPolicy::Manual);
    }
    {
        Harness h;
        GpuAudioTransport transport;
        REQUIRE(transport.prepare(&h.node, automatic));
        CHECK(transport.capability_report().recovery_policy ==
              GpuAudioRecoveryPolicy::AutoReacquire);
        transport.release();
        REQUIRE(transport.prepare(&h.node, {}));
        CHECK(transport.capability_report().recovery_policy == GpuAudioRecoveryPolicy::Manual);
    }
}

TEST_CASE("WaveNet re-acquire after an input stall delivers GPU blocks again",
          "[gpu_audio][wavenet][realtime][reacquire]") {
    Harness h;
    ChannelFactory factory{.control = &h.controls[1]};
    factory.install(h.node);
    // No worker for longer than the ring: the callback saturates the input.
    for (int block = 0; block < 8; ++block)
        h.callback(float(h.sequence + 1));
    h.service();
    REQUIRE(h.node.recovery_reason() == GpuAudioRecoveryReason::InputSaturated);
    REQUIRE(h.path.reprepare(h.path.context, 1'000'000));
    int ready = 0;
    for (int block = 0; block < 8; ++block) {
        ready += h.callback(float(h.sequence + 1)) == detail::kRealtimeGpuReady ? 1 : 0;
        h.service();
        h.service();
    }
    CHECK(ready >= 5);
    CHECK(checked_status(h.node).recovery_reason == GpuAudioRecoveryReason::ProviderReacquired);
}

TEST_CASE("Transport re-acquire backs off and caps when a new epoch fails before delivering",
          "[gpu_audio][wavenet][realtime][reacquire]") {
    // Every replacement is lost at once. A long backoff allows one attempt in
    // the run; no backoff still stops at the configured maximum.
    const auto slow = run_reacquire(ReacquireFault::Lose, GpuAudioRecoveryPolicy::AutoReacquire,
                                    false, {.lost_replacements = -1, .backoff_ns = 10'000'000'000});
    CHECK(slow.status.reacquire_attempts == 1);
    CHECK(slow.discontinuities == 0);
    const auto capped = run_reacquire(ReacquireFault::Lose, GpuAudioRecoveryPolicy::AutoReacquire,
                                      false, {.lost_replacements = -1});
    CHECK(capped.status.reacquire_attempts == 3);
    CHECK(capped.status.reacquire_successes == 3);
    CHECK(capped.created == 3);
    CHECK(capped.discontinuities == 0);
}

TEST_CASE("Transport time to GPU counts from the first outage across a failed new epoch",
          "[gpu_audio][wavenet][realtime][reacquire]") {
    // The first replacement is lost and takes 30 ms to create; the second is
    // healthy. The measured outage must include the failed epoch.
    const auto run = run_reacquire(
        ReacquireFault::Lose, GpuAudioRecoveryPolicy::AutoReacquire, false,
        {.lost_replacements = 1, .first_replacement_delay = std::chrono::milliseconds(30)});
    REQUIRE(run.status.reacquire_successes == 2);
    CHECK(run.status.recovery_reason == GpuAudioRecoveryReason::ProviderReacquired);
    CHECK(run.status.last_reacquire_to_gpu_ns >= 30'000'000);
    CHECK(run.discontinuities == 0);
}

TEST_CASE("WaveNet bounded release and re-acquire stay within budget across hung channels",
          "[gpu_audio][wavenet][realtime][teardown]") {
    const auto hung_node = [](Shape& shape, GpuWaveNetRealtimeNode& node,
                              std::array<SlowProvider*, 2>& providers) {
        (void)shape;
        std::vector<std::unique_ptr<detail::WaveNetRealtimeChannel>> channels;
        for (auto& slot : providers) {
            auto provider = std::make_unique<SlowProvider>();
            slot = provider.get();
            auto session = std::make_unique<detail::SharedIoProgramSession>();
            REQUIRE(session->prepare({std::move(provider), std::make_unique<PassthroughProgram>()},
                                     {.slots = 2,
                                      .input_bytes_per_slot = 2 * sizeof(float),
                                      .output_bytes_per_slot = 2 * sizeof(float)}));
            channels.push_back(
                std::make_unique<pulp::test::ProgramSessionChannel>(std::move(session)));
        }
        REQUIRE(detail::WaveNetRealtimeTestAccess::prepare(node, std::move(channels)));
        for (auto* provider : providers)
            provider->hang_drain = true;
    };
    {
        Shape shape;
        GpuWaveNetRealtimeNode node(shape.config(1, 2));
        std::array<SlowProvider*, 2> providers{};
        hung_node(shape, node, providers);
        const auto started = std::chrono::steady_clock::now();
        CHECK_FALSE(node.release_bounded(250'000'000));
        CHECK(std::chrono::steady_clock::now() - started <= std::chrono::milliseconds(500));
        for (auto* provider : providers)
            provider->hang_drain = false;
        CHECK(node.release());
    }
    {
        Shape shape;
        GpuWaveNetRealtimeNode node(shape.config(1, 2));
        std::array<SlowProvider*, 2> providers{};
        hung_node(shape, node, providers);
        std::array<Control, 2> healthy{};
        ChannelFactory factory{.control = &healthy[0]};
        factory.install(node);
        detail::WaveNetRealtimeTestAccess::request_recovery(
            node, detail::SharedIoRecoveryReason::ProviderLost, 0);
        auto path = detail::realtime_gpu_node_path(&node);
        const auto started = std::chrono::steady_clock::now();
        CHECK(path.reprepare(path.context, 250'000'000));
        CHECK(std::chrono::steady_clock::now() - started <= std::chrono::milliseconds(500));
        CHECK(detail::WaveNetRealtimeTestAccess::quarantined_channels(node) == 2);
        for (auto* provider : providers)
            provider->hang_drain = false;
        CHECK(node.release());
    }
}

TEST_CASE("Stamped bridge refuses an epoch adoption that a racing recovery reached",
          "[gpu_audio][wavenet][reacquire]") {
    using Bridge = detail::SharedIoStampedBridge;
    const auto drive = [](Bridge& bridge, std::uint64_t sequence) {
        std::array<float, 2> input{}, output{};
        auto callback = bridge.begin_callback(input, sequence, 0);
        (void)bridge.consume_output(callback, output);
    };
    // A recovery inside request_epoch, between its snapshot and its clearing
    // of the old cause. The clearing erases this cause too; the adoption must
    // still be refused and the cause restored, so the owner can try again.
    Bridge bridge;
    REQUIRE(bridge.prepare({.capacity = 4, .channels = 1, .block_size = 2, .lead_blocks = 1}, 1));
    bridge.request_recovery(detail::SharedIoRecoveryReason::ProviderLost, 0);
    bridge.set_request_epoch_hook_for_testing(
        [](void* context) {
            static_cast<Bridge*>(context)->request_recovery(
                detail::SharedIoRecoveryReason::SequenceGap, 7);
        },
        &bridge);
    REQUIRE(bridge.request_epoch(2));
    bridge.set_request_epoch_hook_for_testing(nullptr, nullptr);
    drive(bridge, 0);
    CHECK(bridge.delivery_epoch() == 0);
    CHECK(bridge.recovery_reason() == detail::SharedIoRecoveryReason::SequenceGap);
    CHECK(bridge.recovery_sequence() == 7);
    // Positive control: the same request with no racing recovery is adopted.
    Bridge clean;
    REQUIRE(clean.prepare({.capacity = 4, .channels = 1, .block_size = 2, .lead_blocks = 1}, 1));
    clean.request_recovery(detail::SharedIoRecoveryReason::ProviderLost, 0);
    REQUIRE(clean.request_epoch(2));
    drive(clean, 0);
    CHECK(clean.delivery_epoch() == 2);
    CHECK(clean.recovery_reason() == detail::SharedIoRecoveryReason::None);
}

TEST_CASE("WaveNet release drops a parked channel while a quarantined one keeps it owned",
          "[gpu_audio][wavenet][realtime][pool]") {
    GpuAudioProviderPool pool;
    std::array<Control, 2> controls{}; // declared before the node that uses them
    controls[0].recyclable = true;
    controls[0].pool_key = "fake:1";
    controls[1].release = false;
    Shape shape;
    auto config = shape.config(1);
    config.provider_pool = &pool;
    GpuWaveNetRealtimeNode node(config);
    std::vector<std::unique_ptr<detail::WaveNetRealtimeChannel>> providers;
    providers.push_back(std::make_unique<Channel>(controls[0]));
    REQUIRE(detail::WaveNetRealtimeTestAccess::prepare(node, std::move(providers)));
    detail::WaveNetRealtimeTestAccess::quarantine_for_testing(
        node, std::make_unique<Channel>(controls[1]));
    CHECK_FALSE(node.release()); // the live channel parks; the quarantine stays
    CHECK(pool.stats().warm_entries == 1);
    CHECK_FALSE(node.release()); // must not touch the parked channel again
    CHECK(controls[0].recycles == 1);
    controls[1].release = true;
    CHECK(node.release());
    pool.clear();
}

TEST_CASE("Provider pool evicts a lost provider within the caller's budget and checks the "
          "program exactly",
          "[gpu_audio][wavenet][realtime][pool]") {
    using Access = detail::GpuAudioProviderPoolTestAccess;
    Shape shape;
    const auto config = shape.config(1).session;
    {
        Control control;
        control.pool_key = "fake:1";
        GpuAudioProviderPool pool;
        std::unique_ptr<detail::WaveNetRealtimeChannel> channel =
            std::make_unique<Channel>(control);
        REQUIRE(Access::give(pool, config, "fake:1", channel));
        control.lost = true; // the device was lost while parked
        CHECK(Access::acquire(pool, config, "fake:1", 12'345) == nullptr);
        CHECK(control.last_release_budget >= 1);
        CHECK(control.last_release_budget <= 12'345);
        CHECK(pool.stats().evictions == 1);
        CHECK(pool.stats().warm_entries == 0);
    }
    {
        // Every program hashes alike; the exact check must refuse other
        // weights. Positive control: the same program is lent.
        Control control;
        GpuAudioProviderPool pool;
        Access::set_key_hash(
            pool, [](const GpuWaveNetSession::Config&) noexcept { return std::uint64_t{42}; });
        std::unique_ptr<detail::WaveNetRealtimeChannel> channel =
            std::make_unique<Channel>(control);
        REQUIRE(Access::give(pool, config, "fake:1", channel));
        std::array<float, 9> other_weights{};
        other_weights[0] = 1.0f;
        auto other = config;
        other.weights = other_weights;
        CHECK(Access::acquire(pool, other, "fake:1", 0) == nullptr);
        CHECK(Access::acquire(pool, config, "fake:2", 0) == nullptr); // other provider
        CHECK(Access::acquire(pool, config, "fake:1", 0) != nullptr);
    }
}

TEST_CASE("Transport re-acquire count resets once the new epoch delivered, even if a fault "
          "followed before the next pump",
          "[gpu_audio][wavenet][realtime][reacquire]") {
    Shape shape;
    auto config = shape.config(1);
    config.miss_policy = MissPolicy::CpuFallback;
    config.supports_cpu_fallback = true;
    std::array<Control, 3> controls{};
    IdentityShadowNode node(config);
    std::vector<std::unique_ptr<detail::WaveNetRealtimeChannel>> providers;
    providers.push_back(std::make_unique<Channel>(controls[0]));
    REQUIRE(detail::WaveNetRealtimeTestAccess::prepare(node, std::move(providers)));
    ChannelFactory factory{
        .control = &controls[2], .first_control = &controls[1], .first_count = 1};
    factory.install(node);
    GpuAudioTransport transport;
    // A long backoff: a second attempt in this run only happens if the first
    // re-acquire's GPU delivery reset the count.
    REQUIRE(transport.prepare(&node, {.recovery_policy = GpuAudioRecoveryPolicy::AutoReacquire,
                                      .reacquire_backoff_ns = 10'000'000'000}));
    std::array<float, 2> input{}, output{};
    const float* ip = input.data();
    float* op = output.data();
    pulp::audio::BufferView<const float> in(&ip, 1, 2);
    pulp::audio::BufferView<float> out(&op, 1, 2);
    bool second_fault = false;
    for (int block = 0; block < 40; ++block) {
        if (block == 5)
            controls[0].lost = true;
        input.fill(float(block + 1));
        const auto gpu_before = transport.delivery_snapshot().gpu_blocks;
        transport.process(in, out, 2);
        // The re-acquired provider's first GPU block: fault it before any pump.
        if (!second_fault && transport.status_snapshot().reacquire_successes == 1 &&
            transport.delivery_snapshot().gpu_blocks > gpu_before) {
            controls[1].lost = true;
            second_fault = true;
        }
        transport.pump();
        transport.pump();
    }
    REQUIRE(second_fault);
    const auto status = transport.status_snapshot();
    CHECK(status.reacquire_attempts == 2);
    CHECK(status.reacquire_successes == 2);
    CHECK(status.recovery_reason == GpuAudioRecoveryReason::ProviderReacquired);
    CHECK(validate_gpu_audio_status(status).accepted());
    transport.release();
}

TEST_CASE("Stamped bridge withdraws a request whose adoption the callback refused first",
          "[gpu_audio][wavenet][reacquire]") {
    using Bridge = detail::SharedIoStampedBridge;
    struct Race {
        Bridge* bridge;
        std::uint64_t sequence;
        static void callback(void* context) {
            auto& race = *static_cast<Race*>(context);
            std::array<float, 2> input{}, output{};
            auto block = race.bridge->begin_callback(input, race.sequence, 0);
            (void)race.bridge->consume_output(block, output);
        }
    };
    for (const bool refused : {true, false}) {
        CAPTURE(refused);
        Bridge bridge;
        REQUIRE(
            bridge.prepare({.capacity = 4, .channels = 1, .block_size = 2, .lead_blocks = 1}, 1));
        bridge.request_recovery(detail::SharedIoRecoveryReason::ProviderLost, 0);
        REQUIRE(bridge.request_epoch(2));
        if (refused) // a new fault after the request: the callback refuses it
            bridge.request_recovery(detail::SharedIoRecoveryReason::ProviderLost, 0);
        // The callback claims the request between the worker's load and its
        // withdrawal.
        Race race{&bridge, 0};
        bridge.set_cancel_epoch_hook_for_testing(&Race::callback, &race);
        const bool withdrawn = bridge.cancel_requested_epoch();
        bridge.set_cancel_epoch_hook_for_testing(nullptr, nullptr);
        // Refused: nothing pending and delivery closed, so the worker may
        // replace it. Adopted: delivery is open and the worker must not.
        CHECK(withdrawn == refused);
        CHECK(bridge.delivery_epoch() == (refused ? 0u : 2u));
    }
}

TEST_CASE("Provider pool releases several lost providers within one shared budget",
          "[gpu_audio][wavenet][realtime][pool]") {
    using Access = detail::GpuAudioProviderPoolTestAccess;
    Shape shape;
    const auto config = shape.config(1).session;
    // Static: providers that never drain stay quarantined for the process,
    // and must not outlive what they reference.
    static std::array<Control, 4> controls{};
    GpuAudioProviderPool pool;
    for (auto& control : controls) {
        control = Control{};
        std::unique_ptr<detail::WaveNetRealtimeChannel> channel =
            std::make_unique<Channel>(control);
        REQUIRE(Access::give(pool, config, "fake:1", channel));
        control.lost = true;
        control.hang_release = true;
    }
    const auto started = std::chrono::steady_clock::now();
    CHECK(Access::acquire(pool, config, "fake:1", 100'000'000) == nullptr);
    const auto elapsed = std::chrono::steady_clock::now() - started;
    // Four providers, each willing to wait its whole budget: one deadline
    // keeps the total near one budget, not four.
    CHECK(elapsed <= std::chrono::milliseconds(200));
    CHECK(pool.stats().evictions == 4);
    for (const auto& control : controls)
        CHECK(control.last_release_budget >= 1);
}

TEST_CASE("WaveNet drops a completion from the epoch a re-acquire replaced",
          "[gpu_audio][wavenet][realtime][reacquire]") {
    Harness h;
    ChannelFactory factory{.control = &h.controls[1]};
    factory.install(h.node);
    h.callback(1);
    h.service();
    h.service();
    h.controls[0].lost = true;
    h.service();
    REQUIRE(h.node.recovery_reason() == GpuAudioRecoveryReason::ProviderLost);
    REQUIRE(h.path.reprepare(h.path.context, 1'000'000));
    // The callback adopts the new epoch; the worker admits its first block.
    h.callback(2);
    h.service();
    const auto stale_before = detail::WaveNetRealtimeTestAccess::stale_completions(h.node);
    // That block's completion arrives carrying the replaced epoch's ticket.
    h.controls[1].stale_generation = true;
    h.service();
    h.controls[1].stale_generation = false;
    CHECK(detail::WaveNetRealtimeTestAccess::stale_completions(h.node) == stale_before + 1);
    // Dropped and counted, not a provider fault, and never delivered.
    CHECK(h.node.recovery_reason() == GpuAudioRecoveryReason::None);
    CHECK(checked_status(h.node).recovery_reason == GpuAudioRecoveryReason::ProviderReacquired);
    for (int block = 0; block < 2; ++block)
        CHECK(h.callback(float(10 + block)) != detail::kRealtimeGpuReady);
}

TEST_CASE("Transport re-acquire is reachable only while the transport is prepared",
          "[gpu_audio][wavenet][realtime][reacquire]") {
    Shape shape;
    auto config = shape.config(1);
    config.miss_policy = MissPolicy::CpuFallback;
    config.supports_cpu_fallback = true;
    std::array<Control, 2> controls{};
    IdentityShadowNode node(config);
    std::vector<std::unique_ptr<detail::WaveNetRealtimeChannel>> providers;
    providers.push_back(std::make_unique<Channel>(controls[0]));
    REQUIRE(detail::WaveNetRealtimeTestAccess::prepare(node, std::move(providers)));
    ChannelFactory factory{.control = &controls[1]};
    factory.install(node);
    GpuAudioTransport transport;
    REQUIRE(transport.prepare(&node, {.recovery_policy = GpuAudioRecoveryPolicy::AutoReacquire,
                                      .reacquire_backoff_ns = 0}));
    std::array<float, 2> input{1, 1}, output{};
    const float* ip = input.data();
    float* op = output.data();
    pulp::audio::BufferView<const float> in(&ip, 1, 2);
    pulp::audio::BufferView<float> out(&op, 1, 2);
    transport.process(in, out, 2);
    transport.pump();
    transport.release();
    // Fenced after release: neither release() nor a later pump re-acquires.
    detail::WaveNetRealtimeTestAccess::request_recovery(
        node, detail::SharedIoRecoveryReason::ProviderLost, 0);
    transport.pump();
    transport.pump();
    CHECK(factory.created == 0);
    CHECK(transport.status_snapshot().reacquire_attempts == 0);
    CHECK(transport.capability_report().recovery_policy == GpuAudioRecoveryPolicy::Manual);
    // Positive control: prepared again, the same fence is re-acquired.
    REQUIRE(transport.prepare(&node, {.recovery_policy = GpuAudioRecoveryPolicy::AutoReacquire,
                                      .reacquire_backoff_ns = 0}));
    transport.pump();
    CHECK(factory.created == 1);
    transport.release();
}
TEST_CASE("WaveNet watchdog ignores worker wakes that complete nothing",
          "[gpu_audio][wavenet][realtime][hang][wake]") {
    // An event-driven worker may wake many times per callback (completion
    // notifications, coalesced posts). A wake that consumes no completion is
    // not progress: the budget still trips after exactly its callback blocks.
    for (const std::uint32_t pumps : {1u, 16u, 64u}) {
        CAPTURE(pumps);
        const auto run = run_hang({.capacity = 16, .watchdog_blocks = 5}, 1, pumps);
        CHECK(run.reason == GpuAudioRecoveryReason::ProviderUnresponsive);
        CHECK(run.blocks_after_hang == 5);
    }
}

TEST_CASE("WaveNet transport routes completion events from every channel or none",
          "[gpu_audio][wavenet][wake]") {
    struct Case {
        bool first_supported, second_supported;
        int expected_installs;
    };
    for (const auto c : {Case{true, true, 1}, Case{true, false, 0}, Case{false, true, 0}}) {
        CAPTURE(c.first_supported, c.second_supported);
        Harness h(Tweak{.channels = 2});
        h.controls[0].notifier_supported = c.first_supported;
        h.controls[1].notifier_supported = c.second_supported;
        GpuAudioTransport transport;
        GpuAudioTransport::Config config;
        config.ring_blocks = 8;
        config.run_worker_thread = true;
        REQUIRE(transport.prepare(&h.node, config));
        for (const auto& control : h.controls) {
            // All or nothing: a partial install is rolled back.
            CHECK((control.notifier.notify != nullptr) == (c.expected_installs == 1));
            if (c.expected_installs == 1)
                CHECK(control.notifier_installs == 1);
        }
        transport.release();
        for (const auto& control : h.controls)
            CHECK(control.notifier.notify == nullptr);
    }
}
