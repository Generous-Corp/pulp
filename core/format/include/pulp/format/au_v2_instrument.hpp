#pragma once

// The AU v2 instrument adapter wraps Apple's AudioUnitSDK (Apple-only,
// developer-supplied). The whole header is gated on __APPLE__ so it stays
// self-contained — an empty no-op — on the Linux header-hygiene check and any
// non-Apple TU.
#if defined(__APPLE__)

#include <AudioUnitSDK/MusicDeviceBase.h>

#include <pulp/format/processor.hpp>
#include <pulp/format/host_quirks.hpp>
#include <pulp/format/detail/playhead_diff.hpp>
#include <pulp/runtime/spsc_queue.hpp>

#include <array>
#include <cstdint>
#include <memory>
#include <vector>

namespace pulp::format::au {

/// Number of AU output ELEMENTS (buses) a multi-output instrument advertises:
/// one per declared output bus, floored at 1 and capped at
/// `BusBufferSet::kMaxBuses`. Passed to the `MusicDeviceBase` base constructor
/// so an AU host (Logic, Live, Cubase) lists each output bus — main plus every
/// aux — and can route them to separate mixer channels.
///
/// The cap is load-bearing, not cosmetic: `Render()` only fills output views up
/// to `kMaxOutputBuses` (== `BusBufferSet::kMaxBuses`), so advertising more AU
/// elements than that would materialise output elements the render path never
/// touches — the host would show buses that stay permanently silent. Clamp here
/// so the advertised element count and the routed count agree; the constructor's
/// caller (`registry_output_element_count`) logs a warning when a descriptor's
/// declared bus count exceeds the cap so the truncation is observable, not
/// silent.
///
/// Pure so the multi-output element-count contract is unit-testable without an
/// `AudioComponentInstance`. Mirrors how VST3 (`addAudioOutput` per bus) and
/// CLAP (audio-ports per bus) already iterate `descriptor().output_buses`, and
/// how CLAP caps routed output buses at its own `kMaxOutputBuses`.
inline std::size_t instrument_output_element_count(
    const PluginDescriptor& desc) noexcept {
    const std::size_t declared =
        desc.output_buses.empty() ? std::size_t{1} : desc.output_buses.size();
    return declared < BusBufferSet::kMaxBuses ? declared
                                              : BusBufferSet::kMaxBuses;
}

/// Fill one `ProcessBusBufferInfo` per declared output bus (index 0 = Main, the
/// rest = Aux) from the descriptor. Bus names are `string_view`s INTO `desc`, so
/// `desc` must outlive the filled infos (the render path views the adapter's
/// cached descriptor, whose strings live for the plugin's lifetime).
///
/// Pure / allocation-free: the render path uses it to tag each per-block output
/// view so a multi-out processor that overrides `process(ProcessBuffers&)` sees
/// stable bus identity, and it is unit-testable without a live AU host. Writes
/// at most `cap` entries and returns the count.
inline std::size_t build_output_bus_infos(const PluginDescriptor& desc,
                                          ProcessBusBufferInfo* out,
                                          std::size_t cap) noexcept {
    std::size_t n = 0;
    for (std::size_t i = 0; i < desc.output_buses.size() && n < cap; ++i) {
        out[n].name = desc.output_buses[i].name;
        out[n].index = i;
        out[n].direction = BusDirection::Output;
        out[n].role = (i == 0) ? BusRole::Main : BusRole::Aux;
        out[n].declared_channels = desc.output_buses[i].default_channels;
        // Aux buses are optional; a host may leave them disconnected. The main
        // bus (index 0) is always required.
        out[n].optional = (i != 0);
        out[n].active = true;
        ++n;
    }
    return n;
}

class PulpAUInstrument : public ausdk::MusicDeviceBase {
public:
    explicit PulpAUInstrument(AudioComponentInstance ci);

    OSStatus GetParameterList(AudioUnitScope inScope,
                              AudioUnitParameterID* outParameterList,
                              UInt32& outNumParameters) override;
    OSStatus GetParameterInfo(AudioUnitScope inScope,
                              AudioUnitParameterID inParameterID,
                              AudioUnitParameterInfo& outParameterInfo) override;

    // Single source of truth: the host reads/writes the plugin's StateStore
    // directly — no separate Globals copy to reconcile per block. Mirrors the
    // effect adapter (see au_v2_adapter). See the auv2 skill.
    OSStatus GetParameter(AudioUnitParameterID inID, AudioUnitScope inScope,
                          AudioUnitElement inElement, Float32& outValue) override;
    OSStatus SetParameter(AudioUnitParameterID inID, AudioUnitScope inScope,
                          AudioUnitElement inElement, Float32 inValue,
                          UInt32 inBufferOffsetInFrames) override;

    // Serve the Pulp editor-context property (so the Cocoa view factory can
    // reach this instance's Processor + StateStore) and advertise the Cocoa
    // view to the host (kAudioUnitProperty_CocoaUI). Without these the host
    // never loads the Pulp editor and shows its own generic param view —
    // the AU-instrument editor gap ChainerSynth surfaced.
    OSStatus GetPropertyInfo(AudioUnitPropertyID inID, AudioUnitScope inScope,
                             AudioUnitElement inElement, UInt32& outDataSize,
                             bool& outWritable) override;
    OSStatus GetProperty(AudioUnitPropertyID inID, AudioUnitScope inScope,
                         AudioUnitElement inElement, void* outData) override;

    OSStatus Initialize() override;
    void Cleanup() override;

    bool StreamFormatWritable(AudioUnitScope scope, AudioUnitElement element) override;
    bool CanScheduleParameters() const noexcept override;

    OSStatus HandleNoteOn(UInt8 inChannel, UInt8 inNoteNumber,
                          UInt8 inVelocity, UInt32 inStartFrame) override;
    OSStatus HandleNoteOff(UInt8 inChannel, UInt8 inNoteNumber,
                           UInt8 inVelocity, UInt32 inStartFrame) override;

    OSStatus Render(AudioUnitRenderActionFlags& ioActionFlags,
                    const AudioTimeStamp& inTimeStamp,
                    UInt32 inNumberFrames) override;

    OSStatus SaveState(CFPropertyListRef* outData) override;
    OSStatus RestoreState(CFPropertyListRef plist) override;

    bool SupportsTail() override;
    Float64 GetTailTime() override;
    Float64 GetLatency() override;

private:
    // The store is declared before the Processor so it is destroyed after it.
    // `Processor::state()` dereferences a pointer to this store, and a Processor
    // may read it from its destructor or from a worker thread that destructor is
    // about to join. Reversing these two lines hands that thread a freed store.
    state::StateStore store_;
    std::unique_ptr<Processor> processor_;

    // Immutable plugin metadata, cached once in the constructor. Owns the bus
    // name strings that the per-block `ProcessBusBufferInfo`s view — copying it
    // per render block would allocate (std::string members) on the audio thread.
    PluginDescriptor descriptor_{};

    // Host accommodations, resolved once at init via the runtime policy.
    HostQuirks host_quirks_{};

    // Per-output-bus channel-pointer storage. One vector per AU output element
    // (bus); index 0 is the main bus, 1..N-1 the aux buses. Pre-reserved in
    // Initialize() so the per-block resize to the host buffer count is a no-op
    // realloc and the render path never grows a vector on the audio thread.
    static constexpr std::size_t kMaxOutputBuses = BusBufferSet::kMaxBuses;
    std::array<std::vector<float*>, kMaxOutputBuses> output_bus_ptrs_{};

    // Main-thread listener that pushes editor parameter edits to the host
    // (never from the render thread). Kept alive for the adapter's lifetime.
    state::ListenerToken ui_push_listener_;

    // Lock-free MIDI note input (single producer = host MIDI/render thread via
    // HandleNoteOn/Off; single consumer = Render). No audio-thread mutex.
    runtime::SpscQueue<midi::MidiEvent, 1024> midi_in_queue_;

    // Previous-block transport snapshot used to derive change flags on
    // `ProcessContext`. Default-constructed so the first Render() call
    // after Initialize() reports no changes.
    detail::PlayheadSnapshot playhead_prev_{};
};

} // namespace pulp::format::au

#endif // defined(__APPLE__)
