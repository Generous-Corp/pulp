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

#include <cstdint>
#include <memory>
#include <vector>

namespace pulp::format::au {

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

    // Host accommodations, resolved once at init via the runtime policy.
    HostQuirks host_quirks_{};
    std::vector<float*> output_ptrs_;

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
