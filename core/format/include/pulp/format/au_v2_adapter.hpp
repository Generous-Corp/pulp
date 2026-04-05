#pragma once

#include <AudioUnitSDK/AUEffectBase.h>

#include <pulp/format/processor.hpp>

#include <memory>
#include <vector>

namespace pulp::format::au {

class PulpAUEffect : public ausdk::AUEffectBase {
public:
    explicit PulpAUEffect(AudioComponentInstance ci);

    OSStatus GetParameterList(AudioUnitScope inScope,
                              AudioUnitParameterID* outParameterList,
                              UInt32& outNumParameters) override;
    OSStatus GetParameterInfo(AudioUnitScope inScope,
                              AudioUnitParameterID inParameterID,
                              AudioUnitParameterInfo& outParameterInfo) override;
    OSStatus GetParameterValueStrings(AudioUnitScope inScope,
                                      AudioUnitParameterID inParameterID,
                                      CFArrayRef* outStrings) override;

    OSStatus Initialize() override;
    void Cleanup() override;

    OSStatus ProcessBufferLists(AudioUnitRenderActionFlags& ioActionFlags,
                                const AudioBufferList& inBuffer,
                                AudioBufferList& outBuffer,
                                UInt32 inFramesToProcess) override;

    OSStatus SaveState(CFPropertyListRef* outData) override;
    OSStatus RestoreState(CFPropertyListRef plist) override;

    bool SupportsTail() override;
    Float64 GetTailTime() override;
    Float64 GetLatency() override;

private:
    std::unique_ptr<Processor> processor_;
    state::StateStore store_;
    std::vector<const float*> input_ptrs_;
    std::vector<float*> output_ptrs_;
};

} // namespace pulp::format::au
