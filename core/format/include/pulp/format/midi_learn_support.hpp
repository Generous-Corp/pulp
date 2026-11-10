#pragma once

/// @file midi_learn_support.hpp
/// Glue between `state::MidiLearnEngine` and a plug-in format: what each format
/// can send back to a controller, a one-call per-block helper, and the handle
/// that lets a host adapter reach the engine.
///
/// @code
/// class MySynth : public Processor {
///     state::MidiLearnEngine learn_;
///     format::MidiLearnHandle learn_handle_{*this, learn_}; // after learn_
///     void process(..., midi::MidiBuffer& midi_in, midi::MidiBuffer& midi_out, ...) override {
///         format::midi_learn_process_block(learn_, state(), midi_in, midi_out,
///                                          format::midi_feedback_capabilities(
///                                              host_format_, descriptor()));
///     }
/// };
/// @endcode
///
/// Capabilities report what the ADAPTER writes to the host's MIDI output. They
/// say nothing about whether a given host passes that output on or what it does
/// with it; that is host behaviour, not claimed here.

#include <pulp/format/plugin_descriptor.hpp>
#include <pulp/format/processor.hpp>
#include <pulp/midi/buffer.hpp>
#include <pulp/midi/message.hpp>
#include <pulp/state/midi_learn_engine.hpp>
#include <pulp/state/store.hpp>

namespace pulp::format {

/// The plug-in format an instance is running as.
enum class MidiLearnFormat : uint8_t { vst3, clap, au_v2, au_v3, lv2, aax, standalone };

/// What a format's adapter can send back to a controller.
struct MidiFeedbackCapabilities {
    bool supported = false;            ///< The adapter writes processor MIDI output to the host.
    state::MidiSourceKindMask kinds{}; ///< Source kinds it can carry (empty when unsupported).

    constexpr bool can_send(state::MidiSourceKind kind) const {
        return supported && kinds.has(kind);
    }
};

/// Feedback support for `format`. A plug-in that does not declare MIDI output
/// (`descriptor.produces_midi`) gets none: every adapter below publishes MIDI
/// output only for such a plug-in.
///
/// Verified from the adapters' outbound paths: VST3 (every channel-voice
/// message as a LegacyMIDICCOutEvent), CLAP (every short message as
/// CLAP_EVENT_MIDI), AU v2 (a MIDIPacketList through the host's
/// MIDIOutputCallback) and AU v3 (MIDIOutputEventBlock). LV2, AAX and the
/// standalone app are not claimed here.
constexpr MidiFeedbackCapabilities midi_feedback_capabilities(MidiLearnFormat format,
                                                              bool produces_midi) {
    switch (format) {
    case MidiLearnFormat::vst3:
    case MidiLearnFormat::clap:
    case MidiLearnFormat::au_v2:
    case MidiLearnFormat::au_v3:
        if (produces_midi)
            return {true, state::kFeedbackEncodableKinds};
        return {};
    case MidiLearnFormat::lv2:
    case MidiLearnFormat::aax:
    case MidiLearnFormat::standalone:
        return {};
    }
    return {};
}

inline MidiFeedbackCapabilities midi_feedback_capabilities(MidiLearnFormat format,
                                                           const PluginDescriptor& descriptor) {
    return midi_feedback_capabilities(format, descriptor.produces_midi);
}

/// One call per audio block: adopt the newest mappings, route every incoming
/// short message, then send feedback for mappings that asked for it into
/// `midi_out` (at sample offset 0). Real-time safe.
inline void midi_learn_process_block(state::MidiLearnEngine& engine, state::StateStore& store,
                                     const midi::MidiBuffer& midi_in, midi::MidiBuffer& midi_out,
                                     const MidiFeedbackCapabilities& capabilities) {
    engine.begin_block();
    for (const auto& e : midi_in) {
        if (e.size() >= 1 && e.size() <= 3)
            engine.route(store, state::raw_midi_from_bytes(e.data(), e.size()));
    }
    if (!capabilities.supported)
        return;
    engine.emit_feedback(store, capabilities.kinds, [&](const state::RawMidi& m) {
        midi_out.add(midi::MidiEvent{choc::midi::ShortMessage(m.status, m.data1, m.data2), 0, 0.0});
    });
}

/// Registers `engine` with `processor` for the lifetime of this object. Declare
/// it after the engine member so it unregisters first.
class MidiLearnHandle {
  public:
    MidiLearnHandle(Processor& processor, state::MidiLearnEngine& engine) : processor_(processor) {
        processor_.set_midi_learn_engine(&engine);
    }
    ~MidiLearnHandle() {
        processor_.set_midi_learn_engine(nullptr);
    }
    MidiLearnHandle(const MidiLearnHandle&) = delete;
    MidiLearnHandle& operator=(const MidiLearnHandle&) = delete;

  private:
    Processor& processor_;
};

} // namespace pulp::format
