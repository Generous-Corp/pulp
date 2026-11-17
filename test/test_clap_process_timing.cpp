// CLAP adapter -> public Processor -> host event timeline traces.
//
// The real CLAP adapter is driven through its C entry points. A Processor
// records the ProcessContext and MIDI it receives; a test-side host binding
// then advances one HostBlockTimeline from that context alone and projects
// the delivered events through HostEventIngress, as a graph-owning Processor
// would. Nothing below reads CLAP fields directly after the adapter call.

#include <catch2/catch_test_macros.hpp>

#include <pulp/format/clap_adapter.hpp>
#include <pulp/format/processor.hpp>
#include <pulp/host/graph_event_contract.hpp>
#include <pulp/host/host_event_timestamps.hpp>

#include <clap/clap.h>

#include <cmath>
#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

using namespace pulp;
using namespace pulp::format;
using host::HostBlockBoundary;
using host::HostBlockTimeline;
using host::HostBlockWindow;
using host::HostEventIngress;
using host::HostEventRecord;
using host::HostEventResult;

namespace {

constexpr double kSampleRate = 44100.0;
constexpr std::uint32_t kMaxFrames = 512;

struct ObservedBlock {
    ProcessContext ctx;
    std::vector<std::int32_t> offsets;
};

std::vector<ObservedBlock>* g_sink = nullptr;

class TimingProbe final : public Processor {
  public:
    PluginDescriptor descriptor() const override {
        PluginDescriptor d;
        d.name = "PulpTimingProbe";
        d.manufacturer = "PulpTest";
        d.bundle_id = "com.pulp.test.process-timing";
        d.version = "1.0.0";
        d.category = PluginCategory::Effect;
        d.input_buses = {{"In", 2}};
        d.output_buses = {{"Out", 2}};
        d.accepts_midi = true;
        return d;
    }
    void define_parameters(state::StateStore&) override {}
    void prepare(const PrepareContext&) override {}
    void process(audio::BufferView<float>& out, const audio::BufferView<const float>&,
                 midi::MidiBuffer& midi_in, midi::MidiBuffer&, const ProcessContext& ctx) override {
        for (std::size_t ch = 0; ch < out.num_channels(); ++ch) {
            auto oc = out.channel(ch);
            for (std::size_t i = 0; i < out.num_samples(); ++i)
                oc[i] = 0.0f;
        }
        if (!g_sink)
            return;
        ObservedBlock block;
        block.ctx = ctx;
        for (const auto& event : midi_in)
            block.offsets.push_back(event.sample_offset);
        g_sink->push_back(block);
    }
};

std::unique_ptr<Processor> make_probe() {
    return std::make_unique<TimingProbe>();
}

struct HostTransport {
    std::int64_t sample = 0;
    bool playing = true;
};

// One CLAP host instance plus the stimulus plumbing for a block.
struct ClapHost {
    clap_adapter::PulpClapPlugin plugin;
    std::vector<ObservedBlock> observed;

    ClapHost() {
        g_sink = &observed;
        plugin.factory = &make_probe;
        plugin.plugin.plugin_data = &plugin;
        REQUIRE(clap_adapter::clap_init(&plugin.plugin));
        REQUIRE(clap_adapter::clap_activate(&plugin.plugin, kSampleRate, 1, kMaxFrames));
        REQUIRE(clap_adapter::clap_start_processing(&plugin.plugin));
    }
    ~ClapHost() {
        clap_adapter::clap_deactivate(&plugin.plugin);
        g_sink = nullptr;
    }

    // Run one host block. `steady` < 0 means the host has no steady_time;
    // `transport` absent means no transport event. `event_times` are CLAP
    // header times. `malformed` delivers a layout the adapter must refuse.
    void run(std::int64_t steady, std::uint32_t frames, std::optional<HostTransport> transport,
             const std::vector<std::uint32_t>& event_times = {}, bool malformed = false) {
        std::vector<float> in_l(frames), in_r(frames), out_l(frames), out_r(frames);
        float* in_ptrs[2] = {in_l.data(), in_r.data()};
        float* out_ptrs[2] = {out_l.data(), out_r.data()};
        clap_audio_buffer_t in_bus{};
        in_bus.channel_count = 2;
        in_bus.data32 = in_ptrs;
        clap_audio_buffer_t out_bus{};
        out_bus.channel_count = 2;
        out_bus.data32 = out_ptrs;

        std::vector<clap_event_note_t> notes;
        for (const auto t : event_times) {
            clap_event_note_t note{};
            note.header.size = sizeof(note);
            note.header.type = CLAP_EVENT_NOTE_ON;
            note.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
            note.header.time = t;
            note.port_index = 0;
            note.channel = 0;
            note.key = 60;
            note.note_id = -1;
            note.velocity = 0.8;
            notes.push_back(note);
        }
        struct EventList {
            std::vector<clap_event_note_t>* notes;
            static std::uint32_t size(const clap_input_events_t* l) {
                return static_cast<std::uint32_t>(static_cast<EventList*>(l->ctx)->notes->size());
            }
            static const clap_event_header_t* get(const clap_input_events_t* l, std::uint32_t i) {
                return &(*static_cast<EventList*>(l->ctx)->notes)[i].header;
            }
        } list{&notes};
        clap_input_events_t in_events{};
        in_events.ctx = &list;
        in_events.size = &EventList::size;
        in_events.get = &EventList::get;

        clap_event_transport_t tr{};
        if (transport) {
            tr.header.size = sizeof(tr);
            tr.header.type = CLAP_EVENT_TRANSPORT;
            tr.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
            tr.flags = CLAP_TRANSPORT_HAS_SECONDS_TIMELINE |
                       (transport->playing ? CLAP_TRANSPORT_IS_PLAYING : 0u);
            const long double seconds = static_cast<long double>(transport->sample) / kSampleRate;
            tr.song_pos_seconds =
                static_cast<clap_sectime>(std::llround(seconds * CLAP_SECTIME_FACTOR));
        }

        clap_process_t proc{};
        proc.steady_time = steady;
        proc.frames_count = frames;
        proc.audio_inputs = &in_bus;
        proc.audio_inputs_count = malformed ? 0 : 1;
        proc.audio_outputs = &out_bus;
        proc.audio_outputs_count = 1;
        proc.in_events = &in_events;
        proc.transport = transport ? &tr : nullptr;
        REQUIRE(clap_adapter::clap_process(&plugin.plugin, &proc) == CLAP_PROCESS_CONTINUE);
    }
};

// The host binding a graph-owning Processor runs per block.
struct BlockTrace {
    HostBlockBoundary boundary = HostBlockBoundary::First;
    HostBlockWindow window;
    std::vector<HostEventResult> results;
    std::vector<std::int64_t> projected;
};

struct Binding {
    HostBlockTimeline timeline;
    HostEventIngress<64> ingress;
    std::uint64_t sequence = 0;

    BlockTrace feed(const ObservedBlock& block) {
        BlockTrace trace;
        REQUIRE(host::advance_from_context(timeline, block.ctx, trace.window) ==
                HostEventResult::Accepted);
        trace.boundary = timeline.last_boundary();
        REQUIRE(ingress.begin_block(trace.window.epoch, trace.window.sample_start,
                                    trace.window.frames) == HostEventResult::Accepted);
        for (const auto offset : block.offsets) {
            HostEventRecord record;
            auto result =
                host::make_record_from_block_offset(timeline, trace.window, offset, 1, ++sequence,
                                                    midi::MidiEvent::note_on(0, 60, 100), record);
            if (result == HostEventResult::Accepted)
                result = ingress.publish(record);
            trace.results.push_back(result);
        }
        std::array<HostEventRecord, 64> out{};
        std::size_t count = 0;
        REQUIRE(ingress.project(out, count) == HostEventResult::Accepted);
        for (std::size_t i = 0; i < count; ++i)
            trace.projected.push_back(out[i].absolute_sample);
        REQUIRE(ingress.end_block() == HostEventResult::Accepted);
        return trace;
    }
};

} // namespace

TEST_CASE("CLAP activation marks the first observed block discontinuous",
          "[process-timing][clap][activation]") {
    ClapHost host;
    // The adapter's warm-up render during activation is not a block the
    // Processor saw and must not consume the activation flag.
    REQUIRE(host.observed.empty());
    host.run(0, 64, HostTransport{0, true});
    host.run(64, 64, HostTransport{64, true});
    REQUIRE(host.observed.size() == 2);
    const auto& first = host.observed[0].ctx.timing;
    REQUIRE(first.discontinuity);
    REQUIRE(first.has_steady_sample);
    REQUIRE(first.steady_sample == 0);
    REQUIRE(first.has_transport_sample);
    REQUIRE(first.transport_moving);
    REQUIRE(first.playback_rate == PlaybackRate::Unknown);
    REQUIRE_FALSE(host.observed[1].ctx.timing.discontinuity);
    REQUIRE(host.observed[1].ctx.timing.transport_sample == 64);
    // The neutral transport fact agrees with the legacy field.
    REQUIRE(host.observed[1].ctx.position_samples == 64);

    // clap_reset() is a discontinuity for exactly the next observed block.
    clap_adapter::clap_reset(&host.plugin.plugin);
    host.run(128, 64, HostTransport{128, true});
    host.run(192, 64, HostTransport{192, true});
    REQUIRE(host.observed[2].ctx.timing.discontinuity);
    REQUIRE_FALSE(host.observed[3].ctx.timing.discontinuity);
}

TEST_CASE("CLAP timing projects identically for every block partition",
          "[process-timing][clap][partition]") {
    // Events at absolute host samples; the seconds transport is the CLAP
    // fixed-point value a 44.1 kHz host derives from each block start.
    const std::vector<std::int64_t> event_samples{0, 1, 100, 127, 128, 200, 383};
    const std::vector<std::vector<std::uint32_t>> partitions{
        {384}, {128, 128, 128}, {64, 64, 64, 64, 64, 64}, {127, 127, 130}, {1, 255, 128}};
    std::vector<std::vector<std::int64_t>> projections;
    for (const auto& partition : partitions) {
        ClapHost host;
        std::int64_t start = 4410;
        for (const auto frames : partition) {
            std::vector<std::uint32_t> times;
            for (const auto sample : event_samples)
                if (sample >= start - 4410 && sample < start - 4410 + frames)
                    times.push_back(static_cast<std::uint32_t>(sample - (start - 4410)));
            host.run(start, frames, HostTransport{start, true}, times);
            start += frames;
        }
        Binding binding;
        std::vector<std::int64_t> projected;
        for (std::size_t i = 0; i < host.observed.size(); ++i) {
            const auto trace = binding.feed(host.observed[i]);
            REQUIRE(trace.boundary ==
                    (i == 0 ? HostBlockBoundary::First : HostBlockBoundary::Continuous));
            REQUIRE(trace.window.epoch == 1);
            projected.insert(projected.end(), trace.projected.begin(), trace.projected.end());
        }
        projections.push_back(projected);
    }
    REQUIRE(projections.front() == event_samples);
    for (const auto& projection : projections)
        REQUIRE(projection == projections.front());
}

TEST_CASE("CLAP seeks, loops, stopped transport and skipped blocks place blocks correctly",
          "[process-timing][clap][boundaries]") {
    ClapHost host;
    host.run(0, 64, HostTransport{1000, true});      // activation
    host.run(64, 64, HostTransport{1064, true});     // continuous
    host.run(128, 64, HostTransport{9000, true});    // seek while playing
    host.run(192, 64, HostTransport{9064, true});    // continuous after seek
    host.run(256, 64, HostTransport{4000, true});    // loop wrap back to loop start
    host.run(320, 64, HostTransport{4064, false});   // stop
    host.run(384, 64, HostTransport{20000, false});  // relocate while stopped
    host.run(1024, 64, HostTransport{20000, false}); // host skipped blocks
    host.run(1088, 64, HostTransport{20000, true});  // play again
    host.run(1280, 64, HostTransport{20192, true});  // playing gap: transport and counter agree
    REQUIRE(host.observed.size() == 10);

    Binding binding;
    std::vector<HostBlockBoundary> boundaries;
    std::vector<std::uint64_t> starts;
    for (const auto& block : host.observed) {
        const auto trace = binding.feed(block);
        boundaries.push_back(trace.boundary);
        starts.push_back(trace.window.sample_start);
    }
    REQUIRE(boundaries == std::vector<HostBlockBoundary>{
                              HostBlockBoundary::First, HostBlockBoundary::Continuous,
                              HostBlockBoundary::NewEpoch, HostBlockBoundary::Continuous,
                              HostBlockBoundary::NewEpoch, HostBlockBoundary::Continuous,
                              HostBlockBoundary::Continuous, HostBlockBoundary::Gap,
                              HostBlockBoundary::Continuous, HostBlockBoundary::Gap});
    // The gaps advance the window by the skipped samples in the same epoch.
    REQUIRE(starts[7] == 1024 - 256);
    REQUIRE(starts[9] == 1280 - 256);
    REQUIRE(binding.timeline.current().epoch == 3);
}

TEST_CASE("CLAP blocks the Processor never saw are measured or marked",
          "[process-timing][clap][skipped]") {
    SECTION("with steady_time the skip is a gap") {
        ClapHost host;
        host.run(0, 64, std::nullopt);
        host.run(64, 64, std::nullopt, {}, /*malformed=*/true); // adapter refuses the layout
        host.run(128, 64, std::nullopt);
        REQUIRE(host.observed.size() == 2);
        REQUIRE_FALSE(host.observed[1].ctx.timing.discontinuity);
        Binding binding;
        (void)binding.feed(host.observed[0]);
        const auto trace = binding.feed(host.observed[1]);
        REQUIRE(trace.boundary == HostBlockBoundary::Gap);
        REQUIRE(trace.window.sample_start == 128);
    }
    SECTION("without steady_time the next observed block is a discontinuity") {
        ClapHost host;
        host.run(-1, 64, std::nullopt);
        host.run(-1, 64, std::nullopt, {}, /*malformed=*/true);
        host.run(-1, 64, std::nullopt);
        host.run(-1, 64, std::nullopt);
        REQUIRE(host.observed.size() == 3);
        REQUIRE(host.observed[1].ctx.timing.discontinuity);
        REQUIRE_FALSE(host.observed[2].ctx.timing.discontinuity);
        Binding binding;
        (void)binding.feed(host.observed[0]);
        REQUIRE(binding.feed(host.observed[1]).boundary == HostBlockBoundary::NewEpoch);
        REQUIRE(binding.feed(host.observed[2]).boundary == HostBlockBoundary::Continuous);

        // Negative control: without the adapter's fact the skipped samples
        // would be silently retimed onto a contiguous window.
        auto stripped = host.observed[1];
        stripped.ctx.timing.discontinuity = false;
        Binding control;
        (void)control.feed(host.observed[0]);
        REQUIRE(control.feed(stripped).boundary == HostBlockBoundary::Continuous);
    }
    SECTION("a block clamped to the prepared maximum leaves its tail unseen") {
        ClapHost host;
        host.run(-1, 64, std::nullopt);
        host.run(-1, 2 * kMaxFrames, std::nullopt); // renders only kMaxFrames
        host.run(-1, 64, std::nullopt);
        host.run(-1, 64, std::nullopt);
        REQUIRE(host.observed.size() == 4);
        REQUIRE(host.observed[1].ctx.num_samples == static_cast<int>(kMaxFrames));
        REQUIRE_FALSE(host.observed[1].ctx.timing.discontinuity);
        REQUIRE(host.observed[2].ctx.timing.discontinuity);
        REQUIRE_FALSE(host.observed[3].ctx.timing.discontinuity);
    }
}

TEST_CASE("CLAP clock-source changes open a new epoch", "[process-timing][clap][clock]") {
    ClapHost host;
    host.run(-1, 64, std::nullopt);
    host.run(-1, 64, std::nullopt);
    host.run(128, 64, std::nullopt); // steady_time appears
    host.run(192, 64, std::nullopt);
    host.run(-1, 64, std::nullopt); // and disappears
    REQUIRE_FALSE(host.observed[0].ctx.timing.has_steady_sample);
    REQUIRE(host.observed[2].ctx.timing.has_steady_sample);
    Binding binding;
    std::vector<HostBlockBoundary> boundaries;
    for (const auto& block : host.observed)
        boundaries.push_back(binding.feed(block).boundary);
    REQUIRE(boundaries == std::vector<HostBlockBoundary>{
                              HostBlockBoundary::First, HostBlockBoundary::Continuous,
                              HostBlockBoundary::NewEpoch, HostBlockBoundary::Continuous,
                              HostBlockBoundary::NewEpoch});
}

TEST_CASE("CLAP event offsets outside the block are refused, never clamped",
          "[process-timing][clap][offsets]") {
    ClapHost host;
    host.run(0, 64, HostTransport{0, true}, {0, 63, 64, 0xFFFFFFF0u});
    REQUIRE(host.observed.size() == 1);
    // The adapter forwards host times as delivered; the projection decides.
    REQUIRE(host.observed[0].offsets.size() == 4);
    Binding binding;
    const auto trace = binding.feed(host.observed[0]);
    REQUIRE(trace.results ==
            std::vector<HostEventResult>{HostEventResult::Accepted, HostEventResult::Accepted,
                                         HostEventResult::OutOfRange, HostEventResult::OutOfRange});
    REQUIRE(trace.projected == std::vector<std::int64_t>{0, 63});
}

TEST_CASE("CLAP records timed against a retired epoch are refused",
          "[process-timing][clap][stale]") {
    ClapHost host;
    host.run(0, 64, HostTransport{0, true}, {10});
    host.run(64, 64, HostTransport{50000, true}, {10}); // seek
    Binding binding;
    const auto before = binding.feed(host.observed[0]);
    REQUIRE(before.projected == std::vector<std::int64_t>{10});
    // A record built against the old window, then the seek.
    HostEventRecord stale;
    REQUIRE(binding.timeline.make_record(before.window, 20, 1, 99,
                                         midi::MidiEvent::note_on(0, 61, 100),
                                         stale) == HostEventResult::Accepted);
    const auto after = binding.feed(host.observed[1]);
    REQUIRE(after.boundary == HostBlockBoundary::NewEpoch);
    REQUIRE(after.projected == std::vector<std::int64_t>{10});
    REQUIRE(binding.ingress.begin_block(after.window.epoch, after.window.sample_start + 64, 64) ==
            HostEventResult::Accepted);
    REQUIRE(binding.ingress.publish(stale) == HostEventResult::StaleEpoch);
    std::int64_t absolute = 0;
    REQUIRE(binding.timeline.project(before.window, 20, absolute) == HostEventResult::StaleEpoch);
    REQUIRE(binding.ingress.end_block() == HostEventResult::Accepted);
}
