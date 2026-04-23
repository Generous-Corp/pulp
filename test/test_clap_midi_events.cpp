// CLAP adapter inbound/outbound MIDI event coverage (issue-pending).
//
// This suite pins the behaviour added alongside
// feature/clap-midi-cc-coverage:
//
//   Inbound:
//     - CLAP_EVENT_MIDI    — CC, pitch bend, channel/poly aftertouch,
//                            program change decoded into MidiBuffer.
//     - CLAP_EVENT_NOTE_EXPRESSION
//                           — mapped to channel aftertouch / pitch
//                             bend / CC when the plugin opted in to
//                             MPE; dropped otherwise.
//     - CLAP_EVENT_NOTE_CHOKE
//                           — synthesised as a zero-velocity note-off
//                             at the event's sample offset.
//     - CLAP_EVENT_MIDI2    — routed straight to the UMP sidecar when
//                             the plugin opted in to UMP.
//   Outbound:
//     - midi_out → CLAP_EVENT_MIDI on out_events.
//     - midi_out sysex → CLAP_EVENT_MIDI_SYSEX on out_events.
//
// The tests build an in-memory clap_input_events_t / clap_output_events_t
// with vtables, drive pulp::format::clap_adapter::clap_process(), and
// assert that the TestProcessor saw the MIDI event (or that the output
// stream captured the right bytes). No real DAW or shared library is
// needed.

#include <catch2/catch_test_macros.hpp>

#include <pulp/format/clap_adapter.hpp>
#include <pulp/format/clap_entry.hpp>
#include <pulp/format/processor.hpp>
#include <pulp/midi/buffer.hpp>
#include <pulp/midi/message.hpp>

#include <cstddef>
#include <cstring>
#include <memory>
#include <vector>

using namespace pulp;
using namespace pulp::format;

namespace {

// ── Event-stream helpers ────────────────────────────────────────────────

// Heterogeneous event stream that copies the full CLAP struct payload into
// a contiguous byte vector, so the `clap_input_events_t` get() vtable can
// hand back stable pointers to each event header.
class InputEventList {
public:
    template <typename Event>
    void push(const Event& e) {
        const auto align = alignof(Event);
        const auto offset = (bytes_used_ + (align - 1)) & ~(align - 1);
        const auto end = offset + sizeof(Event);
        storage_.resize((end + sizeof(std::max_align_t) - 1) / sizeof(std::max_align_t));
        auto* bytes = reinterpret_cast<std::uint8_t*>(storage_.data());
        std::memcpy(bytes + offset, &e, sizeof(Event));
        bytes_used_ = end;
        offsets_.push_back(offset);
    }

    clap_input_events_t make_vtable() {
        vtable_.ctx = this;
        vtable_.size = [](const clap_input_events_t* list) -> uint32_t {
            auto* self = static_cast<InputEventList*>(list->ctx);
            return static_cast<uint32_t>(self->offsets_.size());
        };
        vtable_.get = [](const clap_input_events_t* list, uint32_t idx)
            -> const clap_event_header_t* {
            auto* self = static_cast<InputEventList*>(list->ctx);
            auto* bytes = reinterpret_cast<const std::uint8_t*>(self->storage_.data());
            return reinterpret_cast<const clap_event_header_t*>(
                bytes + self->offsets_[idx]);
        };
        return vtable_;
    }

private:
    // Back the byte stream with max_align_t storage so the CLAP event
    // structs we memcpy into it keep their natural alignment under UBSan.
    std::vector<std::max_align_t> storage_;
    std::size_t bytes_used_ = 0;
    std::vector<std::size_t> offsets_;
    clap_input_events_t vtable_{};
};

// Collects the pushed CLAP events as raw byte copies so the test can walk
// back over them after clap_process() returns. Sysex bodies are copied
// into `sysex_bodies_` and the stored event header's `buffer` pointer is
// rebased to point at that copy, mirroring what a real host's try_push()
// does — otherwise the adapter's local MidiBuffer data would be freed by
// the time the test inspects the event.
class OutputEventList {
public:
    clap_output_events_t make_vtable() {
        vtable_.ctx = this;
        vtable_.try_push = [](const clap_output_events_t* list,
                              const clap_event_header_t* hdr) -> bool {
            auto* self = static_cast<OutputEventList*>(list->ctx);
            std::vector<std::uint8_t> buf(hdr->size);
            std::memcpy(buf.data(), hdr, hdr->size);
            if (hdr->type == CLAP_EVENT_MIDI_SYSEX) {
                auto* sx = reinterpret_cast<const clap_event_midi_sysex_t*>(hdr);
                std::vector<std::uint8_t> body(sx->buffer, sx->buffer + sx->size);
                self->sysex_bodies_.push_back(std::move(body));
                auto* stored = reinterpret_cast<clap_event_midi_sysex_t*>(buf.data());
                stored->buffer = self->sysex_bodies_.back().data();
            }
            self->events_.push_back(std::move(buf));
            return true;
        };
        return vtable_;
    }

    std::size_t size() const { return events_.size(); }
    const clap_event_header_t* at(std::size_t i) const {
        return reinterpret_cast<const clap_event_header_t*>(events_[i].data());
    }

    // Filter by type.
    template <typename Event>
    std::vector<const Event*> by_type(uint16_t type) const {
        std::vector<const Event*> out;
        for (const auto& buf : events_) {
            auto* h = reinterpret_cast<const clap_event_header_t*>(buf.data());
            if (h->type == type) {
                out.push_back(reinterpret_cast<const Event*>(buf.data()));
            }
        }
        return out;
    }

private:
    std::vector<std::vector<std::uint8_t>> events_;
    // Owns the copied sysex payloads so their pointers stay valid for the
    // lifetime of the OutputEventList. Must be declared before events_ is
    // resized via push_back to avoid invalidation — plain vector<vector>
    // reallocates on growth, so we reserve() in the ctor.
    std::vector<std::vector<std::uint8_t>> sysex_bodies_;
    clap_output_events_t vtable_{};

public:
    OutputEventList() { sysex_bodies_.reserve(16); events_.reserve(64); }
};

// ── Test processors ─────────────────────────────────────────────────────

// Captures midi_in so the test can assert what the CLAP adapter handed up.
class CapturingProcessor : public Processor {
public:
    bool opts_mpe = false;
    bool opts_ump = false;

    // Mutable state captured each time process() runs.
    mutable midi::MidiBuffer captured_midi;
    mutable std::vector<midi::UmpEvent> captured_ump;
    mutable bool had_mpe_input = false;
    mutable bool had_ump_input = false;

    PluginDescriptor descriptor() const override {
        PluginDescriptor d;
        d.name = "CapturingCLAP";
        d.manufacturer = "PulpTest";
        d.bundle_id = "com.pulp.test.clap.capture";
        d.version = "1.0.0";
        d.accepts_midi = true;
        d.supports_mpe = opts_mpe;
        d.supports_ump = opts_ump;
        return d;
    }
    void define_parameters(state::StateStore&) override {}
    void prepare(const PrepareContext&) override {}
    void process(audio::BufferView<float>&,
                 const audio::BufferView<const float>&,
                 midi::MidiBuffer& midi_in,
                 midi::MidiBuffer&,
                 const ProcessContext&) override {
        captured_midi.clear();
        for (const auto& ev : midi_in) captured_midi.add(ev);
        for (const auto& se : midi_in.sysex()) {
            captured_midi.add_sysex(se.data, se.sample_offset, se.timestamp);
        }
        had_mpe_input = (mpe_input() != nullptr);
        had_ump_input = (ump_input() != nullptr);
        captured_ump.clear();
        if (auto* ump = ump_input()) {
            for (const auto& e : *ump) captured_ump.push_back(e);
        }
    }
};

// Emits a pre-programmed set of MIDI events on midi_out so we can test
// the outbound bridge.
class EmittingProcessor : public Processor {
public:
    std::vector<midi::MidiEvent> to_emit;
    std::vector<midi::MidiBuffer::SysexEvent> sysex_to_emit;

    PluginDescriptor descriptor() const override {
        PluginDescriptor d;
        d.name = "EmittingCLAP";
        d.manufacturer = "PulpTest";
        d.bundle_id = "com.pulp.test.clap.emit";
        d.version = "1.0.0";
        d.produces_midi = true;
        return d;
    }
    void define_parameters(state::StateStore&) override {}
    void prepare(const PrepareContext&) override {}
    void process(audio::BufferView<float>&,
                 const audio::BufferView<const float>&,
                 midi::MidiBuffer&,
                 midi::MidiBuffer& midi_out,
                 const ProcessContext&) override {
        for (const auto& ev : to_emit) midi_out.add(ev);
        for (const auto& se : sysex_to_emit) {
            midi_out.add_sysex(se.data, se.sample_offset, se.timestamp);
        }
    }
};

// Processor factory hooks have to be raw function pointers. Route through
// singletons so the tests can configure the active processor before the
// adapter instantiates one.
CapturingProcessor* g_capturing = nullptr;
EmittingProcessor*  g_emitting  = nullptr;
bool g_pending_opts_mpe = false;
bool g_pending_opts_ump = false;
std::vector<midi::MidiEvent> g_pending_emit;
std::vector<midi::MidiBuffer::SysexEvent> g_pending_sysex;

std::unique_ptr<Processor> make_capturing() {
    auto up = std::make_unique<CapturingProcessor>();
    g_capturing = up.get();
    if (g_pending_opts_mpe) up->opts_mpe = true;
    if (g_pending_opts_ump) up->opts_ump = true;
    return up;
}

std::unique_ptr<Processor> make_emitting() {
    auto up = std::make_unique<EmittingProcessor>();
    g_emitting = up.get();
    if (!g_pending_emit.empty()) up->to_emit = g_pending_emit;
    if (!g_pending_sysex.empty()) up->sysex_to_emit = g_pending_sysex;
    return up;
}

// ── Driver: spin up a PulpClapPlugin, run one process block ─────────────

struct Harness {
    static constexpr uint32_t kFrames = 64;
    static constexpr int      kChannels = 2;

    clap_adapter::PulpClapPlugin plugin;
    std::vector<float> in_left, in_right;
    std::vector<float> out_left, out_right;
    const float* in_ptrs[2]{};
    float* out_ptrs[2]{};
    clap_audio_buffer_t audio_in{}, audio_out{};

    explicit Harness(ProcessorFactory factory) {
        plugin.factory = factory;
        plugin.plugin.plugin_data = &plugin;
        REQUIRE(clap_adapter::clap_init(&plugin.plugin));
        REQUIRE(clap_adapter::clap_activate(&plugin.plugin, 48000.0, 32, kFrames));
        in_left.assign(kFrames, 0.0f);
        in_right.assign(kFrames, 0.0f);
        out_left.assign(kFrames, 0.0f);
        out_right.assign(kFrames, 0.0f);
        in_ptrs[0] = in_left.data();
        in_ptrs[1] = in_right.data();
        out_ptrs[0] = out_left.data();
        out_ptrs[1] = out_right.data();
        audio_in.data32 = const_cast<float**>(in_ptrs);
        audio_in.channel_count = kChannels;
        audio_out.data32 = out_ptrs;
        audio_out.channel_count = kChannels;
    }

    ~Harness() {
        clap_adapter::clap_deactivate(&plugin.plugin);
    }

    clap_process_status run(InputEventList& in_list,
                            OutputEventList* out_list = nullptr) {
        clap_input_events_t in_vt = in_list.make_vtable();
        clap_output_events_t out_vt{};
        if (out_list) out_vt = out_list->make_vtable();

        clap_process_t proc{};
        proc.frames_count = kFrames;
        proc.audio_inputs = &audio_in;
        proc.audio_inputs_count = 1;
        proc.audio_outputs = &audio_out;
        proc.audio_outputs_count = 1;
        proc.in_events = &in_vt;
        proc.out_events = out_list ? &out_vt : nullptr;
        return clap_adapter::clap_process(&plugin.plugin, &proc);
    }
};

clap_event_header_t make_header(uint32_t size, uint16_t type, uint32_t time) {
    clap_event_header_t h{};
    h.size = size;
    h.type = type;
    h.time = time;
    h.space_id = CLAP_CORE_EVENT_SPACE_ID;
    h.flags = 0;
    return h;
}

} // namespace

// ── Inbound: CLAP_EVENT_MIDI ────────────────────────────────────────────

TEST_CASE("CLAP_EVENT_MIDI decodes CC into MidiBuffer",
          "[clap][midi][issue-pending]") {
    g_pending_opts_mpe = false;
    g_pending_opts_ump = false;
    Harness h(make_capturing);
    REQUIRE(g_capturing != nullptr);

    InputEventList events;
    clap_event_midi_t ev{};
    ev.header = make_header(sizeof(ev), CLAP_EVENT_MIDI, 17);
    ev.port_index = 0;
    ev.data[0] = 0xB1;  // CC on channel 1
    ev.data[1] = 74;    // Brightness
    ev.data[2] = 99;
    events.push(ev);

    REQUIRE(h.run(events) == CLAP_PROCESS_CONTINUE);
    REQUIRE(g_capturing->captured_midi.size() == 1);
    const auto& got = g_capturing->captured_midi[0];
    REQUIRE(got.is_cc());
    REQUIRE(got.channel() == 1);
    REQUIRE(got.cc_number() == 74);
    REQUIRE(got.cc_value() == 99);
    REQUIRE(got.sample_offset == 17);
}

TEST_CASE("CLAP_EVENT_MIDI decodes pitch bend, program change, poly AT, channel AT",
          "[clap][midi][issue-pending]") {
    g_pending_opts_mpe = false;
    g_pending_opts_ump = false;
    Harness h(make_capturing);

    InputEventList events;
    // Pitch bend: channel 2, value 0x3FFF (roughly +max).
    clap_event_midi_t pb{};
    pb.header = make_header(sizeof(pb), CLAP_EVENT_MIDI, 4);
    pb.data[0] = 0xE2;
    pb.data[1] = 0x7F;  // LSB
    pb.data[2] = 0x7F;  // MSB
    events.push(pb);
    // Program change.
    clap_event_midi_t pc{};
    pc.header = make_header(sizeof(pc), CLAP_EVENT_MIDI, 5);
    pc.data[0] = 0xC3;
    pc.data[1] = 42;
    pc.data[2] = 0;
    events.push(pc);
    // Poly aftertouch: key 60 value 77 on channel 4.
    clap_event_midi_t poly{};
    poly.header = make_header(sizeof(poly), CLAP_EVENT_MIDI, 6);
    poly.data[0] = 0xA4;
    poly.data[1] = 60;
    poly.data[2] = 77;
    events.push(poly);
    // Channel pressure: value 33 on channel 5.
    clap_event_midi_t chat{};
    chat.header = make_header(sizeof(chat), CLAP_EVENT_MIDI, 7);
    chat.data[0] = 0xD5;
    chat.data[1] = 33;
    chat.data[2] = 0;
    events.push(chat);

    REQUIRE(h.run(events) == CLAP_PROCESS_CONTINUE);
    REQUIRE(g_capturing->captured_midi.size() == 4);

    const auto& gp = g_capturing->captured_midi[0];
    REQUIRE(gp.is_pitch_bend());
    REQUIRE(gp.channel() == 2);
    REQUIRE(gp.sample_offset == 4);
    REQUIRE(gp.data()[1] == 0x7F);
    REQUIRE(gp.data()[2] == 0x7F);

    const auto& gpc = g_capturing->captured_midi[1];
    REQUIRE(gpc.is_program_change());
    REQUIRE(gpc.channel() == 3);
    REQUIRE(gpc.data()[1] == 42);

    const auto& gpoly = g_capturing->captured_midi[2];
    REQUIRE(gpoly.channel() == 4);
    REQUIRE(gpoly.data()[0] == 0xA4);
    REQUIRE(gpoly.data()[1] == 60);
    REQUIRE(gpoly.data()[2] == 77);

    const auto& gchat = g_capturing->captured_midi[3];
    REQUIRE(gchat.channel() == 5);
    REQUIRE(gchat.data()[0] == 0xD5);
    REQUIRE(gchat.data()[1] == 33);
}

// ── Inbound: CLAP_EVENT_NOTE_CHOKE ──────────────────────────────────────

TEST_CASE("CLAP_EVENT_NOTE_CHOKE synthesises a zero-velocity note-off",
          "[clap][midi][issue-pending]") {
    g_pending_opts_mpe = false;
    g_pending_opts_ump = false;
    Harness h(make_capturing);

    InputEventList events;
    clap_event_note_t ev{};
    ev.header = make_header(sizeof(ev), CLAP_EVENT_NOTE_CHOKE, 23);
    ev.note_id = -1;
    ev.port_index = 0;
    ev.channel = 3;
    ev.key = 64;
    ev.velocity = 0.0;  // spec-ignored
    events.push(ev);

    REQUIRE(h.run(events) == CLAP_PROCESS_CONTINUE);
    REQUIRE(g_capturing->captured_midi.size() == 1);
    const auto& got = g_capturing->captured_midi[0];
    REQUIRE(got.is_note_off());
    REQUIRE(got.channel() == 3);
    REQUIRE(got.note() == 64);
    REQUIRE(got.velocity() == 0);
    REQUIRE(got.sample_offset == 23);
}

// ── Inbound: CLAP_EVENT_NOTE_EXPRESSION ─────────────────────────────────

TEST_CASE("CLAP_EVENT_NOTE_EXPRESSION pressure -> channel-AT when MPE opted in",
          "[clap][midi][issue-pending]") {
    g_pending_opts_mpe = true;
    g_pending_opts_ump = false;
    Harness h(make_capturing);

    InputEventList events;
    clap_event_note_expression_t ev{};
    ev.header = make_header(sizeof(ev), CLAP_EVENT_NOTE_EXPRESSION, 11);
    ev.expression_id = CLAP_NOTE_EXPRESSION_PRESSURE;
    ev.note_id = -1;
    ev.port_index = 0;
    ev.channel = 2;
    ev.key = 72;
    ev.value = 0.5;   // → roughly 64/127
    events.push(ev);

    REQUIRE(h.run(events) == CLAP_PROCESS_CONTINUE);
    // After the MPE tracker runs, midi_in still contains the synthesised
    // channel-AT message (we deliberately don't consume it). Check for it.
    bool found = false;
    for (const auto& got : g_capturing->captured_midi) {
        if (got.data()[0] == 0xD2) {
            REQUIRE(got.sample_offset == 11);
            // 0.5 → 64 (rounded).
            REQUIRE(got.data()[1] == 64);
            found = true;
        }
    }
    REQUIRE(found);
}

TEST_CASE("CLAP_EVENT_NOTE_EXPRESSION dropped when MPE not opted in",
          "[clap][midi][issue-pending]") {
    g_pending_opts_mpe = false;
    g_pending_opts_ump = false;
    Harness h(make_capturing);

    InputEventList events;
    clap_event_note_expression_t ev{};
    ev.header = make_header(sizeof(ev), CLAP_EVENT_NOTE_EXPRESSION, 3);
    ev.expression_id = CLAP_NOTE_EXPRESSION_PRESSURE;
    ev.note_id = -1;
    ev.port_index = 0;
    ev.channel = 0;
    ev.key = 60;
    ev.value = 0.5;
    events.push(ev);

    REQUIRE(h.run(events) == CLAP_PROCESS_CONTINUE);
    // Plugin opted out of MPE — adapter should drop the expression.
    REQUIRE(g_capturing->captured_midi.empty());
}

// Helper: build a note-expression event with common MPE-opted-in defaults.
namespace {
clap_event_note_expression_t make_note_expression(uint32_t expr_id,
                                                   uint8_t channel,
                                                   uint8_t key,
                                                   double value,
                                                   uint32_t time) {
    clap_event_note_expression_t ev{};
    ev.header = make_header(sizeof(ev), CLAP_EVENT_NOTE_EXPRESSION, time);
    ev.expression_id = expr_id;
    ev.note_id = -1;
    ev.port_index = 0;
    ev.channel = channel;
    ev.key = key;
    ev.value = value;
    return ev;
}

// Locate the first MidiEvent in `captured` with the given status byte (top
// nibble = message type, low nibble = channel).
const midi::MidiEvent* find_status(const midi::MidiBuffer& captured,
                                    uint8_t status_byte) {
    for (const auto& got : captured) {
        if (got.data()[0] == status_byte) return &got;
    }
    return nullptr;
}
} // namespace

TEST_CASE("CLAP_EVENT_NOTE_EXPRESSION tuning -> pitch bend when MPE opted in",
          "[clap][midi][issue-pending]") {
    g_pending_opts_mpe = true;
    g_pending_opts_ump = false;
    Harness h(make_capturing);

    InputEventList events;
    // 1 semitone of tuning; default member-bend range is ±48 semitones, so
    // norm = 1.0/48 ≈ 0.02083 → bend14 ≈ 8192 + 0.02083*8191 = 8363.67 →
    // rounded to 8364 → low 7 bits = 0x2C, high 7 bits = 0x41.
    events.push(make_note_expression(
        CLAP_NOTE_EXPRESSION_TUNING, /*channel*/2, /*key*/64,
        /*value*/1.0, /*time*/9));

    REQUIRE(h.run(events) == CLAP_PROCESS_CONTINUE);
    const auto* pb = find_status(g_capturing->captured_midi, 0xE2);
    REQUIRE(pb != nullptr);
    REQUIRE(pb->is_pitch_bend());
    REQUIRE(pb->channel() == 2);
    REQUIRE(pb->sample_offset == 9);
    // Center is 8192; 1 semitone over a ±48st range should round to a
    // specific 14-bit value — pin it so a regression in the scaling
    // constant is caught loudly.
    const int bend14 = pb->data()[1] | (pb->data()[2] << 7);
    REQUIRE(bend14 == 8363);  // lround(8192 + (1.0/48.0) * 8191.0)
}

TEST_CASE("CLAP_EVENT_NOTE_EXPRESSION tuning clamps huge positive value to +max",
          "[clap][midi][issue-pending]") {
    g_pending_opts_mpe = true;
    g_pending_opts_ump = false;
    Harness h(make_capturing);

    InputEventList events;
    // 200 semitones is far past the ±48st member-bend range; should
    // clamp to bend14 = 16383 (data[1]=0x7F, data[2]=0x7F).
    events.push(make_note_expression(
        CLAP_NOTE_EXPRESSION_TUNING, /*channel*/0, /*key*/60,
        /*value*/200.0, /*time*/0));

    REQUIRE(h.run(events) == CLAP_PROCESS_CONTINUE);
    const auto* pb = find_status(g_capturing->captured_midi, 0xE0);
    REQUIRE(pb != nullptr);
    REQUIRE(pb->is_pitch_bend());
    REQUIRE(pb->data()[1] == 0x7F);
    REQUIRE(pb->data()[2] == 0x7F);
}

TEST_CASE("CLAP_EVENT_NOTE_EXPRESSION brightness -> CC 74 when MPE opted in",
          "[clap][midi][issue-pending]") {
    g_pending_opts_mpe = true;
    g_pending_opts_ump = false;
    Harness h(make_capturing);

    InputEventList events;
    // value 1.0 → v7 = round(1.0 * 127) = 127.
    events.push(make_note_expression(
        CLAP_NOTE_EXPRESSION_BRIGHTNESS, /*channel*/3, /*key*/60,
        /*value*/1.0, /*time*/7));

    REQUIRE(h.run(events) == CLAP_PROCESS_CONTINUE);
    const auto* cc = find_status(g_capturing->captured_midi, 0xB3);
    REQUIRE(cc != nullptr);
    REQUIRE(cc->is_cc());
    REQUIRE(cc->channel() == 3);
    REQUIRE(cc->cc_number() == 74);
    REQUIRE(cc->cc_value() == 127);
    REQUIRE(cc->sample_offset == 7);
}

TEST_CASE("CLAP_EVENT_NOTE_EXPRESSION brightness clamps negative to 0",
          "[clap][midi][issue-pending]") {
    g_pending_opts_mpe = true;
    g_pending_opts_ump = false;
    Harness h(make_capturing);

    InputEventList events;
    events.push(make_note_expression(
        CLAP_NOTE_EXPRESSION_BRIGHTNESS, /*channel*/0, /*key*/60,
        /*value*/-0.5, /*time*/0));

    REQUIRE(h.run(events) == CLAP_PROCESS_CONTINUE);
    const auto* cc = find_status(g_capturing->captured_midi, 0xB0);
    REQUIRE(cc != nullptr);
    REQUIRE(cc->cc_number() == 74);
    REQUIRE(cc->cc_value() == 0);
}

TEST_CASE("CLAP_EVENT_NOTE_EXPRESSION volume -> CC 7 with 0..4 -> 0..127 scaling",
          "[clap][midi][issue-pending]") {
    g_pending_opts_mpe = true;
    g_pending_opts_ump = false;
    Harness h(make_capturing);

    InputEventList events;
    // value 2.0 (half of max=4) → v7 = round(0.5 * 127) = 64.
    events.push(make_note_expression(
        CLAP_NOTE_EXPRESSION_VOLUME, /*channel*/4, /*key*/60,
        /*value*/2.0, /*time*/3));
    // value 4.0 (max) → 127.
    events.push(make_note_expression(
        CLAP_NOTE_EXPRESSION_VOLUME, /*channel*/4, /*key*/60,
        /*value*/4.0, /*time*/4));
    // value 10.0 should clamp to 4 → v7 = 127.
    events.push(make_note_expression(
        CLAP_NOTE_EXPRESSION_VOLUME, /*channel*/4, /*key*/60,
        /*value*/10.0, /*time*/5));

    REQUIRE(h.run(events) == CLAP_PROCESS_CONTINUE);

    // Collect all CC 7 / channel 4 events.
    std::vector<const midi::MidiEvent*> cc7;
    for (const auto& got : g_capturing->captured_midi) {
        if (got.data()[0] == 0xB4 && got.data()[1] == 7) cc7.push_back(&got);
    }
    REQUIRE(cc7.size() == 3);
    REQUIRE(cc7[0]->cc_value() == 64);
    REQUIRE(cc7[0]->sample_offset == 3);
    REQUIRE(cc7[1]->cc_value() == 127);
    REQUIRE(cc7[1]->sample_offset == 4);
    REQUIRE(cc7[2]->cc_value() == 127);  // clamped
}

TEST_CASE("CLAP_EVENT_NOTE_EXPRESSION pan -> CC 10 with 0..1 -> 0..127 scaling",
          "[clap][midi][issue-pending]") {
    g_pending_opts_mpe = true;
    g_pending_opts_ump = false;
    Harness h(make_capturing);

    InputEventList events;
    // value 0.5 → v7 = round(0.5 * 127 + 0.5) = 64.
    events.push(make_note_expression(
        CLAP_NOTE_EXPRESSION_PAN, /*channel*/5, /*key*/60,
        /*value*/0.5, /*time*/14));
    // value 0 → 0, extreme-left.
    events.push(make_note_expression(
        CLAP_NOTE_EXPRESSION_PAN, /*channel*/5, /*key*/60,
        /*value*/0.0, /*time*/15));
    // value 1.0 → 127, extreme-right.
    events.push(make_note_expression(
        CLAP_NOTE_EXPRESSION_PAN, /*channel*/5, /*key*/60,
        /*value*/1.0, /*time*/16));

    REQUIRE(h.run(events) == CLAP_PROCESS_CONTINUE);

    std::vector<const midi::MidiEvent*> cc10;
    for (const auto& got : g_capturing->captured_midi) {
        if (got.data()[0] == 0xB5 && got.data()[1] == 10) cc10.push_back(&got);
    }
    REQUIRE(cc10.size() == 3);
    REQUIRE(cc10[0]->cc_value() == 64);
    REQUIRE(cc10[1]->cc_value() == 0);
    REQUIRE(cc10[2]->cc_value() == 127);
}

TEST_CASE("CLAP_EVENT_NOTE_EXPRESSION vibrato / expression dropped silently when MPE opted in",
          "[clap][midi][issue-pending]") {
    // Covers the default-arm of the expression switch: vibrato and
    // expression IDs leave `emitted = false`, so no MIDI event is added
    // to midi_in even though MPE is opted in.
    g_pending_opts_mpe = true;
    g_pending_opts_ump = false;
    Harness h(make_capturing);

    InputEventList events;
    events.push(make_note_expression(
        CLAP_NOTE_EXPRESSION_VIBRATO, /*channel*/0, /*key*/60,
        /*value*/0.5, /*time*/0));
    events.push(make_note_expression(
        CLAP_NOTE_EXPRESSION_EXPRESSION, /*channel*/0, /*key*/60,
        /*value*/0.5, /*time*/1));

    REQUIRE(h.run(events) == CLAP_PROCESS_CONTINUE);
    // No MIDI event should have been added from either expression.
    REQUIRE(g_capturing->captured_midi.empty());
}

// ── Inbound: CLAP_EVENT_MIDI_SYSEX ──────────────────────────────────────

TEST_CASE("CLAP_EVENT_MIDI_SYSEX decodes into MidiBuffer sysex sidecar",
          "[clap][midi][issue-pending]") {
    g_pending_opts_mpe = false;
    g_pending_opts_ump = false;
    Harness h(make_capturing);

    static const uint8_t kPayload[] = {0xF0, 0x7E, 0x7F, 0x06, 0x01, 0xF7};
    InputEventList events;
    clap_event_midi_sysex_t ev{};
    ev.header = make_header(sizeof(ev), CLAP_EVENT_MIDI_SYSEX, 21);
    ev.port_index = 0;
    ev.buffer = kPayload;
    ev.size = sizeof(kPayload);
    events.push(ev);

    REQUIRE(h.run(events) == CLAP_PROCESS_CONTINUE);
    // The sysex stream survives as a sidecar entry on MidiBuffer.
    REQUIRE(g_capturing->captured_midi.sysex().size() == 1);
    const auto& sx = g_capturing->captured_midi.sysex()[0];
    REQUIRE(sx.sample_offset == 21);
    REQUIRE(sx.data.size() == sizeof(kPayload));
    for (std::size_t i = 0; i < sx.data.size(); ++i) {
        REQUIRE(sx.data[i] == kPayload[i]);
    }
}

TEST_CASE("CLAP_EVENT_MIDI_SYSEX with empty payload is dropped",
          "[clap][midi][issue-pending]") {
    // Covers the `ev->buffer && ev->size > 0` guard — a zero-length or
    // null-buffer sysex must NOT be added to the sidecar.
    g_pending_opts_mpe = false;
    g_pending_opts_ump = false;
    Harness h(make_capturing);

    InputEventList events;
    clap_event_midi_sysex_t ev{};
    ev.header = make_header(sizeof(ev), CLAP_EVENT_MIDI_SYSEX, 5);
    ev.port_index = 0;
    ev.buffer = nullptr;
    ev.size = 0;
    events.push(ev);

    REQUIRE(h.run(events) == CLAP_PROCESS_CONTINUE);
    REQUIRE(g_capturing->captured_midi.sysex().empty());
    REQUIRE(g_capturing->captured_midi.empty());
}

// ── Inbound: CLAP_EVENT_MIDI2 ───────────────────────────────────────────

#if defined(CLAP_VERSION_GE) && CLAP_VERSION_GE(1, 1, 0)
TEST_CASE("CLAP_EVENT_MIDI2 routed straight to UMP sidecar when opted in",
          "[clap][midi][issue-pending]") {
    g_pending_opts_mpe = false;
    g_pending_opts_ump = true;
    Harness h(make_capturing);

    InputEventList events;
    // Build a MIDI 2.0 note-on UMP packet; velocity = 0xFFFF.
    auto packet = midi::UmpPacket::note_on_2(/*group*/0, /*channel*/1,
                                              /*note*/60, /*vel16*/0xFFFF);
    clap_event_midi2_t ev{};
    ev.header = make_header(sizeof(ev), CLAP_EVENT_MIDI2, 13);
    ev.port_index = 0;
    ev.data[0] = packet.words[0];
    ev.data[1] = packet.words[1];
    ev.data[2] = 0;
    ev.data[3] = 0;
    events.push(ev);

    REQUIRE(h.run(events) == CLAP_PROCESS_CONTINUE);
    REQUIRE(g_capturing->had_ump_input);
    REQUIRE(g_capturing->captured_ump.size() == 1);
    const auto& got = g_capturing->captured_ump[0];
    REQUIRE(got.sample_offset == 13);
    REQUIRE(got.packet.message_type() == midi::UmpMessageType::Midi2ChannelVoice);
    REQUIRE(got.packet.channel() == 1);
    REQUIRE(got.packet.note_number() == 60);
    REQUIRE(got.packet.velocity_16() == 0xFFFF);
}

TEST_CASE("Mixed CLAP_EVENT_MIDI2 + NOTE_ON: both reach UMP sidecar",
          "[clap][midi][issue-pending]") {
    // Real CLAP hosts mix transports: notes flow through CLAP_EVENT_NOTE_*
    // while CCs / pitch bend / aftertouch flow through CLAP_EVENT_MIDI2.
    // A `supports_ump` processor reads `ump_input` as its primary stream
    // and would lose every note if midi1_to_ump synthesis were skipped
    // when MIDI2 is present.
    //
    // This test models that mixed shape: one native MIDI2 note-on packet
    // (representing what would in practice be a CC/PB/AT — using a note
    // here only because the harness already has a UMP note helper) AND
    // a separate co-delivered CLAP_EVENT_NOTE_ON on a different note.
    // Both must appear in the UMP sidecar after process() returns. The
    // earlier shape of this test (PR #627 v1) asserted the synthesis
    // was skipped, but Codex P1 review on PR #627 showed that
    // assumption silently dropped real-world note streams. Inverted to
    // pin the corrected behaviour.
    g_pending_opts_mpe = false;
    g_pending_opts_ump = true;
    Harness h(make_capturing);

    InputEventList events;
    // One native MIDI2 packet (note 60, here standing in for any
    // non-NOTE_* event the host might deliver natively as MIDI2).
    auto packet = midi::UmpPacket::note_on_2(/*group*/0, /*channel*/0,
                                              /*note*/60, /*vel16*/0x4000);
    clap_event_midi2_t e2{};
    e2.header = make_header(sizeof(e2), CLAP_EVENT_MIDI2, 1);
    e2.port_index = 0;
    e2.data[0] = packet.words[0];
    e2.data[1] = packet.words[1];
    e2.data[2] = 0;
    e2.data[3] = 0;
    events.push(e2);
    // A separate CLAP_EVENT_NOTE_ON on a different note — this is the
    // case that previously got dropped from the UMP buffer.
    clap_event_note_t en{};
    en.header = make_header(sizeof(en), CLAP_EVENT_NOTE_ON, 2);
    en.note_id = -1;
    en.port_index = 0;
    en.channel = 0;
    en.key = 72;
    en.velocity = 1.0;
    events.push(en);

    REQUIRE(h.run(events) == CLAP_PROCESS_CONTINUE);
    // UMP sidecar contains BOTH: the native MIDI2 packet (note 60) AND
    // a synthesised packet from the NOTE_ON (note 72). The earlier
    // (buggy) behaviour produced size() == 1 here.
    REQUIRE(g_capturing->had_ump_input);
    REQUIRE(g_capturing->captured_ump.size() == 2);
    // Spec doesn't pin ordering between native and synthesised entries
    // beyond preserving sample_offset within each path; assert presence
    // by note rather than position.
    bool saw_60 = false;
    bool saw_72 = false;
    for (const auto& entry : g_capturing->captured_ump) {
        if (entry.packet.note_number() == 60) saw_60 = true;
        if (entry.packet.note_number() == 72) saw_72 = true;
    }
    REQUIRE(saw_60);
    REQUIRE(saw_72);
    // The NOTE_ON still reaches midi_in for MIDI 1.0 consumers / MPE.
    REQUIRE(g_capturing->captured_midi.size() == 1);
    REQUIRE(g_capturing->captured_midi[0].is_note_on());
    REQUIRE(g_capturing->captured_midi[0].note() == 72);
}

TEST_CASE("UMP sidecar is cleared on every process() when opted in",
          "[clap][midi][issue-pending]") {
    // A block with a native MIDI2 packet leaves ump_buffer holding one
    // entry; the next block must start from an empty buffer so the
    // plugin doesn't see stale data. This pins the
    // `if (self->ump_enabled) self->ump_buffer.clear();` line at the
    // top of each process() call.
    g_pending_opts_mpe = false;
    g_pending_opts_ump = true;
    Harness h(make_capturing);

    // Block 1: deliver a native MIDI2 note-on.
    {
        InputEventList events;
        auto packet = midi::UmpPacket::note_on_2(
            /*group*/0, /*channel*/2, /*note*/60, /*vel16*/0x4000);
        clap_event_midi2_t ev{};
        ev.header = make_header(sizeof(ev), CLAP_EVENT_MIDI2, 0);
        ev.port_index = 0;
        ev.data[0] = packet.words[0];
        ev.data[1] = packet.words[1];
        ev.data[2] = 0;
        ev.data[3] = 0;
        events.push(ev);
        REQUIRE(h.run(events) == CLAP_PROCESS_CONTINUE);
        REQUIRE(g_capturing->captured_ump.size() == 1);
    }
    // Block 2: deliver nothing. The UMP sidecar should be empty — the
    // previous packet must not leak through.
    {
        InputEventList events;
        REQUIRE(h.run(events) == CLAP_PROCESS_CONTINUE);
        REQUIRE(g_capturing->had_ump_input);
        REQUIRE(g_capturing->captured_ump.empty());
    }
}

TEST_CASE("CLAP_EVENT_MIDI2 dropped when plugin did not opt in to UMP",
          "[clap][midi][issue-pending]") {
    // Pins the `if (self->ump_enabled)` guard on the MIDI2 branch.
    g_pending_opts_mpe = false;
    g_pending_opts_ump = false;
    Harness h(make_capturing);

    InputEventList events;
    auto packet = midi::UmpPacket::note_on_2(
        /*group*/0, /*channel*/0, /*note*/60, /*vel16*/0xFFFF);
    clap_event_midi2_t ev{};
    ev.header = make_header(sizeof(ev), CLAP_EVENT_MIDI2, 0);
    ev.port_index = 0;
    ev.data[0] = packet.words[0];
    ev.data[1] = packet.words[1];
    ev.data[2] = 0;
    ev.data[3] = 0;
    events.push(ev);

    REQUIRE(h.run(events) == CLAP_PROCESS_CONTINUE);
    // Plugin didn't opt in — no UMP sidecar exposed, packet is dropped.
    REQUIRE_FALSE(g_capturing->had_ump_input);
    REQUIRE(g_capturing->captured_ump.empty());
    REQUIRE(g_capturing->captured_midi.empty());
}
#endif

// ── Outbound: midi_out → CLAP_EVENT_MIDI / SYSEX ────────────────────────

TEST_CASE("midi_out CC + pitch-bend surface on CLAP out_events",
          "[clap][midi][issue-pending]") {
    g_pending_emit.clear();
    g_pending_sysex.clear();
    auto cc = midi::MidiEvent::cc(/*ch*/0, /*num*/7, /*val*/100);
    cc.sample_offset = 12;
    auto pb = midi::MidiEvent::pitch_bend(/*ch*/0, /*val*/12345);
    pb.sample_offset = 20;
    g_pending_emit = {cc, pb};

    Harness h(make_emitting);
    InputEventList in;
    OutputEventList out;
    REQUIRE(h.run(in, &out) == CLAP_PROCESS_CONTINUE);

    auto midis = out.by_type<clap_event_midi_t>(CLAP_EVENT_MIDI);
    REQUIRE(midis.size() == 2);

    // Sort guarantee — sample_offset ascending.
    REQUIRE(midis[0]->header.time == 12);
    REQUIRE(midis[0]->data[0] == 0xB0);
    REQUIRE(midis[0]->data[1] == 7);
    REQUIRE(midis[0]->data[2] == 100);

    REQUIRE(midis[1]->header.time == 20);
    REQUIRE(midis[1]->data[0] == 0xE0);
    // pitch bend low = value & 0x7F, high = (value >> 7) & 0x7F
    REQUIRE(midis[1]->data[1] == (12345 & 0x7F));
    REQUIRE(midis[1]->data[2] == ((12345 >> 7) & 0x7F));
}

TEST_CASE("midi_out sysex surfaces on CLAP out_events",
          "[clap][midi][issue-pending]") {
    g_pending_emit.clear();
    g_pending_sysex.clear();
    midi::MidiBuffer::SysexEvent se;
    se.data = {0xF0, 0x7D, 0x01, 0x02, 0xF7};
    se.sample_offset = 5;
    g_pending_sysex = {se};

    Harness h(make_emitting);
    InputEventList in;
    OutputEventList out;
    REQUIRE(h.run(in, &out) == CLAP_PROCESS_CONTINUE);

    auto sysexes = out.by_type<clap_event_midi_sysex_t>(CLAP_EVENT_MIDI_SYSEX);
    REQUIRE(sysexes.size() == 1);
    REQUIRE(sysexes[0]->header.time == 5);
    REQUIRE(sysexes[0]->size == 5);
    REQUIRE(sysexes[0]->buffer[0] == 0xF0);
    REQUIRE(sysexes[0]->buffer[4] == 0xF7);
}

TEST_CASE("midi_out program change (2-byte) clamps size padding on CLAP out_events",
          "[clap][midi][issue-pending]") {
    // Covers the `me.size() > 2 ? ... : uint8_t{0}` padding branch of
    // the outbound short-message loop — a 2-byte message like program
    // change must still write a zero into data[2].
    g_pending_emit.clear();
    g_pending_sysex.clear();
    auto pc = midi::MidiEvent::program_change(/*ch*/1, /*program*/42);
    pc.sample_offset = 8;
    g_pending_emit = {pc};

    Harness h(make_emitting);
    InputEventList in;
    OutputEventList out;
    REQUIRE(h.run(in, &out) == CLAP_PROCESS_CONTINUE);

    auto midis = out.by_type<clap_event_midi_t>(CLAP_EVENT_MIDI);
    REQUIRE(midis.size() == 1);
    REQUIRE(midis[0]->header.time == 8);
    REQUIRE(midis[0]->data[0] == 0xC1);
    REQUIRE(midis[0]->data[1] == 42);
    REQUIRE(midis[0]->data[2] == 0);
}

TEST_CASE("midi_out empty sysex payload is skipped on CLAP out_events",
          "[clap][midi][issue-pending]") {
    // Covers the `if (se.data.empty()) continue;` guard in the outbound
    // sysex loop.
    g_pending_emit.clear();
    g_pending_sysex.clear();
    midi::MidiBuffer::SysexEvent empty_se;
    empty_se.data = {};
    empty_se.sample_offset = 3;
    midi::MidiBuffer::SysexEvent real_se;
    real_se.data = {0xF0, 0x7E, 0xF7};
    real_se.sample_offset = 4;
    g_pending_sysex = {empty_se, real_se};

    Harness h(make_emitting);
    InputEventList in;
    OutputEventList out;
    REQUIRE(h.run(in, &out) == CLAP_PROCESS_CONTINUE);

    // The empty entry is dropped; only the non-empty one surfaces.
    auto sysexes = out.by_type<clap_event_midi_sysex_t>(CLAP_EVENT_MIDI_SYSEX);
    REQUIRE(sysexes.size() == 1);
    REQUIRE(sysexes[0]->size == 3);
    REQUIRE(sysexes[0]->header.time == 4);
}

TEST_CASE("midi_out shorts + sysex interleave by sample_offset on out_events",
          "[clap][midi][issue-pending]") {
    // Covers the two-cursor merge added by the Codex P2 sweep on PR #627.
    // CLAP's out_events contract requires events pushed in ascending
    // sample-time order across event types; the earlier two-pass shape
    // (all shorts, then all sysex) violated it whenever a sysex
    // scheduled at offset N preceded a short at offset N+1. This test
    // enqueues a short at offset 5, a sysex at offset 3, and another
    // short at offset 10 — the merged emission must arrive on
    // out_events as (sysex@3, short@5, short@10), not (short@5,
    // short@10, sysex@3).
    g_pending_emit.clear();
    g_pending_sysex.clear();
    auto cc1 = midi::MidiEvent::cc(/*ch*/0, /*controller*/74, /*value*/64);
    cc1.sample_offset = 5;
    auto cc2 = midi::MidiEvent::cc(/*ch*/0, /*controller*/74, /*value*/96);
    cc2.sample_offset = 10;
    g_pending_emit = {cc1, cc2};
    midi::MidiBuffer::SysexEvent se;
    se.data = {0xF0, 0x7D, 0x99, 0xF7};
    se.sample_offset = 3;
    g_pending_sysex = {se};

    Harness h(make_emitting);
    InputEventList in;
    OutputEventList out;
    REQUIRE(h.run(in, &out) == CLAP_PROCESS_CONTINUE);

    // Walk the captured out_events in push order and assert their
    // headers are non-decreasing in time, with the sysex landing
    // between the two CCs. by_type discards ordering, so use the
    // raw at() accessor on the push log instead.
    REQUIRE(out.size() == 3);
    REQUIRE(out.at(0)->time == 3);
    REQUIRE(out.at(0)->type == CLAP_EVENT_MIDI_SYSEX);
    REQUIRE(out.at(1)->time == 5);
    REQUIRE(out.at(1)->type == CLAP_EVENT_MIDI);
    REQUIRE(out.at(2)->time == 10);
    REQUIRE(out.at(2)->type == CLAP_EVENT_MIDI);
}
