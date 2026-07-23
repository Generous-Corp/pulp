#pragma once

#include <pulp/runtime/alive_token.hpp>

// Pieces of the AU v2 adapter surface that no single adapter owns.
//
// Pulp has three AU v2 adapters — the effect (`AUMIDIEffectBase`), the
// instrument (`MusicDeviceBase`), and the MIDI processor (`MusicDeviceBase`
// with no audio buses) — and the AudioUnitSDK forces each to derive a
// different base. Everything here is expressed against `AUBase`, `StateStore`,
// `Processor`, or plain bytes rather than a specific adapter base, so all
// three route through one implementation instead of three copies that drift.
//
// Apple-only: the whole header is gated on __APPLE__ so it stays an empty
// no-op on the Linux header-hygiene check.
#if defined(__APPLE__)

#include <AudioUnitSDK/AUBase.h>
#include <AudioToolbox/AudioUnitProperties.h>
#include <AudioUnit/AudioUnit.h>
#include <CoreFoundation/CoreFoundation.h>
#include <CoreMIDI/MIDIServices.h>
#include <mach/mach_time.h>

#include <pulp/format/processor.hpp>
#include <pulp/format/detail/midi_out_offset.hpp>
#include <pulp/format/detail/playhead_diff.hpp>
#include <pulp/midi/buffer.hpp>
#include <pulp/midi/message.hpp>
#include <pulp/state/listener_token.hpp>
#include <pulp/state/store.hpp>

#include <array>
#include <atomic>
#include <cstdint>
#include <limits>

namespace pulp::format::au {

/// Custom AU property ID the Cocoa view factory queries to obtain the
/// host-side Processor + StateStore pointers. Fixes the former
/// "dual-Processor" bug where the Cocoa view created a second Processor
/// instance that silently desynchronized from the host's audio-thread
/// Processor. Property is Global scope, read-only.
static constexpr AudioUnitPropertyID kPulpEditorContextProperty = 0x50754564; // 'PuEd'

struct PulpEditorContext {
    pulp::format::Processor* processor = nullptr;
    pulp::state::StateStore* store = nullptr;
    pulp::runtime::AliveToken::Handle owner_alive;
};

/// Cross-TU Cocoa-view hook. The AU adapter classes are compiled into the
/// shared `pulp-format` library WITHOUT `PULP_AU_GUI`, while the Cocoa view
/// factory + its `AudioUnitCocoaViewInfo` filler live in
/// `au_v2_cocoa_view.mm`, added per-plugin to the `*_AU` target WITH
/// `PULP_AU_GUI`. A compile-time `#ifdef` in the adapters would therefore
/// always be off. Instead, the GUI module registers its filler here at
/// static-init, and the adapters read it (ungated) from
/// `GetProperty(kAudioUnitProperty_CocoaUI)`.
///
/// Null when no GUI module is linked (CLAP / Standalone / headless tests) —
/// the adapters then report no Cocoa view and the host uses its generic view.
/// Without this hook the host is NEVER told the plugin has a custom editor.
using CocoaViewInfoFiller = bool (*)(void*);
extern CocoaViewInfoFiller g_cocoa_view_info_filler;

/// Fills an `AudioUnitCocoaViewInfo` with the Pulp Cocoa view factory's bundle
/// URL + class name. Defined in `au_v2_cocoa_view.mm` (PULP_AU_GUI); installed
/// into `g_cocoa_view_info_filler` at static-init. Returns false if the factory
/// class isn't available. `outData` must point to an `AudioUnitCocoaViewInfo`.
bool fill_cocoa_view_info(void* outData);

/// Decode a short MIDI status byte and two data bytes into a
/// ``midi::MidiEvent``. Exposed at namespace scope so the AU-MIDI routing
/// can be unit tested without constructing a real ``AudioComponentInstance``.
///
/// Channel voice messages arrive at ``AUMIDIBase::HandleMIDIEvent`` already
/// split into ``inStatus`` (top nibble, e.g. ``0x80``, ``0xB0``) and
/// ``inChannel`` (bottom nibble, 0–15). This helper re-combines them into
/// a single status byte so the resulting ``choc::midi::ShortMessage`` has a
/// consistent on-the-wire layout regardless of whether the caller already
/// split the channel out.
///
/// @param inStatus  Status byte (top nibble, bottom nibble optional).
/// @param inChannel MIDI channel (0–15). Ignored for system messages.
/// @param inData1   Data byte 1 (note number, CC number, etc.).
/// @param inData2   Data byte 2 (velocity, CC value, etc.).
/// @returns MidiEvent with ``sample_offset == 0`` — callers should set
///          the sample offset themselves before enqueuing.
midi::MidiEvent decode_midi_event(uint8_t inStatus,
                                  uint8_t inChannel,
                                  uint8_t inData1,
                                  uint8_t inData2) noexcept;

/// Build the AU v2 render-path ProcessContext fields that are independent of
/// host callbacks. AU v2 render callbacks are always realtime; offline bounce
/// intent is not surfaced by the v2 SDK, so hosts that need explicit offline
/// hints should use AU v3 or another adapter that provides that signal.
inline ProcessContext make_render_process_context(double sample_rate,
                                                  int num_samples) noexcept {
    ProcessContext ctx;
    ctx.sample_rate = sample_rate;
    ctx.num_samples = num_samples;
    ctx.process_mode = ProcessMode::Realtime;
    ctx.render_speed_hint = RenderSpeedHint::Realtime;
    return ctx;
}

/// Populate AU v2 render-context transport metadata from host callbacks and
/// derive per-block playhead change flags. The AU v2 SDK only exposes these
/// callbacks through AUBase during render, but keeping the mapping here lets
/// every adapter share one contract and lets tests install HostCallbackInfo
/// without constructing a full AudioComponentInstance.
inline void apply_host_callbacks_to_process_context(
    ProcessContext& ctx,
    const ausdk::AUBase& unit,
    detail::PlayheadSnapshot& previous) noexcept {
    Float64 beat = 0.0;
    Float64 tempo = 0.0;
    if (unit.CallHostBeatAndTempo(&beat, &tempo) == noErr) {
        ctx.position_beats = beat;
        if (tempo > 0.0) ctx.tempo_bpm = tempo;
    }

    UInt32 delta_samples = 0;
    Float32 ts_num = 0.0f;
    UInt32 ts_denom = 0;
    Float64 current_measure_downbeat = 0.0;
    if (unit.CallHostMusicalTimeLocation(&delta_samples, &ts_num, &ts_denom,
                                         &current_measure_downbeat) == noErr) {
        if (ts_num > 0.0f) ctx.time_sig_numerator = static_cast<int>(ts_num);
        if (ts_denom > 0) ctx.time_sig_denominator = static_cast<int>(ts_denom);
    }

    Boolean is_playing = false;
    Boolean transport_state_changed = false;
    Float64 current_sample_in_timeline = 0.0;
    Boolean is_cycling = false;
    Float64 cycle_start = 0.0;
    Float64 cycle_end = 0.0;
    if (unit.CallHostTransportState(&is_playing, &transport_state_changed,
                                    &current_sample_in_timeline, &is_cycling,
                                    &cycle_start, &cycle_end) == noErr) {
        ctx.is_playing = (is_playing != 0);
        ctx.position_samples = static_cast<int64_t>(current_sample_in_timeline);
        ctx.is_looping = (is_cycling != 0);
        if (ctx.is_looping) {
            ctx.loop_start_beats = cycle_start;
            ctx.loop_end_beats = cycle_end;
        }
    }

    static mach_timebase_info_data_t timebase = {0, 0};
    if (timebase.denom == 0) mach_timebase_info(&timebase);
    if (timebase.denom != 0) {
        const uint64_t now = mach_absolute_time();
        ctx.host_time_ns = static_cast<int64_t>(
            (now * timebase.numer) / timebase.denom);
    }

    detail::derive_bar_from_beats(ctx);
    detail::compute_playhead_changes(ctx, previous);
}

inline Float64 tail_samples_to_seconds(int tail_samples,
                                       double sample_rate) noexcept {
    if (tail_samples < 0) return std::numeric_limits<Float64>::infinity();
    if (tail_samples == 0 || sample_rate <= 0.0) return 0.0;
    return static_cast<Float64>(tail_samples) / sample_rate;
}

/// RT-safe builder for the `MIDIPacketList` an AU v2 plugin hands the host via
/// `kAudioUnitProperty_MIDIOutputCallback`. Backed by a fixed-size byte buffer
/// so building the list never allocates on the audio thread — the same
/// pre-reserved-storage discipline the MIDI input queues use.
///
/// Short channel-voice / system messages (1–3 bytes) and the Processor's output
/// SysEx are merged into a single ascending-`sample_offset` order before being
/// appended via the CoreMIDI packet-list builders (which write into the caller's
/// buffer). CoreMIDI packet lists are expected time-ordered, so the merge
/// guarantees a SysEx at offset 0 is delivered before a note at offset 64 even
/// though they live in separate sidecars. Each packet carries the Processor's
/// per-event `sample_offset`, clamped to the current block, as its timestamp —
/// the offset semantics `kAudioUnitProperty_MIDIOutputCallback` documents.
//
// The per-block MIDI event budget shared by every AU v2 adapter's
// `kMaxEventsPerBlock` and the output packet builder's `kMaxOutputEvents`.
// This lives in one place on purpose: if the input bound and the output bound
// ever drift apart, host-bound MIDI is silently truncated on a dense
// arp/generator block. Reference this constant, never re-spell the literal.
inline constexpr std::size_t kMaxMidiEventsPerBlock = 2048;

struct MidiOutputPacketBuilder {
    // Bound the per-block output by the same event budget the MIDI input path
    // reserves, so a dense arp/generator block is delivered in full rather than
    // silently truncated. Short messages are at most 3 wire bytes; the CoreMIDI
    // packet header + per-packet overhead is small, so a budget of
    // (events * ~16 bytes) comfortably covers the worst case. SysEx shares the
    // same buffer and yields to the byte bound. Events past the buffer bound are
    // dropped (counted in `dropped`) rather than overflowing.
    static constexpr std::size_t kMaxOutputEvents = kMaxMidiEventsPerBlock;
    static constexpr std::size_t kBufferBytes = kMaxOutputEvents * 16;
    alignas(MIDIPacketList) std::array<std::uint8_t, kBufferBytes> storage{};

    // Number of events dropped on the most recent build() because the packet
    // buffer or the merge index filled. 0 in steady state; a diagnostic the
    // adapter / tests can read to detect a too-dense block.
    std::size_t dropped = 0;

    /// Build a `MIDIPacketList` from `midi_out` into the internal buffer.
    /// `frame_count` is the current block size; per-event offsets are clamped to
    /// `[0, frame_count - 1]` so a stray out-of-block offset (mirroring the
    /// AU v3 input-side defensive clamp) never lands a packet past the block.
    /// Returns the packet list (pointing into `storage`) when at least one packet
    /// was written, or nullptr when there is nothing to send. The returned
    /// pointer is valid until the next `build` call or destruction.
    /// Allocation-free.
    const MIDIPacketList* build(const midi::MidiBuffer& midi_out,
                                std::uint32_t frame_count) noexcept {
        dropped = 0;

        // Build a merge index over both sources (short events + sysex) so we can
        // emit in ascending sample_offset order. Each source is referenced by a
        // tagged index; the index array is fixed-capacity (no allocation).
        struct Ref {
            std::int32_t offset;
            std::uint32_t order;  // stable tiebreak: preserves per-source order
            std::uint32_t index;  // index within its source
            bool is_sysex;
        };
        std::array<Ref, kMaxOutputEvents> refs;
        std::size_t n_refs = 0;
        std::uint32_t order = 0;

        const auto clamp_offset = [&](std::int32_t off) -> std::int32_t {
            // Shared cross-format offset contract (see detail/midi_out_offset.hpp
            // and test_midi_out_offset_parity.cpp): an in-block offset N is
            // preserved; a stray out-of-block offset is clamped into the block.
            return detail::au_output_offset(off, frame_count);
        };

        std::uint32_t ev_index = 0;
        for (const auto& e : midi_out) {
            const std::uint32_t len = e.message.length();
            if (len != 0 && len <= 3) {  // short messages only
                if (n_refs < refs.size()) {
                    refs[n_refs++] = Ref{clamp_offset(e.sample_offset),
                                         order++, ev_index, false};
                } else {
                    ++dropped;
                }
            }
            ++ev_index;
        }
        const auto& sysex = midi_out.sysex();
        for (std::uint32_t i = 0; i < sysex.size(); ++i) {
            if (sysex[i].data.empty()) continue;
            if (n_refs < refs.size()) {
                refs[n_refs++] = Ref{clamp_offset(sysex[i].sample_offset),
                                     order++, i, true};
            } else {
                ++dropped;
            }
        }

        // Stable sort by offset (insertion sort — small n, no allocation, and
        // stable so same-offset events keep per-source emission order).
        for (std::size_t i = 1; i < n_refs; ++i) {
            Ref key = refs[i];
            std::size_t j = i;
            while (j > 0 && (refs[j - 1].offset > key.offset ||
                             (refs[j - 1].offset == key.offset &&
                              refs[j - 1].order > key.order))) {
                refs[j] = refs[j - 1];
                --j;
            }
            refs[j] = key;
        }

        auto* list = reinterpret_cast<MIDIPacketList*>(storage.data());
        MIDIPacket* cur = MIDIPacketListInit(list);
        bool wrote = false;

        for (std::size_t i = 0; i < n_refs; ++i) {
            const Ref& r = refs[i];
            const auto ts = static_cast<MIDITimeStamp>(r.offset);
            MIDIPacket* next = nullptr;
            if (r.is_sysex) {
                const auto& sx = sysex[r.index];
                next = MIDIPacketListAdd(list, kBufferBytes, cur, ts,
                                         sx.data.size(), sx.data.data());
            } else {
                // Random-access the short event by its buffer index.
                const midi::MidiEvent& e = midi_out[r.index];
                const std::uint32_t len = e.message.length();
                std::uint8_t bytes[3] = {0, 0, 0};
                const auto* d = e.data();
                for (std::uint32_t b = 0; b < len; ++b) bytes[b] = d[b];
                next = MIDIPacketListAdd(list, kBufferBytes, cur, ts, len, bytes);
            }
            if (next == nullptr) {  // buffer full — drop the rest
                dropped += (n_refs - i);
                break;
            }
            cur = next;
            wrote = true;
        }

        return wrote ? list : nullptr;
    }
};

// ── Parameter surface ────────────────────────────────────────────────────
//
// The StateStore IS the host's parameter store (see the auv2 skill's
// "Parameters are single-source-of-truth" section) — these helpers only read
// and write it, never a parallel AUElement copy.

/// `GetParameterList`: report every global-scope parameter ID. A non-global
/// scope reports zero parameters rather than an error, matching AUBase.
OSStatus fill_parameter_list(const state::StateStore& store,
                             AudioUnitScope scope,
                             AudioUnitParameterID* out_list,
                             UInt32& out_count);

/// `GetParameterInfo`: name, range, and unit.
///
/// `advertise_value_strings` controls `kAudioUnitParameterFlag_ValuesHaveStrings`
/// for parameters that declare a `to_string` or explicit value labels. Setting
/// the flag is what makes a host ASK for display text — through
/// `GetParameterValueStrings` for discrete parameters and
/// `kAudioUnitProperty_ParameterStringFromValue` / `...ValueFromString` for
/// continuous ones. An adapter that does not serve those must pass false, or
/// the host asks a question nothing answers.
OSStatus fill_parameter_info(const state::StateStore& store,
                             AudioUnitScope scope,
                             AudioUnitParameterID param_id,
                             AudioUnitParameterInfo& out_info,
                             bool advertise_value_strings);

/// `GetParameterValueStrings`: the enumerated display strings for a DISCRETE
/// parameter. Continuous parameters reach the host through
/// `kAudioUnitProperty_ParameterStringFromValue` instead and report
/// `kAudioUnitErr_InvalidPropertyValue` here.
OSStatus fill_parameter_value_strings(const state::StateStore& store,
                                      AudioUnitScope scope,
                                      AudioUnitParameterID param_id,
                                      CFArrayRef* out_strings);

/// `kAudioUnitProperty_ParameterStringFromValue`. The host owns and releases
/// `outString`, so it is created with a +1 retain; `inValue == nullptr` means
/// "use the parameter's current value".
OSStatus parameter_string_from_value(const state::StateStore& store,
                                     void* out_data);

/// `kAudioUnitProperty_ParameterValueFromString` — text entry, string → value.
OSStatus parameter_value_from_string(const state::StateStore& store,
                                     void* out_data);

// ── Host ↔ store parameter bridge ────────────────────────────────────────

/// True while this thread is inside a host-originated `SetParameter`. The
/// store's UI-push listener fires inline on the writing thread, so it consults
/// this to skip echoing the host's own write back at it.
bool host_is_writing_param() noexcept;

/// RAII marker for the host-write window. Scoped to the calling thread.
class ScopedHostParamWrite {
public:
    ScopedHostParamWrite() noexcept;
    ~ScopedHostParamWrite() noexcept;
    ScopedHostParamWrite(const ScopedHostParamWrite&) = delete;
    ScopedHostParamWrite& operator=(const ScopedHostParamWrite&) = delete;
};

/// Wire the editor → host parameter path: gesture begin/end brackets (so a
/// drag records one clean automation pass) and a value-change notification so
/// the host re-reads through `GetParameter`. All three are plain
/// `AUEventListenerNotify` calls on the editing thread — never
/// `AudioUnitSetParameter` on ourselves, and never from the render thread.
///
/// `unit` is captured by value, so the callbacks never dereference the adapter
/// after it is gone. The returned listener token must be held for the
/// adapter's lifetime.
void wire_host_parameter_bridge(state::StateStore& store,
                                AudioUnit unit,
                                state::ListenerToken& out_listener);

// ── State (preset) serialization ─────────────────────────────────────────

/// Attach Pulp's serialized state to the AU class-info dictionary the base
/// class produced. Takes ownership semantics identical to `AUBase::SaveState`:
/// `*out_data` is replaced with a dictionary carrying the `pulp-state` blob.
OSStatus save_pulp_state(state::StateStore& store,
                         Processor& processor,
                         CFPropertyListRef* out_data);

/// Read the `pulp-state` blob back out of an AU class-info dictionary. A
/// dictionary without the key restores nothing and succeeds (an AU preset
/// saved by a different plugin version is not an error).
OSStatus restore_pulp_state(state::StateStore& store,
                            Processor& processor,
                            CFPropertyListRef plist);

// ── MIDI output callback publishing ──────────────────────────────────────

/// Publishes the host's `kAudioUnitProperty_MIDIOutputCallback` pair from the
/// main thread to the render thread.
///
/// The `(callback, userData)` pair is written by `SetProperty` on the main
/// thread and read by the render thread every block. A plain two-pointer
/// struct is a data race that can pair a fresh callback with a stale
/// `userData`, so the pair is double-buffered: the writer fills the inactive
/// slot then release-stores a pointer to it and flips the write cursor; the
/// reader does a single acquire-load and therefore never observes a torn pair.
/// "AU serializes property writes against render" is NOT relied on.
class MidiOutputCallbackPublisher {
public:
    struct Pair {
        AUMIDIOutputCallback callback = nullptr;
        void* user_data = nullptr;
    };

    /// Publish the pair carried by a `kAudioUnitProperty_MIDIOutputCallback`
    /// SetProperty payload. Returns `kAudioUnitErr_InvalidPropertyValue` for a
    /// null or undersized payload.
    OSStatus publish(const void* in_data, UInt32 in_data_size) noexcept;

    /// Reflect the currently published pair into a
    /// `AUMIDIOutputCallbackStruct` for `GetProperty`.
    OSStatus reflect(void* out_data) const noexcept;

    /// Acquire-load the published pair, or nullptr when the host has installed
    /// none. Render-thread entry point.
    const Pair* load() const noexcept {
        return active_.load(std::memory_order_acquire);
    }

private:
    std::array<Pair, 2> slots_{};
    std::atomic<std::uint8_t> write_slot_{0};
    std::atomic<const Pair*> active_{nullptr};
};

/// Build the one-element `CFArrayRef` a host reads from
/// `kAudioUnitProperty_MIDIOutputCallbackInfo` — the names of the plugin's
/// MIDI output streams. The host owns and releases the array, so it is created
/// with a +1 retain.
CFArrayRef make_midi_output_names(const char* stream_name);

} // namespace pulp::format::au

#endif // defined(__APPLE__)
