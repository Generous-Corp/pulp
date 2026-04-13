// Audio Unit v2 adapter for Pulp
// Uses Apple's AudioUnitSDK (Apache 2.0) for proper AU v2 hosting contract
// Subclasses AUEffectBase and overrides ProcessBufferLists for multi-channel

#include <AudioUnitSDK/AUPlugInDispatch.h>
#include <AudioToolbox/AudioUnitUtilities.h>

#include <pulp/format/au_v2_adapter.hpp>
#include <pulp/format/registry.hpp>
#include <pulp/runtime/log.hpp>

#include <cstring>
#include <limits>

namespace pulp::format::au {

// Parameter IDs for the AU system map 1:1 from Pulp ParamIDs
// AU uses AudioUnitParameterID (UInt32) which matches our state::ParamID

PulpAUEffect::PulpAUEffect(AudioComponentInstance ci)
    : AUEffectBase(ci, /*inProcessesInPlace=*/true)
{
    auto factory = registered_factory();
    if (factory) {
        processor_ = factory();
        if (processor_) {
            processor_->set_state_store(&store_);
            processor_->define_parameters(store_);

            // Wire gesture callbacks for undo grouping support.
            store_.set_gesture_callbacks(
                [this](state::ParamID id) {
                    AudioUnitEvent event;
                    event.mEventType = kAudioUnitEvent_BeginParameterChangeGesture;
                    event.mArgument.mParameter.mAudioUnit = GetComponentInstance();
                    event.mArgument.mParameter.mParameterID = static_cast<AudioUnitParameterID>(id);
                    event.mArgument.mParameter.mScope = kAudioUnitScope_Global;
                    event.mArgument.mParameter.mElement = 0;
                    AUEventListenerNotify(nullptr, nullptr, &event);
                },
                [this](state::ParamID id) {
                    AudioUnitEvent event;
                    event.mEventType = kAudioUnitEvent_EndParameterChangeGesture;
                    event.mArgument.mParameter.mAudioUnit = GetComponentInstance();
                    event.mArgument.mParameter.mParameterID = static_cast<AudioUnitParameterID>(id);
                    event.mArgument.mParameter.mScope = kAudioUnitScope_Global;
                    event.mArgument.mParameter.mElement = 0;
                    AUEventListenerNotify(nullptr, nullptr, &event);
                });

            // Set defaults in AU parameter system at construction time so hosts
            // can inspect them before Initialize() is called.
            for (const auto& param : store_.all_params()) {
                Globals()->SetParameter(
                    static_cast<AudioUnitParameterID>(param.id),
                    param.range.default_value);
            }
        }
    }
}

OSStatus PulpAUEffect::GetParameterList(AudioUnitScope inScope,
                                        AudioUnitParameterID* outParameterList,
                                        UInt32& outNumParameters)
{
    if (inScope != kAudioUnitScope_Global) {
        outNumParameters = 0;
        return noErr;
    }

    outNumParameters = static_cast<UInt32>(store_.param_count());
    if (outParameterList) {
        auto params = store_.all_params();
        for (std::size_t i = 0; i < params.size(); ++i) {
            outParameterList[i] = static_cast<AudioUnitParameterID>(params[i].id);
        }
    }
    return noErr;
}

OSStatus PulpAUEffect::GetParameterInfo(AudioUnitScope inScope,
                                        AudioUnitParameterID inParameterID,
                                        AudioUnitParameterInfo& outParameterInfo)
{
    if (inScope != kAudioUnitScope_Global)
        return kAudioUnitErr_InvalidParameter;

    const auto* param = store_.info(static_cast<state::ParamID>(inParameterID));
    if (!param) return kAudioUnitErr_InvalidParameter;

    outParameterInfo.flags = kAudioUnitParameterFlag_IsWritable
                           | kAudioUnitParameterFlag_IsReadable
                           | kAudioUnitParameterFlag_HasCFNameString;

    CFStringRef name = CFStringCreateWithCString(
        kCFAllocatorDefault, param->name.c_str(), kCFStringEncodingUTF8);
    outParameterInfo.cfNameString = name;
    strlcpy(reinterpret_cast<char*>(outParameterInfo.name),
            param->name.c_str(), sizeof(outParameterInfo.name));

    outParameterInfo.minValue = param->range.min;
    outParameterInfo.maxValue = param->range.max;
    outParameterInfo.defaultValue = param->range.default_value;

    if (param->unit == "dB") {
        outParameterInfo.unit = kAudioUnitParameterUnit_Decibels;
    } else if (param->unit == "Hz") {
        outParameterInfo.unit = kAudioUnitParameterUnit_Hertz;
    } else if (param->unit == "%") {
        outParameterInfo.unit = kAudioUnitParameterUnit_Percent;
    } else if (param->range.step >= 1.0f
               && param->range.min == 0.0f
               && param->range.max == 1.0f) {
        outParameterInfo.unit = kAudioUnitParameterUnit_Boolean;
    } else {
        outParameterInfo.unit = kAudioUnitParameterUnit_Generic;
    }

    return noErr;
}

OSStatus PulpAUEffect::GetParameterValueStrings(AudioUnitScope inScope,
                                                AudioUnitParameterID inParameterID,
                                                CFArrayRef* outStrings)
{
    if (inScope != kAudioUnitScope_Global)
        return kAudioUnitErr_InvalidParameter;

    const auto* param = store_.info(static_cast<state::ParamID>(inParameterID));
    if (!param) return kAudioUnitErr_InvalidParameter;

    if (param->range.step < 1.0f || !param->to_string)
        return kAudioUnitErr_InvalidPropertyValue;

    int count = static_cast<int>((param->range.max - param->range.min) / param->range.step) + 1;
    CFMutableArrayRef strings = CFArrayCreateMutable(kCFAllocatorDefault, count, &kCFTypeArrayCallBacks);
    for (int i = 0; i < count; ++i) {
        float value = param->range.min + i * param->range.step;
        auto str = param->to_string(value);
        CFStringRef cfStr = CFStringCreateWithCString(
            kCFAllocatorDefault, str.c_str(), kCFStringEncodingUTF8);
        CFArrayAppendValue(strings, cfStr);
        CFRelease(cfStr);
    }
    *outStrings = strings;
    return noErr;
}

OSStatus PulpAUEffect::GetPropertyInfo(AudioUnitPropertyID inID, AudioUnitScope inScope,
                                       AudioUnitElement inElement, UInt32& outDataSize,
                                       bool& outWritable)
{
    if (inID == kPulpEditorContextProperty) {
        if (inScope != kAudioUnitScope_Global) return kAudioUnitErr_InvalidScope;
        if (inElement != 0) return kAudioUnitErr_InvalidElement;
        outDataSize = sizeof(PulpEditorContext);
        outWritable = false;
        return noErr;
    }
    return AUEffectBase::GetPropertyInfo(inID, inScope, inElement, outDataSize, outWritable);
}

OSStatus PulpAUEffect::GetProperty(AudioUnitPropertyID inID, AudioUnitScope inScope,
                                   AudioUnitElement inElement, void* outData)
{
    if (inID == kPulpEditorContextProperty) {
        if (inScope != kAudioUnitScope_Global) return kAudioUnitErr_InvalidScope;
        if (inElement != 0) return kAudioUnitErr_InvalidElement;
        if (!outData) return kAudioUnitErr_InvalidProperty;
        auto* ctx = static_cast<PulpEditorContext*>(outData);
        ctx->processor = processor_.get();
        ctx->store = &store_;
        return noErr;
    }
    return AUEffectBase::GetProperty(inID, inScope, inElement, outData);
}

OSStatus PulpAUEffect::Initialize()
{
    auto result = AUEffectBase::Initialize();
    if (result != noErr) return result;

    if (processor_) {
        PrepareContext ctx;
        ctx.sample_rate = GetSampleRate();
        ctx.max_buffer_size = GetMaxFramesPerSlice();
        ctx.input_channels = static_cast<int>(GetNumberOfChannels());
        ctx.output_channels = static_cast<int>(GetNumberOfChannels());
        processor_->prepare(ctx);

        for (const auto& param : store_.all_params()) {
            auto au_id = static_cast<AudioUnitParameterID>(param.id);
            float value = Globals()->GetParameter(au_id);
            store_.set_value(param.id, value);
        }
    }

    runtime::log_info("AU v2: initialized with {} channels at {} Hz",
                      GetNumberOfChannels(), GetSampleRate());
    return noErr;
}

void PulpAUEffect::Cleanup()
{
    if (processor_) processor_->release();
    AUEffectBase::Cleanup();
}

OSStatus PulpAUEffect::ProcessBufferLists(AudioUnitRenderActionFlags& ioActionFlags,
                                          const AudioBufferList& inBuffer,
                                          AudioBufferList& outBuffer,
                                          UInt32 inFramesToProcess)
{
    if (!processor_) {
        for (UInt32 i = 0; i < outBuffer.mNumberBuffers; ++i) {
            memset(outBuffer.mBuffers[i].mData, 0,
                   outBuffer.mBuffers[i].mDataByteSize);
        }
        return noErr;
    }

    for (const auto& param : store_.all_params()) {
        auto au_id = static_cast<AudioUnitParameterID>(param.id);
        float value = GetParameter(au_id);
        store_.set_value(param.id, value);
    }

    UInt32 in_channels = inBuffer.mNumberBuffers;
    UInt32 out_channels = outBuffer.mNumberBuffers;

    input_ptrs_.resize(in_channels);
    output_ptrs_.resize(out_channels);

    for (UInt32 i = 0; i < in_channels; ++i) {
        input_ptrs_[i] = static_cast<const float*>(inBuffer.mBuffers[i].mData);
    }
    for (UInt32 i = 0; i < out_channels; ++i) {
        output_ptrs_[i] = static_cast<float*>(outBuffer.mBuffers[i].mData);
    }

    audio::BufferView<const float> input_view(
        input_ptrs_.data(), in_channels, inFramesToProcess);
    audio::BufferView<float> output_view(
        output_ptrs_.data(), out_channels, inFramesToProcess);

    midi::MidiBuffer midi_in, midi_out;

    ProcessContext ctx;
    ctx.sample_rate = GetSampleRate();
    ctx.num_samples = static_cast<int>(inFramesToProcess);

    processor_->process(output_view, input_view, midi_in, midi_out, ctx);

    return noErr;
}

OSStatus PulpAUEffect::SaveState(CFPropertyListRef* outData)
{
    auto result = AUEffectBase::SaveState(outData);
    if (result != noErr) return result;

    auto data = store_.serialize();
    CFDataRef cfData = CFDataCreate(kCFAllocatorDefault,
                                    data.data(),
                                    static_cast<CFIndex>(data.size()));
    if (cfData && *outData) {
        CFMutableDictionaryRef dict = CFDictionaryCreateMutableCopy(
            kCFAllocatorDefault, 0,
            static_cast<CFDictionaryRef>(*outData));
        CFDictionarySetValue(dict, CFSTR("pulp-state"), cfData);
        CFRelease(*outData);
        *outData = dict;
        CFRelease(cfData);
    }
    return noErr;
}

OSStatus PulpAUEffect::RestoreState(CFPropertyListRef plist)
{
    auto result = AUEffectBase::RestoreState(plist);
    if (result != noErr) return result;

    if (CFGetTypeID(plist) == CFDictionaryGetTypeID()) {
        auto dict = static_cast<CFDictionaryRef>(plist);
        auto cfData = static_cast<CFDataRef>(
            CFDictionaryGetValue(dict, CFSTR("pulp-state")));
        if (cfData && CFGetTypeID(cfData) == CFDataGetTypeID()) {
            auto* bytes = CFDataGetBytePtr(cfData);
            auto length = CFDataGetLength(cfData);
            store_.deserialize({bytes, static_cast<size_t>(length)});

            for (const auto& param : store_.all_params()) {
                Globals()->SetParameter(
                    static_cast<AudioUnitParameterID>(param.id),
                    store_.get_value(param.id));
            }
        }
    }
    return noErr;
}

bool PulpAUEffect::SupportsTail()
{
    return true;
}

Float64 PulpAUEffect::GetTailTime()
{
    if (!processor_) return 0.0;
    auto tail = processor_->descriptor().tail_samples;
    if (tail < 0) return std::numeric_limits<Float64>::infinity();
    return tail > 0 ? static_cast<Float64>(tail) / GetSampleRate() : 0.0;
}

Float64 PulpAUEffect::GetLatency()
{
    if (!processor_) return 0.0;
    int latency = processor_->latency_samples();
    return latency > 0 ? static_cast<Float64>(latency) / GetSampleRate() : 0.0;
}

} // namespace pulp::format::au
