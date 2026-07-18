#pragma once

// VST3 Adapter for Pulp
// Implements Steinberg's VST3 interfaces by wrapping pulp::format::Processor
// Uses SingleComponentEffect pattern (combined processor + controller)
// Built from VST3 SDK headers (MIT license), not from any existing adapter

// VST3 SDK requires one of these to be defined
#ifndef NDEBUG
    #ifndef DEVELOPMENT
        #define DEVELOPMENT 1
    #endif
#else
    #ifndef RELEASE
        #define RELEASE 1
    #endif
#endif

// Must come first per VST3 SDK requirements
#include <public.sdk/source/vst/vstsinglecomponenteffect.h>

#include <pluginterfaces/vst/ivstparameterchanges.h>
#include <pluginterfaces/vst/ivstevents.h>
#include <pluginterfaces/vst/ivstmidicontrollers.h>
#include <pluginterfaces/vst/ivstnoteexpression.h>
#include <pluginterfaces/base/ibstream.h>

#include <pulp/events/plugin_main_thread.hpp>
#include <pulp/format/processor.hpp>
#include <pulp/format/host_quirks.hpp>
#include <pulp/format/detail/playhead_diff.hpp>
#include <pulp/format/detail/vst3_restart_publisher.hpp>
#include <pulp/midi/mpe_buffer.hpp>
#include <pulp/midi/mpe_voice_tracker.hpp>
#include <pulp/state/parameter_event_queue.hpp>
#include <pulp/signal/delay_line.hpp>
#include <pulp/runtime/alive_token.hpp>

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <vector>

#include <pluginterfaces/gui/iplugview.h>

namespace pulp::format::vst3 {

// The VST3 combined processor + controller
// Wraps a pulp::format::Processor for the VST3 host
//
// Also implements IMidiMapping so MIDI controllers (CCs, mod wheel,
// sustain, channel aftertouch, pitch bend) reach instruments. VST3 has no
// raw MIDI controller events: the host queries getMidiControllerAssignment
// for the ParamID each controller maps to, then delivers them as ordinary
// parameter changes. The adapter reserves a private ParamID range for these
// controllers (see detail/vst3_midi_mapping.hpp) and decodes them back into
// MIDI messages in process().
//
// Implements INoteExpressionController so per-note expression (MPE) reaches
// MPE-aware instruments. VST3 delivers per-note pitch / pressure / timbre as
// Event::kNoteExpressionValueEvent keyed by the originating note-on's noteId.
// The host first queries getNoteExpressionInfo to learn which expression
// types the plug-in accepts; the adapter declares them only when the
// descriptor opts into MPE. process() routes each value event through the
// shared MpeVoiceTracker / MpeBuffer sidecar (the same contract the CLAP
// adapter uses), mapping VST3 tuning -> per-note pitch bend, a volume-domain
// expression -> per-note pressure, and brightness -> per-note timbre (CC74).
class PulpVst3Processor : public Steinberg::Vst::SingleComponentEffect,
                          public Steinberg::Vst::IMidiMapping,
                          public Steinberg::Vst::INoteExpressionController {
public:
    PulpVst3Processor(ProcessorFactory factory);

    // FUnknown — expose IMidiMapping alongside the SingleComponentEffect
    // interfaces. Refcounting is inherited from EditControllerEx1 via the
    // base; only queryInterface needs the extra interface.
    Steinberg::tresult PLUGIN_API queryInterface(const Steinberg::TUID iid,
                                                 void** obj) override;
    Steinberg::uint32 PLUGIN_API addRef() override {
        return Steinberg::Vst::SingleComponentEffect::addRef();
    }
    Steinberg::uint32 PLUGIN_API release() override {
        return Steinberg::Vst::SingleComponentEffect::release();
    }

    // IPluginBase
    Steinberg::tresult PLUGIN_API initialize(Steinberg::FUnknown* context) override;
    Steinberg::tresult PLUGIN_API terminate() override;

    // IMidiMapping — report which ParamID each (bus, channel, controller)
    // maps to. Only populated when the descriptor accepts MIDI input.
    Steinberg::tresult PLUGIN_API getMidiControllerAssignment(
        Steinberg::int32 busIndex, Steinberg::int16 channel,
        Steinberg::Vst::CtrlNumber midiControllerNumber,
        Steinberg::Vst::ParamID& id) override;

    // INoteExpressionController — declare the per-note expression types the
    // plug-in accepts and convert between normalized values and strings.
    // getNoteExpressionCount returns 0 (and the info/string methods decline)
    // unless the descriptor opted into MPE, so a non-MPE plug-in advertises
    // no note-expression surface and pays zero per-note cost in process().
    Steinberg::int32 PLUGIN_API getNoteExpressionCount(
        Steinberg::int32 busIndex, Steinberg::int16 channel) override;
    Steinberg::tresult PLUGIN_API getNoteExpressionInfo(
        Steinberg::int32 busIndex, Steinberg::int16 channel,
        Steinberg::int32 noteExpressionIndex,
        Steinberg::Vst::NoteExpressionTypeInfo& info /*out*/) override;
    Steinberg::tresult PLUGIN_API getNoteExpressionStringByValue(
        Steinberg::int32 busIndex, Steinberg::int16 channel,
        Steinberg::Vst::NoteExpressionTypeID id,
        Steinberg::Vst::NoteExpressionValue valueNormalized /*in*/,
        Steinberg::Vst::String128 string /*out*/) override;
    Steinberg::tresult PLUGIN_API getNoteExpressionValueByString(
        Steinberg::int32 busIndex, Steinberg::int16 channel,
        Steinberg::Vst::NoteExpressionTypeID id,
        const Steinberg::Vst::TChar* string /*in*/,
        Steinberg::Vst::NoteExpressionValue& valueNormalized /*out*/) override;

    // IEditController — parameter value/string conversion. Route through the
    // author-supplied ParamInfo::to_string / from_string so a host's generic
    // editor and text-entry field show and accept the plugin's own formatting
    // (e.g. "-6.0 dB", "1.2 kHz"). Both decline to SingleComponentEffect's
    // stock numeric formatting when the parameter declares no converter, so
    // existing plugins are unchanged.
    Steinberg::tresult PLUGIN_API getParamStringByValue(
        Steinberg::Vst::ParamID tag,
        Steinberg::Vst::ParamValue valueNormalized,
        Steinberg::Vst::String128 string) override;
    Steinberg::tresult PLUGIN_API getParamValueByString(
        Steinberg::Vst::ParamID tag, Steinberg::Vst::TChar* string,
        Steinberg::Vst::ParamValue& valueNormalized) override;

    // IEditController — editor view creation
    Steinberg::IPlugView* PLUGIN_API createView(Steinberg::FIDString name) override;

    // IAudioProcessor
    Steinberg::tresult PLUGIN_API setBusArrangements(
        Steinberg::Vst::SpeakerArrangement* inputs, Steinberg::int32 numIns,
        Steinberg::Vst::SpeakerArrangement* outputs, Steinberg::int32 numOuts) override;

    Steinberg::tresult PLUGIN_API canProcessSampleSize(
        Steinberg::int32 symbolicSampleSize) override;

    Steinberg::tresult PLUGIN_API setupProcessing(
        Steinberg::Vst::ProcessSetup& setup) override;

    Steinberg::tresult PLUGIN_API setActive(Steinberg::TBool state) override;

    Steinberg::uint32 PLUGIN_API getLatencySamples() override;
    Steinberg::uint32 PLUGIN_API getTailSamples() override;

    Steinberg::tresult PLUGIN_API process(
        Steinberg::Vst::ProcessData& data) override;

    const state::ParameterEventQueue& last_input_param_events() const {
        return param_events_;
    }

    /// Returns the StateStore ParamID the adapter routes VST3's
    /// `kIsBypass` parameter to. `process()` short-circuits to
    /// pass-through when this parameter's value is >= 0.5. Returns 0
    /// when no bypass parameter is available.
    state::ParamID bypass_parameter_id() const { return bypass_param_id_; }

    /// Saturating count of per-note expression routings the adapter could not
    /// deliver: a note-on whose noteId could not claim a slot in the bounded
    /// noteId map (table full), plus any kNoteExpressionValueEvent whose noteId
    /// is not mapped to a live note (unknown or already released). Mirrors
    /// MpeBuffer::dropped_event_count() in spirit — an observable signal that a
    /// host exceeded the adapter's fixed per-note capacity. RT-safe (atomic,
    /// never logs on the audio thread).
    std::uint32_t note_expression_drop_count() const {
        return note_expression_drops_.load(std::memory_order_relaxed);
    }

    // IComponent (state)
    Steinberg::tresult PLUGIN_API getState(Steinberg::IBStream* state) override;
    Steinberg::tresult PLUGIN_API setState(Steinberg::IBStream* state) override;

    static Steinberg::FUnknown* createInstance(void*);

private:
    ProcessorFactory factory_;
    // The store is declared before the Processor so it is destroyed after it.
    // `Processor::state()` dereferences a pointer to this store, and a Processor
    // may read it from its destructor or from a worker thread that destructor is
    // about to join. Reversing these two lines hands that thread a freed store.
    state::StateStore store_;
    std::unique_ptr<Processor> processor_;
    // Declared after processor_ so reverse member destruction retires retained
    // editor handles before either referenced object is released.
    runtime::AliveToken owner_alive_;

    // Working buffers
    std::vector<float*> input_ptrs_;
    std::vector<float*> output_ptrs_;
    std::vector<const double*> input64_ptrs_;
    std::vector<double*> output64_ptrs_;
    std::vector<std::vector<float>> f64_input_scratch_;
    std::vector<std::vector<float>> f64_output_scratch_;
    // Second input bus routed to Processor::set_sidechain(). Only input
    // bus 1 is consumed; additional buses are ignored because the
    // Processor API exposes a single sidechain slot.
    std::vector<float*> sidechain_ptrs_;
    std::vector<const double*> sidechain64_ptrs_;
    std::vector<std::vector<float>> f64_sidechain_scratch_;
    // Per-bus channel-pointer storage for secondary (aux) output buses,
    // indexed [aux_bus][channel]. aux_output_ptrs_[i] backs the BufferView for
    // the host output bus at index i+1, routed to the Processor's richer
    // process(ProcessBuffers&) surface for multi-out instruments. Each
    // sub-vector is sized in setupProcessing() from the ACCEPTED VST3 bus
    // arrangement (which setBusArrangements() may have shifted mono↔stereo away
    // from the descriptor default), so the routing path reuses this storage,
    // allocates nothing on the audio thread, and never silently drops a channel
    // the host negotiated.
    std::vector<std::vector<float*>> aux_output_ptrs_;
    std::vector<std::vector<double*>> aux_output64_ptrs_;
    // Descriptor-declared channel count per secondary output bus (index i = host
    // output bus i+1), captured in setupProcessing(). Reported as the aux
    // ProcessBusBufferView's declared_channels so matches_declared_layout()
    // compares the routed count against the declared layout rather than itself.
    std::vector<int> declared_aux_channels_;
    std::vector<std::vector<std::vector<float>>> f64_aux_output_scratch_;

    // Per-block MIDI buffers, reused across process() calls. Reserved +
    // realtime-capacity-limited in setupProcessing() so add()/add_sysex_copy()
    // never heap-allocate on the audio thread (they drop past the reserved
    // worst-case instead). Previously these were block-local, so the first
    // add() on any block carrying MIDI allocated.
    midi::MidiBuffer midi_in_;
    midi::MidiBuffer midi_out_;

    // Parameter output: snapshot values before process to detect plugin-side changes
    std::vector<float> param_snapshot_;
    state::ParameterEventQueue param_events_;
    // Sample-accurate parameter OUTPUT: the processor pushes offset-tagged events
    // via push_output_param_event(); we drain them into per-parameter VST3 output
    // queues after process(). All three vectors are sized off the audio thread
    // (at prepare / block start alongside param_snapshot_) so the per-block drain
    // stays allocation-free. output_param_has_event_ is the skip-set that keeps
    // the offset-0 snapshot fallback from double-reporting a param that already
    // emitted explicit events; output_param_queue_cache_ caches one VST3 queue
    // per parameter index so a param with multiple offsets reuses one queue in
    // ascending-offset order.
    state::ParameterEventQueue output_param_events_;
    std::vector<std::uint8_t> output_param_has_event_;
    std::vector<Steinberg::Vst::IParamValueQueue*> output_param_queue_cache_;

    // ── Per-note expression (MPE) sidecar ───────────────────────────────────
    //
    // Mirrors the CLAP adapter's MPE wiring: inbound note events plus the MIDI
    // messages synthesized from note-expression value events run through
    // mpe_tracker_, whose callbacks append per-note expression deltas to
    // mpe_buffer_. The buffer is handed to the Processor via set_mpe_input()
    // for the duration of each process() call. Reserved + capacity-limited in
    // setupProcessing() so the process path never allocates. Only active when
    // the descriptor opts into MPE (mpe_enabled_).
    midi::MpeVoiceTracker mpe_tracker_;
    midi::MpeBuffer mpe_buffer_;
    int32_t mpe_current_sample_offset_ = 0;
    bool mpe_enabled_ = false;

    // noteId -> (channel, note) linkage. VST3 note-expression value events
    // reference the noteId carried by the originating note-on, so the adapter
    // must remember which (channel, note) each live noteId maps to in order to
    // route an expression to the right MPE voice. Fixed-capacity and
    // allocation-free: a note-on with noteId >= 0 claims a slot, the matching
    // note-off releases it. A slot's `active` flag identifies a live mapping.
    struct NoteIdSlot {
        bool active = false;
        Steinberg::int32 note_id = -1;
        uint8_t channel = 0;
        uint8_t note = 0;
    };
    static constexpr std::size_t kMaxLiveNoteIds = 128;
    std::array<NoteIdSlot, kMaxLiveNoteIds> note_id_map_{};

    // Saturating drop counter for per-note expression routing failures (full
    // noteId map on insert, or a value event for an unmapped noteId). Atomic so
    // a host thread can read it via note_expression_drop_count() while the audio
    // thread increments it; never the source of an audio-thread log.
    std::atomic<std::uint32_t> note_expression_drops_{0};

    // Record / release / look up a noteId -> (channel, note) mapping. All
    // O(kMaxLiveNoteIds), allocation-free, audio-thread safe.
    // note_id_map_insert returns false when the table is full (the mapping was
    // dropped) so the caller can bump the drop counter.
    bool note_id_map_insert(Steinberg::int32 note_id, uint8_t channel,
                            uint8_t note);
    void note_id_map_erase(Steinberg::int32 note_id);
    const NoteIdSlot* note_id_map_find(Steinberg::int32 note_id) const;
    void note_id_map_clear();

    // Cached parameter ID of the VST3 bypass parameter. 0 when none is
    // available, so process() never short-circuits.
    state::ParamID bypass_param_id_ = 0;

    // True when the descriptor accepts MIDI input, so the adapter has
    // registered the reserved hidden controller parameters and
    // getMidiControllerAssignment / the process()-side controller decode
    // are live. Mirrors the accepts_midi gating on the event input bus.
    bool midi_controller_mapping_active_ = false;

    // Membership bitmap over the reserved controller ParamID range, indexed
    // by (id - kVst3MidiCcParamBase). A slot is true only if the adapter
    // ACTUALLY registered that ID as a hidden controller parameter — i.e.
    // the ID did not collide with a real plug-in parameter. Registration,
    // getMidiControllerAssignment, and the process()-side diversion all use
    // this one predicate, so a real plug-in parameter that happens to fall
    // in the reserved range is never reported as a controller assignment and
    // never diverted to MIDI (its host param-changes still reach store_).
    // Sized once at init; the process()-thread lookup is O(1) and never
    // allocates. Empty when MIDI mapping is inactive.
    std::vector<bool> registered_controller_ids_;

    // True iff `id` was registered as a hidden controller parameter. O(1),
    // allocation-free — safe to call on the audio thread.
    bool is_registered_controller(state::ParamID id) const;

    // Per-output-channel dry delay used during the bypass pass-through.
    // A plugin that reports latency gets host plugin-delay-compensation on
    // its path, so the bypassed dry signal must be delayed by exactly that
    // many samples to stay sample-aligned with the compensated timeline.
    // Sized once in setupProcessing() (off the audio thread). Empty / unused
    // when the reported latency is 0, preserving the zero-copy fast path.
    std::vector<signal::DelayLine> bypass_dry_delay_;
    int bypass_delay_samples_ = 0;

    // Host accommodations, resolved once in initialize() via the runtime
    // policy (env / API / compile default). Adapters consult these flags
    // instead of hardcoding DAW workarounds.
    HostQuirks quirks_{};

    // The processor is always prepare()'d with the descriptor-default
    // channel counts (see setupProcessing), so these cache what the
    // processor's buffers are sized for. When setBusArrangements accepts
    // an arrangement the processor does not natively support, process()
    // hands the processor only its prepared channel counts and zero-fills
    // the host's extra channels.
    int native_in_ = 0;
    int native_out_ = 0;
    bool silence_unsupported_active_ = false;
    bool native_f64_enabled_ = false;
    Steinberg::int32 selected_sample_size_ = Steinberg::Vst::kSample32;

    // Block size the processor's scratch buffers were prepared for in
    // setupProcessing() (setup.maxSamplesPerBlock). process() clamps an
    // oversized render to this so a host that exceeds the advertised max
    // can't overrun the prepared buffers. 0 means "no limit set yet".
    int max_block_size_ = 0;

    // Previous-block transport snapshot used to derive the
    // `tempo_changed` / `time_sig_changed` / `transport_changed` flags
    // on `ProcessContext`. Default-constructed so the first process()
    // call after construction reports no changes.
    detail::PlayheadSnapshot playhead_prev_{};

    // MainThreadDispatcher backend token. Acquired in initialize(),
    // released in terminate(). On macOS this lets adapter-side and
    // view-side code marshal work onto the DAW's main thread via
    // `pulp::events::MainThreadDispatcher::call_async`.
    pulp::events::MainThreadDispatcher::Token main_thread_token_ = 0;

    // Marshals IComponentHandler::restartComponent off the audio thread.
    // process() OR-accumulates latency/tail restart flags into this publisher
    // (RT-safe); the actual host callback fires on the main thread via
    // drain_pending_restart(). See vst3_restart_publisher.hpp for the contract.
    detail::Vst3RestartPublisher restart_publisher_;

    // Liveness/lifetime token shared with every main-thread lambda this adapter
    // posts to the MainThreadDispatcher. A posted lambda captures this shared_ptr
    // (keeping the flag alive) plus a raw `this`, and checks the flag before
    // touching `this`. terminate() flips it to false, so a callback still queued
    // on the host's main thread when the component is destroyed becomes a no-op
    // instead of dereferencing freed memory. Created in initialize().
    std::shared_ptr<std::atomic<bool>> drain_alive_;

    // True between setActive(true) and setActive(false)/terminate(): drives the
    // self-rescheduling paced main-thread poll so a mid-stream latency/tail
    // change is delivered without depending on an incidental host query.
    std::shared_ptr<std::atomic<bool>> poll_active_;

    // Main-thread drain of restart_publisher_: if a restart is pending, call
    // componentHandler->restartComponent(flags) here (never from process()).
    // Driven by (a) a paced self-rescheduling poll on the host main thread while
    // the component is active and (b) main-thread host entrypoints the adapter
    // already receives (getLatencySamples / getTailSamples / setActive / getState).
    // When a MainThreadDispatcher backend reports we are off the main thread, the
    // call is marshaled there. All paths run off the audio thread only.
    void drain_pending_restart();
    void deliver_pending_restart();

    // Start / stop the paced main-thread poll loop. start is idempotent; stop
    // relies on poll_active_ going false so the next scheduled tick does not
    // re-post. Both are main-thread only.
    void start_restart_poll();
    void schedule_restart_poll_tick();

public:
    // Test-only hook: drive the main-thread restart drain directly. Mirrors
    // what a host-driven main-thread entrypoint does in production. Returns the
    // coalesced flags delivered (0 when nothing was pending).
    std::int32_t poll_pending_restart_for_test() {
        std::int32_t delivered = 0;
        auto* handler = getComponentHandler();
        restart_publisher_.poll_main_thread(
            [&](std::int32_t flags) {
                delivered = flags;
                if (handler) handler->restartComponent(flags);
            });
        return delivered;
    }

    bool restart_dispatch_armed_for_test() const {
        return restart_publisher_.dispatch_armed();
    }
};

} // namespace pulp::format::vst3
