// CV To OSC — what leaves the process, and what never touches the audio thread.
//
// Scope note. These drive `Processor::process()` and `publish_once()` directly
// rather than waiting on the sender thread's clock, so nothing here is timing
// dependent. One test binds a real UDP socket on the loopback; it is the only one
// that can be flaky, and it skips rather than fails when a socket is unavailable.

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include "cv_osc_processor.hpp"
#include <pulp/format/headless.hpp>
#include <pulp/state/store.hpp>

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <thread>
#include <limits>
#include <vector>

using namespace pulp;
using namespace pulp::examples::brew;

namespace {

/// Records what would have gone on the wire.
///
/// Locked, because the sender thread calls into it while a test reads it. The
/// non-threaded tests never contend, but an unsynchronized `std::vector` shared
/// across two threads is undefined behavior whether or not it looks like it works.
struct RecordingSink final : MessageSink {
    struct Sent { std::string address; float value; };

    bool connect(const std::string& host, std::uint16_t port) override {
        std::lock_guard<std::mutex> lock(m);
        connects.push_back({host, port});
        return !refuse_connect;
    }
    bool send(const osc::Message& msg) override {
        std::lock_guard<std::mutex> lock(m);
        if (refuse_send) return false;
        sent.push_back({msg.address, msg.get_float(0)});
        return true;
    }

    [[nodiscard]] std::vector<Sent> snapshot() {
        std::lock_guard<std::mutex> lock(m);
        return sent;
    }

    mutable std::mutex m;
    std::vector<std::pair<std::string, std::uint16_t>> connects;
    std::vector<Sent> sent;
    bool refuse_connect = false;
    bool refuse_send = false;
};

/// A processor wired to a sink we can read. The sender thread never starts,
/// because nothing calls prepare() — `publish_once()` is driven by hand, so every
/// assertion below is deterministic rather than a race against a clock.
///
/// The store is constructed here and handed to the processor: `Processor::state()`
/// dereferences a pointer the *host* installs, so a bare processor has none.
struct Rig {
    explicit Rig(std::unique_ptr<RecordingSink> s = nullptr) {
        auto owned = s ? std::move(s) : std::make_unique<RecordingSink>();
        sink = owned.get();
        proc = std::make_unique<CvOscProcessor>(std::move(owned));
        proc->set_state_store(&store);
        proc->define_parameters(store);
    }

    /// Push a block of constant samples through, without a HeadlessHost, so the
    /// sender thread stays unstarted and the test stays deterministic.
    std::vector<float> run(float left, float right, bool bypassed = false,
                           std::size_t frames = 64) {
        audio::Buffer<float> in(2, frames), out(2, frames);
        in.clear();
        out.clear();
        for (std::size_t n = 0; n < frames; ++n) {
            in.channel(0)[n] = left;
            in.channel(1)[n] = right;
        }
        const float* ip[2] = {in.channel(0).data(), in.channel(1).data()};
        audio::BufferView<const float> iv(ip, 2, frames);
        auto ov = out.view();
        midi::MidiBuffer mi, mo;
        format::ProcessContext ctx;
        ctx.sample_rate = 48000.0;
        ctx.num_samples = static_cast<int>(frames);
        ctx.is_bypassed = bypassed;
        proc->process(ov, iv, mi, mo, ctx);
        return std::vector<float>(out.channel(0).begin(), out.channel(0).end());
    }

    void enable() { store.set_value(CvOscProcessor::kEnabled, 1.0f); }

    // Declared before `proc`: the processor holds a pointer to it and is
    // destroyed first.
    state::StateStore store;
    RecordingSink* sink = nullptr;
    std::unique_ptr<CvOscProcessor> proc;
};

}  // namespace

TEST_CASE("CV To OSC is an insert with a CV thru", "[brew][osc]") {
    Rig rig;
    const auto desc = rig.proc->descriptor();
    REQUIRE(desc.name == "CV To OSC");
    REQUIRE(desc.input_buses.size() == 1);
    REQUIRE(desc.output_buses.size() == 1);
}

// ------------------------------------------------------- nothing leaves by default

TEST_CASE("nothing is sent until Send is switched on", "[brew][osc][safety]") {
    // A plug-in that begins emitting UDP the moment a project loads trips a
    // firewall prompt on some machines and quietly streams the user's patch on
    // others. Off is the only defensible default, and it must not even connect.
    Rig rig;
    REQUIRE(rig.store.get_value(CvOscProcessor::kEnabled) == 0.0f);

    rig.run(0.5f, -0.5f);
    rig.proc->publish_once();
    rig.proc->publish_once();

    REQUIRE(rig.sink->sent.empty());
    REQUIRE(rig.sink->connects.empty());
    REQUIRE(rig.proc->sent_count() == 0);
}

TEST_CASE("switching Send on connects to the loopback and nowhere else",
          "[brew][osc][safety]") {
    Rig rig;
    rig.enable();
    rig.run(0.25f, 0.25f);
    rig.proc->publish_once();

    REQUIRE(rig.sink->connects.size() == 1);
    REQUIRE(rig.sink->connects[0].first == "127.0.0.1");
    REQUIRE(rig.sink->connects[0].second == 9000);
}

// --------------------------------------------------------------- the pass-through

TEST_CASE("the input reaches the output bit-exactly", "[brew][osc][safety]") {
    // An insert in a CV chain must be a wire. Anything else — a scale, a smooth,
    // a clamp — is a wrong voltage downstream, and this plug-in exists only to
    // observe.
    Rig rig;
    rig.enable();
    const std::vector<float> levels = {-1.0f, -0.333333f, 0.0f, 0.1234567f, 1.0f};
    for (float v : levels) {
        CAPTURE(v);
        const auto out = rig.run(v, v);
        for (float s : out) REQUIRE(s == v);
    }
}

TEST_CASE("bypass keeps the wire but stops the reporting",
          "[brew][osc][bypass]") {
    // Bypassed means out of the circuit. The voltage still passes — an insert's
    // bypass is a wire, not a mute — but a stale reading must not keep flowing to
    // a receiver that would believe it is current.
    Rig rig;
    rig.enable();
    rig.run(0.8f, 0.8f);
    REQUIRE(rig.proc->latest(0) == 0.8f);

    const auto out = rig.run(-0.6f, -0.6f, /*bypassed=*/true);
    for (float s : out) REQUIRE(s == -0.6f);   // still a wire
    REQUIRE(rig.proc->latest(0) == 0.8f);      // but the reading did not advance
}

// -------------------------------------------------------------------- the deadband

TEST_CASE("the first value is always sent", "[brew][osc]") {
    // Without this a DC source that never moves would never announce itself and
    // the patch would look dead at the receiver.
    Rig rig;
    rig.enable();
    rig.run(0.0f, 0.0f);
    rig.proc->publish_once();

    REQUIRE(rig.sink->sent.size() == 2);
    REQUIRE(rig.sink->sent[0].address == "/brew/cv/0");
    REQUIRE(rig.sink->sent[1].address == "/brew/cv/1");
    REQUIRE(rig.sink->sent[0].value == 0.0f);
}

TEST_CASE("a value that has not moved past the deadband is not resent",
          "[brew][osc]") {
    // Float noise on the last bit of a steady voltage would otherwise flood the
    // receiver at the full send rate, and a receiver smoothing its input cannot
    // tell that flood from a real signal.
    Rig rig;
    rig.enable();
    rig.store.set_value(CvOscProcessor::kDeadband, 0.01f);

    rig.run(0.5f, 0.5f);
    rig.proc->publish_once();
    const auto after_first = rig.sink->sent.size();
    REQUIRE(after_first == 2);

    // Below the deadband: silence.
    rig.run(0.505f, 0.505f);
    rig.proc->publish_once();
    REQUIRE(rig.sink->sent.size() == after_first);

    // Clearly past it: a send. (0.51 would not do — `0.51f - 0.5f` is
    // 0.00999999… and lands *under* a 0.01 deadband. The `>=` boundary itself is
    // pinned below, on exactly-representable values, where the claim is about the
    // comparison rather than about float.)
    rig.run(0.52f, 0.52f);
    rig.proc->publish_once();
    REQUIRE(rig.sink->sent.size() == after_first + 2);
}

TEST_CASE("a step of exactly the deadband is sent", "[brew][osc]") {
    // `>=`, not `>`: a signal that advances by exactly the deadband every tick
    // would otherwise never be reported and would sit frozen at the receiver.
    // Powers of two, so the subtraction is exact and this tests the comparison.
    REQUIRE(should_send(false, 0.5f, 0.75f, 0.25f));
    REQUIRE_FALSE(should_send(false, 0.5f, 0.625f, 0.25f));
    // Symmetric: it is a magnitude, so falling counts as much as rising.
    REQUIRE(should_send(false, 0.5f, 0.25f, 0.25f));
}

TEST_CASE("a zero deadband sends on every tick", "[brew][osc]") {
    Rig rig;
    rig.enable();
    rig.store.set_value(CvOscProcessor::kDeadband, 0.0f);
    rig.run(0.5f, 0.5f);
    rig.proc->publish_once();
    rig.proc->publish_once();
    rig.proc->publish_once();
    REQUIRE(rig.sink->sent.size() == 6);
}

TEST_CASE("the deadband is measured from the last value actually sent",
          "[brew][osc]") {
    // Measuring against the previous *sample* instead would let a slow ramp creep
    // arbitrarily far without ever crossing the threshold in one step — the
    // receiver's value would drift away from the truth and never be corrected.
    Rig rig;
    rig.enable();
    rig.store.set_value(CvOscProcessor::kDeadband, 0.1f);
    rig.run(0.0f, 0.0f);
    rig.proc->publish_once();
    const auto baseline = rig.sink->sent.size();

    // Six steps of 0.03: no single step crosses 0.1, but the total is 0.18.
    for (int i = 1; i <= 6; ++i) {
        rig.run(0.03f * static_cast<float>(i), 0.0f);
        rig.proc->publish_once();
    }
    REQUIRE(rig.sink->sent.size() > baseline);
    REQUIRE(rig.sink->sent.back().value == Catch::Approx(0.12f).margin(1e-5));
}

TEST_CASE("a non-finite sample is never put on the wire",
          "[brew][osc][safety]") {
    // A NaN reaching a receiver is a NaN in someone else's synth. The deadband
    // comparison against NaN is false either way, so the guard has to be explicit.
    REQUIRE_FALSE(should_send(false, 0.0f, std::nanf(""), 0.001f));
    REQUIRE_FALSE(should_send(false, 0.0f,
                              std::numeric_limits<float>::infinity(), 0.001f));
    // But a first send of a finite value still goes.
    REQUIRE(should_send(true, 0.0f, 0.5f, 1.0f));
}

// ------------------------------------------------------------- failures and ports

TEST_CASE("a refused connect sends nothing and does not latch the port",
          "[brew][osc][safety]") {
    auto sink = std::make_unique<RecordingSink>();
    sink->refuse_connect = true;
    Rig rig(std::move(sink));
    rig.enable();
    rig.run(0.5f, 0.5f);

    rig.proc->publish_once();
    REQUIRE(rig.sink->sent.empty());
    // It retries next tick rather than believing it is connected.
    rig.proc->publish_once();
    REQUIRE(rig.sink->connects.size() == 2);
}

TEST_CASE("a refused send does not advance the last-sent value",
          "[brew][osc][safety]") {
    // Otherwise a single dropped packet would make the plug-in believe the
    // receiver knows a value it never got, and the deadband would suppress the
    // retry until the signal moved again.
    auto sink = std::make_unique<RecordingSink>();
    sink->refuse_send = true;
    Rig rig(std::move(sink));
    rig.enable();
    rig.store.set_value(CvOscProcessor::kDeadband, 0.1f);
    rig.run(0.5f, 0.5f);
    rig.proc->publish_once();
    REQUIRE(rig.proc->sent_count() == 0);

    rig.sink->refuse_send = false;
    rig.proc->publish_once();
    REQUIRE(rig.sink->sent.size() == 2);
    REQUIRE(rig.sink->sent[0].value == 0.5f);
}

TEST_CASE("changing the port reconnects and re-announces the resting value",
          "[brew][osc]") {
    // A new receiver has never heard the value. Without the re-announce it would
    // wait for the signal to move before learning anything.
    Rig rig;
    rig.enable();
    rig.store.set_value(CvOscProcessor::kDeadband, 0.1f);
    rig.run(0.5f, 0.5f);
    rig.proc->publish_once();
    REQUIRE(rig.sink->connects.size() == 1);
    const auto before = rig.sink->sent.size();

    rig.store.set_value(CvOscProcessor::kPort, 9001.0f);
    rig.proc->publish_once();
    REQUIRE(rig.sink->connects.size() == 2);
    REQUIRE(rig.sink->connects[1].second == 9001);
    REQUIRE(rig.sink->sent.size() == before + 2);   // resent, despite no movement
}

// ------------------------------------------------------------------ pure functions

TEST_CASE("the send interval is clamped away from zero and from a flood",
          "[brew][osc][safety]") {
    REQUIRE(send_interval_seconds(0.0f) == send_interval_seconds(kMinRateHz));
    REQUIRE(send_interval_seconds(-10.0f) == send_interval_seconds(kMinRateHz));
    REQUIRE(send_interval_seconds(1e9f) == send_interval_seconds(kMaxRateHz));
    REQUIRE(send_interval_seconds(60.0f) == Catch::Approx(1.0 / 60.0));
}

TEST_CASE("one OSC address per channel", "[brew][osc]") {
    REQUIRE(osc_address(0) == "/brew/cv/0");
    REQUIRE(osc_address(1) == "/brew/cv/1");
}

// ------------------------------------------------------------------ the thread

TEST_CASE("the sender thread delivers, and joins promptly on destruction",
          "[brew][osc][thread]") {
    // Two hazards live here and nowhere else. The thread must actually wake and
    // send without the audio thread doing it; and it must be joinable at once,
    // rather than sleeping out a full interval — at 1 Hz a bare sleep would hang
    // every plug-in close for a second, which in a project with ten instances is
    // ten seconds of a spinning cursor.
    state::StateStore store;
    std::uint64_t observed = 0;
    std::string first_address;
    {
        // The processor owns the sink, so everything read from it must be read
        // *inside* this scope. Reading `sink->sent` after the processor is
        // destroyed is a use-after-free that happily reports the value you hoped
        // for.
        auto sink_owned = std::make_unique<RecordingSink>();
        auto* sink = sink_owned.get();
        CvOscProcessor proc(std::move(sink_owned));
        proc.set_state_store(&store);
        proc.define_parameters(store);
        store.set_value(CvOscProcessor::kEnabled, 1.0f);
        store.set_value(CvOscProcessor::kRateHz, 200.0f);

        format::PrepareContext pc;
        pc.sample_rate = 48000.0;
        pc.max_buffer_size = 64;
        proc.prepare(pc);   // starts the thread

        const auto deadline =
            std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (proc.sent_count() == 0 &&
               std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        observed = proc.sent_count();

        // Slow it right down before the destructor runs: joining must not wait
        // for the next tick.
        store.set_value(CvOscProcessor::kRateHz, 1.0f);
        std::this_thread::sleep_for(std::chrono::milliseconds(20));

        const auto seen = sink->snapshot();
        if (!seen.empty()) first_address = seen[0].address;
        // ~proc runs here, joining the thread and destroying the sink.
    }
    REQUIRE(observed >= 1);
    REQUIRE(first_address == "/brew/cv/0");
}

TEST_CASE("destruction with the sender at its slowest does not stall",
          "[brew][osc][thread]") {
    state::StateStore store;
    const auto start = std::chrono::steady_clock::now();
    {
        CvOscProcessor proc(std::make_unique<RecordingSink>());
        proc.set_state_store(&store);
        proc.define_parameters(store);
        store.set_value(CvOscProcessor::kEnabled, 1.0f);
        store.set_value(CvOscProcessor::kRateHz, kMinRateHz);   // one second a tick
        format::PrepareContext pc;
        pc.sample_rate = 48000.0;
        pc.max_buffer_size = 64;
        proc.prepare(pc);
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
    }
    const auto elapsed = std::chrono::steady_clock::now() - start;
    // A bare `sleep_for(interval)` would put this near a full second. The
    // condition variable makes it immediate; half a second is a generous ceiling
    // that still fails the sleeping implementation.
    REQUIRE(elapsed < std::chrono::milliseconds(500));
}

// ------------------------------------------------------------------ the real socket

TEST_CASE("a real OSC float arrives on the loopback", "[brew][osc][net]") {
    // The one test that binds a socket. Everything above proves the decisions;
    // this proves the bytes. It skips rather than fails when a socket cannot be
    // had — a sandboxed CI runner is not a broken plug-in.
    constexpr std::uint16_t kPort = 19876;

    osc::Receiver receiver;
    std::atomic<int> received{0};
    std::atomic<float> value{0.0f};

    if (!receiver.listen(kPort, [&](const osc::Message& m) {
            if (m.address == "/brew/cv/0") {
                value.store(m.get_float(0), std::memory_order_relaxed);
                received.fetch_add(1, std::memory_order_relaxed);
            }
        })) {
        SKIP("cannot bind a UDP socket on the loopback in this environment");
    }

    state::StateStore store;
    CvOscProcessor proc;   // the real UdpSink
    proc.set_state_store(&store);
    proc.define_parameters(store);
    store.set_value(CvOscProcessor::kEnabled, 1.0f);
    store.set_value(CvOscProcessor::kPort, static_cast<float>(kPort));

    audio::Buffer<float> in(2, 16), out(2, 16);
    in.clear();
    out.clear();
    for (std::size_t n = 0; n < 16; ++n) in.channel(0)[n] = -0.75f;
    const float* ip[2] = {in.channel(0).data(), in.channel(1).data()};
    audio::BufferView<const float> iv(ip, 2, 16);
    auto ov = out.view();
    midi::MidiBuffer mi, mo;
    format::ProcessContext ctx;
    ctx.sample_rate = 48000.0;
    ctx.num_samples = 16;
    proc.process(ov, iv, mi, mo, ctx);
    proc.publish_once();

    // UDP on the loopback is fast but not instantaneous.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (received.load(std::memory_order_relaxed) == 0 &&
           std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(5));

    receiver.stop();
    REQUIRE(received.load(std::memory_order_relaxed) >= 1);
    REQUIRE(value.load(std::memory_order_relaxed) == Catch::Approx(-0.75f));
}
