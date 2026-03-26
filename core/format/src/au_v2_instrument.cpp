// Audio Unit v2 instrument adapter for Pulp
// Uses Apple's AudioUnitSDK MusicDeviceBase for AU instruments (aumu type)
// Handles MIDI note events and renders audio output with no audio input

#include <AudioUnitSDK/MusicDeviceBase.h>
#include <AudioUnitSDK/AUPlugInDispatch.h>
#include <AudioUnitSDK/AUOutputElement.h>

#include <pulp/format/processor.hpp>
#include <pulp/format/registry.hpp>
#include <pulp/runtime/log.hpp>

#include <memory>
#include <vector>
#include <cmath>
#include <mutex>

namespace pulp::format::au {

class PulpAUInstrument : public ausdk::MusicDeviceBase {
public:
    explicit PulpAUInstrument(AudioComponentInstance ci)
        : MusicDeviceBase(ci, /*numInputs=*/0, /*numOutputs=*/1, /*numGroups=*/0)
    {
        auto factory = registered_factory();
        if (factory) {
            processor_ = factory();
            if (processor_) {
                processor_->set_state_store(&store_);
                processor_->define_parameters(store_);

                // Set default values in the AU parameter system at construction time.
                // This ensures auval can read defaults before Initialize() is called,
                // and that parameters start with the correct values.
                for (const auto& param : store_.all_params()) {
                    Globals()->SetParameter(
                        static_cast<AudioUnitParameterID>(param.id),
                        param.range.default_value);
                }
            }
        }
    }

    // ── Parameter system ────────────────────────────────────────────────

    OSStatus GetParameterList(AudioUnitScope inScope,
                              AudioUnitParameterID* outParameterList,
                              UInt32& outNumParameters) override
    {
        if (inScope != kAudioUnitScope_Global) {
            outNumParameters = 0;
            return noErr;
        }
        outNumParameters = static_cast<UInt32>(store_.param_count());
        if (outParameterList) {
            auto params = store_.all_params();
            for (std::size_t i = 0; i < params.size(); ++i)
                outParameterList[i] = static_cast<AudioUnitParameterID>(params[i].id);
        }
        return noErr;
    }

    OSStatus GetParameterInfo(AudioUnitScope inScope,
                              AudioUnitParameterID inParameterID,
                              AudioUnitParameterInfo& outParameterInfo) override
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

        if (param->unit == "dB")
            outParameterInfo.unit = kAudioUnitParameterUnit_Decibels;
        else if (param->unit == "Hz")
            outParameterInfo.unit = kAudioUnitParameterUnit_Hertz;
        else
            outParameterInfo.unit = kAudioUnitParameterUnit_Generic;

        return noErr;
    }

    // ── Lifecycle ───────────────────────────────────────────────────────

    OSStatus Initialize() override {
        auto result = MusicDeviceBase::Initialize();
        if (result != noErr) return result;

        if (processor_) {
            PrepareContext ctx;
            ctx.sample_rate = GetOutput(0)->GetStreamFormat().mSampleRate;
            ctx.max_buffer_size = GetMaxFramesPerSlice();
            ctx.input_channels = 0;
            ctx.output_channels = static_cast<int>(
                GetOutput(0)->GetStreamFormat().mChannelsPerFrame);
            processor_->prepare(ctx);

            // Sync AU parameter system → Pulp store.
            // At this point, Globals() has either the defaults (set in constructor)
            // or values the host set before Initialize. Either way, sync them.
            for (const auto& param : store_.all_params()) {
                auto au_id = static_cast<AudioUnitParameterID>(param.id);
                float value = Globals()->GetParameter(au_id);
                store_.set_value(param.id, value);
            }
        }

        runtime::log_info("AU v2 instrument: initialized at {} Hz",
                          GetOutput(0)->GetStreamFormat().mSampleRate);
        return noErr;
    }

    void Cleanup() override {
        if (processor_) processor_->release();
        MusicDeviceBase::Cleanup();
    }

    bool StreamFormatWritable(AudioUnitScope scope, AudioUnitElement) override {
        return scope == kAudioUnitScope_Output || scope == kAudioUnitScope_Input;
    }

    bool CanScheduleParameters() const noexcept override { return true; }

    // ── MIDI handling ───────────────────────────────────────────────────

    OSStatus HandleNoteOn(UInt8 inChannel, UInt8 inNoteNumber,
                          UInt8 inVelocity, UInt32 inStartFrame) override
    {
        std::lock_guard lock(midi_mutex_);
        auto me = midi::MidiEvent::note_on(inChannel, inNoteNumber, inVelocity);
        me.sample_offset = static_cast<int32_t>(inStartFrame);
        pending_midi_.add(me);
        return noErr;
    }

    OSStatus HandleNoteOff(UInt8 inChannel, UInt8 inNoteNumber,
                           UInt8 inVelocity, UInt32 inStartFrame) override
    {
        std::lock_guard lock(midi_mutex_);
        auto me = midi::MidiEvent::note_off(inChannel, inNoteNumber, inVelocity);
        me.sample_offset = static_cast<int32_t>(inStartFrame);
        pending_midi_.add(me);
        return noErr;
    }

    // ── Rendering ───────────────────────────────────────────────────────

    OSStatus Render(AudioUnitRenderActionFlags& ioActionFlags,
                    const AudioTimeStamp& inTimeStamp,
                    UInt32 inNumberFrames) override
    {
        if (!processor_) return noErr;

        // Sync parameter values
        for (const auto& param : store_.all_params()) {
            float value = Globals()->GetParameterRT(
                static_cast<AudioUnitParameterID>(param.id));
            store_.set_value(param.id, value);
        }

        // Get output buffer
        auto* output = GetOutput(0);
        AudioBufferList& outBL = output->PrepareBuffer(inNumberFrames);

        UInt32 out_channels = outBL.mNumberBuffers;
        output_ptrs_.resize(out_channels);
        for (UInt32 i = 0; i < out_channels; ++i)
            output_ptrs_[i] = static_cast<float*>(outBL.mBuffers[i].mData);

        audio::BufferView<float> output_view(
            output_ptrs_.data(), out_channels, inNumberFrames);

        // Empty input (instrument has no audio input)
        audio::BufferView<const float> input_view;

        // Collect MIDI
        midi::MidiBuffer midi_in, midi_out;
        {
            std::lock_guard lock(midi_mutex_);
            midi_in = std::move(pending_midi_);
            pending_midi_ = midi::MidiBuffer{};
        }

        ProcessContext ctx;
        ctx.sample_rate = GetOutput(0)->GetStreamFormat().mSampleRate;
        ctx.num_samples = static_cast<int>(inNumberFrames);

        processor_->process(output_view, input_view, midi_in, midi_out, ctx);

        return noErr;
    }

    // ── State ───────────────────────────────────────────────────────────

    OSStatus SaveState(CFPropertyListRef* outData) override {
        auto result = MusicDeviceBase::SaveState(outData);
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

    OSStatus RestoreState(CFPropertyListRef plist) override {
        auto result = MusicDeviceBase::RestoreState(plist);
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

    // ── Component info ──────────────────────────────────────────────────

    bool SupportsTail() override { return true; }
    Float64 GetTailTime() override { return 0.0; }
    Float64 GetLatency() override { return 0.0; }

private:
    std::unique_ptr<Processor> processor_;
    state::StateStore store_;
    std::vector<float*> output_ptrs_;

    std::mutex midi_mutex_;
    midi::MidiBuffer pending_midi_;
};

} // namespace pulp::format::au
