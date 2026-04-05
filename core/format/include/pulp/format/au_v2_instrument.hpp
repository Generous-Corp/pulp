#pragma once

#include <AudioUnitSDK/MusicDeviceBase.h>

#include <pulp/format/processor.hpp>

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
    std::vector<float*> output_ptrs_;
    std::mutex midi_mutex_;
    midi::MidiBuffer pending_midi_;
};

} // namespace pulp::format::au
