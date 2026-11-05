// Processor-level MIDI panic: request_panic() raised from any thread, serviced
// by the host at the next block, voices reset and panic written to MIDI out.

#include <catch2/catch_test_macros.hpp>

#include <pulp/format/headless.hpp>
#include <pulp/format/panic.hpp>
#include <pulp/format/processor.hpp>
#include <pulp/midi/panic.hpp>
#include <pulp/midi/synthesiser.hpp>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <thread>
#include <vector>

using namespace pulp;
using format::PanicMode;

namespace {

// Sounds a constant while held, and for kReleaseSamples after note-off.
class ToneVoice : public midi::SynthesiserVoice {
  public:
    static constexpr int kReleaseSamples = 4096;
    void on_note_on(const midi::SynthesiserNote& note) override {
        SynthesiserVoice::on_note_on(note);
        release_left_ = kReleaseSamples;
    }
    void render(float* out, int num_samples) override {
        if (!active_)
            return;
        for (int i = 0; i < num_samples; ++i) {
            out[i] += 0.5f;
            if (releasing_ && --release_left_ <= 0) {
                mark_inactive();
                return;
            }
        }
    }

  private:
    int release_left_ = 0;
};

class PanicSynth : public format::Processor {
  public:
    explicit PanicSynth(bool produces_midi) : produces_midi_(produces_midi) {}

    format::PluginDescriptor descriptor() const override {
        return {
            .name = "PanicSynth",
            .manufacturer = "PulpTest",
            .bundle_id = "com.pulp.test.panic-synth",
            .version = "1.0.0",
            .category = format::PluginCategory::Instrument,
            .input_buses = {},
            .output_buses = {{"Out", 2}},
            .accepts_midi = true,
            .produces_midi = produces_midi_,
        };
    }
    void define_parameters(state::StateStore&) override {}
    void prepare(const format::PrepareContext&) override {}

    void on_panic(PanicMode mode) override {
        ++panic_calls;
        last_mode = mode;
        synth_.panic(mode == PanicMode::immediate);
    }

    void process(audio::BufferView<float>& output, const audio::BufferView<const float>&,
                 midi::MidiBuffer& midi_in, midi::MidiBuffer& midi_out,
                 const format::ProcessContext&) override {
        const int n = static_cast<int>(output.num_samples());
        float* left = output.channel_ptr(0);
        std::fill(left, left + n, 0.0f);
        synth_.process(midi_in, left, n);
        for (std::size_t c = 1; c < output.num_channels(); ++c)
            std::copy(left, left + n, output.channel_ptr(c));
        if (produces_midi_) {
            // Pass notes through downstream so the host can track them.
            for (const auto& e : midi_in)
                midi_out.add(e);
        }
    }

    int panic_calls = 0;
    PanicMode last_mode = PanicMode::release;

  private:
    bool produces_midi_;
    midi::Synthesiser<ToneVoice> synth_{4};
};

PanicSynth* g_synth = nullptr;

std::unique_ptr<format::Processor> make_synth_with_midi() {
    auto p = std::make_unique<PanicSynth>(true);
    g_synth = p.get();
    return p;
}
std::unique_ptr<format::Processor> make_synth_without_midi() {
    auto p = std::make_unique<PanicSynth>(false);
    g_synth = p.get();
    return p;
}

struct Rig {
    explicit Rig(format::ProcessorFactory factory, std::size_t midi_capacity = 1024)
        : host(factory), out(2, kBlock) {
        host.prepare(48000.0, kBlock, 0, 2);
        midi_out.reserve(midi_capacity);
        // A small capacity is the point of the overflow cases: refuse events
        // instead of growing, as the adapters' fixed buffers do.
        if (midi_capacity < 1024)
            midi_out.set_realtime_capacity_limit(true);
    }
    static constexpr std::size_t kBlock = 256;

    // One block with `midi_in` (may be empty); returns peak of channel 0.
    float run(midi::MidiBuffer& midi_in) {
        midi_out.clear();
        auto view = out.view();
        audio::BufferView<const float> no_input;
        host.process(view, no_input, midi_in, midi_out);
        float peak = 0.0f;
        for (std::size_t i = 0; i < kBlock; ++i)
            peak = std::max(peak, std::fabs(out.channel(0)[i]));
        return peak;
    }
    float run() {
        midi::MidiBuffer none;
        return run(none);
    }
    float run_note_on(std::uint8_t note = 60) {
        midi::MidiBuffer in;
        in.add(midi::MidiEvent::note_on(0, note, 100));
        return run(in);
    }

    std::size_t count_kind(format::PanicMode, midi::PanicMessageKind kind) const {
        std::size_t n = 0;
        for (const auto& e : midi_out)
            if (midi::classify_panic_message(e) == kind)
                ++n;
        return n;
    }
    std::size_t note_offs() const {
        std::size_t n = 0;
        for (const auto& e : midi_out)
            if (e.message.isNoteOff())
                ++n;
        return n;
    }

    format::HeadlessHost host;
    audio::Buffer<float> out;
    midi::MidiBuffer midi_out;
};

} // namespace

TEST_CASE("request_panic stops a held note at once and the next note plays", "[format][panic]") {
    Rig rig(make_synth_with_midi);
    REQUIRE(rig.run_note_on(60) > 0.4f);
    REQUIRE(rig.run() > 0.4f); // still held

    g_synth->request_panic(); // default: immediate
    CHECK(rig.run() == 0.0f); // silenced in the very next block
    CHECK(g_synth->panic_calls == 1);
    CHECK(g_synth->last_mode == PanicMode::immediate);

    // The held note was ended downstream, then the full channel panic.
    CHECK(rig.note_offs() == 1);
    CHECK(rig.count_kind(PanicMode::immediate, midi::PanicMessageKind::all_sound_off) == 16);
    CHECK(rig.count_kind(PanicMode::immediate, midi::PanicMessageKind::all_notes_off) == 16);
    CHECK(rig.midi_out.size() == 1 + midi::panic_message_count(midi::PanicScope{}));

    // A request is consumed once: the following block is quiet and clean.
    CHECK(rig.run() == 0.0f);
    CHECK(rig.midi_out.empty());
    CHECK(g_synth->panic_calls == 1);

    // And the synth is not left broken.
    CHECK(rig.run_note_on(64) > 0.4f);
}

TEST_CASE("release-only panic lets the tail ring and omits All Sound Off", "[format][panic]") {
    Rig rig(make_synth_with_midi);
    REQUIRE(rig.run_note_on(60) > 0.4f);

    g_synth->request_panic(PanicMode::release);
    CHECK(rig.run() > 0.4f); // release tail still sounding this block
    CHECK(g_synth->last_mode == PanicMode::release);
    CHECK(rig.count_kind(PanicMode::release, midi::PanicMessageKind::all_sound_off) == 0);
    CHECK(rig.count_kind(PanicMode::release, midi::PanicMessageKind::all_notes_off) == 16);
    CHECK(rig.note_offs() == 1);

    // The tail ends on its own (4096 samples = 16 blocks).
    float peak = 1.0f;
    for (int i = 0; i < 20; ++i)
        peak = rig.run();
    CHECK(peak == 0.0f);
}

TEST_CASE("panic with nothing held sends only the channel messages", "[format][panic]") {
    Rig rig(make_synth_with_midi);
    g_synth->request_panic();
    rig.run();
    CHECK(rig.note_offs() == 0);
    CHECK(rig.midi_out.size() == midi::panic_message_count(midi::PanicScope{}));
}

TEST_CASE("a note released before the panic is not ended twice", "[format][panic]") {
    Rig rig(make_synth_with_midi);
    rig.run_note_on(60);
    midi::MidiBuffer off;
    off.add(midi::MidiEvent::note_off(0, 60));
    rig.run(off);
    g_synth->request_panic();
    rig.run();
    CHECK(rig.note_offs() == 0);
}

TEST_CASE("without MIDI output the voices reset but nothing is written", "[format][panic]") {
    Rig rig(make_synth_without_midi);
    REQUIRE(rig.run_note_on(60) > 0.4f);
    g_synth->request_panic();
    CHECK(rig.run() == 0.0f);
    CHECK(rig.midi_out.empty());
    CHECK(g_synth->panic_calls == 1);
}

TEST_CASE("panic_capabilities reports what the host can do", "[format][panic]") {
    // Not attached to any host adapter: requests are recorded, nothing runs them.
    PanicSynth bare(true);
    CHECK_FALSE(bare.panic_capabilities().serviced);
    CHECK_FALSE(bare.panic_capabilities().midi_out);

    Rig with_midi(make_synth_with_midi);
    CHECK(g_synth->panic_capabilities().serviced);
    CHECK(g_synth->panic_capabilities().midi_out);

    Rig without_midi(make_synth_without_midi);
    CHECK(g_synth->panic_capabilities().serviced);
    CHECK_FALSE(g_synth->panic_capabilities().midi_out);
}

TEST_CASE("PanicState keeps the harder of two pending requests", "[format][panic]") {
    format::PanicState state;
    PanicMode mode = PanicMode::release;
    CHECK_FALSE(state.take(mode));

    state.request(PanicMode::release);
    state.request(PanicMode::immediate);
    REQUIRE(state.take(mode));
    CHECK(mode == PanicMode::immediate);
    CHECK_FALSE(state.take(mode));

    state.request(PanicMode::immediate);
    state.request(PanicMode::release);
    REQUIRE(state.take(mode));
    CHECK(mode == PanicMode::immediate);
}

TEST_CASE("service_panic does nothing and writes nothing when no request is pending",
          "[format][panic]") {
    PanicSynth synth(true);
    auto state = synth.panic_state();
    state->attach(true);
    midi::MidiBuffer out;
    out.reserve(16);
    const auto outcome = format::service_panic(synth, *state, out);
    CHECK_FALSE(outcome.serviced);
    CHECK(out.empty());
    CHECK(synth.panic_calls == 0);
}

TEST_CASE("concurrent request_panic calls are serviced without loss or crash",
          "[format][panic][rt-safety]") {
    Rig rig(make_synth_with_midi);
    std::atomic<bool> stop{false};
    std::vector<std::thread> requesters;
    for (int t = 0; t < 4; ++t) {
        requesters.emplace_back([&, t] {
            for (int i = 0; i < 20000; ++i)
                g_synth->request_panic(((i + t) & 1) ? PanicMode::immediate : PanicMode::release);
        });
    }
    std::thread audio([&] {
        while (!stop.load())
            rig.run_note_on(60);
    });
    for (auto& t : requesters)
        t.join();
    stop.store(true);
    audio.join();

    // Drain: one more block services anything still pending, then nothing is.
    rig.run();
    CHECK_FALSE(g_synth->panic_state()->pending());
    CHECK(g_synth->panic_calls > 0);
    // Requests collapse into serviced panics; never more services than requests.
    CHECK(g_synth->panic_calls <= 80000);
}

TEST_CASE("a panic larger than the MIDI buffer is carried over the next blocks, not lost",
          "[format][panic][overflow]") {
    Rig rig(make_synth_with_midi, /*midi_capacity=*/40);
    for (std::uint8_t note = 60; note < 70; ++note)
        rig.run_note_on(note); // 10 held

    g_synth->request_panic();
    auto state = g_synth->panic_state();
    std::vector<midi::MidiEvent> all;
    std::size_t blocks = 0;
    rig.run();
    ++blocks;
    all.insert(all.end(), rig.midi_out.begin(), rig.midi_out.end());
    // 10 note-offs + 80 messages = 90 events into room for 40.
    CHECK(rig.midi_out.size() == 40);
    CHECK(state->carrying());
    CHECK(state->deferred_events() == 50);
    CHECK(g_synth->panic_calls == 1); // voices are reset once, not per block

    while (state->carrying() && blocks < 10) {
        rig.run();
        ++blocks;
        all.insert(all.end(), rig.midi_out.begin(), rig.midi_out.end());
    }
    CHECK(blocks == 3);
    CHECK_FALSE(state->carrying());
    CHECK(g_synth->panic_calls == 1);

    // Everything arrived exactly once, in order: ten note-offs, then each
    // channel message once.
    REQUIRE(all.size() == 10 + midi::panic_message_count(midi::PanicScope{}));
    for (std::size_t i = 0; i < 10; ++i) {
        CHECK(all[i].message.isNoteOff());
        CHECK(all[i].note() == 60 + i);
    }
    std::array<midi::MidiEvent, midi::kMaxPanicMessages> expected;
    const auto n = midi::panic_messages(midi::PanicScope{}, expected);
    REQUIRE(n == all.size() - 10);
    for (std::size_t i = 0; i < n; ++i) {
        CHECK(all[10 + i].data()[0] == expected[i].data()[0]);
        CHECK(all[10 + i].data()[1] == expected[i].data()[1]);
        CHECK(all[10 + i].data()[2] == expected[i].data()[2]);
    }

    // Finished: nothing further, and the counter keeps its total.
    CHECK(rig.run() == 0.0f);
    CHECK(rig.midi_out.empty());
    CHECK(state->deferred_events() > 0);
}

TEST_CASE("a panic that fits reports nothing deferred", "[format][panic][overflow]") {
    Rig rig(make_synth_with_midi);
    rig.run_note_on(60);
    g_synth->request_panic();
    rig.run();
    auto state = g_synth->panic_state();
    CHECK_FALSE(state->carrying());
    CHECK(state->deferred_events() == 0);
}

TEST_CASE("a new request during a carried panic keeps the harder mode",
          "[format][panic][overflow]") {
    Rig rig(make_synth_with_midi, /*midi_capacity=*/40);
    rig.run_note_on(60);
    g_synth->request_panic(PanicMode::immediate);
    rig.run(); // 1 note-off + 39 of 80 messages; carrying as immediate
    auto state = g_synth->panic_state();
    REQUIRE(state->carrying());

    g_synth->request_panic(PanicMode::release);
    rig.run();
    CHECK(g_synth->last_mode == PanicMode::immediate);
}
