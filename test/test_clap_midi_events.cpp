// CLAP adapter inbound/outbound MIDI event coverage (issue-pending).
//
// This suite pins the behavior added alongside
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
//     - CLAP_EVENT_MIDI_SYSEX
//                           — copied into MidiBuffer's preallocated SysEx
//                             sidecar.
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
#include <pulp/format/quirk_apply.hpp>
#include <pulp/format/processor.hpp>
#include <clap/ext/latency.h>
#include <clap/ext/tail.h>
#include <pulp/midi/buffer.hpp>
#include <pulp/midi/message.hpp>
#include <pulp/signal/scoped_flush_denormals.hpp>
#include <pulp/state/parameter_event_queue.hpp>

#if PULP_CLAP_PROCESS_RT_TRAP_TESTS
#include "native_components/rt_test_scope.hpp"
#endif

#include <clap/ext/preset-load.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstring>
#include <memory>
#include <optional>
#include <utility>
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
    static constexpr state::ParamID kParamId = 9001;
    static constexpr state::ParamID kBypassParamId = 9002;
    static constexpr std::size_t kMaxCapturedSysex = 8;
    static constexpr std::size_t kMaxCapturedSysexBytes = 256;

    struct CapturedSysex {
        std::array<uint8_t, kMaxCapturedSysexBytes> data{};
        std::size_t size = 0;
        int32_t sample_offset = 0;
        double timestamp = 0.0;
    };

    CapturingProcessor() {
        captured_midi.reserve_events(32);
        captured_ump.reserve(32);
        captured_param_events.reserve(32);
    }

    bool opts_mpe = false;
    bool opts_ump = false;
    bool opts_node_mpe = false;
    bool opts_node_ump = false;
    bool declares_bypass = false;
    bool declares_sidechain = false;

    // Mutable state captured each time process() runs.
    mutable midi::MidiBuffer captured_midi;
    mutable std::array<CapturedSysex, kMaxCapturedSysex> captured_sysex{};
    mutable std::size_t captured_sysex_count = 0;
    mutable std::vector<midi::UmpEvent> captured_ump;
    mutable std::vector<state::ParameterEvent> captured_param_events;
    mutable bool had_mpe_input = false;
    mutable bool had_ump_input = false;
    mutable bool had_param_events = false;
    mutable PrepareContext captured_prepare{};
    mutable ProcessContext captured_context{};
    mutable int prepare_count = 0;
    mutable int release_count = 0;
    mutable int process_count = 0;
    mutable bool flushed_during_process = false;
    mutable int captured_input_channels = 0;
    mutable int captured_output_channels = 0;
    mutable int captured_num_samples = 0;
    mutable bool had_sidechain_input = false;
    mutable int captured_sidechain_channels = 0;
    mutable int captured_sidechain_samples = 0;
    mutable float captured_sidechain_first_sample = 0.0f;
    mutable float captured_sidechain_second_sample = 0.0f;
    mutable float captured_modulated_value = 0.0f;
    bool mutate_param_in_process = false;
    float process_param_value = 0.0f;

    PluginDescriptor descriptor() const override {
        PluginDescriptor d;
        d.name = "CapturingCLAP";
        d.manufacturer = "PulpTest";
        d.bundle_id = "com.pulp.test.clap.capture";
        d.version = "1.0.0";
        d.accepts_midi = true;
        d.supports_mpe = opts_mpe;
        d.supports_ump = opts_ump;
        d.node_capabilities.supports_mpe = opts_node_mpe;
        d.node_capabilities.supports_ump = opts_node_ump;
        if (declares_sidechain) {
            d.input_buses.push_back({"Sidechain", 2, true});
        }
        return d;
    }
    void define_parameters(state::StateStore& store) override {
        store.add_parameter({
            .id = kParamId,
            .name = "Capture Gain",
            .range = {0.0f, 1.0f, 0.0f, 0.0f},
        });
        if (declares_bypass) {
            store.add_parameter({
                .id = kBypassParamId,
                .name = "Bypass",
                .range = {0.0f, 1.0f, 0.0f, 1.0f},
            });
        }
    }
    void prepare(const PrepareContext& context) override {
        captured_prepare = context;
        captured_midi.reserve(64, 16, 4096);
        captured_midi.set_realtime_capacity_limit(true);
        captured_ump.reserve(64);
        captured_param_events.reserve(64);
        ++prepare_count;
    }
    void release() override { ++release_count; }
    void process(audio::BufferView<float>& audio_output,
                 const audio::BufferView<const float>& audio_input,
                 midi::MidiBuffer& midi_in,
                 midi::MidiBuffer&,
                 const ProcessContext& context) override {
        ++process_count;
        flushed_during_process = pulp::signal::denormals_are_flushed();
        captured_context = context;
        captured_input_channels = static_cast<int>(audio_input.num_channels());
        captured_output_channels = static_cast<int>(audio_output.num_channels());
        captured_num_samples = static_cast<int>(audio_input.num_samples());
        captured_modulated_value = state().get_modulated(kParamId);
        had_sidechain_input = sidechain_input() != nullptr;
        captured_sidechain_channels = 0;
        captured_sidechain_samples = 0;
        captured_sidechain_first_sample = 0.0f;
        captured_sidechain_second_sample = 0.0f;
        if (auto* sc = sidechain_input()) {
            captured_sidechain_channels = static_cast<int>(sc->num_channels());
            captured_sidechain_samples = static_cast<int>(sc->num_samples());
            if (sc->num_channels() > 0 && sc->num_samples() > 0) {
                captured_sidechain_first_sample = sc->channel(0)[0];
            }
            if (sc->num_channels() > 1 && sc->num_samples() > 0) {
                captured_sidechain_second_sample = sc->channel(1)[0];
            }
        }
        captured_midi.clear();
        captured_midi.clear_sysex();
        for (const auto& ev : midi_in) captured_midi.add(ev);
        for (auto& se : midi_in.sysex()) {
            // This test processor runs behind the CLAP adapter's no-alloc
            // process guard, so capture into this processor's own prepared
            // SysEx payload pool rather than moving adapter-owned storage.
            captured_midi.add_sysex_copy(
                se.data.data(), se.data.size(), se.sample_offset, se.timestamp);
        }
        had_mpe_input = (mpe_input() != nullptr);
        had_ump_input = (ump_input() != nullptr);
        had_param_events = (param_events() != nullptr);
        captured_param_events.clear();
        if (auto* events = param_events()) {
            for (const auto& event : *events) captured_param_events.push_back(event);
        }
        captured_ump.clear();
        if (auto* ump = ump_input()) {
            for (const auto& e : *ump) captured_ump.push_back(e);
        }
        if (mutate_param_in_process) {
            state().set_value_rt(kParamId, process_param_value);
        }
    }
};

class ProcessBuffersCapturingProcessor : public CapturingProcessor {
public:
    using CapturingProcessor::process;

    void process(ProcessBuffers& audio,
                 midi::MidiBuffer& midi_in,
                 midi::MidiBuffer& midi_out,
                 const ProcessContext& context) override {
        ++process_buffer_calls;
        captured_input_bus_count = static_cast<int>(audio.inputs.size());
        captured_output_bus_count = static_cast<int>(audio.outputs.size());
        captured_active_input_bus_count = static_cast<int>(audio.inputs.active_count());
        captured_active_output_bus_count = static_cast<int>(audio.outputs.active_count());
        saw_process_buffer_sidechain =
            audio.inputs.sidechain() != nullptr && audio.inputs.sidechain()->active();
        process_buffer_layouts_match = audio.layouts_match_descriptors();
        process_buffer_storage_valid = audio.active_buses_have_storage();

        Processor::process(audio, midi_in, midi_out, context);
    }

    int process_buffer_calls = 0;
    int captured_input_bus_count = 0;
    int captured_output_bus_count = 0;
    int captured_active_input_bus_count = 0;
    int captured_active_output_bus_count = 0;
    bool saw_process_buffer_sidechain = false;
    bool process_buffer_layouts_match = false;
    bool process_buffer_storage_valid = false;
};

// Declares two output buses and writes a distinct constant to each via the
// richer process(ProcessBuffers&) surface. Proves the adapter routes every
// declared output bus to the Processor instead of zero-filling secondary buses.
class MultiOutProcessor : public Processor {
public:
    static constexpr float kMainValue = 0.5f;
    static constexpr float kAuxValue = -0.25f;
    static constexpr state::ParamID kBypassParamId = 12000;

    // Per-instance config seeded by the factory from pending flags. Lets a test
    // declare a Bypass param (to exercise the adapter's bypass short-circuit) or
    // declare a 1-channel aux bus (to exercise host-vs-declared mismatch).
    bool declares_bypass = false;
    int declared_aux_channels = 2;

    PluginDescriptor descriptor() const override {
        PluginDescriptor d;
        d.name = "MultiOutCLAP";
        d.manufacturer = "PulpTest";
        d.bundle_id = "com.pulp.test.clap.multiout";
        d.version = "1.0.0";
        d.category = PluginCategory::Instrument;
        d.input_buses = {};
        d.output_buses = {{"Main Out", 2}, {"Aux Out", declared_aux_channels}};
        return d;
    }
    void define_parameters(state::StateStore& store) override {
        if (declares_bypass) {
            store.add_parameter({.id = kBypassParamId, .name = "Bypass",
                                 .range = {0.0f, 1.0f, 0.0f, 1.0f}});
        }
    }
    void prepare(const PrepareContext&) override { ++prepare_count; }

    using Processor::process;
    void process(audio::BufferView<float>& audio_output,
                 const audio::BufferView<const float>&,
                 midi::MidiBuffer&,
                 midi::MidiBuffer&,
                 const ProcessContext&) override {
        // Fallback for the single-output legacy path: only fills the main bus.
        fill(audio_output, kMainValue);
    }

    void process(ProcessBuffers& audio,
                 midi::MidiBuffer&,
                 midi::MidiBuffer&,
                 const ProcessContext&) override {
        ++process_buffer_calls;
        captured_output_bus_count = static_cast<int>(audio.outputs.size());
        captured_layouts_match = audio.layouts_match_descriptors();
        if (auto* main = audio.outputs.main(); main && main->active()) {
            fill(main->buffer, kMainValue);
            wrote_main = true;
        }
        if (auto* aux = audio.outputs.find(BusRole::Aux); aux) {
            aux_was_active = aux->active();
            captured_aux_declared = aux->info.declared_channels;
            if (aux->active()) {
                fill(aux->buffer, kAuxValue);
                wrote_aux = true;
                captured_aux_channels = static_cast<int>(aux->num_channels());
            }
        }
    }

    int prepare_count = 0;
    int process_buffer_calls = 0;
    int captured_output_bus_count = 0;
    int captured_aux_channels = 0;
    int captured_aux_declared = -1;
    bool aux_was_active = false;
    bool captured_layouts_match = false;
    bool wrote_main = false;
    bool wrote_aux = false;

private:
    static void fill(audio::BufferView<float>& view, float value) {
        for (std::size_t ch = 0; ch < view.num_channels(); ++ch) {
            auto data = view.channel(ch);
            for (std::size_t n = 0; n < view.num_samples(); ++n) data[n] = value;
        }
    }
};

// Copies the main input to the main output (unity gain) and records the
// block size it was handed. Used to prove the oversized-block guard: the
// processor must only ever see the prepared max, and the adapter zeros the
// host's un-processable tail.
class UnityCopyProcessor : public Processor {
public:
    mutable int observed_num_samples = 0;
    mutable int process_count = 0;
    // Per-channel scratch sized to the prepared max in prepare(); process()
    // stages each block through it before writing the output. This mirrors a
    // real DSP processor and is what an oversized block overruns — the host
    // output buffer alone would be large enough, so the scratch is required to
    // exercise the actual corruption the guard prevents.
    int prepared_max = 0;
    std::vector<std::vector<float>> scratch;

    PluginDescriptor descriptor() const override {
        PluginDescriptor d;
        d.name = "UnityCopyCLAP";
        d.manufacturer = "PulpTest";
        d.bundle_id = "com.pulp.test.clap.unity-copy";
        d.version = "1.0.0";
        return d;
    }
    void define_parameters(state::StateStore&) override {}
    void prepare(const PrepareContext& context) override {
        prepared_max = context.max_buffer_size;
        scratch.assign(2, std::vector<float>(static_cast<std::size_t>(prepared_max), 0.0f));
    }
    void process(audio::BufferView<float>& audio_output,
                 const audio::BufferView<const float>& audio_input,
                 midi::MidiBuffer&,
                 midi::MidiBuffer&,
                 const ProcessContext& context) override {
        ++process_count;
        observed_num_samples = context.num_samples;
        const auto channels =
            std::min(audio_output.num_channels(), audio_input.num_channels());
        const auto frames = static_cast<std::size_t>(context.num_samples);
        for (std::size_t ch = 0; ch < channels && ch < scratch.size(); ++ch) {
            const auto* in = audio_input.channel_ptr(ch);
            auto* out = audio_output.channel_ptr(ch);
            auto& sc = scratch[ch];
            // Stage through the prepared-max scratch. Without the adapter's
            // clamp, frames > prepared_max overruns sc here (ASan trap).
            for (std::size_t i = 0; i < frames; ++i) sc[i] = in[i];
            for (std::size_t i = 0; i < frames; ++i) out[i] = sc[i];
        }
    }
};

class NativeF64CopyProcessor : public Processor {
public:
    int process_count = 0;
    int process_f64_count = 0;
    int observed_input_channels = 0;
    int observed_output_channels = 0;
    int observed_num_samples = 0;
    bool layouts_match = false;
    bool had_main_input = false;
    bool had_main_output = false;

    PluginDescriptor descriptor() const override {
        PluginDescriptor d;
        d.name = "NativeF64CopyCLAP";
        d.manufacturer = "PulpTest";
        d.bundle_id = "com.pulp.test.clap.native-f64-copy";
        d.version = "1.0.0";
        d.supports_f64_audio = true;
        return d;
    }
    void define_parameters(state::StateStore&) override {}
    void prepare(const PrepareContext&) override {}
    void process(audio::BufferView<float>& out,
                 const audio::BufferView<const float>& in,
                 midi::MidiBuffer&,
                 midi::MidiBuffer&,
                 const ProcessContext& context) override {
        // Mirrors the f64 copy below so the mixed 32/64 demotion test can
        // observe the boundary-converted input on the f32 surface.
        ++process_count;
        observed_num_samples = context.num_samples;
        const auto channels = std::min(in.num_channels(), out.num_channels());
        for (std::size_t ch = 0; ch < channels; ++ch) {
            const auto ic = in.channel(ch);
            auto oc = out.channel(ch);
            for (std::size_t i = 0; i < ic.size() && i < oc.size(); ++i)
                oc[i] = ic[i];
        }
    }
    void process_f64(ProcessBuffers64& audio,
                     midi::MidiBuffer&,
                     midi::MidiBuffer&,
                     const ProcessContext& context) override {
        ++process_f64_count;
        observed_num_samples = context.num_samples;
        layouts_match = audio.layouts_match_descriptors();
        auto* input = audio.main_input();
        auto* output = audio.main_output();
        had_main_input = input != nullptr;
        had_main_output = output != nullptr;
        if (!input || !output) return;
        observed_input_channels = static_cast<int>(input->num_channels());
        observed_output_channels = static_cast<int>(output->num_channels());
        const auto channels = std::min(input->num_channels(), output->num_channels());
        for (std::size_t ch = 0; ch < channels; ++ch) {
            const auto in = input->channel(ch);
            auto out = output->channel(ch);
            for (std::size_t i = 0; i < in.size(); ++i) out[i] = in[i];
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
        for (auto& se : sysex_to_emit) {
            midi_out.add_sysex(std::move(se));
        }
    }
};

// Exercises the sample-accurate parameter OUTPUT merge. Pushes two explicit
// output param events via push_output_param_event() and (optionally) emits a
// MIDI short BETWEEN their offsets, so the adapter must interleave params +
// shorts into out_events in one globally ascending-time stream. The
// store-only path (no explicit event) is exercised via mutate_store_only.
class OutputParamProcessor : public Processor {
public:
    static constexpr state::ParamID kParamId = 7100;

    // Configured per-instance before a run (via the g_output_param pointer).
    bool push_events = false;         // push param@0 and param@16
    bool emit_midi_between = false;    // note-on @8, between the two params
    bool mutate_store_only = false;    // store change with no explicit event
    float store_only_value = 0.0f;
    float value_0 = 0.25f;             // exactly representable in float+double
    float value_1 = 0.75f;

    PluginDescriptor descriptor() const override {
        PluginDescriptor d;
        d.name = "OutputParamCLAP";
        d.manufacturer = "PulpTest";
        d.bundle_id = "com.pulp.test.clap.outparam";
        d.version = "1.0.0";
        d.produces_midi = true;
        return d;
    }
    void define_parameters(state::StateStore& store) override {
        store.add_parameter({
            .id = kParamId,
            .name = "Out Param",
            .range = {0.0f, 1.0f, 0.0f, 0.0f},
        });
    }
    void prepare(const PrepareContext&) override {}
    void process(audio::BufferView<float>&,
                 const audio::BufferView<const float>&,
                 midi::MidiBuffer&,
                 midi::MidiBuffer& midi_out,
                 const ProcessContext&) override {
        if (push_events) {
            push_output_param_event(kParamId, value_0, 0);
            push_output_param_event(kParamId, value_1, 16);
        }
        if (emit_midi_between) {
            auto note = midi::MidiEvent::note_on(1, 64, 100);
            note.sample_offset = 8;
            midi_out.add(note);
        }
        if (mutate_store_only) {
            state().set_value_rt(kParamId, store_only_value);
        }
    }
};

class ObservingMidiInProcessor : public Processor {
public:
    std::size_t observed_event_count = 0;
    std::size_t observed_event_capacity = 0;
    std::size_t observed_sysex_count = 0;
    std::size_t observed_sysex_capacity = 0;
    std::size_t observed_sysex_payload_capacity = 0;
    std::size_t observed_first_sysex_size = 0;
    int32_t observed_first_sysex_offset = -1;
    std::array<uint8_t, 8> observed_first_sysex_prefix{};
    std::uint32_t observed_event_drops = 0;
    std::uint32_t observed_sysex_drops = 0;

    PluginDescriptor descriptor() const override {
        PluginDescriptor d;
        d.name = "ObservingMidiInCLAP";
        d.manufacturer = "PulpTest";
        d.bundle_id = "com.pulp.test.clap.observing-midi-in";
        d.version = "1.0.0";
        d.accepts_midi = true;
        return d;
    }
    void define_parameters(state::StateStore&) override {}
    void prepare(const PrepareContext&) override {}
    void process(audio::BufferView<float>&,
                 const audio::BufferView<const float>&,
                 midi::MidiBuffer& midi_in,
                 midi::MidiBuffer&,
                 const ProcessContext&) override {
        observed_event_count = midi_in.size();
        observed_event_capacity = midi_in.event_capacity();
        observed_sysex_count = midi_in.sysex_size();
        observed_sysex_capacity = midi_in.sysex_capacity();
        observed_sysex_payload_capacity = midi_in.sysex_copy_payload_capacity();
        if (!midi_in.sysex().empty()) {
            const auto& sx = midi_in.sysex().front();
            observed_first_sysex_size = sx.data.size();
            observed_first_sysex_offset = sx.sample_offset;
            for (std::size_t i = 0;
                 i < sx.data.size() && i < observed_first_sysex_prefix.size();
                 ++i) {
                observed_first_sysex_prefix[i] = sx.data[i];
            }
        }
        observed_event_drops = midi_in.dropped_event_count();
        observed_sysex_drops = midi_in.dropped_sysex_count();
    }
};

class ObservingParamIngressProcessor : public Processor {
public:
    static constexpr state::ParamID kParamId = 9101;

    bool observed_param_events_attached = false;
    std::size_t observed_param_event_count = 0;
    std::size_t observed_param_event_capacity = 0;
    bool observed_param_event_overflowed = false;
    std::uint32_t observed_param_event_drops = 0;
    int32_t observed_first_sample_offset = -1;
    int32_t observed_last_sample_offset = -1;
    float observed_first_value = -1.0f;
    float observed_last_value = -1.0f;
    float observed_state_value = -1.0f;
    int observed_num_samples = 0;

    PluginDescriptor descriptor() const override {
        PluginDescriptor d;
        d.name = "ObservingParamIngressCLAP";
        d.manufacturer = "PulpTest";
        d.bundle_id = "com.pulp.test.clap.observing-param-ingress";
        d.version = "1.0.0";
        return d;
    }
    void define_parameters(state::StateStore& store) override {
        store.add_parameter({
            .id = kParamId,
            .name = "Observed Param",
            .range = {0.0f, 1.0f, 0.0f, 0.0f},
        });
    }
    void prepare(const PrepareContext&) override {}
    void process(audio::BufferView<float>&,
                 const audio::BufferView<const float>&,
                 midi::MidiBuffer&,
                 midi::MidiBuffer&,
                 const ProcessContext& context) override {
        observed_num_samples = context.num_samples;
        observed_state_value = state().get_value(kParamId);
        observed_param_events_attached = param_events() != nullptr;
        if (auto* events = param_events()) {
            observed_param_event_count = events->size();
            observed_param_event_capacity = events->capacity();
            observed_param_event_overflowed = events->overflowed();
            observed_param_event_drops = events->dropped_event_count();
            if (!events->empty()) {
                const auto first = events->begin();
                const auto last = events->end() - 1;
                observed_first_sample_offset = first->sample_offset;
                observed_first_value = first->value;
                observed_last_sample_offset = last->sample_offset;
                observed_last_value = last->value;
            }
        }
    }
};

class ObservingSidecarProcessor : public Processor {
public:
    bool opts_mpe = false;
    bool opts_ump = false;
    bool observed_mpe_attached = false;
    bool observed_ump_attached = false;
    std::size_t observed_mpe_count = 0;
    std::size_t observed_mpe_capacity = 0;
    std::uint32_t observed_mpe_drops = 0;
    std::size_t observed_ump_count = 0;
    std::size_t observed_ump_capacity = 0;
    std::uint32_t observed_ump_drops = 0;

    PluginDescriptor descriptor() const override {
        PluginDescriptor d;
        d.name = "ObservingSidecarCLAP";
        d.manufacturer = "PulpTest";
        d.bundle_id = "com.pulp.test.clap.observing-sidecar";
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
                 midi::MidiBuffer&,
                 midi::MidiBuffer&,
                 const ProcessContext&) override {
        observed_mpe_attached = mpe_input() != nullptr;
        observed_ump_attached = ump_input() != nullptr;
        if (auto* mpe = mpe_input()) {
            observed_mpe_count = mpe->size();
            observed_mpe_capacity = mpe->capacity();
            observed_mpe_drops = mpe->dropped_event_count();
        }
        if (auto* ump = ump_input()) {
            observed_ump_count = ump->size();
            observed_ump_capacity = ump->capacity();
            observed_ump_drops = ump->dropped_event_count();
        }
    }
};

class OverflowingMidiOutProcessor : public Processor {
public:
    bool emit_sysex = false;
    std::vector<midi::MidiBuffer::SysexEvent> sysex_to_emit;
    std::size_t observed_event_count = 0;
    std::size_t observed_sysex_count = 0;
    std::uint32_t observed_event_drops = 0;
    std::uint32_t observed_sysex_drops = 0;
    std::size_t observed_event_capacity = 0;
    std::size_t observed_sysex_capacity = 0;

    PluginDescriptor descriptor() const override {
        PluginDescriptor d;
        d.name = "OverflowingMidiOutCLAP";
        d.manufacturer = "PulpTest";
        d.bundle_id = "com.pulp.test.clap.overflowing-midi-out";
        d.version = "1.0.0";
        d.produces_midi = true;
        return d;
    }
    void define_parameters(state::StateStore&) override {}
    void prepare(const PrepareContext&) override {
        if (emit_sysex) {
            sysex_to_emit.clear();
            sysex_to_emit.reserve(129);
            for (std::size_t i = 0; i < 129; ++i) {
                midi::MidiBuffer::SysexEvent se;
                se.data = {0xF0, 0x7D, static_cast<uint8_t>(i & 0x7F), 0xF7};
                se.sample_offset = static_cast<int32_t>(i);
                sysex_to_emit.push_back(se);
            }
        }
    }
    void process(audio::BufferView<float>&,
                 const audio::BufferView<const float>&,
                 midi::MidiBuffer&,
                 midi::MidiBuffer& midi_out,
                 const ProcessContext&) override {
        for (std::size_t i = 0; i < state::ParameterEventQueue::kCapacity + 1; ++i) {
            auto ev = midi::MidiEvent::cc(0, 7, static_cast<uint8_t>(i & 0x7F));
            ev.sample_offset = static_cast<int32_t>(i);
            midi_out.add(ev);
        }
        if (emit_sysex) {
            for (auto& se : sysex_to_emit) {
                midi_out.add_sysex(std::move(se));
            }
        }
        observed_event_count = midi_out.size();
        observed_sysex_count = midi_out.sysex_size();
        observed_event_drops = midi_out.dropped_event_count();
        observed_sysex_drops = midi_out.dropped_sysex_count();
        observed_event_capacity = midi_out.event_capacity();
        observed_sysex_capacity = midi_out.sysex_capacity();
    }
};

class ForwardingSysexProcessor : public Processor {
public:
    std::size_t observed_input_sysex_count = 0;
    std::size_t observed_output_sysex_count = 0;
    std::uint32_t observed_input_sysex_drops = 0;
    std::uint32_t observed_output_sysex_drops = 0;

    PluginDescriptor descriptor() const override {
        PluginDescriptor d;
        d.name = "ForwardingSysexCLAP";
        d.manufacturer = "PulpTest";
        d.bundle_id = "com.pulp.test.clap.forwarding-sysex";
        d.version = "1.0.0";
        d.accepts_midi = true;
        d.produces_midi = true;
        return d;
    }
    void define_parameters(state::StateStore&) override {}
    void prepare(const PrepareContext&) override {}
    void process(audio::BufferView<float>&,
                 const audio::BufferView<const float>&,
                 midi::MidiBuffer& midi_in,
                 midi::MidiBuffer& midi_out,
                 const ProcessContext&) override {
        for (auto& sx : midi_in.sysex()) {
            midi_out.add_sysex(std::move(sx));
        }
        observed_input_sysex_count = midi_in.sysex_size();
        observed_output_sysex_count = midi_out.sysex_size();
        observed_input_sysex_drops = midi_in.dropped_sysex_count();
        observed_output_sysex_drops = midi_out.dropped_sysex_count();
    }
};

class LatencyTailProcessor : public Processor {
public:
    PluginDescriptor descriptor() const override {
        PluginDescriptor d;
        d.name = "LatencyTailCLAP";
        d.manufacturer = "PulpTest";
        d.bundle_id = "com.pulp.test.clap.latency-tail";
        d.version = "1.0.0";
        d.tail_samples = tail_samples;
        return d;
    }
    void define_parameters(state::StateStore&) override {}
    void prepare(const PrepareContext&) override {}
    void process(audio::BufferView<float>&,
                 const audio::BufferView<const float>&,
                 midi::MidiBuffer&,
                 midi::MidiBuffer&,
                 const ProcessContext&) override {}
    int latency_samples() const override { return latency; }

    int latency = 0;
    int tail_samples = 0;
};

// Declares a real Bypass parameter and reports a non-zero latency, so the
// adapter routes the bypass pass-through through the per-channel dry delay.
class BypassLatencyProcessor : public Processor {
public:
    static constexpr state::ParamID kBypassId = 7777;

    PluginDescriptor descriptor() const override {
        PluginDescriptor d;
        d.name = "BypassLatencyCLAP";
        d.manufacturer = "PulpTest";
        d.bundle_id = "com.pulp.test.clap.bypass-latency";
        d.version = "1.0.0";
        return d;
    }
    void define_parameters(state::StateStore& store) override {
        store.add_parameter({
            .id = kBypassId,
            .name = "Bypass",
            .range = {0.0f, 1.0f, 0.0f, 1.0f},
        });
    }
    void prepare(const PrepareContext&) override {}
    void process(audio::BufferView<float>&,
                 const audio::BufferView<const float>&,
                 midi::MidiBuffer&,
                 midi::MidiBuffer&,
                 const ProcessContext&) override {}
    int latency_samples() const override { return latency; }

    int latency = 0;
};

// Processor factory hooks have to be raw function pointers. Route through
// singletons so the tests can configure the active processor before the
// adapter instantiates one.
CapturingProcessor* g_capturing = nullptr;
ProcessBuffersCapturingProcessor* g_process_buffers_capturing = nullptr;
EmittingProcessor*  g_emitting  = nullptr;
ObservingMidiInProcessor* g_observing_midi_in = nullptr;
ObservingParamIngressProcessor* g_observing_param_ingress = nullptr;
ObservingSidecarProcessor* g_observing_sidecar = nullptr;
OverflowingMidiOutProcessor* g_overflowing_midi_out = nullptr;
ForwardingSysexProcessor* g_forwarding_sysex = nullptr;
UnityCopyProcessor* g_unity_copy = nullptr;
NativeF64CopyProcessor* g_native_f64_copy = nullptr;
OutputParamProcessor* g_output_param = nullptr;
int g_pending_latency_samples = 0;
int g_pending_tail_samples = 0;
bool g_pending_opts_mpe = false;
bool g_pending_opts_ump = false;
bool g_pending_opts_node_mpe = false;
bool g_pending_opts_node_ump = false;
bool g_pending_capturing_bypass = false;
bool g_pending_capturing_sidechain = false;
bool g_pending_overflow_sysex = false;
std::vector<midi::MidiEvent> g_pending_emit;
std::vector<midi::MidiBuffer::SysexEvent> g_pending_sysex;

std::unique_ptr<Processor> make_capturing() {
    auto up = std::make_unique<CapturingProcessor>();
    g_capturing = up.get();
    if (g_pending_opts_mpe) up->opts_mpe = true;
    if (g_pending_opts_ump) up->opts_ump = true;
    if (g_pending_opts_node_mpe) up->opts_node_mpe = true;
    if (g_pending_opts_node_ump) up->opts_node_ump = true;
    up->declares_bypass = g_pending_capturing_bypass;
    up->declares_sidechain = g_pending_capturing_sidechain;
    g_pending_opts_mpe = false;
    g_pending_opts_ump = false;
    g_pending_opts_node_mpe = false;
    g_pending_opts_node_ump = false;
    g_pending_capturing_bypass = false;
    g_pending_capturing_sidechain = false;
    return up;
}

std::unique_ptr<Processor> make_process_buffers_capturing() {
    auto up = std::make_unique<ProcessBuffersCapturingProcessor>();
    g_capturing = up.get();
    g_process_buffers_capturing = up.get();
    if (g_pending_opts_mpe) up->opts_mpe = true;
    if (g_pending_opts_ump) up->opts_ump = true;
    if (g_pending_opts_node_mpe) up->opts_node_mpe = true;
    if (g_pending_opts_node_ump) up->opts_node_ump = true;
    up->declares_sidechain = g_pending_capturing_sidechain;
    g_pending_opts_mpe = false;
    g_pending_opts_ump = false;
    g_pending_opts_node_mpe = false;
    g_pending_opts_node_ump = false;
    g_pending_capturing_sidechain = false;
    return up;
}

std::unique_ptr<Processor> make_output_param() {
    auto up = std::make_unique<OutputParamProcessor>();
    g_output_param = up.get();
    return up;
}

std::unique_ptr<Processor> make_unity_copy() {
    auto up = std::make_unique<UnityCopyProcessor>();
    g_unity_copy = up.get();
    return up;
}

std::unique_ptr<Processor> make_native_f64_copy() {
    auto up = std::make_unique<NativeF64CopyProcessor>();
    g_native_f64_copy = up.get();
    return up;
}

MultiOutProcessor* g_multi_out = nullptr;
bool g_pending_multi_out_bypass = false;
int g_pending_multi_out_aux_channels = 2;
std::unique_ptr<Processor> make_multi_out() {
    auto up = std::make_unique<MultiOutProcessor>();
    up->declares_bypass = g_pending_multi_out_bypass;
    up->declared_aux_channels = g_pending_multi_out_aux_channels;
    g_pending_multi_out_bypass = false;
    g_pending_multi_out_aux_channels = 2;
    g_multi_out = up.get();
    return up;
}

std::unique_ptr<Processor> make_emitting() {
    auto up = std::make_unique<EmittingProcessor>();
    g_emitting = up.get();
    if (!g_pending_emit.empty()) up->to_emit = g_pending_emit;
    if (!g_pending_sysex.empty()) up->sysex_to_emit = g_pending_sysex;
    return up;
}

std::unique_ptr<Processor> make_observing_midi_in() {
    auto up = std::make_unique<ObservingMidiInProcessor>();
    g_observing_midi_in = up.get();
    return up;
}

std::unique_ptr<Processor> make_observing_param_ingress() {
    auto up = std::make_unique<ObservingParamIngressProcessor>();
    g_observing_param_ingress = up.get();
    return up;
}

std::unique_ptr<Processor> make_observing_sidecar() {
    auto up = std::make_unique<ObservingSidecarProcessor>();
    g_observing_sidecar = up.get();
    if (g_pending_opts_mpe) up->opts_mpe = true;
    if (g_pending_opts_ump) up->opts_ump = true;
    g_pending_opts_mpe = false;
    g_pending_opts_ump = false;
    g_pending_opts_node_mpe = false;
    g_pending_opts_node_ump = false;
    return up;
}

std::unique_ptr<Processor> make_overflowing_midi_out() {
    auto up = std::make_unique<OverflowingMidiOutProcessor>();
    g_overflowing_midi_out = up.get();
    up->emit_sysex = g_pending_overflow_sysex;
    g_pending_overflow_sysex = false;
    return up;
}

std::unique_ptr<Processor> make_forwarding_sysex() {
    auto up = std::make_unique<ForwardingSysexProcessor>();
    g_forwarding_sysex = up.get();
    return up;
}

std::unique_ptr<Processor> make_latency_tail() {
    auto up = std::make_unique<LatencyTailProcessor>();
    up->latency = g_pending_latency_samples;
    up->tail_samples = g_pending_tail_samples;
    g_pending_latency_samples = 0;
    g_pending_tail_samples = 0;
    return up;
}

int g_pending_bypass_latency = 0;

std::unique_ptr<Processor> make_bypass_latency() {
    auto up = std::make_unique<BypassLatencyProcessor>();
    up->latency = g_pending_bypass_latency;
    g_pending_bypass_latency = 0;
    return up;
}

std::unique_ptr<Processor> make_null_processor() {
    return nullptr;
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
    bool active = false;

    explicit Harness(ProcessorFactory factory, uint32_t max_frames = kFrames) {
        plugin.factory = factory;
        plugin.plugin.plugin_data = &plugin;
        REQUIRE(clap_adapter::clap_init(&plugin.plugin));
        REQUIRE(clap_adapter::clap_activate(&plugin.plugin, 48000.0, 32, max_frames));
        active = true;
        in_left.assign(max_frames, 0.0f);
        in_right.assign(max_frames, 0.0f);
        out_left.assign(max_frames, 0.0f);
        out_right.assign(max_frames, 0.0f);
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
        if (active) clap_adapter::clap_deactivate(&plugin.plugin);
    }

    void deactivate() {
        REQUIRE(active);
        clap_adapter::clap_deactivate(&plugin.plugin);
        active = false;
    }

    clap_process_status run(InputEventList& in_list,
                            OutputEventList* out_list = nullptr) {
        return run_custom(&in_list, out_list,
                          &audio_in, 1,
                          &audio_out, 1,
                          kFrames, nullptr);
    }

    clap_process_status run_custom(InputEventList* in_list,
                                   OutputEventList* out_list,
                                   clap_audio_buffer_t* inputs,
                                   uint32_t input_count,
                                   clap_audio_buffer_t* outputs,
                                   uint32_t output_count,
                                   uint32_t frames = kFrames,
                                   const clap_event_transport_t* transport = nullptr) {
        clap_input_events_t in_vt{};
        if (in_list) in_vt = in_list->make_vtable();
        clap_output_events_t out_vt{};
        if (out_list) out_vt = out_list->make_vtable();

        clap_process_t proc{};
        proc.frames_count = frames;
        proc.audio_inputs = inputs;
        proc.audio_inputs_count = input_count;
        proc.audio_outputs = outputs;
        proc.audio_outputs_count = output_count;
        proc.in_events = in_list ? &in_vt : nullptr;
        proc.out_events = out_list ? &out_vt : nullptr;
        proc.transport = transport;
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

// ── Lifecycle / process-shape edge cases ───────────────────────────────

TEST_CASE("CLAP lifecycle forwards prepare/release and handles no-op callbacks",
          "[clap][lifecycle]") {
    g_pending_opts_mpe = false;
    g_pending_opts_ump = false;
    Harness h(make_capturing);
    REQUIRE(g_capturing != nullptr);

    REQUIRE(g_capturing->prepare_count == 1);
    REQUIRE(g_capturing->captured_prepare.sample_rate == 48000.0);
    REQUIRE(g_capturing->captured_prepare.max_buffer_size == static_cast<int>(Harness::kFrames));
    REQUIRE(g_capturing->captured_prepare.input_channels == 2);
    REQUIRE(g_capturing->captured_prepare.output_channels == 2);

    REQUIRE(clap_adapter::clap_start_processing(&h.plugin.plugin));
    clap_adapter::clap_stop_processing(&h.plugin.plugin);
    clap_adapter::clap_reset(&h.plugin.plugin);
    clap_adapter::clap_on_main_thread(&h.plugin.plugin);

    h.deactivate();
    REQUIRE(g_capturing->release_count == 1);

    auto* raw = new clap_adapter::PulpClapPlugin;
    raw->factory = make_capturing;
    raw->plugin.plugin_data = raw;
    REQUIRE(clap_adapter::clap_init(&raw->plugin));
    auto owner_alive = raw->owner_alive.capture();
    REQUIRE(runtime::AliveToken::is_alive(owner_alive));
    clap_adapter::clap_destroy(&raw->plugin);
    REQUIRE_FALSE(runtime::AliveToken::is_alive(owner_alive));
    g_capturing = nullptr;
}

TEST_CASE("CLAP latency and tail extensions report processor runtime contract",
          "[clap][latency][tail]") {
    g_pending_latency_samples = 256;
    g_pending_tail_samples = 4096;
    Harness finite(make_latency_tail);
    REQUIRE(clap_generic::latency_get(&finite.plugin.plugin) == 256u);
    REQUIRE(clap_generic::tail_get(&finite.plugin.plugin) == 4096u);

    finite.deactivate();

    g_pending_latency_samples = -128;
    g_pending_tail_samples = -1;
    Harness infinite(make_latency_tail);
    REQUIRE(clap_generic::latency_get(&infinite.plugin.plugin) == 0u);
    REQUIRE(clap_generic::tail_get(&infinite.plugin.plugin) == UINT32_MAX);
}

TEST_CASE("CLAP init and process fail cleanly without a processor",
          "[clap][lifecycle]") {
    clap_adapter::PulpClapPlugin init_plugin;
    init_plugin.factory = make_null_processor;
    init_plugin.plugin.plugin_data = &init_plugin;
    REQUIRE_FALSE(clap_adapter::clap_init(&init_plugin.plugin));

    clap_adapter::PulpClapPlugin process_plugin;
    process_plugin.plugin.plugin_data = &process_plugin;
    clap_process_t proc{};
    proc.frames_count = Harness::kFrames;
    REQUIRE(clap_adapter::clap_process(&process_plugin.plugin, &proc) == CLAP_PROCESS_ERROR);
}

TEST_CASE("CLAP survives repeated init activate process deactivate destroy cycles",
          "[clap][lifecycle][churn]") {
    constexpr uint32_t kFrames = 64;
    std::array<float, kFrames> in_l{};
    std::array<float, kFrames> in_r{};
    std::array<float, kFrames> out_l{};
    std::array<float, kFrames> out_r{};
    float* inputs[2] = {in_l.data(), in_r.data()};
    float* outputs[2] = {out_l.data(), out_r.data()};
    clap_audio_buffer_t input{};
    input.data32 = inputs;
    input.channel_count = 2;
    clap_audio_buffer_t output{};
    output.data32 = outputs;
    output.channel_count = 2;

    for (int cycle = 0; cycle < 200; ++cycle) {
        g_pending_capturing_bypass = true;
        auto* raw = new clap_adapter::PulpClapPlugin;
        raw->factory = make_capturing;
        raw->plugin.plugin_data = raw;
        REQUIRE(clap_adapter::clap_init(&raw->plugin));
        REQUIRE(raw->store.param_count() == 2);
        REQUIRE(raw->store.all_params()[0].id == CapturingProcessor::kParamId);
        REQUIRE(raw->store.all_params()[1].id == CapturingProcessor::kBypassParamId);
        REQUIRE(raw->store.get_value(CapturingProcessor::kParamId) == 0.0f);
        raw->store.set_value(CapturingProcessor::kParamId,
                             static_cast<float>((cycle % 99) + 1) / 100.0f);
        auto alive = raw->owner_alive.capture();
        REQUIRE(clap_adapter::clap_activate(
            &raw->plugin, cycle % 2 == 0 ? 44100.0 : 96000.0, 16, kFrames));
        REQUIRE(clap_adapter::clap_start_processing(&raw->plugin));

        clap_process_t process{};
        process.frames_count = cycle % 2 == 0 ? 32 : kFrames;
        process.audio_inputs = &input;
        process.audio_inputs_count = 1;
        process.audio_outputs = &output;
        process.audio_outputs_count = 1;
        raw->store.set_value(CapturingProcessor::kBypassParamId, 1.0f);
        REQUIRE(clap_adapter::clap_process(&raw->plugin, &process) != CLAP_PROCESS_ERROR);
        REQUIRE(g_capturing->process_count == 0);

        raw->store.set_value(CapturingProcessor::kBypassParamId, 0.0f);
        REQUIRE(clap_adapter::clap_process(&raw->plugin, &process) != CLAP_PROCESS_ERROR);
        REQUIRE(g_capturing->process_count == 1);

        clap_adapter::clap_stop_processing(&raw->plugin);
        clap_adapter::clap_deactivate(&raw->plugin);
        clap_adapter::clap_destroy(&raw->plugin);
        REQUIRE_FALSE(runtime::AliveToken::is_alive(alive));
    }
}

TEST_CASE("CLAP zero-frame process block returns without touching processor state",
          "[clap][process]") {
    g_pending_opts_mpe = false;
    g_pending_opts_ump = false;
    Harness h(make_capturing);
    REQUIRE(g_capturing != nullptr);

    InputEventList events;
    clap_event_midi_t ev{};
    ev.header = make_header(sizeof(ev), CLAP_EVENT_MIDI, 0);
    ev.data[0] = 0xB0;
    ev.data[1] = 74;
    ev.data[2] = 64;
    events.push(ev);

    REQUIRE(h.run_custom(&events, nullptr,
                         &h.audio_in, 1,
                         &h.audio_out, 1,
                         0, nullptr) == CLAP_PROCESS_CONTINUE);
    REQUIRE(g_capturing->process_count == 0);
    REQUIRE(g_capturing->captured_midi.empty());
}

// The adapter wraps the whole audio callback in a flush-to-zero scope so quiet
// recursive DSP tails can't stall the host's audio thread, then restores the
// host's prior FP mode on exit. This proves both halves on the real
// clap_process() path: flush is active while the Processor runs, and the
// shared host thread is left exactly as it was found.
TEST_CASE("CLAP process flushes denormals during the callback and restores host FP mode",
          "[clap][process][denormal][numeric-mode]") {
    g_pending_opts_mpe = false;
    g_pending_opts_ump = false;
    Harness h(make_capturing);
    REQUIRE(g_capturing != nullptr);

    // Drive the call from a known FP mode. denormals_are_flushed() reads the
    // live register; capture it so the restore assertion holds whether the
    // test process started flushing or not.
    const bool host_mode_before = pulp::signal::denormals_are_flushed();

    InputEventList events;
    REQUIRE(h.run(events) == CLAP_PROCESS_CONTINUE);
    REQUIRE(g_capturing->process_count == 1);

    if constexpr (pulp::signal::kHardwareFlushSupported) {
        // The Processor ran with the hardware flush bit set.
        REQUIRE(g_capturing->flushed_during_process);
    } else {
        SUCCEED("hardware flush-to-zero not available on this target");
    }
    // Regardless of platform, the host thread's FP mode is exactly restored.
    REQUIRE(pulp::signal::denormals_are_flushed() == host_mode_before);
}

#if PULP_CLAP_PROCESS_RT_TRAP_TESTS
TEST_CASE("CLAP steady-state process path is no-alloc and no-lock",
          "[clap][process][rt-safety]") {
    g_pending_opts_mpe = false;
    g_pending_opts_ump = false;
    Harness h(make_capturing);

    InputEventList warmup;
    REQUIRE(h.run(warmup) == CLAP_PROCESS_CONTINUE);
    REQUIRE(g_capturing->process_count == 1);

    InputEventList steady_state;
    clap_process_status status = CLAP_PROCESS_ERROR;
    {
        native_components::test::RtNoAllocScope guard;
        status = h.run(steady_state);
    }

    REQUIRE(status == CLAP_PROCESS_CONTINUE);
    REQUIRE(g_capturing->process_count == 2);
}
#endif

TEST_CASE("CLAP process ignores non-core event namespaces",
          "[clap][events][namespace]") {
    g_pending_opts_mpe = false;
    g_pending_opts_ump = false;
    Harness h(make_capturing);

    InputEventList events;
    clap_event_param_value_t param{};
    param.header = make_header(sizeof(param), CLAP_EVENT_PARAM_VALUE, 3);
    param.header.space_id = 99;
    param.param_id = CapturingProcessor::kParamId;
    param.value = 0.75;
    events.push(param);

    clap_event_midi_t midi{};
    midi.header = make_header(sizeof(midi), CLAP_EVENT_MIDI, 4);
    midi.header.space_id = 99;
    midi.data[0] = 0xB0;
    midi.data[1] = 7;
    midi.data[2] = 127;
    events.push(midi);

    REQUIRE(h.run(events) == CLAP_PROCESS_CONTINUE);
    REQUIRE(g_capturing->captured_midi.empty());
    REQUIRE(g_capturing->captured_param_events.empty());
    REQUIRE(g_capturing->state().get_value(CapturingProcessor::kParamId) == 0.0f);
}

TEST_CASE("CLAP parameter modulation applies for one block then resets",
          "[clap][params][modulation]") {
    g_pending_opts_mpe = false;
    g_pending_opts_ump = false;
    Harness h(make_capturing);

    InputEventList mod_events;
    clap_event_param_mod_t mod{};
    mod.header = make_header(sizeof(mod), CLAP_EVENT_PARAM_MOD, 5);
    mod.param_id = CapturingProcessor::kParamId;
    mod.amount = 0.375;
    mod_events.push(mod);

    REQUIRE(h.run(mod_events) == CLAP_PROCESS_CONTINUE);
    REQUIRE(g_capturing->had_param_events);
    REQUIRE(g_capturing->captured_param_events.empty());
    REQUIRE(g_capturing->captured_modulated_value == 0.375f);

    InputEventList empty;
    REQUIRE(h.run(empty) == CLAP_PROCESS_CONTINUE);
    REQUIRE(g_capturing->captured_modulated_value == 0.0f);
}

TEST_CASE("CLAP parameter modulation projects to the typed global lane",
          "[clap][params][modulation][lane]") {
    g_pending_opts_mpe = false;
    g_pending_opts_ump = false;
    Harness h(make_capturing);

    clap_event_param_mod_t mod{};
    mod.header = make_header(sizeof(mod), CLAP_EVENT_PARAM_MOD, 5);
    mod.param_id = CapturingProcessor::kParamId;
    mod.note_id = 123;
    mod.port_index = 0;
    mod.channel = 2;
    mod.key = 64;
    mod.amount = 0.375;

    state::ModulationLane lane;
    REQUIRE(clap_adapter::clap_param_modulation_lane(h.plugin, mod, lane));
    REQUIRE(lane.source.id == clap_adapter::kClapHostModulationSourceId);
    REQUIRE(lane.source.scope == state::ModulationScope::Global);
    REQUIRE(lane.source.rate == state::ModulationRate::Control);
    REQUIRE(lane.target.param_id == CapturingProcessor::kParamId);
    REQUIRE(lane.target.scope == state::ModulationScope::Global);
    REQUIRE(lane.target.param_rate == state::ParamRate::ControlRate);
    REQUIRE(lane.target.modulatable);
    REQUIRE(lane.target.writable);
    REQUIRE(lane.mix == state::ModulationMixMode::Add);
    REQUIRE(lane.depth == 0.375f);
    REQUIRE(state::validate_modulation_lane(lane).accepted);

    mod.param_id = CapturingProcessor::kParamId + 1;
    REQUIRE_FALSE(clap_adapter::clap_param_modulation_lane(h.plugin, mod, lane));
}

TEST_CASE("CLAP gesture events forward through StateStore callbacks",
          "[clap][params][gesture]") {
    g_pending_opts_mpe = false;
    g_pending_opts_ump = false;
    Harness h(make_capturing);

    std::vector<state::ParamID> began;
    std::vector<state::ParamID> ended;
    h.plugin.store.set_gesture_callbacks(
        [&](state::ParamID id) { began.push_back(id); },
        [&](state::ParamID id) { ended.push_back(id); });

    InputEventList events;
    clap_event_param_gesture_t begin{};
    begin.header = make_header(sizeof(begin), CLAP_EVENT_PARAM_GESTURE_BEGIN, 1);
    begin.param_id = CapturingProcessor::kParamId;
    events.push(begin);
    clap_event_param_gesture_t end{};
    end.header = make_header(sizeof(end), CLAP_EVENT_PARAM_GESTURE_END, 9);
    end.param_id = CapturingProcessor::kParamId;
    events.push(end);

    REQUIRE(h.run(events) == CLAP_PROCESS_CONTINUE);
    REQUIRE(began.size() == 1);
    REQUIRE(ended.size() == 1);
    REQUIRE(began[0] == CapturingProcessor::kParamId);
    REQUIRE(ended[0] == CapturingProcessor::kParamId);
}

TEST_CASE("CLAP routes a descriptor-declared sidechain input",
          "[clap][audio][sidechain]") {
    g_pending_opts_mpe = false;
    g_pending_opts_ump = false;
    g_pending_capturing_sidechain = true;
    Harness h(make_capturing);

    std::vector<float> sc_left(Harness::kFrames, 0.25f);
    std::vector<float> sc_right(Harness::kFrames, -0.5f);
    float* sc_ptrs[2] = {sc_left.data(), sc_right.data()};
    clap_audio_buffer_t sidechain{};
    sidechain.data32 = sc_ptrs;
    sidechain.channel_count = 2;

    clap_audio_buffer_t inputs[2] = {h.audio_in, sidechain};
    REQUIRE(h.run_custom(nullptr, nullptr,
                         inputs, 2,
                         &h.audio_out, 1) == CLAP_PROCESS_CONTINUE);

    REQUIRE(g_capturing->had_sidechain_input);
    REQUIRE(g_capturing->captured_sidechain_channels == 2);
    REQUIRE(g_capturing->captured_sidechain_samples == static_cast<int>(Harness::kFrames));
    REQUIRE(g_capturing->captured_sidechain_first_sample == 0.25f);
    REQUIRE(g_capturing->captured_sidechain_second_sample == -0.5f);
    REQUIRE(g_capturing->captured_input_channels == 2);
    REQUIRE(g_capturing->captured_output_channels == 2);
}

TEST_CASE("CLAP processes data64 buffers through the f32 boundary path",
          "[clap][audio][f64]") {
    g_unity_copy = nullptr;
    Harness h(make_unity_copy);
    REQUIRE(g_unity_copy != nullptr);

    std::array<double, Harness::kFrames> in_l{};
    std::array<double, Harness::kFrames> in_r{};
    std::array<double, Harness::kFrames> out_l{};
    std::array<double, Harness::kFrames> out_r{};
    for (std::size_t i = 0; i < Harness::kFrames; ++i) {
        in_l[i] = 0.1 * static_cast<double>(i + 1);
        in_r[i] = -0.125 * static_cast<double>(i + 1);
    }
    double* in_ptrs[2] = {in_l.data(), in_r.data()};
    double* out_ptrs[2] = {out_l.data(), out_r.data()};
    clap_audio_buffer_t audio_in{};
    audio_in.data64 = in_ptrs;
    audio_in.channel_count = 2;
    clap_audio_buffer_t audio_out{};
    audio_out.data64 = out_ptrs;
    audio_out.channel_count = 2;

    REQUIRE(h.run_custom(nullptr, nullptr,
                         &audio_in, 1,
                         &audio_out, 1) == CLAP_PROCESS_CONTINUE);
    REQUIRE(g_unity_copy->process_count == 1);
    REQUIRE(g_unity_copy->observed_num_samples == static_cast<int>(Harness::kFrames));

    for (std::size_t i = 0; i < Harness::kFrames; ++i) {
        REQUIRE(out_l[i] == static_cast<double>(static_cast<float>(in_l[i])));
        REQUIRE(out_r[i] == static_cast<double>(static_cast<float>(in_r[i])));
    }
}

TEST_CASE("CLAP routes data64 buffers to native process_f64 when opted in",
          "[clap][audio][f64]") {
    g_native_f64_copy = nullptr;
    Harness h(make_native_f64_copy);
    REQUIRE(g_native_f64_copy != nullptr);

    std::array<double, Harness::kFrames> in_l{};
    std::array<double, Harness::kFrames> in_r{};
    std::array<double, Harness::kFrames> out_l{};
    std::array<double, Harness::kFrames> out_r{};
    for (std::size_t i = 0; i < Harness::kFrames; ++i) {
        in_l[i] = 0.1 + 1.0e-12 * static_cast<double>(i + 1);
        in_r[i] = -0.125 + 1.0e-12 * static_cast<double>(i + 1);
    }
    REQUIRE(in_l[0] != static_cast<double>(static_cast<float>(in_l[0])));

    double* in_ptrs[2] = {in_l.data(), in_r.data()};
    double* out_ptrs[2] = {out_l.data(), out_r.data()};
    clap_audio_buffer_t audio_in{};
    audio_in.data64 = in_ptrs;
    audio_in.channel_count = 2;
    clap_audio_buffer_t audio_out{};
    audio_out.data64 = out_ptrs;
    audio_out.channel_count = 2;

    REQUIRE(h.run_custom(nullptr, nullptr,
                         &audio_in, 1,
                         &audio_out, 1) == CLAP_PROCESS_CONTINUE);
    REQUIRE(g_native_f64_copy->process_count == 0);
    REQUIRE(g_native_f64_copy->process_f64_count == 1);
    REQUIRE(g_native_f64_copy->observed_num_samples == static_cast<int>(Harness::kFrames));
    REQUIRE(g_native_f64_copy->observed_input_channels == 2);
    REQUIRE(g_native_f64_copy->observed_output_channels == 2);
    REQUIRE(g_native_f64_copy->had_main_input);
    REQUIRE(g_native_f64_copy->had_main_output);
    REQUIRE(g_native_f64_copy->layouts_match);

    for (std::size_t i = 0; i < Harness::kFrames; ++i) {
        REQUIRE(out_l[i] == in_l[i]);
        REQUIRE(out_r[i] == in_r[i]);
    }
}

TEST_CASE("CLAP demotes mixed 32/64 buses to the f32 boundary path",
          "[clap][audio][f64]") {
    // A native-f64 plugin handed a 64-bit input but a 32-bit output cannot
    // dispatch process_f64 (the block is not uniformly double), so the adapter
    // must fall back to the f32 surface and still demote the f64 input into
    // the boundary scratch — this pins the deferred-conversion ordering.
    g_native_f64_copy = nullptr;
    Harness h(make_native_f64_copy);
    REQUIRE(g_native_f64_copy != nullptr);

    std::array<double, Harness::kFrames> in_l{};
    std::array<double, Harness::kFrames> in_r{};
    std::array<float, Harness::kFrames> out_l{};
    std::array<float, Harness::kFrames> out_r{};
    for (std::size_t i = 0; i < Harness::kFrames; ++i) {
        in_l[i] = 0.1 * static_cast<double>(i + 1);
        in_r[i] = -0.125 * static_cast<double>(i + 1);
    }
    double* in_ptrs[2] = {in_l.data(), in_r.data()};
    float* out_ptrs[2] = {out_l.data(), out_r.data()};
    clap_audio_buffer_t audio_in{};
    audio_in.data64 = in_ptrs;
    audio_in.channel_count = 2;
    clap_audio_buffer_t audio_out{};
    audio_out.data32 = out_ptrs;
    audio_out.channel_count = 2;

    REQUIRE(h.run_custom(nullptr, nullptr,
                         &audio_in, 1,
                         &audio_out, 1) == CLAP_PROCESS_CONTINUE);
    REQUIRE(g_native_f64_copy->process_f64_count == 0);
    REQUIRE(g_native_f64_copy->process_count == 1);
    REQUIRE(g_native_f64_copy->observed_num_samples == static_cast<int>(Harness::kFrames));

    for (std::size_t i = 0; i < Harness::kFrames; ++i) {
        REQUIRE(out_l[i] == static_cast<float>(in_l[i]));
        REQUIRE(out_r[i] == static_cast<float>(in_r[i]));
    }
}

TEST_CASE("CLAP supplies ProcessBuffers to processors that override the richer surface",
          "[clap][audio][process-buffers]") {
    g_pending_opts_mpe = false;
    g_pending_opts_ump = false;
    g_pending_capturing_sidechain = true;
    Harness h(make_process_buffers_capturing);

    std::vector<float> sc_left(Harness::kFrames, 0.25f);
    std::vector<float> sc_right(Harness::kFrames, -0.5f);
    float* sc_ptrs[2] = {sc_left.data(), sc_right.data()};
    clap_audio_buffer_t sidechain{};
    sidechain.data32 = sc_ptrs;
    sidechain.channel_count = 2;

    clap_audio_buffer_t inputs[2] = {h.audio_in, sidechain};
    REQUIRE(h.run_custom(nullptr, nullptr,
                         inputs, 2,
                         &h.audio_out, 1) == CLAP_PROCESS_CONTINUE);

    REQUIRE(g_process_buffers_capturing != nullptr);
    REQUIRE(g_process_buffers_capturing->process_buffer_calls == 1);
    REQUIRE(g_process_buffers_capturing->captured_input_bus_count == 2);
    REQUIRE(g_process_buffers_capturing->captured_output_bus_count == 1);
    REQUIRE(g_process_buffers_capturing->captured_active_input_bus_count == 2);
    REQUIRE(g_process_buffers_capturing->captured_active_output_bus_count == 1);
    REQUIRE(g_process_buffers_capturing->saw_process_buffer_sidechain);
    REQUIRE(g_process_buffers_capturing->process_buffer_layouts_match);
    REQUIRE(g_process_buffers_capturing->process_buffer_storage_valid);

    REQUIRE(g_capturing->process_count == 1);
    REQUIRE(g_capturing->had_sidechain_input);
    REQUIRE(g_capturing->captured_sidechain_channels == 2);
    REQUIRE(g_capturing->captured_sidechain_first_sample == 0.25f);
    REQUIRE(g_capturing->captured_sidechain_second_sample == -0.5f);
    REQUIRE(g_capturing->sidechain_input() == nullptr);
}

TEST_CASE("CLAP routes a declared secondary output bus to the Processor",
          "[clap][audio][multi-out]") {
    g_multi_out = nullptr;
    Harness h(make_multi_out);
    REQUIRE(g_multi_out != nullptr);

    // Two host output buses, stereo each, pre-seeded with a sentinel that must
    // be overwritten by the routed processor (proves it is not left untouched).
    std::vector<float> main_l(Harness::kFrames, 99.0f);
    std::vector<float> main_r(Harness::kFrames, 99.0f);
    std::vector<float> aux_l(Harness::kFrames, 99.0f);
    std::vector<float> aux_r(Harness::kFrames, 99.0f);
    float* main_ptrs[2] = {main_l.data(), main_r.data()};
    float* aux_ptrs[2] = {aux_l.data(), aux_r.data()};

    clap_audio_buffer_t main_out{};
    main_out.data32 = main_ptrs;
    main_out.channel_count = 2;
    clap_audio_buffer_t aux_out{};
    aux_out.data32 = aux_ptrs;
    aux_out.channel_count = 2;

    clap_audio_buffer_t outputs[2] = {main_out, aux_out};
    REQUIRE(h.run_custom(nullptr, nullptr,
                         nullptr, 0,
                         outputs, 2) == CLAP_PROCESS_CONTINUE);

    // The Processor saw both output buses via the richer surface.
    REQUIRE(g_multi_out->process_buffer_calls == 1);
    REQUIRE(g_multi_out->captured_output_bus_count == 2);
    REQUIRE(g_multi_out->wrote_main);
    REQUIRE(g_multi_out->wrote_aux);
    REQUIRE(g_multi_out->captured_aux_channels == 2);
    // declared_channels reflects the descriptor (2), and with host == declared
    // the layout matches — proving declared_channels is sourced from the
    // descriptor, not echoed from the routed count.
    REQUIRE(g_multi_out->captured_aux_declared == 2);
    REQUIRE(g_multi_out->captured_layouts_match);

    // BOTH buses carry their distinct audio — the aux bus is no longer
    // zero-filled, and main and aux hold different signals.
    REQUIRE(std::all_of(main_l.begin(), main_l.end(),
                        [](float v) { return v == MultiOutProcessor::kMainValue; }));
    REQUIRE(std::all_of(main_r.begin(), main_r.end(),
                        [](float v) { return v == MultiOutProcessor::kMainValue; }));
    REQUIRE(std::all_of(aux_l.begin(), aux_l.end(),
                        [](float v) { return v == MultiOutProcessor::kAuxValue; }));
    REQUIRE(std::all_of(aux_r.begin(), aux_r.end(),
                        [](float v) { return v == MultiOutProcessor::kAuxValue; }));
    REQUIRE(MultiOutProcessor::kMainValue != MultiOutProcessor::kAuxValue);
}

TEST_CASE("CLAP fails closed when the host supplies more buses than advertised",
          "[clap][audio][multi-out][malformed-layout]") {
    g_pending_opts_mpe = false;
    g_pending_opts_ump = false;
    Harness h(make_capturing);  // CapturingProcessor declares one output bus
    REQUIRE(g_capturing != nullptr);

    std::vector<float> aux_l(Harness::kFrames, 7.0f);
    std::vector<float> aux_r(Harness::kFrames, -7.0f);
    float* aux_ptrs[2] = {aux_l.data(), aux_r.data()};
    clap_audio_buffer_t aux_out{};
    aux_out.data32 = aux_ptrs;
    aux_out.channel_count = 2;

    clap_audio_buffer_t outputs[2] = {h.audio_out, aux_out};
    REQUIRE(h.run_custom(nullptr, nullptr,
                         &h.audio_in, 1,
                         outputs, 2) == CLAP_PROCESS_CONTINUE);

    REQUIRE(g_capturing->process_count == 0);
    REQUIRE(std::all_of(h.out_left.begin(), h.out_left.end(), [](float v) { return v == 0.0f; }));
    REQUIRE(std::all_of(h.out_right.begin(), h.out_right.end(), [](float v) { return v == 0.0f; }));
    REQUIRE(std::all_of(aux_l.begin(), aux_l.end(), [](float v) { return v == 0.0f; }));
    REQUIRE(std::all_of(aux_r.begin(), aux_r.end(), [](float v) { return v == 0.0f; }));
}

TEST_CASE("CLAP fails closed for an active output bus with null storage",
          "[clap][audio][multi-out][rt-safety][malformed-layout]") {
    // P0 regression: a host can present a deactivated aux output bus with
    // channel_count > 0 but data32 == nullptr. The pre-zero loop and the
    // routing path must both guard the bus pointer — no null deref on the audio
    // thread — and the processor must see the aux bus as inactive.
    g_multi_out = nullptr;
    Harness h(make_multi_out);
    REQUIRE(g_multi_out != nullptr);

    std::vector<float> main_l(Harness::kFrames, 99.0f);
    std::vector<float> main_r(Harness::kFrames, 99.0f);
    float* main_ptrs[2] = {main_l.data(), main_r.data()};
    clap_audio_buffer_t main_out{};
    main_out.data32 = main_ptrs;
    main_out.channel_count = 2;

    // Inactive aux bus: channel_count reported but no channel-pointer array.
    clap_audio_buffer_t aux_out{};
    aux_out.data32 = nullptr;
    aux_out.channel_count = 2;

    clap_audio_buffer_t outputs[2] = {main_out, aux_out};
    REQUIRE(h.run_custom(nullptr, nullptr,
                         nullptr, 0,
                         outputs, 2) == CLAP_PROCESS_CONTINUE);

    REQUIRE(g_multi_out->process_buffer_calls == 0);
    REQUIRE(std::all_of(main_l.begin(), main_l.end(),
                        [](float v) { return v == 0.0f; }));
}

TEST_CASE("CLAP fails closed for a wrong output channel count",
          "[clap][audio][multi-out][malformed-layout]") {
    g_multi_out = nullptr;
    g_pending_multi_out_aux_channels = 1;  // descriptor declares mono aux
    Harness h(make_multi_out);
    REQUIRE(g_multi_out != nullptr);
    REQUIRE(g_multi_out->declared_aux_channels == 1);

    std::vector<float> main_l(Harness::kFrames, 0.0f);
    std::vector<float> main_r(Harness::kFrames, 0.0f);
    std::vector<float> aux_l(Harness::kFrames, 99.0f);
    std::vector<float> aux_r(Harness::kFrames, 99.0f);
    float* main_ptrs[2] = {main_l.data(), main_r.data()};
    float* aux_ptrs[2] = {aux_l.data(), aux_r.data()};
    clap_audio_buffer_t main_out{};
    main_out.data32 = main_ptrs;
    main_out.channel_count = 2;
    clap_audio_buffer_t aux_out{};
    aux_out.data32 = aux_ptrs;
    aux_out.channel_count = 2;  // host drives 2 channels on a mono-declared bus

    clap_audio_buffer_t outputs[2] = {main_out, aux_out};
    REQUIRE(h.run_custom(nullptr, nullptr,
                         nullptr, 0,
                         outputs, 2) == CLAP_PROCESS_CONTINUE);

    REQUIRE(g_multi_out->process_buffer_calls == 0);
    REQUIRE(std::all_of(main_l.begin(), main_l.end(), [](float v) { return v == 0.0f; }));
    REQUIRE(std::all_of(main_r.begin(), main_r.end(), [](float v) { return v == 0.0f; }));
    REQUIRE(std::all_of(aux_l.begin(), aux_l.end(), [](float v) { return v == 0.0f; }));
    REQUIRE(std::all_of(aux_r.begin(), aux_r.end(), [](float v) { return v == 0.0f; }));
}

TEST_CASE("CLAP fails closed when the host supplies fewer buses than advertised",
          "[clap][audio][multi-out][malformed-layout]") {
    g_multi_out = nullptr;
    Harness h(make_multi_out);
    REQUIRE(g_multi_out != nullptr);

    std::vector<float> main_l(Harness::kFrames, 99.0f);
    std::vector<float> main_r(Harness::kFrames, 99.0f);
    float* main_ptrs[2] = {main_l.data(), main_r.data()};
    clap_audio_buffer_t main_out{};
    main_out.data32 = main_ptrs;
    main_out.channel_count = 2;

    REQUIRE(h.run_custom(nullptr, nullptr, nullptr, 0, &main_out, 1) ==
            CLAP_PROCESS_CONTINUE);
    REQUIRE(g_multi_out->process_buffer_calls == 0);
    REQUIRE(std::all_of(main_l.begin(), main_l.end(), [](float v) { return v == 0.0f; }));
    REQUIRE(std::all_of(main_r.begin(), main_r.end(), [](float v) { return v == 0.0f; }));
}

TEST_CASE("CLAP clamps an oversized block on a secondary output bus",
          "[clap][audio][multi-out][rt-safety]") {
    // The clamp + tail-zero applies to aux buses too: with a host block larger
    // than the prepared max, the processor only writes [0, prepared_max) and the
    // un-processable tail [prepared_max, original) on the aux bus reads silence.
    constexpr uint32_t kPreparedMax = 64;
    constexpr uint32_t kRenderFrames = 256;
    g_multi_out = nullptr;
    Harness h(make_multi_out, kPreparedMax);
    REQUIRE(g_multi_out != nullptr);

    std::vector<float> main_l(kRenderFrames, -9.0f);
    std::vector<float> main_r(kRenderFrames, -9.0f);
    std::vector<float> aux_l(kRenderFrames, -9.0f);
    std::vector<float> aux_r(kRenderFrames, -9.0f);
    float* main_ptrs[2] = {main_l.data(), main_r.data()};
    float* aux_ptrs[2] = {aux_l.data(), aux_r.data()};
    clap_audio_buffer_t main_out{};
    main_out.data32 = main_ptrs;
    main_out.channel_count = 2;
    clap_audio_buffer_t aux_out{};
    aux_out.data32 = aux_ptrs;
    aux_out.channel_count = 2;

    clap_audio_buffer_t outputs[2] = {main_out, aux_out};
    REQUIRE(h.run_custom(nullptr, nullptr,
                         nullptr, 0,
                         outputs, 2, kRenderFrames, nullptr) != CLAP_PROCESS_ERROR);

    // Processed region carries the aux signal; the tail is silence.
    for (uint32_t i = 0; i < kPreparedMax; ++i) {
        REQUIRE(aux_l[i] == MultiOutProcessor::kAuxValue);
        REQUIRE(aux_r[i] == MultiOutProcessor::kAuxValue);
    }
    for (uint32_t i = kPreparedMax; i < kRenderFrames; ++i) {
        REQUIRE(aux_l[i] == 0.0f);
        REQUIRE(aux_r[i] == 0.0f);
    }
}

TEST_CASE("CLAP leaves a secondary output bus silent while bypassed",
          "[clap][audio][multi-out][bypass]") {
    // When the plugin-declared Bypass is engaged the adapter skips the
    // Processor entirely. The aux bus is pre-zeroed and never written, so it
    // reads silence (no stale/uninitialised aux audio leaks while bypassed).
    g_multi_out = nullptr;
    g_pending_multi_out_bypass = true;
    Harness h(make_multi_out);
    REQUIRE(g_multi_out != nullptr);
    REQUIRE(h.plugin.bypass_param_id == MultiOutProcessor::kBypassParamId);
    h.plugin.store.set_value(h.plugin.bypass_param_id, 1.0f);

    std::vector<float> main_l(Harness::kFrames, 99.0f);
    std::vector<float> main_r(Harness::kFrames, 99.0f);
    std::vector<float> aux_l(Harness::kFrames, 99.0f);
    std::vector<float> aux_r(Harness::kFrames, 99.0f);
    float* main_ptrs[2] = {main_l.data(), main_r.data()};
    float* aux_ptrs[2] = {aux_l.data(), aux_r.data()};
    clap_audio_buffer_t main_out{};
    main_out.data32 = main_ptrs;
    main_out.channel_count = 2;
    clap_audio_buffer_t aux_out{};
    aux_out.data32 = aux_ptrs;
    aux_out.channel_count = 2;

    clap_audio_buffer_t outputs[2] = {main_out, aux_out};
    REQUIRE(h.run_custom(nullptr, nullptr,
                         nullptr, 0,
                         outputs, 2) != CLAP_PROCESS_ERROR);

    REQUIRE(g_multi_out->process_buffer_calls == 0);  // Processor skipped
    REQUIRE(std::all_of(aux_l.begin(), aux_l.end(), [](float v) { return v == 0.0f; }));
    REQUIRE(std::all_of(aux_r.begin(), aux_r.end(), [](float v) { return v == 0.0f; }));
}

TEST_CASE("CLAP silences oversized host topology without OOB",
          "[clap][audio][multi-out][rt-safety][malformed-layout]") {
    // A host that presents more output buses than kMaxOutputBuses, and an aux
    // bus wider than kMaxChannels, must not index past the preallocated storage.
    // Surplus buses are zero-filled but not routed; surplus channels are zeroed.
    g_multi_out = nullptr;
    Harness h(make_multi_out);
    REQUIRE(g_multi_out != nullptr);

    constexpr int kHostBuses = pulp::format::clap_adapter::kMaxOutputBuses + 4;
    constexpr int kWideChannels = pulp::format::clap_adapter::kMaxChannels + 3;

    // Backing storage for each bus's channels (wide aux at bus 1).
    std::vector<std::vector<float>> storage;
    std::vector<std::vector<float*>> ptr_storage;
    std::vector<clap_audio_buffer_t> buses(kHostBuses);
    for (int b = 0; b < kHostBuses; ++b) {
        int chans = (b == 1) ? kWideChannels : 2;
        ptr_storage.emplace_back();
        for (int c = 0; c < chans; ++c) {
            storage.emplace_back(Harness::kFrames, 99.0f);
            ptr_storage.back().push_back(storage.back().data());
        }
        buses[b].data32 = ptr_storage.back().data();
        buses[b].channel_count = static_cast<uint32_t>(chans);
    }

    REQUIRE(h.run_custom(nullptr, nullptr,
                         nullptr, 0,
                         buses.data(), static_cast<uint32_t>(kHostBuses))
            != CLAP_PROCESS_ERROR);

    REQUIRE(g_multi_out->process_buffer_calls == 0);
    for (int b = 0; b < kHostBuses; ++b) {
        for (uint32_t c = 0; c < buses[b].channel_count; ++c) {
            const float* ch = buses[b].data32[c];
            REQUIRE(std::all_of(ch, ch + Harness::kFrames,
                                [](float v) { return v == 0.0f; }));
        }
    }
}

TEST_CASE("CLAP fails closed for null or partial sidechain storage",
          "[clap][audio][sidechain][malformed-layout]") {
    g_pending_opts_mpe = false;
    g_pending_opts_ump = false;
    g_pending_capturing_sidechain = true;
    Harness h(make_capturing);

    clap_audio_buffer_t inactive_sidechain{};
    inactive_sidechain.data32 = nullptr;
    inactive_sidechain.channel_count = 2;
    clap_audio_buffer_t inputs_with_inactive[2] = {h.audio_in, inactive_sidechain};
    REQUIRE(h.run_custom(nullptr, nullptr,
                         inputs_with_inactive, 2,
                         &h.audio_out, 1) == CLAP_PROCESS_CONTINUE);
    REQUIRE(g_capturing->process_count == 0);
    REQUIRE(std::all_of(h.out_left.begin(), h.out_left.end(), [](float v) { return v == 0.0f; }));
    REQUIRE(std::all_of(h.out_right.begin(), h.out_right.end(), [](float v) { return v == 0.0f; }));

    std::vector<float> sc_left(Harness::kFrames, 0.5f);
    float* partial_ptrs[2] = {sc_left.data(), nullptr};
    clap_audio_buffer_t partial_sidechain{};
    partial_sidechain.data32 = partial_ptrs;
    partial_sidechain.channel_count = 2;
    clap_audio_buffer_t inputs_with_partial[2] = {h.audio_in, partial_sidechain};
    REQUIRE(h.run_custom(nullptr, nullptr,
                         inputs_with_partial, 2,
                         &h.audio_out, 1) == CLAP_PROCESS_CONTINUE);
    REQUIRE(g_capturing->process_count == 0);
}

TEST_CASE("CLAP transport state maps into ProcessContext",
          "[clap][transport]") {
    g_pending_opts_mpe = false;
    g_pending_opts_ump = false;
    Harness h(make_capturing);

    clap_event_transport_t transport{};
    transport.header = make_header(sizeof(transport), CLAP_EVENT_TRANSPORT, 0);
    // Item 1.3 — adapter now gates `tempo_bpm` on CLAP_TRANSPORT_HAS_TEMPO
    // per the CLAP spec contract (transport.tempo is only valid when the
    // HAS_TEMPO flag is set; pre-1.3 code read it unconditionally).
    transport.flags = CLAP_TRANSPORT_IS_PLAYING
                    | CLAP_TRANSPORT_HAS_TEMPO
                    | CLAP_TRANSPORT_HAS_BEATS_TIMELINE
                    | CLAP_TRANSPORT_HAS_SECONDS_TIMELINE
                    | CLAP_TRANSPORT_HAS_TIME_SIGNATURE;
    transport.tempo = 132.25;
    transport.song_pos_beats = 25 * (CLAP_BEATTIME_FACTOR / 2);
    transport.song_pos_seconds = static_cast<clap_sectime>(
        2.25 * static_cast<double>(CLAP_SECTIME_FACTOR));
    transport.tsig_num = 7;
    transport.tsig_denom = 8;

    InputEventList events;
    REQUIRE(h.run_custom(&events, nullptr,
                         &h.audio_in, 1,
                         &h.audio_out, 1,
                         Harness::kFrames,
                         &transport) == CLAP_PROCESS_CONTINUE);

    REQUIRE(g_capturing->captured_context.is_playing);
    REQUIRE(g_capturing->captured_context.process_mode == ProcessMode::Realtime);
    REQUIRE(g_capturing->captured_context.render_speed_hint ==
            RenderSpeedHint::Realtime);
    REQUIRE_FALSE(g_capturing->captured_context.is_offline());
    REQUIRE_FALSE(g_capturing->captured_context.allows_offline_quality_work());
    REQUIRE_FALSE(g_capturing->captured_context.is_maintenance_render());
    REQUIRE(g_capturing->captured_context.tempo_bpm == 132.25);
    REQUIRE(g_capturing->captured_context.position_beats == 12.5);
    REQUIRE(g_capturing->captured_context.position_samples == 108000);
    REQUIRE(g_capturing->captured_context.time_sig_numerator == 7);
    REQUIRE(g_capturing->captured_context.time_sig_denominator == 8);
    REQUIRE(g_capturing->captured_context.num_samples == static_cast<int>(Harness::kFrames));
    REQUIRE(g_capturing->captured_context.has_transport(TransportField::Playing));
    REQUIRE(g_capturing->captured_context.has_transport(TransportField::Recording));
    REQUIRE(g_capturing->captured_context.has_transport(TransportField::Looping));
    REQUIRE(g_capturing->captured_context.has_transport(TransportField::Tempo));
    REQUIRE(g_capturing->captured_context.has_transport(TransportField::BeatPosition));
    REQUIRE(g_capturing->captured_context.has_transport(TransportField::SamplePosition));
    REQUIRE(g_capturing->captured_context.has_transport(TransportField::TimeSignature));
    REQUIRE(g_capturing->captured_context.has_transport(TransportField::Bar));
    REQUIRE_FALSE(g_capturing->captured_context.has_transport(TransportField::LoopRange));
    REQUIRE_FALSE(g_capturing->captured_context.has_transport(TransportField::HostTime));
    REQUIRE_FALSE(g_capturing->captured_context.has_transport(TransportField::FrameRate));
}

TEST_CASE("CLAP transport without seconds timeline leaves sample position unset",
          "[clap][transport]") {
    g_pending_opts_mpe = false;
    g_pending_opts_ump = false;
    Harness h(make_capturing);

    clap_event_transport_t transport{};
    transport.header = make_header(sizeof(transport), CLAP_EVENT_TRANSPORT, 0);
    transport.flags = CLAP_TRANSPORT_IS_PLAYING
                    | CLAP_TRANSPORT_HAS_TEMPO
                    | CLAP_TRANSPORT_HAS_BEATS_TIMELINE;
    transport.tempo = 120.0;
    transport.song_pos_beats = 16 * CLAP_BEATTIME_FACTOR;

    REQUIRE(h.run_custom(nullptr, nullptr,
                         &h.audio_in, 1,
                         &h.audio_out, 1,
                         Harness::kFrames,
                         &transport) == CLAP_PROCESS_CONTINUE);

    REQUIRE(g_capturing->captured_context.is_playing);
    REQUIRE(g_capturing->captured_context.tempo_bpm == 120.0);
    REQUIRE(g_capturing->captured_context.position_beats == 16.0);
    REQUIRE(g_capturing->captured_context.position_samples == 0);
    REQUIRE(g_capturing->captured_context.has_transport(TransportField::BeatPosition));
    REQUIRE_FALSE(g_capturing->captured_context.has_transport(
        TransportField::SamplePosition));
}

TEST_CASE("CLAP transport preserves valid zero positions",
          "[clap][transport][validity]") {
    g_pending_opts_mpe = false;
    g_pending_opts_ump = false;
    Harness h(make_capturing);

    clap_event_transport_t transport{};
    transport.header = make_header(sizeof(transport), CLAP_EVENT_TRANSPORT, 0);
    transport.flags = CLAP_TRANSPORT_HAS_TEMPO
                    | CLAP_TRANSPORT_HAS_BEATS_TIMELINE
                    | CLAP_TRANSPORT_HAS_SECONDS_TIMELINE
                    | CLAP_TRANSPORT_HAS_TIME_SIGNATURE;
    transport.tempo = 120.0;
    transport.tsig_num = 4;
    transport.tsig_denom = 4;

    REQUIRE(h.run_custom(nullptr, nullptr,
                         &h.audio_in, 1,
                         &h.audio_out, 1,
                         Harness::kFrames,
                         &transport) == CLAP_PROCESS_CONTINUE);

    const auto& ctx = g_capturing->captured_context;
    REQUIRE(ctx.position_beats == 0.0);
    REQUIRE(ctx.position_samples == 0);
    REQUIRE(ctx.bar == 0);
    REQUIRE(ctx.has_transport(TransportField::BeatPosition));
    REQUIRE(ctx.has_transport(TransportField::SamplePosition));
    REQUIRE(ctx.has_transport(TransportField::Bar));
    REQUIRE_FALSE(ctx.transport_jump);
}

TEST_CASE("CLAP null transport reports validity loss without a jump",
          "[clap][transport][validity]") {
    g_pending_opts_mpe = false;
    g_pending_opts_ump = false;
    Harness h(make_capturing);

    clap_event_transport_t transport{};
    transport.header = make_header(sizeof(transport), CLAP_EVENT_TRANSPORT, 0);
    transport.flags = CLAP_TRANSPORT_IS_PLAYING
                    | CLAP_TRANSPORT_HAS_TEMPO
                    | CLAP_TRANSPORT_HAS_SECONDS_TIMELINE;
    transport.tempo = 120.0;
    transport.song_pos_seconds = CLAP_SECTIME_FACTOR;

    REQUIRE(h.run_custom(nullptr, nullptr,
                         &h.audio_in, 1,
                         &h.audio_out, 1,
                         Harness::kFrames,
                         &transport) == CLAP_PROCESS_CONTINUE);
    REQUIRE_FALSE(g_capturing->captured_context.transport_validity.empty());

    REQUIRE(h.run_custom(nullptr, nullptr,
                         &h.audio_in, 1,
                         &h.audio_out, 1,
                         Harness::kFrames,
                         nullptr) == CLAP_PROCESS_CONTINUE);

    const auto& ctx = g_capturing->captured_context;
    REQUIRE(ctx.transport_validity.empty());
    REQUIRE(ctx.tempo_changed);
    REQUIRE(ctx.transport_changed);
    REQUIRE_FALSE(ctx.transport_jump);
}

TEST_CASE("CLAP position source switching does not synthesize a jump",
          "[clap][transport][validity]") {
    g_pending_opts_mpe = false;
    g_pending_opts_ump = false;
    Harness h(make_capturing);

    clap_event_transport_t transport{};
    transport.header = make_header(sizeof(transport), CLAP_EVENT_TRANSPORT, 0);
    transport.flags = CLAP_TRANSPORT_HAS_SECONDS_TIMELINE;
    transport.song_pos_seconds = CLAP_SECTIME_FACTOR;
    REQUIRE(h.run_custom(nullptr, nullptr,
                         &h.audio_in, 1,
                         &h.audio_out, 1,
                         Harness::kFrames,
                         &transport) == CLAP_PROCESS_CONTINUE);

    transport.flags = CLAP_TRANSPORT_HAS_BEATS_TIMELINE;
    transport.song_pos_beats = 8 * CLAP_BEATTIME_FACTOR;
    REQUIRE(h.run_custom(nullptr, nullptr,
                         &h.audio_in, 1,
                         &h.audio_out, 1,
                         Harness::kFrames,
                         &transport) == CLAP_PROCESS_CONTINUE);
    REQUIRE_FALSE(g_capturing->captured_context.transport_jump);
}

TEST_CASE("CLAP transport jumps request processor reset through ProcessContext",
          "[clap][transport][reset]") {
    g_pending_opts_mpe = false;
    g_pending_opts_ump = false;
    Harness h(make_capturing);

    clap_event_transport_t transport{};
    transport.header = make_header(sizeof(transport), CLAP_EVENT_TRANSPORT, 0);
    transport.flags = CLAP_TRANSPORT_IS_PLAYING
                    | CLAP_TRANSPORT_HAS_TEMPO
                    | CLAP_TRANSPORT_HAS_BEATS_TIMELINE;
    transport.tempo = 87.890625;  // 64 frames at 48 kHz advances exactly 1/512 beat.

    auto run_at_beats = [&](clap_beattime beat_position) {
        transport.song_pos_beats = beat_position;
        REQUIRE(h.run_custom(nullptr, nullptr,
                             &h.audio_in, 1,
                             &h.audio_out, 1,
                             Harness::kFrames,
                             &transport) == CLAP_PROCESS_CONTINUE);
        return g_capturing->captured_context;
    };

    const auto first = run_at_beats(8 * CLAP_BEATTIME_FACTOR);
    REQUIRE_FALSE(first.transport_jump);
    REQUIRE_FALSE(first.should_reset_dsp_state());

    const auto continuous =
        run_at_beats(8 * CLAP_BEATTIME_FACTOR + CLAP_BEATTIME_FACTOR / 512);
    REQUIRE_FALSE(continuous.transport_jump);
    REQUIRE_FALSE(continuous.should_reset_dsp_state());

    const auto jumped = run_at_beats(10 * CLAP_BEATTIME_FACTOR);
    REQUIRE(jumped.transport_jump);
    REQUIRE(jumped.should_reset_dsp_state());
}

TEST_CASE("CLAP item-1.3 transport extensions land on ProcessContext",
          "[clap][transport][item-13]") {
    g_pending_opts_mpe = false;
    g_pending_opts_ump = false;
    Harness h(make_capturing);

    clap_event_transport_t transport{};
    transport.header = make_header(sizeof(transport), CLAP_EVENT_TRANSPORT, 0);
    transport.flags = CLAP_TRANSPORT_IS_PLAYING
                    | CLAP_TRANSPORT_IS_RECORDING
                    | CLAP_TRANSPORT_IS_LOOP_ACTIVE
                    | CLAP_TRANSPORT_HAS_TEMPO
                    | CLAP_TRANSPORT_HAS_BEATS_TIMELINE
                    | CLAP_TRANSPORT_HAS_TIME_SIGNATURE;
    transport.tempo = 100.0;
    transport.song_pos_beats = 8 * CLAP_BEATTIME_FACTOR;       // beat 8
    transport.loop_start_beats = 4 * CLAP_BEATTIME_FACTOR;     // beat 4
    transport.loop_end_beats = 12 * CLAP_BEATTIME_FACTOR;      // beat 12
    transport.bar_number = 2;
    transport.tsig_num = 4;
    transport.tsig_denom = 4;

    REQUIRE(h.run_custom(nullptr, nullptr,
                         &h.audio_in, 1,
                         &h.audio_out, 1,
                         Harness::kFrames,
                         &transport) == CLAP_PROCESS_CONTINUE);

    const auto& ctx = g_capturing->captured_context;
    REQUIRE(ctx.is_playing);
    REQUIRE(ctx.is_recording);
    REQUIRE(ctx.is_looping);
    REQUIRE(ctx.loop_start_beats == 4.0);
    REQUIRE(ctx.loop_end_beats == 12.0);
    REQUIRE(ctx.bar == 2);
    // CLAP carries neither host_time_ns nor frame_rate — the adapter
    // leaves the documented sentinels alone.
    REQUIRE(ctx.host_time_ns == 0);
    REQUIRE(ctx.frame_rate == pulp::format::FrameRate::unknown);
    // First block raises no change flags (no previous-block snapshot).
    REQUIRE_FALSE(ctx.tempo_changed);
    REQUIRE_FALSE(ctx.time_sig_changed);
    REQUIRE_FALSE(ctx.transport_changed);
}

// Regression coverage: the CLAP adapter must reset `self->playhead_prev` across
// lifecycle transitions, so the first
// block after a deactivate / reactivate (or reset) diff'd against stale
// transport data from the previous activation and spuriously raised
// tempo_changed / time_sig_changed / transport_changed.
TEST_CASE("CLAP playhead snapshot is reset on activate / deactivate / reset",
          "[clap][transport][item-13][issue-2963]") {
    g_pending_opts_mpe = false;
    g_pending_opts_ump = false;
    Harness h(make_capturing);

    // Block 1 — establish a baseline tempo/time-sig/playing snapshot.
    clap_event_transport_t baseline{};
    baseline.header = make_header(sizeof(baseline), CLAP_EVENT_TRANSPORT, 0);
    baseline.flags = CLAP_TRANSPORT_IS_PLAYING
                   | CLAP_TRANSPORT_HAS_TEMPO
                   | CLAP_TRANSPORT_HAS_TIME_SIGNATURE;
    baseline.tempo = 140.0;
    baseline.tsig_num = 7;
    baseline.tsig_denom = 8;
    REQUIRE(h.run_custom(nullptr, nullptr,
                         &h.audio_in, 1, &h.audio_out, 1,
                         Harness::kFrames, &baseline) == CLAP_PROCESS_CONTINUE);
    // First block must not raise change flags.
    REQUIRE_FALSE(g_capturing->captured_context.tempo_changed);
    REQUIRE_FALSE(g_capturing->captured_context.time_sig_changed);
    REQUIRE_FALSE(g_capturing->captured_context.transport_changed);

    // Block 2 — same transport values → no change flags.
    REQUIRE(h.run_custom(nullptr, nullptr,
                         &h.audio_in, 1, &h.audio_out, 1,
                         Harness::kFrames, &baseline) == CLAP_PROCESS_CONTINUE);
    REQUIRE_FALSE(g_capturing->captured_context.tempo_changed);
    REQUIRE_FALSE(g_capturing->captured_context.time_sig_changed);
    REQUIRE_FALSE(g_capturing->captured_context.transport_changed);

    // ── Path A: clap_reset() must wipe the snapshot ─────────────────
    clap_adapter::clap_reset(&h.plugin.plugin);
    REQUIRE(h.run_custom(nullptr, nullptr,
                         &h.audio_in, 1, &h.audio_out, 1,
                         Harness::kFrames, &baseline) == CLAP_PROCESS_CONTINUE);
    // Same transport values still — but the snapshot was wiped, so
    // this block is treated as the FIRST block again → no flags.
    REQUIRE_FALSE(g_capturing->captured_context.tempo_changed);
    REQUIRE_FALSE(g_capturing->captured_context.time_sig_changed);
    REQUIRE_FALSE(g_capturing->captured_context.transport_changed);

    // ── Path B: deactivate + reactivate must wipe the snapshot ─────
    h.deactivate();
    REQUIRE(clap_adapter::clap_activate(&h.plugin.plugin, 48000.0, 32,
                                         Harness::kFrames));
    h.active = true;
    REQUIRE(h.run_custom(nullptr, nullptr,
                         &h.audio_in, 1, &h.audio_out, 1,
                         Harness::kFrames, &baseline) == CLAP_PROCESS_CONTINUE);
    // The bug would have compared against the still-stored snapshot
    // from before deactivate. With the fix, the new activation starts
    // with a fresh snapshot, so the first block raises no flags.
    REQUIRE_FALSE(g_capturing->captured_context.tempo_changed);
    REQUIRE_FALSE(g_capturing->captured_context.time_sig_changed);
    REQUIRE_FALSE(g_capturing->captured_context.transport_changed);
}

TEST_CASE("CLAP item-1.3 change flags flip on the second block",
          "[clap][transport][item-13][change-flags]") {
    g_pending_opts_mpe = false;
    g_pending_opts_ump = false;
    Harness h(make_capturing);

    clap_event_transport_t initial{};
    initial.header = make_header(sizeof(initial), CLAP_EVENT_TRANSPORT, 0);
    initial.flags = CLAP_TRANSPORT_HAS_TEMPO | CLAP_TRANSPORT_HAS_TIME_SIGNATURE;
    initial.tempo = 100.0;
    initial.tsig_num = 4;
    initial.tsig_denom = 4;

    REQUIRE(h.run_custom(nullptr, nullptr,
                         &h.audio_in, 1,
                         &h.audio_out, 1,
                         Harness::kFrames, &initial) == CLAP_PROCESS_CONTINUE);

    // First block — no previous snapshot — never raises change flags.
    REQUIRE_FALSE(g_capturing->captured_context.tempo_changed);

    clap_event_transport_t bumped{};
    bumped.header = make_header(sizeof(bumped), CLAP_EVENT_TRANSPORT, 0);
    bumped.flags = CLAP_TRANSPORT_IS_PLAYING
                 | CLAP_TRANSPORT_HAS_TEMPO
                 | CLAP_TRANSPORT_HAS_TIME_SIGNATURE;
    bumped.tempo = 180.0;        // changed
    bumped.tsig_num = 7;         // changed
    bumped.tsig_denom = 8;       // changed

    REQUIRE(h.run_custom(nullptr, nullptr,
                         &h.audio_in, 1,
                         &h.audio_out, 1,
                         Harness::kFrames, &bumped) == CLAP_PROCESS_CONTINUE);

    const auto& ctx = g_capturing->captured_context;
    REQUIRE(ctx.tempo_changed);
    REQUIRE(ctx.time_sig_changed);
    REQUIRE(ctx.transport_changed);  // is_playing flipped false → true
}

TEST_CASE("CLAP emits parameter event when processor changes automatable state",
          "[clap][params][out-events]") {
    g_pending_opts_mpe = false;
    g_pending_opts_ump = false;
    Harness h(make_capturing);
    g_capturing->mutate_param_in_process = true;
    g_capturing->process_param_value = 0.625f;

    InputEventList events;
    OutputEventList out;
    REQUIRE(h.run(events, &out) == CLAP_PROCESS_CONTINUE);

    auto params = out.by_type<clap_event_param_value_t>(CLAP_EVENT_PARAM_VALUE);
    REQUIRE(params.size() == 1);
    REQUIRE(params[0]->header.time == 0);
    REQUIRE(params[0]->header.space_id == CLAP_CORE_EVENT_SPACE_ID);
    REQUIRE(params[0]->param_id == CapturingProcessor::kParamId);
    REQUIRE(params[0]->note_id == -1);
    REQUIRE(params[0]->port_index == -1);
    REQUIRE(params[0]->channel == -1);
    REQUIRE(params[0]->key == -1);
    REQUIRE(params[0]->value == 0.625);
}

TEST_CASE("CLAP interleaves sample-accurate output params with MIDI in ascending "
          "time order",
          "[clap][params][out-events][sample-accurate]") {
    // The processor pushes two output param events (time 0 and 16) and emits a
    // MIDI note-on at time 8 — BETWEEN them. CLAP requires out_events to be
    // globally non-decreasing in header.time, so the adapter's three-cursor
    // merge must produce exactly: param@0, midi@8, param@16.
    g_pending_opts_mpe = false;
    g_pending_opts_ump = false;
    Harness h(make_output_param);
    REQUIRE(g_output_param != nullptr);
    g_output_param->push_events = true;
    g_output_param->emit_midi_between = true;

    InputEventList events;
    OutputEventList out;
    REQUIRE(h.run(events, &out) == CLAP_PROCESS_CONTINUE);

    // Three events total: two params + one MIDI short.
    REQUIRE(out.size() == 3);

    // (a) Global ordering: header.time must be non-decreasing across ALL events.
    uint32_t prev_time = 0;
    for (std::size_t i = 0; i < out.size(); ++i) {
        REQUIRE(out.at(i)->time >= prev_time);
        prev_time = out.at(i)->time;
    }

    // (b) Exact interleave: param@0, midi@8, param@16.
    REQUIRE(out.at(0)->type == CLAP_EVENT_PARAM_VALUE);
    REQUIRE(out.at(0)->time == 0);
    REQUIRE(out.at(1)->type == CLAP_EVENT_MIDI);
    REQUIRE(out.at(1)->time == 8);
    REQUIRE(out.at(2)->type == CLAP_EVENT_PARAM_VALUE);
    REQUIRE(out.at(2)->time == 16);

    // (c) The param events carry the right PLAIN-domain values at the right times.
    auto params = out.by_type<clap_event_param_value_t>(CLAP_EVENT_PARAM_VALUE);
    REQUIRE(params.size() == 2);
    REQUIRE(params[0]->param_id == OutputParamProcessor::kParamId);
    REQUIRE(params[0]->header.time == 0);
    REQUIRE(params[0]->value == 0.25);   // CLAP param values are plain domain
    REQUIRE(params[1]->param_id == OutputParamProcessor::kParamId);
    REQUIRE(params[1]->header.time == 16);
    REQUIRE(params[1]->value == 0.75);

    auto midis = out.by_type<clap_event_midi_t>(CLAP_EVENT_MIDI);
    REQUIRE(midis.size() == 1);
    REQUIRE(midis[0]->header.time == 8);
}

TEST_CASE("CLAP store-only param change still emits a single event at time 0",
          "[clap][params][out-events][regression]") {
    // Regression for the snapshot-diff fallback: a plugin that changes a param
    // through the store WITHOUT pushing an explicit output event must still be
    // reported once, at time 0.
    g_pending_opts_mpe = false;
    g_pending_opts_ump = false;
    Harness h(make_output_param);
    REQUIRE(g_output_param != nullptr);
    g_output_param->mutate_store_only = true;
    g_output_param->store_only_value = 0.625f;

    InputEventList events;
    OutputEventList out;
    REQUIRE(h.run(events, &out) == CLAP_PROCESS_CONTINUE);

    auto params = out.by_type<clap_event_param_value_t>(CLAP_EVENT_PARAM_VALUE);
    REQUIRE(params.size() == 1);
    REQUIRE(params[0]->header.time == 0);
    REQUIRE(params[0]->param_id == OutputParamProcessor::kParamId);
    REQUIRE(params[0]->value == 0.625);
}

TEST_CASE("CLAP preset-load extension is exposed only for supported ids",
          "[clap][preset]") {
    g_pending_opts_mpe = false;
    g_pending_opts_ump = false;
    Harness h(make_capturing);

    auto* preset = static_cast<const clap_plugin_preset_load_t*>(
        clap_adapter::clap_get_extension(&h.plugin.plugin, CLAP_EXT_PRESET_LOAD));
    REQUIRE(preset != nullptr);
    REQUIRE(clap_adapter::clap_get_extension(
        &h.plugin.plugin, CLAP_EXT_PRESET_LOAD_COMPAT) == preset);
    REQUIRE(clap_adapter::clap_get_extension(&h.plugin.plugin, "pulp.unknown") == nullptr);

    REQUIRE_FALSE(preset->from_location(&h.plugin.plugin, 0, nullptr, nullptr));
    REQUIRE_FALSE(preset->from_location(&h.plugin.plugin, 42, "/tmp/pulp-missing.preset", nullptr));
    REQUIRE_FALSE(preset->from_location(&h.plugin.plugin, 0, "/tmp/pulp-missing.preset", nullptr));
}

// ── Inbound: CLAP_EVENT_MIDI ────────────────────────────────────────────

TEST_CASE("CLAP param automation preserves all sample offsets while dual-writing StateStore",
          "[clap][params][rate-model]") {
    g_pending_opts_mpe = false;
    g_pending_opts_ump = false;
    Harness h(make_capturing);
    REQUIRE(g_capturing != nullptr);

    InputEventList events;
    clap_event_param_value_t p0{};
    p0.header = make_header(sizeof(p0), CLAP_EVENT_PARAM_VALUE, 0);
    p0.param_id = CapturingProcessor::kParamId;
    p0.value = 0.25;
    events.push(p0);

    clap_event_param_value_t p1{};
    p1.header = make_header(sizeof(p1), CLAP_EVENT_PARAM_VALUE, 16);
    p1.param_id = CapturingProcessor::kParamId;
    p1.value = 0.50;
    events.push(p1);

    clap_event_param_value_t p2{};
    p2.header = make_header(sizeof(p2), CLAP_EVENT_PARAM_VALUE, 48);
    p2.param_id = CapturingProcessor::kParamId;
    p2.value = 0.75;
    events.push(p2);

    REQUIRE(h.run(events) == CLAP_PROCESS_CONTINUE);

    REQUIRE(g_capturing->had_param_events);
    REQUIRE(g_capturing->captured_param_events.size() == 3);
    REQUIRE(g_capturing->captured_param_events[0].param_id == CapturingProcessor::kParamId);
    REQUIRE(g_capturing->captured_param_events[0].sample_offset == 0);
    REQUIRE(g_capturing->captured_param_events[0].value == 0.25f);
    REQUIRE(g_capturing->captured_param_events[1].sample_offset == 16);
    REQUIRE(g_capturing->captured_param_events[1].value == 0.50f);
    REQUIRE(g_capturing->captured_param_events[2].sample_offset == 48);
    REQUIRE(g_capturing->captured_param_events[2].value == 0.75f);

    const auto& param_events = h.plugin.param_events.events();
    REQUIRE(param_events.size() == 3);
    REQUIRE(param_events[0].param_id == CapturingProcessor::kParamId);
    REQUIRE(param_events[0].sample_offset == 0);
    REQUIRE(param_events[0].value == 0.25f);
    REQUIRE(param_events[1].sample_offset == 16);
    REQUIRE(param_events[1].value == 0.50f);
    REQUIRE(param_events[2].sample_offset == 48);
    REQUIRE(param_events[2].value == 0.75f);
    REQUIRE(g_capturing->state().get_value(CapturingProcessor::kParamId) == 0.75f);
}

TEST_CASE("CLAP parameter automation drops past realtime event capacity without growing",
          "[clap][params][realtime][capacity]") {
    static constexpr uint32_t kLargeBlockFrames = 4096;

    Harness h(make_observing_param_ingress, kLargeBlockFrames);
    REQUIRE(g_observing_param_ingress != nullptr);

    InputEventList events;
    for (std::size_t i = 0; i < state::ParameterEventQueue::kCapacity + 1; ++i) {
        clap_event_param_value_t ev{};
        ev.header = make_header(
            sizeof(ev), CLAP_EVENT_PARAM_VALUE, static_cast<uint32_t>(i));
        ev.param_id = ObservingParamIngressProcessor::kParamId;
        ev.value = (i == 0) ? 0.25 : 0.5;
        if (i == state::ParameterEventQueue::kCapacity) ev.value = 1.0;
        events.push(ev);
    }

    REQUIRE(h.run_custom(&events, nullptr,
                         &h.audio_in, 1,
                         &h.audio_out, 1,
                         kLargeBlockFrames) == CLAP_PROCESS_CONTINUE);

    REQUIRE(g_observing_param_ingress->observed_num_samples
            == static_cast<int>(kLargeBlockFrames));
    REQUIRE(g_observing_param_ingress->observed_param_events_attached);
    REQUIRE(g_observing_param_ingress->observed_param_event_count
            == state::ParameterEventQueue::kCapacity);
    REQUIRE(g_observing_param_ingress->observed_param_event_capacity
            == state::ParameterEventQueue::kCapacity);
    REQUIRE(g_observing_param_ingress->observed_param_event_overflowed);
    REQUIRE(g_observing_param_ingress->observed_param_event_drops == 1);
    REQUIRE(g_observing_param_ingress->observed_first_sample_offset == 0);
    REQUIRE(g_observing_param_ingress->observed_first_value == 0.25f);
    REQUIRE(g_observing_param_ingress->observed_last_sample_offset
            == static_cast<int32_t>(state::ParameterEventQueue::kCapacity - 1));
    REQUIRE(g_observing_param_ingress->observed_last_value == 0.5f);
    REQUIRE(g_observing_param_ingress->observed_state_value == 1.0f);

    REQUIRE(h.plugin.param_events.size() == state::ParameterEventQueue::kCapacity);
    REQUIRE(h.plugin.param_events.capacity() == state::ParameterEventQueue::kCapacity);
    REQUIRE(h.plugin.param_events.overflowed());
    REQUIRE(h.plugin.param_events.dropped_event_count() == 1);
}

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

TEST_CASE("CLAP note on and note off events decode into MidiBuffer",
          "[clap][midi][notes]") {
    g_pending_opts_mpe = false;
    g_pending_opts_ump = false;
    Harness h(make_capturing);

    InputEventList events;
    clap_event_note_t on{};
    on.header = make_header(sizeof(on), CLAP_EVENT_NOTE_ON, 2);
    on.note_id = -1;
    on.port_index = 0;
    on.channel = 6;
    on.key = 67;
    on.velocity = 0.75;
    events.push(on);

    clap_event_note_t off{};
    off.header = make_header(sizeof(off), CLAP_EVENT_NOTE_OFF, 18);
    off.note_id = -1;
    off.port_index = 0;
    off.channel = 6;
    off.key = 67;
    off.velocity = 0.25;
    events.push(off);

    REQUIRE(h.run(events) == CLAP_PROCESS_CONTINUE);
    REQUIRE(g_capturing->captured_midi.size() == 2);
    REQUIRE(g_capturing->captured_midi[0].is_note_on());
    REQUIRE(g_capturing->captured_midi[0].channel() == 6);
    REQUIRE(g_capturing->captured_midi[0].note() == 67);
    REQUIRE(g_capturing->captured_midi[0].velocity() == 95);
    REQUIRE(g_capturing->captured_midi[0].sample_offset == 2);
    REQUIRE(g_capturing->captured_midi[1].is_note_off());
    REQUIRE(g_capturing->captured_midi[1].channel() == 6);
    REQUIRE(g_capturing->captured_midi[1].note() == 67);
    REQUIRE(g_capturing->captured_midi[1].velocity() == 31);
    REQUIRE(g_capturing->captured_midi[1].sample_offset == 18);
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

TEST_CASE("CLAP_EVENT_MIDI_SYSEX decodes into realtime-owned MidiBuffer sidecar",
          "[clap][midi][issue-pending]") {
    g_pending_opts_mpe = false;
    g_pending_opts_ump = false;
    Harness h(make_observing_midi_in);

    static const uint8_t kPayload[] = {0xF0, 0x7E, 0x7F, 0x06, 0x01, 0xF7};
    InputEventList events;
    clap_event_midi_sysex_t ev{};
    ev.header = make_header(sizeof(ev), CLAP_EVENT_MIDI_SYSEX, 21);
    ev.port_index = 0;
    ev.buffer = kPayload;
    ev.size = sizeof(kPayload);
    events.push(ev);

    REQUIRE(h.run(events) == CLAP_PROCESS_CONTINUE);
#if PULP_CLAP_PROCESS_RT_TRAP_TESTS
    clap_process_status status = CLAP_PROCESS_ERROR;
    {
        native_components::test::RtNoAllocScope guard;
        status = h.run(events);
    }
    REQUIRE(status == CLAP_PROCESS_CONTINUE);
#endif
    REQUIRE(g_observing_midi_in != nullptr);
    REQUIRE(g_observing_midi_in->observed_sysex_capacity == 128);
    REQUIRE(g_observing_midi_in->observed_sysex_payload_capacity >= sizeof(kPayload));
    REQUIRE(g_observing_midi_in->observed_sysex_count == 1);
    REQUIRE(g_observing_midi_in->observed_first_sysex_size == sizeof(kPayload));
    REQUIRE(g_observing_midi_in->observed_first_sysex_offset == 21);
    REQUIRE(g_observing_midi_in->observed_first_sysex_prefix[0] == 0xF0);
    REQUIRE(g_observing_midi_in->observed_first_sysex_prefix[1] == 0x7E);
    REQUIRE(g_observing_midi_in->observed_first_sysex_prefix[5] == 0xF7);
    REQUIRE(g_observing_midi_in->observed_sysex_drops == 0);
}

TEST_CASE("CLAP_EVENT_MIDI_SYSEX sidecar is cleared off-thread across reactivation",
          "[clap][midi][realtime][sysex]") {
    Harness h(make_observing_midi_in);

    static const uint8_t kPayload[] = {0xF0, 0x7E, 0x7F, 0x06, 0x01, 0xF7};
    InputEventList events;
    clap_event_midi_sysex_t ev{};
    ev.header = make_header(sizeof(ev), CLAP_EVENT_MIDI_SYSEX, 11);
    ev.port_index = 0;
    ev.buffer = kPayload;
    ev.size = sizeof(kPayload);
    events.push(ev);

    REQUIRE(h.run(events) == CLAP_PROCESS_CONTINUE);
    REQUIRE(h.plugin.midi_in.sysex_size() == 1);

    h.deactivate();
    REQUIRE(h.plugin.midi_in.sysex_size() == 0);
    REQUIRE(clap_adapter::clap_activate(&h.plugin.plugin, 48000.0, 32,
                                         Harness::kFrames));
    h.active = true;
    REQUIRE(h.plugin.midi_in.sysex_size() == 0);

    REQUIRE(h.run(events) == CLAP_PROCESS_CONTINUE);
    REQUIRE(g_observing_midi_in != nullptr);
    REQUIRE(g_observing_midi_in->observed_sysex_count == 1);
    REQUIRE(g_observing_midi_in->observed_sysex_drops == 0);
}

TEST_CASE("CLAP_EVENT_MIDI_SYSEX prepared capture does not drain realtime payload pool",
          "[clap][midi][realtime][sysex]") {
    Harness h(make_capturing);

    static const uint8_t kPayload[] = {0xF0, 0x7D, 0x44, 0xF7};
    const std::vector<uint8_t> expected(kPayload, kPayload + sizeof(kPayload));

    for (std::size_t i = 0; i < state::ParameterEventQueue::kCapacity + 2; ++i) {
        InputEventList events;
        clap_event_midi_sysex_t ev{};
        ev.header = make_header(sizeof(ev), CLAP_EVENT_MIDI_SYSEX,
                                static_cast<uint32_t>(i % Harness::kFrames));
        ev.port_index = 0;
        ev.buffer = kPayload;
        ev.size = sizeof(kPayload);
        events.push(ev);

        REQUIRE(h.run(events) == CLAP_PROCESS_CONTINUE);
        REQUIRE(g_capturing->captured_midi.sysex_size() == 1);
        REQUIRE(g_capturing->captured_midi.sysex()[0].data == expected);
        REQUIRE(h.plugin.midi_in.dropped_sysex_count() == 0);
    }
}

TEST_CASE("CLAP_EVENT_MIDI_SYSEX forwarding does not drain realtime payload pools",
          "[clap][midi][realtime][sysex]") {
    Harness h(make_forwarding_sysex);

    static const uint8_t kPayload[] = {0xF0, 0x7D, 0x55, 0xF7};
    for (std::size_t i = 0; i < state::ParameterEventQueue::kCapacity + 2; ++i) {
        InputEventList events;
        OutputEventList out;
        clap_event_midi_sysex_t ev{};
        ev.header = make_header(sizeof(ev), CLAP_EVENT_MIDI_SYSEX,
                                static_cast<uint32_t>(i % Harness::kFrames));
        ev.port_index = 0;
        ev.buffer = kPayload;
        ev.size = sizeof(kPayload);
        events.push(ev);

        REQUIRE(h.run(events, &out) == CLAP_PROCESS_CONTINUE);
        REQUIRE(g_forwarding_sysex != nullptr);
        REQUIRE(g_forwarding_sysex->observed_input_sysex_count == 1);
        REQUIRE(g_forwarding_sysex->observed_output_sysex_count == 1);
        REQUIRE(g_forwarding_sysex->observed_input_sysex_drops == 0);
        REQUIRE(g_forwarding_sysex->observed_output_sysex_drops == 0);

        auto sysexes = out.by_type<clap_event_midi_sysex_t>(CLAP_EVENT_MIDI_SYSEX);
        REQUIRE(sysexes.size() == 1);
        REQUIRE(sysexes[0]->size == sizeof(kPayload));
        REQUIRE(sysexes[0]->buffer[0] == 0xF0);
        REQUIRE(sysexes[0]->buffer[2] == 0x55);
        REQUIRE(sysexes[0]->buffer[3] == 0xF7);
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
    REQUIRE(g_capturing->captured_sysex_count == 0);
    REQUIRE(g_capturing->captured_midi.empty());
}

// ── Inbound: CLAP_EVENT_MIDI2 ───────────────────────────────────────────

#if defined(CLAP_VERSION_GE) && CLAP_VERSION_GE(1, 1, 0)
TEST_CASE("CLAP enables sidecars from node capabilities",
          "[clap][midi][node-abi]") {
    g_pending_opts_mpe = false;
    g_pending_opts_ump = false;
    g_pending_opts_node_mpe = true;
    g_pending_opts_node_ump = true;
    Harness h(make_capturing);

    InputEventList events;
    auto packet = midi::UmpPacket::note_on_2(/*group*/0, /*channel*/1,
                                              /*note*/61, /*vel16*/0x8000);
    clap_event_midi2_t midi2{};
    midi2.header = make_header(sizeof(midi2), CLAP_EVENT_MIDI2, 7);
    midi2.port_index = 0;
    midi2.data[0] = packet.words[0];
    midi2.data[1] = packet.words[1];
    events.push(midi2);

    clap_event_note_expression_t expr{};
    expr.header = make_header(sizeof(expr), CLAP_EVENT_NOTE_EXPRESSION, 9);
    expr.expression_id = CLAP_NOTE_EXPRESSION_PRESSURE;
    expr.note_id = -1;
    expr.port_index = 0;
    expr.channel = 2;
    expr.key = 72;
    expr.value = 0.75;
    events.push(expr);

    REQUIRE(h.run(events) == CLAP_PROCESS_CONTINUE);
    REQUIRE(g_capturing->had_mpe_input);
    REQUIRE(g_capturing->had_ump_input);
    REQUIRE_FALSE(g_capturing->captured_ump.empty());
}

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
    // Both must appear in the UMP sidecar after process() returns. An
    // earlier shape of this test asserted the synthesis was skipped, but
    // assuming synthesis could be skipped silently dropped real-world note
    // streams. This pins the corrected behavior.
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
    // (buggy) behavior produced size() == 1 here.
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

TEST_CASE("midi_out negative sample offsets clamp to start of CLAP block",
          "[clap][midi][outbound]") {
    g_pending_emit.clear();
    g_pending_sysex.clear();
    auto cc = midi::MidiEvent::cc(/*ch*/0, /*num*/1, /*val*/11);
    cc.sample_offset = -5;
    g_pending_emit = {cc};
    midi::MidiBuffer::SysexEvent se;
    se.data = {0xF0, 0x7D, 0x02, 0xF7};
    se.sample_offset = -1;
    g_pending_sysex = {se};

    Harness h(make_emitting);
    InputEventList in;
    OutputEventList out;
    REQUIRE(h.run(in, &out) == CLAP_PROCESS_CONTINUE);

    REQUIRE(out.size() == 2);
    REQUIRE(out.at(0)->time == 0);
    REQUIRE(out.at(0)->type == CLAP_EVENT_MIDI);
    REQUIRE(out.at(1)->time == 0);
    REQUIRE(out.at(1)->type == CLAP_EVENT_MIDI_SYSEX);
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

TEST_CASE("CLAP inbound MIDI drops past realtime event capacity without growing",
          "[clap][midi][realtime]") {
    Harness h(make_observing_midi_in);
    InputEventList in;
    for (std::size_t i = 0; i < state::ParameterEventQueue::kCapacity + 1; ++i) {
        clap_event_midi_t ev{};
        ev.header = make_header(sizeof(ev), CLAP_EVENT_MIDI,
                                static_cast<uint32_t>(i % Harness::kFrames));
        ev.port_index = 0;
        ev.data[0] = 0xB0;
        ev.data[1] = 7;
        ev.data[2] = static_cast<uint8_t>(i & 0x7F);
        in.push(ev);
    }

    REQUIRE(h.run(in) == CLAP_PROCESS_CONTINUE);
    REQUIRE(g_observing_midi_in != nullptr);
    REQUIRE(g_observing_midi_in->observed_event_capacity ==
            state::ParameterEventQueue::kCapacity);
    REQUIRE(g_observing_midi_in->observed_event_count ==
            state::ParameterEventQueue::kCapacity);
    REQUIRE(g_observing_midi_in->observed_event_drops == 1);
}

TEST_CASE("CLAP inbound SysEx drops past realtime sidecar capacity without growing",
          "[clap][midi][realtime][sysex]") {
    Harness h(make_observing_midi_in);
    InputEventList in;
    static const uint8_t kPayload[] = {0xF0, 0x7D, 0x01, 0xF7};
    for (std::size_t i = 0; i < 129; ++i) {
        clap_event_midi_sysex_t ev{};
        ev.header = make_header(sizeof(ev), CLAP_EVENT_MIDI_SYSEX,
                                static_cast<uint32_t>(i % Harness::kFrames));
        ev.port_index = 0;
        ev.buffer = kPayload;
        ev.size = sizeof(kPayload);
        in.push(ev);
    }

    REQUIRE(h.run(in) == CLAP_PROCESS_CONTINUE);
    REQUIRE(g_observing_midi_in != nullptr);
    REQUIRE(g_observing_midi_in->observed_sysex_capacity == 128);
    REQUIRE(g_observing_midi_in->observed_sysex_count == 128);
    REQUIRE(g_observing_midi_in->observed_sysex_drops == 1);
}

TEST_CASE("CLAP inbound SysEx drops payloads larger than realtime sidecar storage",
          "[clap][midi][realtime][sysex]") {
    Harness h(make_observing_midi_in);
    InputEventList in;
    std::vector<uint8_t> payload(4097, 0x7D);
    payload.front() = 0xF0;
    payload.back() = 0xF7;

    clap_event_midi_sysex_t ev{};
    ev.header = make_header(sizeof(ev), CLAP_EVENT_MIDI_SYSEX, 9);
    ev.port_index = 0;
    ev.buffer = payload.data();
    ev.size = payload.size();
    in.push(ev);

    REQUIRE(h.run(in) == CLAP_PROCESS_CONTINUE);
    REQUIRE(g_observing_midi_in != nullptr);
    REQUIRE(g_observing_midi_in->observed_sysex_capacity == 128);
    REQUIRE(g_observing_midi_in->observed_sysex_payload_capacity == 4096);
    REQUIRE(g_observing_midi_in->observed_sysex_count == 0);
    REQUIRE(g_observing_midi_in->observed_sysex_drops == 1);
}

TEST_CASE("CLAP outbound MIDI drops past realtime event capacity without growing",
          "[clap][midi][realtime]") {
    g_pending_overflow_sysex = true;
    Harness h(make_overflowing_midi_out);
    InputEventList in;
    OutputEventList out;

    REQUIRE(h.run(in, &out) == CLAP_PROCESS_CONTINUE);
    REQUIRE(g_overflowing_midi_out != nullptr);
    REQUIRE(g_overflowing_midi_out->observed_event_capacity ==
            state::ParameterEventQueue::kCapacity);
    REQUIRE(g_overflowing_midi_out->observed_event_count ==
            state::ParameterEventQueue::kCapacity);
    REQUIRE(g_overflowing_midi_out->observed_event_drops == 1);
    REQUIRE(g_overflowing_midi_out->observed_sysex_capacity == 128);
    REQUIRE(g_overflowing_midi_out->observed_sysex_count == 128);
    REQUIRE(g_overflowing_midi_out->observed_sysex_drops == 1);
}

TEST_CASE("CLAP MPE sidecar drops past realtime event capacity without growing",
          "[clap][midi][realtime]") {
    g_pending_opts_mpe = true;
    g_pending_opts_ump = false;
    Harness h(make_observing_sidecar);
    InputEventList in;

    for (std::size_t i = 0; i < 128; ++i) {
        clap_event_note_t ev{};
        ev.header = make_header(sizeof(ev), CLAP_EVENT_NOTE_ON,
                                static_cast<uint32_t>(i % Harness::kFrames));
        ev.note_id = -1;
        ev.port_index = 0;
        ev.channel = 1;
        ev.key = static_cast<int16_t>(i);
        ev.velocity = 1.0;
        in.push(ev);
    }

    for (std::size_t i = 0; i < 8; ++i) {
        clap_event_midi_t ev{};
        ev.header = make_header(sizeof(ev), CLAP_EVENT_MIDI,
                                static_cast<uint32_t>(i % Harness::kFrames));
        ev.port_index = 0;
        ev.data[0] = 0xD1;
        ev.data[1] = static_cast<uint8_t>(64 + i);
        ev.data[2] = 0;
        in.push(ev);
    }

    REQUIRE(h.run(in) == CLAP_PROCESS_CONTINUE);
    REQUIRE(g_observing_sidecar != nullptr);
    REQUIRE(g_observing_sidecar->observed_mpe_attached);
    REQUIRE(g_observing_sidecar->observed_mpe_capacity ==
            state::ParameterEventQueue::kCapacity);
    REQUIRE(g_observing_sidecar->observed_mpe_count ==
            state::ParameterEventQueue::kCapacity);
    REQUIRE(g_observing_sidecar->observed_mpe_drops == 128);
}

#if defined(CLAP_VERSION_GE) && CLAP_VERSION_GE(1, 1, 0)
TEST_CASE("CLAP UMP sidecar drops past realtime event capacity without growing",
          "[clap][midi][realtime]") {
    g_pending_opts_mpe = false;
    g_pending_opts_ump = true;
    Harness h(make_observing_sidecar);
    InputEventList in;

    for (std::size_t i = 0; i < state::ParameterEventQueue::kCapacity + 1; ++i) {
        const auto packet = midi::UmpPacket::note_on_2(
            /*group*/0, /*channel*/1,
            static_cast<uint8_t>(i & 0x7F), /*vel16*/0x8000);
        clap_event_midi2_t ev{};
        ev.header = make_header(sizeof(ev), CLAP_EVENT_MIDI2,
                                static_cast<uint32_t>(i % Harness::kFrames));
        ev.port_index = 0;
        ev.data[0] = packet.words[0];
        ev.data[1] = packet.words[1];
        ev.data[2] = packet.words[2];
        ev.data[3] = packet.words[3];
        in.push(ev);
    }

    REQUIRE(h.run(in) == CLAP_PROCESS_CONTINUE);
    REQUIRE(g_observing_sidecar != nullptr);
    REQUIRE(g_observing_sidecar->observed_ump_attached);
    REQUIRE(g_observing_sidecar->observed_ump_capacity ==
            state::ParameterEventQueue::kCapacity);
    REQUIRE(g_observing_sidecar->observed_ump_count ==
            state::ParameterEventQueue::kCapacity);
    REQUIRE(g_observing_sidecar->observed_ump_drops == 1);
}
#endif

// ─────────────────────────────────────────────────────────────────────
// host-quirks P3d — synthesize_bypass_parameter honored by the CLAP
// adapter end-to-end. CapturingProcessor declares no Bypass, so with the
// quirk enforced clap_init synthesizes one; engaging it makes clap_process
// pass main input → output verbatim and skip the Processor.
// ─────────────────────────────────────────────────────────────────────
TEST_CASE("CLAP synthesizes + honors a Bypass param when the plugin declares none",
          "[clap][host-quirks][p3][bypass]") {
    pulp::format::set_host_quirk_policy(pulp::format::QuirkFilter{});  // quirk on
    {
        Harness h(make_capturing);
        // clap_init synthesized the Bypass (reserved ID) and detected it.
        REQUIRE(h.plugin.bypass_param_id == pulp::format::kSynthesizedBypassParamId);

        for (uint32_t i = 0; i < Harness::kFrames; ++i) {
            h.in_left[i]  = 0.1f + static_cast<float>(i);
            h.in_right[i] = -(0.1f + static_cast<float>(i));
        }
        // Engage bypass + run with no events → pass-through, Processor skipped.
        h.plugin.store.set_value(h.plugin.bypass_param_id, 1.0f);
        InputEventList empty;
        REQUIRE(h.run(empty) != CLAP_PROCESS_ERROR);
        REQUIRE(h.out_left == h.in_left);
        REQUIRE(h.out_right == h.in_right);
    }
    pulp::format::set_host_quirk_policy(std::nullopt);
}

TEST_CASE("CLAP does NOT synthesize a Bypass param when the quirk is off",
          "[clap][host-quirks][p3][bypass]") {
    pulp::format::set_host_quirk_policy(pulp::format::kQuirkFilterOff);
    {
        Harness h(make_capturing);
        REQUIRE(h.plugin.bypass_param_id == 0u);  // no synthesis
    }
    pulp::format::set_host_quirk_policy(std::nullopt);
}

// A spec-violating host that renders MORE frames than the activated
// max_frames must not overrun the processor's prepared scratch. The adapter
// clamps the processed region to the prepared max and zeros the un-processable
// tail so it reads back as clean silence. (Un-fixed, this path overruns the
// prepared buffers and trips ASan.)
TEST_CASE("CLAP clamps an oversized render block and zeros the tail",
          "[clap][rt-safety][process]") {
    g_unity_copy = nullptr;

    constexpr uint32_t kPreparedMax = 64;
    constexpr uint32_t kRenderFrames = 256;  // host exceeds the advertised max

    // Activate (prepare) for the small max; the processor's scratch is sized
    // to kPreparedMax.
    Harness h(make_unity_copy, kPreparedMax);
    REQUIRE(g_unity_copy != nullptr);

    // Host-provided buffers are sized to the LARGER render count. Input is a
    // sentinel across the whole block; outputs are pre-filled with garbage so
    // a clean tail proves the adapter zeroed it.
    std::vector<float> in_l(kRenderFrames, 0.5f);
    std::vector<float> in_r(kRenderFrames, 0.5f);
    std::vector<float> out_l(kRenderFrames, -9.0f);
    std::vector<float> out_r(kRenderFrames, -9.0f);

    const float* in_ptrs[2] = {in_l.data(), in_r.data()};
    float* out_ptrs[2] = {out_l.data(), out_r.data()};

    clap_audio_buffer_t audio_in{};
    audio_in.data32 = const_cast<float**>(in_ptrs);
    audio_in.channel_count = 2;
    clap_audio_buffer_t audio_out{};
    audio_out.data32 = out_ptrs;
    audio_out.channel_count = 2;

    InputEventList empty;
    // (a) No crash / no overrun — the core guarantee (would trip ASan unfixed).
    REQUIRE(h.run_custom(&empty, nullptr,
                         &audio_in, 1, &audio_out, 1,
                         kRenderFrames, nullptr) != CLAP_PROCESS_ERROR);

    // (b) The processor saw only the prepared-max count.
    REQUIRE(g_unity_copy->process_count == 1);
    REQUIRE(g_unity_copy->observed_num_samples == static_cast<int>(kPreparedMax));

    // (c) The first kPreparedMax frames were processed (unity copy).
    for (uint32_t i = 0; i < kPreparedMax; ++i) {
        REQUIRE(out_l[i] == 0.5f);
        REQUIRE(out_r[i] == 0.5f);
    }
    // (d) The un-processable tail [kPreparedMax, kRenderFrames) is silence.
    for (uint32_t i = kPreparedMax; i < kRenderFrames; ++i) {
        REQUIRE(out_l[i] == 0.0f);
        REQUIRE(out_r[i] == 0.0f);
    }
}

// ─────────────────────────────────────────────────────────────────────
// Bypass pass-through must delay the dry signal by the plugin's reported
// latency. A latency-reporting plugin gets host plugin-delay-compensation
// on its path; the bypassed dry copy must be delayed by the same amount or
// it arrives `latency` samples early and comb-filters against parallel
// tracks. Drive a continuous ramp through the real clap_process() path with
// bypass engaged across several blocks, then assert steady-state
// output[n] == input[n - latency].
// ─────────────────────────────────────────────────────────────────────
TEST_CASE("CLAP bypass pass-through delays the dry signal by the reported latency",
          "[clap][bypass][latency][pdc]") {
    constexpr int kLatency = 16;
    g_pending_bypass_latency = kLatency;

    constexpr uint32_t kBlock = 32;
    Harness h(make_bypass_latency, kBlock);
    REQUIRE(h.plugin.bypass_param_id == BypassLatencyProcessor::kBypassId);

    // Engage bypass for the whole run.
    h.plugin.store.set_value(h.plugin.bypass_param_id, 1.0f);

    constexpr int kBlocks = 6;
    constexpr int kTotal = static_cast<int>(kBlock) * kBlocks;
    std::vector<float> source(kTotal);
    for (int n = 0; n < kTotal; ++n) source[n] = static_cast<float>(n + 1);

    std::vector<float> captured_l(kTotal, 0.0f);
    std::vector<float> captured_r(kTotal, 0.0f);

    for (int b = 0; b < kBlocks; ++b) {
        std::vector<float> in_l(kBlock), in_r(kBlock), out_l(kBlock, 99.0f),
            out_r(kBlock, 99.0f);
        for (uint32_t i = 0; i < kBlock; ++i) {
            in_l[i] = source[b * static_cast<int>(kBlock) + static_cast<int>(i)];
            in_r[i] = -in_l[i];
        }
        const float* in_ptrs[2] = {in_l.data(), in_r.data()};
        float* out_ptrs[2] = {out_l.data(), out_r.data()};
        clap_audio_buffer_t ab_in{};
        ab_in.data32 = const_cast<float**>(in_ptrs);
        ab_in.channel_count = 2;
        clap_audio_buffer_t ab_out{};
        ab_out.data32 = out_ptrs;
        ab_out.channel_count = 2;

        InputEventList empty;
        REQUIRE(h.run_custom(&empty, nullptr, &ab_in, 1, &ab_out, 1, kBlock,
                             nullptr) != CLAP_PROCESS_ERROR);
        for (uint32_t i = 0; i < kBlock; ++i) {
            captured_l[b * static_cast<int>(kBlock) + static_cast<int>(i)] = out_l[i];
            captured_r[b * static_cast<int>(kBlock) + static_cast<int>(i)] = out_r[i];
        }
    }

    // Steady state: output[n] == input[n - kLatency].
    for (int n = kLatency; n < kTotal; ++n) {
        REQUIRE(captured_l[n] == source[n - kLatency]);
        REQUIRE(captured_r[n] == -source[n - kLatency]);
    }
    // Warm-up: the first kLatency samples reflect the pre-bypass (silent)
    // delay-line contents, confirming a real delay rather than a no-op copy.
    for (int n = 0; n < kLatency; ++n) {
        REQUIRE(captured_l[n] == 0.0f);
    }
}

TEST_CASE("CLAP bypass pass-through is a zero-delay copy when latency is zero",
          "[clap][bypass][latency][pdc]") {
    // The bug only exists for latency > 0; a 0-latency plugin keeps the
    // original verbatim pass-through with no warm-up delay.
    g_pending_bypass_latency = 0;
    Harness h(make_bypass_latency);
    REQUIRE(h.plugin.bypass_param_id == BypassLatencyProcessor::kBypassId);

    h.plugin.store.set_value(h.plugin.bypass_param_id, 1.0f);
    for (uint32_t i = 0; i < Harness::kFrames; ++i) {
        h.in_left[i]  = 0.1f + static_cast<float>(i);
        h.in_right[i] = -(0.1f + static_cast<float>(i));
    }
    InputEventList empty;
    REQUIRE(h.run(empty) != CLAP_PROCESS_ERROR);
    REQUIRE(h.out_left == h.in_left);
    REQUIRE(h.out_right == h.in_right);
}

// ── Non-realtime tick: escaping the audio thread through request_callback ──

namespace {

// A processor whose control changes need work process() must never do (decode,
// resample, FFT-plan, allocate) and that has NO worker thread to do it on — every
// wasm build, i.e. WebCLAP. CLAP hands it parameter changes as events INSIDE
// process(), so the only way out is to raise non_realtime_tick_pending() and let
// the adapter ask the host for a main-thread callback.
struct ClapTickState {
    int ticks = 0;
    bool pending = false;
};
ClapTickState g_clap_tick;

class TickProcessor : public Processor {
public:
    PluginDescriptor descriptor() const override {
        PluginDescriptor d;
        d.name = "TickCLAP";
        d.manufacturer = "PulpTest";
        d.bundle_id = "com.pulp.test.clap.tick";
        d.version = "1.0.0";
        return d;
    }
    void define_parameters(state::StateStore&) override {}
    void prepare(const PrepareContext&) override {}
    void process(audio::BufferView<float>&,
                 const audio::BufferView<const float>&,
                 midi::MidiBuffer&,
                 midi::MidiBuffer&,
                 const ProcessContext&) override {}

    // RT-safe by contract: a plain read, no lock, no allocation.
    bool non_realtime_tick_pending() const override { return g_clap_tick.pending; }
    void on_non_realtime_tick() override {
        ++g_clap_tick.ticks;
        g_clap_tick.pending = false;   // the work asked for is now done
    }
};
std::unique_ptr<Processor> make_tick() { return std::make_unique<TickProcessor>(); }

int g_request_callback_count = 0;
void host_request_callback(const clap_host_t*) { ++g_request_callback_count; }
void host_request_restart(const clap_host_t*) {}
void host_request_process(const clap_host_t*) {}
const void* host_get_extension(const clap_host_t*, const char*) { return nullptr; }

clap_host_t make_fake_host() {
    clap_host_t host{};
    host.clap_version = CLAP_VERSION;
    host.name = "PulpTestHost";
    host.vendor = "PulpTest";
    host.url = "";
    host.version = "1.0.0";
    host.get_extension = host_get_extension;
    host.request_restart = host_request_restart;
    host.request_process = host_request_process;
    host.request_callback = host_request_callback;
    return host;
}

} // namespace

TEST_CASE("CLAP requests a main-thread callback for a pending non-realtime tick",
          "[clap][non-realtime-tick]") {
    g_clap_tick = {};
    g_request_callback_count = 0;

    Harness h(make_tick);
    clap_host_t host = make_fake_host();
    h.plugin.host = &host;

    InputEventList empty;

    // Nothing pending: the per-block peek costs one false and asks for nothing.
    REQUIRE(h.run(empty) != CLAP_PROCESS_ERROR);
    REQUIRE(g_request_callback_count == 0);
    REQUIRE(g_clap_tick.ticks == 0);

    // The processor discovers work (a Size event arrived in this block's event
    // list). process() must NOT do it — it must ask the host for the main thread.
    g_clap_tick.pending = true;
    REQUIRE(h.run(empty) != CLAP_PROCESS_ERROR);
    REQUIRE(g_request_callback_count == 1);
    REQUIRE(g_clap_tick.ticks == 0);   // never on the audio thread

    // The flag is PEEKED, not consumed: a host that renders another block before
    // servicing the callback must still see the request, not lose the edge.
    REQUIRE(h.run(empty) != CLAP_PROCESS_ERROR);
    REQUIRE(g_request_callback_count == 2);
    REQUIRE(g_clap_tick.ticks == 0);

    // The host calls back on the main thread. THAT is where the work runs.
    clap_adapter::clap_on_main_thread(&h.plugin.plugin);
    REQUIRE(g_clap_tick.ticks == 1);
    REQUIRE_FALSE(g_clap_tick.pending);

    // Serviced: the next block asks for nothing more.
    REQUIRE(h.run(empty) != CLAP_PROCESS_ERROR);
    REQUIRE(g_request_callback_count == 2);

    // An unsolicited main-thread callback (hosts batch them) is a no-op tick —
    // the default-false query means a processor that never opts in pays nothing.
    clap_adapter::clap_on_main_thread(&h.plugin.plugin);
    REQUIRE(g_clap_tick.ticks == 2);   // the hook fires unconditionally...
    REQUIRE(g_request_callback_count == 2);   // ...but nothing new was requested
}

namespace {

// A processor that flags a latency or tail change from process() when asked.
struct LatencyTailTrigger { bool latency = false; bool tail = false; };
LatencyTailTrigger g_lat_trigger;
int g_host_latency_changed = 0;
int g_host_tail_changed = 0;

class LatencyProcessor : public Processor {
public:
    PluginDescriptor descriptor() const override {
        PluginDescriptor d;
        d.name = "LatencyCLAP";
        d.manufacturer = "PulpTest";
        d.bundle_id = "com.pulp.test.clap.latency";
        d.version = "1.0.0";
        return d;
    }
    void define_parameters(state::StateStore&) override {}
    void prepare(const PrepareContext&) override {}
    void process(audio::BufferView<float>&,
                 const audio::BufferView<const float>&,
                 midi::MidiBuffer&,
                 midi::MidiBuffer&,
                 const ProcessContext&) override {
        if (g_lat_trigger.latency) { flag_latency_changed(); g_lat_trigger.latency = false; }
        if (g_lat_trigger.tail)    { flag_tail_changed();    g_lat_trigger.tail = false; }
    }
};
std::unique_ptr<Processor> make_latency() { return std::make_unique<LatencyProcessor>(); }

void host_latency_changed(const clap_host_t*) { ++g_host_latency_changed; }
void host_tail_changed(const clap_host_t*) { ++g_host_tail_changed; }
const clap_host_latency_t g_host_latency_ext{host_latency_changed};
const clap_host_tail_t g_host_tail_ext{host_tail_changed};
const void* host_get_extension_caps(const clap_host_t*, const char* id) {
    if (std::strcmp(id, CLAP_EXT_LATENCY) == 0) return &g_host_latency_ext;
    if (std::strcmp(id, CLAP_EXT_TAIL) == 0) return &g_host_tail_ext;
    return nullptr;
}

} // namespace

// A latency or tail change must reach the host as clap_host_latency->changed()
// / clap_host_tail->changed(), and only from the main thread (never from
// process()), exactly once per edge. A silent regression here misaligns host
// plugin-delay compensation — audibly smeared transients in any latency-
// compensated session, with no error surfaced. The prior CLAP fake host
// returned nullptr for every extension, so this delivery branch never ran.
TEST_CASE("CLAP delivers latency/tail changed() to the host on the main thread",
          "[clap][latency][tail][conformance]") {
    g_lat_trigger = {};
    g_host_latency_changed = 0;
    g_host_tail_changed = 0;
    g_request_callback_count = 0;

    Harness h(make_latency);
    clap_host_t host = make_fake_host();
    host.get_extension = host_get_extension_caps;  // capturing latency/tail exts
    h.plugin.host = &host;

    InputEventList empty;

    SECTION("latency change requests a callback then delivers changed() once") {
        g_lat_trigger.latency = true;
        REQUIRE(h.run(empty) != CLAP_PROCESS_ERROR);
        REQUIRE(g_request_callback_count == 1);  // asked for the main thread
        REQUIRE(g_host_latency_changed == 0);    // but not from process()
        clap_adapter::clap_on_main_thread(&h.plugin.plugin);
        REQUIRE(g_host_latency_changed == 1);    // delivered exactly once
        REQUIRE(g_host_tail_changed == 0);
    }

    SECTION("tail change delivers tail changed() once") {
        g_lat_trigger.tail = true;
        REQUIRE(h.run(empty) != CLAP_PROCESS_ERROR);
        REQUIRE(g_request_callback_count == 1);
        clap_adapter::clap_on_main_thread(&h.plugin.plugin);
        REQUIRE(g_host_tail_changed == 1);
        REQUIRE(g_host_latency_changed == 0);
    }
}
