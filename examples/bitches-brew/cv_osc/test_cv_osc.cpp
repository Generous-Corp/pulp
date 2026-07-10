// CV To OSC — what leaves the process, and what never touches the audio thread.
//
// Scope note. These drive `Processor::process()` and `publish_once()` directly
// rather than waiting on the sender thread's clock, so nothing here is timing
// dependent. One test binds a real UDP socket on the loopback; it is the only one
// that can be flaky, and it skips rather than fails when a socket is unavailable.

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include "cv_osc_discovery.hpp"
#include "cv_osc_processor.hpp"
#include "cv_osc_ui.hpp"
#include <pulp/format/headless.hpp>
#include <pulp/state/store.hpp>

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
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

state::ParamID opid(state::ParamID id, std::size_t channel) {
    return static_cast<state::ParamID>(param_for(id, channel));
}

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

    void enable(std::size_t channel) {
        store.set_value(opid(CvOscProcessor::kEnable, channel), 1.0f);
        // A host publishes a parameter change to the sender thread on its next
        // process block; this rig never processes, so it publishes here.
        proc->snapshot_parameters();
    }
    void disable(std::size_t channel) {
        store.set_value(opid(CvOscProcessor::kEnable, channel), 0.0f);
        // A host publishes a parameter change to the sender thread on its next
        // process block; this rig never processes, so it publishes here.
        proc->snapshot_parameters();
    }
    void enable() {
        for (std::size_t c = 0; c < kOscChannels; ++c) enable(c);
    }
    void set_threshold(float v) {
        for (std::size_t c = 0; c < kOscChannels; ++c)
            store.set_value(opid(CvOscProcessor::kThreshold, c), v);
        // A host publishes a parameter change to the sender thread on its next
        // process block; this rig never processes, so it publishes here.
        proc->snapshot_parameters();
    }

    // Declared before `proc`: the processor holds a pointer to it and is
    // destroyed first.
    state::StateStore store;
    RecordingSink* sink = nullptr;
    std::unique_ptr<CvOscProcessor> proc;
};

/// An mDNS backend that announces exactly what a test tells it to, so discovery
/// can be asserted on a machine with no network and no Bonjour.
struct FakeMdns final : events::NetworkServiceDiscovery::Backend {
    void browse(std::string_view type, events::NetworkServiceDiscovery& o) override {
        browsed_type = std::string(type);
        owner = &o;
    }
    void stop() override { owner = nullptr; }
    bool register_service(std::string_view, std::string_view, uint16_t) override {
        return false;
    }
    void unregister_service() override {}

    void announce(std::string name, std::string address, std::uint16_t port) {
        events::NetworkServiceDiscovery::Service svc;
        svc.name = std::move(name);
        svc.type = kOscServiceType;
        svc.address = std::move(address);
        svc.port = port;
        if (owner) owner->notify_service_found(svc);
    }
    void withdraw(std::string name) {
        events::NetworkServiceDiscovery::Service svc;
        svc.name = std::move(name);
        svc.type = kOscServiceType;
        if (owner) owner->notify_service_lost(svc);
    }

    events::NetworkServiceDiscovery* owner = nullptr;
    std::string browsed_type;
};

}  // namespace

TEST_CASE("CV To OSC is an insert with a CV thru", "[brew][osc]") {
    Rig rig;
    const auto desc = rig.proc->descriptor();
    REQUIRE(desc.name == "CV To OSC");
    REQUIRE(desc.input_buses.size() == 1);
    REQUIRE(desc.output_buses.size() == 1);
}

TEST_CASE("every per-channel control is registered on both channels",
          "[brew][osc][stereo]") {
    // A control missing from `controls()` compiles, registers nothing, and then
    // reads back zero — which for Threshold is a silent flood and for Enable is a
    // channel that can never be switched on. Only a test notices.
    Rig rig;
    for (const auto& c : CvOscProcessor::controls())
        for (std::size_t ch = 0; ch < kOscChannels; ++ch)
            REQUIRE(rig.store.info(opid(c.id, ch)) != nullptr);
    REQUIRE(rig.store.info(CvOscProcessor::kRateHz) != nullptr);
    // Two per channel, plus the one global rate. Nothing else.
    REQUIRE(rig.store.all_params().size() ==
            CvOscProcessor::controls().size() * kOscChannels + 1);
}

// ------------------------------------------------------- nothing leaves by default

TEST_CASE("nothing is sent until a channel is switched on", "[brew][osc][safety]") {
    // A plug-in that begins emitting UDP the moment a project loads trips a
    // firewall prompt on some machines and quietly streams the user's patch on
    // others. Off is the only defensible default, and it must not even connect.
    Rig rig;
    for (std::size_t c = 0; c < kOscChannels; ++c)
        REQUIRE(rig.store.get_value(opid(CvOscProcessor::kEnable, c)) == 0.0f);

    rig.run(0.5f, -0.5f);
    rig.proc->publish_once();
    rig.proc->publish_once();

    REQUIRE(rig.sink->sent.empty());
    REQUIRE(rig.sink->connects.empty());
    REQUIRE(rig.proc->sent_count() == 0);
}

TEST_CASE("switching a channel on connects to the default target",
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

TEST_CASE("bypass keeps the wire but stops the reporting", "[brew][osc][bypass]") {
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

// ------------------------------------------------------------ independent channels

TEST_CASE("each channel enables on its own", "[brew][osc][stereo]") {
    Rig rig;
    rig.enable(1);   // right only
    rig.run(0.5f, -0.25f);
    rig.proc->publish_once();

    REQUIRE(rig.sink->sent.size() == 1);
    REQUIRE(rig.sink->sent[0].address == "/brew/cv/1");
    REQUIRE(rig.sink->sent[0].value == -0.25f);
    REQUIRE(rig.proc->sent_count(0) == 0);
    REQUIRE(rig.proc->sent_count(1) == 1);
}

TEST_CASE("each channel has its own threshold", "[brew][osc][stereo]") {
    Rig rig;
    rig.enable();
    rig.store.set_value(opid(CvOscProcessor::kThreshold, 0), 0.5f);
    rig.store.set_value(opid(CvOscProcessor::kThreshold, 1), 0.0f);

    rig.run(0.0f, 0.0f);
    rig.proc->publish_once();      // first send on both
    const auto after_first = rig.proc->sent_count(0);
    REQUIRE(after_first == 1);

    // A step of 0.1: under the left's threshold, over the right's zero.
    rig.run(0.1f, 0.1f);
    rig.proc->publish_once();
    REQUIRE(rig.proc->sent_count(0) == 1);
    REQUIRE(rig.proc->sent_count(1) == 2);
}

TEST_CASE("a channel switched off re-announces when it comes back",
          "[brew][osc][stereo]") {
    // Otherwise it resumes mid-threshold against a value the receiver last heard
    // before the channel went quiet, and a static voltage never gets corrected.
    Rig rig;
    rig.enable();
    rig.set_threshold(0.5f);
    rig.run(0.2f, 0.2f);
    rig.proc->publish_once();
    const auto baseline = rig.proc->sent_count(0);

    rig.disable(0);
    rig.proc->publish_once();
    REQUIRE(rig.proc->sent_count(0) == baseline);

    // Back on, same unmoved value: it is announced again.
    rig.enable(0);
    rig.proc->publish_once();
    REQUIRE(rig.proc->sent_count(0) == baseline + 1);
}

// -------------------------------------------------------------------- the threshold

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

TEST_CASE("a value that has not moved past the threshold is not resent",
          "[brew][osc]") {
    // Float noise on the last bit of a steady voltage would otherwise flood the
    // receiver at the full send rate, and a receiver smoothing its input cannot
    // tell that flood from a real signal.
    Rig rig;
    rig.enable();
    rig.set_threshold(0.01f);

    rig.run(0.5f, 0.5f);
    rig.proc->publish_once();
    const auto after_first = rig.sink->sent.size();
    REQUIRE(after_first == 2);

    // Below the threshold: silence.
    rig.run(0.505f, 0.505f);
    rig.proc->publish_once();
    REQUIRE(rig.sink->sent.size() == after_first);

    // Clearly past it: a send. (0.51 would not do — `0.51f - 0.5f` is
    // 0.00999999… and lands *under* a 0.01 threshold. The `>=` boundary itself is
    // pinned below, on exactly-representable values, where the claim is about the
    // comparison rather than about float.)
    rig.run(0.52f, 0.52f);
    rig.proc->publish_once();
    REQUIRE(rig.sink->sent.size() == after_first + 2);
}

TEST_CASE("a step of exactly the threshold is sent", "[brew][osc]") {
    // `>=`, not `>`: a signal that advances by exactly the threshold every tick
    // would otherwise never be reported and would sit frozen at the receiver.
    // Powers of two, so the subtraction is exact and this tests the comparison.
    REQUIRE(should_send(false, 0.5f, 0.75f, 0.25f));
    REQUIRE_FALSE(should_send(false, 0.5f, 0.625f, 0.25f));
    // Symmetric: it is a magnitude, so falling counts as much as rising.
    REQUIRE(should_send(false, 0.5f, 0.25f, 0.25f));
}

TEST_CASE("a zero threshold sends on every tick", "[brew][osc]") {
    Rig rig;
    rig.enable();
    rig.set_threshold(0.0f);
    rig.run(0.5f, 0.5f);
    rig.proc->publish_once();
    rig.proc->publish_once();
    rig.proc->publish_once();
    REQUIRE(rig.sink->sent.size() == 6);
}

TEST_CASE("the threshold is measured from the last value actually sent",
          "[brew][osc]") {
    // Measuring against the previous *sample* instead would let a slow ramp creep
    // arbitrarily far without ever crossing the threshold in one step — the
    // receiver's value would drift away from the truth and never be corrected.
    Rig rig;
    rig.enable();
    rig.set_threshold(0.1f);
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

TEST_CASE("a non-finite sample is never put on the wire", "[brew][osc][safety]") {
    // A NaN reaching a receiver is a NaN in someone else's synth. The threshold
    // comparison against NaN is false either way, so the guard has to be explicit.
    REQUIRE_FALSE(should_send(false, 0.0f, std::nanf(""), 0.001f));
    REQUIRE_FALSE(should_send(false, 0.0f,
                              std::numeric_limits<float>::infinity(), 0.001f));
    // But a first send of a finite value still goes.
    REQUIRE(should_send(true, 0.0f, 0.5f, 1.0f));
}

// ------------------------------------------------------------- failures and targets

TEST_CASE("a refused connect sends nothing and does not latch the target",
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
    // receiver knows a value it never got, and the threshold would suppress the
    // retry until the signal moved again.
    auto sink = std::make_unique<RecordingSink>();
    sink->refuse_send = true;
    Rig rig(std::move(sink));
    rig.enable();
    rig.set_threshold(0.1f);
    rig.run(0.5f, 0.5f);
    rig.proc->publish_once();
    REQUIRE(rig.proc->sent_count() == 0);

    rig.sink->refuse_send = false;
    rig.proc->publish_once();
    REQUIRE(rig.sink->sent.size() == 2);
    REQUIRE(rig.sink->sent[0].value == 0.5f);
}

TEST_CASE("changing the target reconnects and re-announces the resting value",
          "[brew][osc]") {
    // A new receiver has never heard the value. Without the re-announce it would
    // wait for the signal to move before learning anything.
    Rig rig;
    rig.enable();
    rig.set_threshold(0.1f);
    rig.run(0.5f, 0.5f);
    rig.proc->publish_once();
    REQUIRE(rig.sink->connects.size() == 1);
    const auto before = rig.sink->sent.size();

    REQUIRE(rig.proc->set_target("studio.local:9001"));
    rig.proc->publish_once();
    REQUIRE(rig.sink->connects.size() == 2);
    REQUIRE(rig.sink->connects[1].first == "studio.local");
    REQUIRE(rig.sink->connects[1].second == 9001);
    REQUIRE(rig.sink->sent.size() == before + 2);   // resent, despite no movement
}

TEST_CASE("setting the same target does not reconnect", "[brew][osc]") {
    // A DAW that pushes the field's text back on every automation pass would
    // otherwise tear the socket down and re-announce forever.
    Rig rig;
    rig.enable();
    rig.run(0.5f, 0.5f);
    rig.proc->publish_once();
    REQUIRE(rig.sink->connects.size() == 1);

    REQUIRE(rig.proc->set_target("127.0.0.1:9000"));
    rig.proc->publish_once();
    REQUIRE(rig.sink->connects.size() == 1);
}

TEST_CASE("an OSC path the user types is the address on the wire", "[brew][osc]") {
    Rig rig;
    rig.enable();
    REQUIRE(rig.proc->set_path(0, "/synth/cutoff"));
    REQUIRE(rig.proc->set_path(1, "/synth/resonance"));
    rig.run(0.1f, 0.2f);
    rig.proc->publish_once();

    REQUIRE(rig.sink->sent.size() == 2);
    REQUIRE(rig.sink->sent[0].address == "/synth/cutoff");
    REQUIRE(rig.sink->sent[1].address == "/synth/resonance");
}

// ------------------------------------------------------------------ pure functions

TEST_CASE("the send interval is clamped away from zero and from a flood",
          "[brew][osc][safety]") {
    REQUIRE(send_interval_seconds(0.0f) == send_interval_seconds(kMinRateHz));
    REQUIRE(send_interval_seconds(-10.0f) == send_interval_seconds(kMinRateHz));
    REQUIRE(send_interval_seconds(1e9f) == send_interval_seconds(kMaxRateHz));
    REQUIRE(send_interval_seconds(60.0f) == Catch::Approx(1.0 / 60.0));
}

TEST_CASE("one default OSC address per channel", "[brew][osc]") {
    REQUIRE(default_osc_path(0) == "/brew/cv/0");
    REQUIRE(default_osc_path(1) == "/brew/cv/1");
}

TEST_CASE("a target is a host and an unprivileged port", "[brew][osc][target]") {
    SECTION("what parses") {
        REQUIRE(parse_osc_target("127.0.0.1:9000")->host == "127.0.0.1");
        REQUIRE(parse_osc_target("127.0.0.1:9000")->port == 9000);
        REQUIRE(parse_osc_target("studio.local:1024")->port == 1024);
        REQUIRE(parse_osc_target("host:65535")->port == 65535);
        // The port comes from the *last* colon, so an IPv6 literal survives.
        REQUIRE(parse_osc_target("[::1]:9000")->host == "[::1]");
        REQUIRE(parse_osc_target("[::1]:9000")->port == 9000);
    }
    SECTION("what does not") {
        REQUIRE_FALSE(parse_osc_target("127.0.0.1"));        // no port
        REQUIRE_FALSE(parse_osc_target("127.0.0.1:"));       // empty port
        REQUIRE_FALSE(parse_osc_target(":9000"));            // empty host
        REQUIRE_FALSE(parse_osc_target("host:80"));          // privileged: needs root
        REQUIRE_FALSE(parse_osc_target("host:1023"));        // one below the floor
        REQUIRE_FALSE(parse_osc_target("host:65536"));       // one past the ceiling
        REQUIRE_FALSE(parse_osc_target("host:9000x"));       // not a number
        REQUIRE_FALSE(parse_osc_target("host name:9000"));   // whitespace
        REQUIRE_FALSE(parse_osc_target("[::1:9000"));        // unclosed bracket
    }
    SECTION("it round-trips through its own text") {
        const auto t = parse_osc_target("192.168.1.20:7770");
        REQUIRE(t);
        REQUIRE(format_osc_target(*t) == "192.168.1.20:7770");
    }
}

TEST_CASE("a sendable OSC path is not an address pattern", "[brew][osc][target]") {
    // The rule that matters. `/cv/*` is a legal thing for a receiver to match
    // against and an illegal thing for a sender to put in a packet — a bridge that
    // shipped one would look like it was working while the receiver ignored it.
    REQUIRE(is_valid_osc_path("/brew/cv/0"));
    REQUIRE(is_valid_osc_path("/a"));
    REQUIRE(is_valid_osc_path("/synth/1/cutoff-hz"));

    REQUIRE_FALSE(is_valid_osc_path(""));
    REQUIRE_FALSE(is_valid_osc_path("/"));
    REQUIRE_FALSE(is_valid_osc_path("brew/cv/0"));   // no leading slash
    REQUIRE_FALSE(is_valid_osc_path("/brew/"));      // trailing slash
    REQUIRE_FALSE(is_valid_osc_path("/brew//cv"));   // empty part
    REQUIRE_FALSE(is_valid_osc_path("/brew cv"));    // space
    for (const char* pattern : {"/cv/*", "/cv/?", "/cv/[01]", "/cv/{a,b}", "/cv/#"}) {
        CAPTURE(pattern);
        REQUIRE_FALSE(is_valid_osc_path(pattern));
    }
}

TEST_CASE("the plug-in refuses text it cannot use", "[brew][osc][target]") {
    // Rejected rather than sanitized. A silently corrected hostname is a plug-in
    // sending somewhere the user did not ask for.
    Rig rig;
    REQUIRE_FALSE(rig.proc->set_target("nonsense"));
    REQUIRE(rig.proc->osc_settings().target.host == "127.0.0.1");

    REQUIRE_FALSE(rig.proc->set_path(0, "/brew/*"));
    REQUIRE_FALSE(rig.proc->set_path(kOscChannels, "/valid"));   // no such channel
    REQUIRE(rig.proc->osc_settings().paths[0] == "/brew/cv/0");
}

// ------------------------------------------------------------------- text state

TEST_CASE("the target and the paths survive a save and a load",
          "[brew][osc][state]") {
    // A hostname is not a float, so this half of the state lives outside the
    // StateStore, in the blob every format adapter already round-trips.
    format::HeadlessHost host([] { return create_cv_osc(); });
    REQUIRE(host.valid());
    auto* proc = host.processor_as<CvOscProcessor>();
    REQUIRE(proc != nullptr);

    REQUIRE(proc->set_target("10.0.0.4:5510"));
    REQUIRE(proc->set_path(0, "/rack/vco/pitch"));
    REQUIRE(proc->set_path(1, "/rack/vcf/cutoff"));
    host.state().set_value(opid(CvOscProcessor::kEnable, 1), 1.0f);
    const auto blob = host.save_state();

    format::HeadlessHost fresh([] { return create_cv_osc(); });
    REQUIRE(fresh.load_state(blob));
    auto* restored = fresh.processor_as<CvOscProcessor>();
    REQUIRE(restored != nullptr);

    const auto settings = restored->osc_settings();
    REQUIRE(settings.target.host == "10.0.0.4");
    REQUIRE(settings.target.port == 5510);
    REQUIRE(settings.paths[0] == "/rack/vco/pitch");
    REQUIRE(settings.paths[1] == "/rack/vcf/cutoff");
    // ...and the float half came back too.
    REQUIRE(restored->enabled(1));
    REQUIRE_FALSE(restored->enabled(0));
}

TEST_CASE("a text blob we did not write is refused", "[brew][osc][state]") {
    // A blob with no header is a blob whose fields we cannot claim to understand,
    // and guessing gives the sender thread a destination nobody typed.
    REQUIRE_FALSE(deserialize_osc_settings(""));
    REQUIRE_FALSE(deserialize_osc_settings("host=1.2.3.4\n"));
    REQUIRE_FALSE(deserialize_osc_settings("brew.cvosc 99\nhost=1.2.3.4\n"));
}

TEST_CASE("a bad field in a good blob keeps its default", "[brew][osc][state]") {
    // One mistyped path should not stop a project from opening. The target it
    // *can* read is still the target the user chose.
    const auto s = deserialize_osc_settings(
        "brew.cvosc 1\nhost=1.2.3.4\nport=80\npath0=/cv/*\npath1=/good\nnoise\n");
    REQUIRE(s);
    REQUIRE(s->target.host == "1.2.3.4");
    REQUIRE(s->target.port == kDefaultOscPort);   // 80 is privileged: refused
    REQUIRE(s->paths[0] == "/brew/cv/0");         // a pattern: refused
    REQUIRE(s->paths[1] == "/good");
}

TEST_CASE("the serialized blob round-trips exactly", "[brew][osc][state]") {
    OscSettings s;
    s.target = {"studio.local", 7000};
    s.paths = {"/a/b", "/c/d"};
    const auto back = deserialize_osc_settings(serialize_osc_settings(s));
    REQUIRE(back);
    REQUIRE(*back == s);
}

TEST_CASE("an empty blob loads a fresh instance's defaults", "[brew][osc][state]") {
    // A project saved before this plug-in had text state should open, not fail.
    Rig rig;
    REQUIRE(rig.proc->deserialize_plugin_state({}));
    REQUIRE(rig.proc->osc_settings() == OscSettings{});
}

// -------------------------------------------------------------------- discovery

TEST_CASE("Browse lists the OSC receivers the network announces",
          "[brew][osc][discovery]") {
    OscDiscovery discovery;
    auto backend = std::make_unique<FakeMdns>();
    auto* fake = backend.get();
    REQUIRE(discovery.start_with(std::move(backend)));
    REQUIRE(fake->browsed_type == kOscServiceType);

    fake->announce("Modular Rack", "192.168.1.50", 9001);
    fake->announce("Lighting Desk", "192.168.1.51", 7770);
    auto found = discovery.receivers();
    REQUIRE(found.size() == 2);
    REQUIRE(found[0].name == "Modular Rack");
    REQUIRE(format_osc_target(found[0].target) == "192.168.1.50:9001");

    SECTION("a re-announced service replaces its entry rather than doubling it") {
        fake->announce("Modular Rack", "192.168.1.60", 9002);
        found = discovery.receivers();
        REQUIRE(found.size() == 2);
        REQUIRE(format_osc_target(found[0].target) == "192.168.1.60:9002");
    }

    SECTION("a service that disappears is dropped") {
        fake->withdraw("Lighting Desk");
        REQUIRE(discovery.receivers().size() == 1);
    }

    SECTION("a receiver on a privileged port is not offered") {
        // Clicking it would fill the Target field with text the plug-in itself
        // would then refuse — an offer it cannot honor.
        fake->announce("Root Listener", "192.168.1.52", 80);
        REQUIRE(discovery.receivers().size() == 2);
    }

    SECTION("stopping clears the list") {
        discovery.stop();
        REQUIRE_FALSE(discovery.running());
        REQUIRE(discovery.receivers().empty());
    }
}

TEST_CASE("clicking a discovered receiver sets the target",
          "[brew][osc][discovery]") {
    Rig rig;
    CvOscUi ui(rig.store, *rig.proc);

    auto backend = std::make_unique<FakeMdns>();
    auto* fake = backend.get();
    REQUIRE(ui.discovery().start_with(std::move(backend)));
    fake->announce("Modular Rack", "192.168.1.50", 9001);

    // Nothing is offered until the editor pulls a snapshot — the browse callback
    // runs on the backend's thread, and the buttons are only ever touched on this
    // one.
    ui.use_receiver(0);
    REQUIRE(rig.proc->osc_settings().target.host == "127.0.0.1");

    ui.poll_discovery();   // what `paint` does, without a canvas
    REQUIRE(ui.offered().size() == 1);
    ui.use_receiver(0);
    REQUIRE(rig.proc->osc_settings().target.host == "192.168.1.50");
    REQUIRE(rig.proc->osc_settings().target.port == 9001);
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
        store.set_value(opid(CvOscProcessor::kEnable, 0), 1.0f);
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
        store.set_value(opid(CvOscProcessor::kEnable, 0), 1.0f);
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
    store.set_value(opid(CvOscProcessor::kEnable, 0), 1.0f);
    REQUIRE(proc.set_target("127.0.0.1:" + std::to_string(kPort)));

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

// --------------------------------------------------------------- thread lifetime

TEST_CASE("the sender thread survives a host that destroys its store first",
          "[brew][osc][lifetime]") {
    // `Processor::state()` dereferences a pointer the host installs, and Pulp's
    // own hosts — HeadlessHost, the CLAP adapter, the VST3 adapter — all declare
    // their Processor before their StateStore, so the store is destroyed first and
    // the Processor's destructor runs against a corpse.
    //
    // For a Processor with no threads that is invisible: nothing reads the store
    // after teardown begins. This one has a sender thread that ticks every ~16 ms,
    // and it is still ticking while `~CvOscProcessor` walks to its `join()`. One
    // `get_value` in that window is a use-after-free, and it crashed roughly one
    // plug-in close in eight.
    //
    // The fix is that the sender thread reads no store at all: the audio thread
    // publishes what it needs. This test pins that by doing to the processor
    // exactly what the hosts do — destroying the store out from under a running
    // sender thread — and requiring it to keep running.
    auto store = std::make_unique<state::StateStore>();
    auto proc = std::make_unique<CvOscProcessor>(std::make_unique<RecordingSink>());
    proc->set_state_store(store.get());
    proc->define_parameters(*store);

    format::PrepareContext ctx;
    ctx.sample_rate = 48000.0;
    ctx.max_buffer_size = 512;
    ctx.input_channels = 2;
    ctx.output_channels = 2;
    proc->prepare(ctx);  // starts the sender thread

    // Long enough for the thread to have completed a tick and be inside its next.
    std::this_thread::sleep_for(std::chrono::milliseconds(40));

    store.reset();
    // Long enough for several more ticks against the store that no longer exists.
    std::this_thread::sleep_for(std::chrono::milliseconds(40));

    proc.reset();  // joins the sender thread
    SUCCEED("the sender thread never read the destroyed store");
}
