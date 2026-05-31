#pragma once

#include <AudioUnitSDK/MusicDeviceBase.h>

#include <pulp/format/processor.hpp>
#include <pulp/format/host_quirks.hpp>
#include <pulp/format/detail/playhead_diff.hpp>

#include <memory>
#include <mutex>
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
    std::unique_ptr<Processor> processor_;
    state::StateStore store_;

    // Host accommodations, resolved once at init (host-quirks plan, P3).
    HostQuirks host_quirks_{};
    std::vector<float*> output_ptrs_;
    std::mutex midi_mutex_;
    midi::MidiBuffer pending_midi_;

    // Item 1.3 — previous-block transport snapshot used to derive the
    // change flags on `ProcessContext`. Default-constructed so the
    // first Render() call after Initialize() reports no changes.
    detail::PlayheadSnapshot playhead_prev_{};
};

} // namespace pulp::format::au
