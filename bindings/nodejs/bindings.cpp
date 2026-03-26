/// @file bindings.cpp
/// Node.js bindings for Pulp audio plugin framework via Node-API (napi).
///
/// Exposes HeadlessHost, StateStore, MidiBuffer to JavaScript/TypeScript
/// for automation, testing, and web-based plugin workflows.

#include <napi.h>

#include <pulp/format/headless.hpp>
#include <pulp/format/processor.hpp>
#include <pulp/state/store.hpp>
#include <pulp/state/parameter.hpp>
#include <pulp/audio/buffer.hpp>
#include <pulp/midi/message.hpp>
#include <pulp/midi/buffer.hpp>

using namespace pulp;

// ── MidiBuffer wrapper ───────────────────────────────────────────────────

class MidiBufferWrap : public Napi::ObjectWrap<MidiBufferWrap> {
public:
    static Napi::Object Init(Napi::Env env, Napi::Object exports) {
        auto func = DefineClass(env, "MidiBuffer", {
            InstanceMethod("addNoteOn", &MidiBufferWrap::AddNoteOn),
            InstanceMethod("addNoteOff", &MidiBufferWrap::AddNoteOff),
            InstanceMethod("addCC", &MidiBufferWrap::AddCC),
            InstanceMethod("clear", &MidiBufferWrap::Clear),
            InstanceMethod("size", &MidiBufferWrap::Size),
        });
        auto* constructor = new Napi::FunctionReference();
        *constructor = Napi::Persistent(func);
        exports.Set("MidiBuffer", func);
        env.SetInstanceData(constructor);
        return exports;
    }

    MidiBufferWrap(const Napi::CallbackInfo& info)
        : Napi::ObjectWrap<MidiBufferWrap>(info) {}

    midi::MidiBuffer& buffer() { return buffer_; }

private:
    midi::MidiBuffer buffer_;

    Napi::Value AddNoteOn(const Napi::CallbackInfo& info) {
        uint8_t ch = info[0].As<Napi::Number>().Uint32Value();
        uint8_t note = info[1].As<Napi::Number>().Uint32Value();
        uint8_t vel = info[2].As<Napi::Number>().Uint32Value();
        buffer_.add(midi::MidiEvent::note_on(ch, note, vel));
        return info.Env().Undefined();
    }

    Napi::Value AddNoteOff(const Napi::CallbackInfo& info) {
        uint8_t ch = info[0].As<Napi::Number>().Uint32Value();
        uint8_t note = info[1].As<Napi::Number>().Uint32Value();
        buffer_.add(midi::MidiEvent::note_off(ch, note));
        return info.Env().Undefined();
    }

    Napi::Value AddCC(const Napi::CallbackInfo& info) {
        uint8_t ch = info[0].As<Napi::Number>().Uint32Value();
        uint8_t cc = info[1].As<Napi::Number>().Uint32Value();
        uint8_t val = info[2].As<Napi::Number>().Uint32Value();
        buffer_.add(midi::MidiEvent::cc(ch, cc, val));
        return info.Env().Undefined();
    }

    Napi::Value Clear(const Napi::CallbackInfo& info) {
        buffer_.clear();
        return info.Env().Undefined();
    }

    Napi::Value Size(const Napi::CallbackInfo& info) {
        return Napi::Number::New(info.Env(), static_cast<double>(buffer_.size()));
    }
};

// ── HeadlessHost wrapper ─────────────────────────────────────────────────

class HeadlessHostWrap : public Napi::ObjectWrap<HeadlessHostWrap> {
public:
    static Napi::Object Init(Napi::Env env, Napi::Object exports) {
        auto func = DefineClass(env, "HeadlessHost", {
            InstanceMethod("prepare", &HeadlessHostWrap::Prepare),
            InstanceMethod("release", &HeadlessHostWrap::Release),
            InstanceMethod("getValue", &HeadlessHostWrap::GetValue),
            InstanceMethod("setValue", &HeadlessHostWrap::SetValue),
            InstanceMethod("getNormalized", &HeadlessHostWrap::GetNormalized),
            InstanceMethod("setNormalized", &HeadlessHostWrap::SetNormalized),
            InstanceMethod("getDefault", &HeadlessHostWrap::GetDefault),
            InstanceMethod("resetToDefault", &HeadlessHostWrap::ResetToDefault),
            InstanceMethod("paramCount", &HeadlessHostWrap::ParamCount),
            InstanceMethod("getParamInfo", &HeadlessHostWrap::GetParamInfo),
            InstanceMethod("getAllParams", &HeadlessHostWrap::GetAllParams),
            InstanceMethod("getDescriptor", &HeadlessHostWrap::GetDescriptor),
            InstanceMethod("process", &HeadlessHostWrap::Process),
            InstanceMethod("saveState", &HeadlessHostWrap::SaveState),
            InstanceMethod("loadState", &HeadlessHostWrap::LoadState),
        });
        exports.Set("HeadlessHost", func);
        return exports;
    }

    HeadlessHostWrap(const Napi::CallbackInfo& info)
        : Napi::ObjectWrap<HeadlessHostWrap>(info) {
        // Factory must be set via a registration mechanism
        // For now, the host is constructed empty
    }

    void set_host(std::unique_ptr<format::HeadlessHost> h) {
        host_ = std::move(h);
    }

private:
    std::unique_ptr<format::HeadlessHost> host_;

    Napi::Value Prepare(const Napi::CallbackInfo& info) {
        if (!host_) return info.Env().Undefined();
        double sr = info[0].As<Napi::Number>().DoubleValue();
        int bs = info[1].As<Napi::Number>().Int32Value();
        int in_ch = info.Length() > 2 ? info[2].As<Napi::Number>().Int32Value() : 2;
        int out_ch = info.Length() > 3 ? info[3].As<Napi::Number>().Int32Value() : 2;
        host_->prepare(sr, bs, in_ch, out_ch);
        return info.Env().Undefined();
    }

    Napi::Value Release(const Napi::CallbackInfo& info) {
        if (host_) host_->release();
        return info.Env().Undefined();
    }

    Napi::Value GetValue(const Napi::CallbackInfo& info) {
        if (!host_) return Napi::Number::New(info.Env(), 0);
        auto id = static_cast<state::ParamID>(info[0].As<Napi::Number>().Uint32Value());
        return Napi::Number::New(info.Env(), host_->state().get_value(id));
    }

    Napi::Value SetValue(const Napi::CallbackInfo& info) {
        if (!host_) return info.Env().Undefined();
        auto id = static_cast<state::ParamID>(info[0].As<Napi::Number>().Uint32Value());
        float val = info[1].As<Napi::Number>().FloatValue();
        host_->state().set_value(id, val);
        return info.Env().Undefined();
    }

    Napi::Value GetNormalized(const Napi::CallbackInfo& info) {
        if (!host_) return Napi::Number::New(info.Env(), 0);
        auto id = static_cast<state::ParamID>(info[0].As<Napi::Number>().Uint32Value());
        return Napi::Number::New(info.Env(), host_->state().get_normalized(id));
    }

    Napi::Value SetNormalized(const Napi::CallbackInfo& info) {
        if (!host_) return info.Env().Undefined();
        auto id = static_cast<state::ParamID>(info[0].As<Napi::Number>().Uint32Value());
        float val = info[1].As<Napi::Number>().FloatValue();
        host_->state().set_normalized(id, val);
        return info.Env().Undefined();
    }

    Napi::Value GetDefault(const Napi::CallbackInfo& info) {
        if (!host_) return Napi::Number::New(info.Env(), 0);
        auto id = static_cast<state::ParamID>(info[0].As<Napi::Number>().Uint32Value());
        return Napi::Number::New(info.Env(), host_->state().get_default(id));
    }

    Napi::Value ResetToDefault(const Napi::CallbackInfo& info) {
        if (!host_) return info.Env().Undefined();
        auto id = static_cast<state::ParamID>(info[0].As<Napi::Number>().Uint32Value());
        host_->state().reset_to_default(id);
        return info.Env().Undefined();
    }

    Napi::Value ParamCount(const Napi::CallbackInfo& info) {
        if (!host_) return Napi::Number::New(info.Env(), 0);
        return Napi::Number::New(info.Env(), static_cast<double>(host_->state().param_count()));
    }

    Napi::Value GetParamInfo(const Napi::CallbackInfo& info) {
        auto env = info.Env();
        if (!host_) return env.Null();
        auto id = static_cast<state::ParamID>(info[0].As<Napi::Number>().Uint32Value());
        auto* pi = host_->state().info(id);
        if (!pi) return env.Null();

        auto obj = Napi::Object::New(env);
        obj.Set("id", Napi::Number::New(env, pi->id));
        obj.Set("name", Napi::String::New(env, pi->name));
        obj.Set("unit", Napi::String::New(env, pi->unit));
        obj.Set("min", Napi::Number::New(env, pi->range.min));
        obj.Set("max", Napi::Number::New(env, pi->range.max));
        obj.Set("defaultValue", Napi::Number::New(env, pi->range.default_value));
        return obj;
    }

    Napi::Value GetAllParams(const Napi::CallbackInfo& info) {
        auto env = info.Env();
        if (!host_) return Napi::Array::New(env, 0);

        auto params = host_->state().all_params();
        auto arr = Napi::Array::New(env, params.size());
        for (size_t i = 0; i < params.size(); ++i) {
            auto obj = Napi::Object::New(env);
            obj.Set("id", Napi::Number::New(env, params[i].id));
            obj.Set("name", Napi::String::New(env, params[i].name));
            obj.Set("unit", Napi::String::New(env, params[i].unit));
            obj.Set("min", Napi::Number::New(env, params[i].range.min));
            obj.Set("max", Napi::Number::New(env, params[i].range.max));
            obj.Set("defaultValue", Napi::Number::New(env, params[i].range.default_value));
            arr.Set(static_cast<uint32_t>(i), obj);
        }
        return arr;
    }

    Napi::Value GetDescriptor(const Napi::CallbackInfo& info) {
        auto env = info.Env();
        if (!host_) return env.Null();

        auto& d = host_->descriptor();
        auto obj = Napi::Object::New(env);
        obj.Set("name", Napi::String::New(env, d.name));
        obj.Set("manufacturer", Napi::String::New(env, d.manufacturer));
        obj.Set("version", Napi::String::New(env, d.version));
        obj.Set("bundleId", Napi::String::New(env, d.bundle_id));
        obj.Set("isInstrument", Napi::Boolean::New(env,
            d.category == format::PluginCategory::Instrument));
        return obj;
    }

    /// Process audio using Float32Arrays.
    /// Input: { channels: number, frames: number, data: Float32Array[] }
    /// Returns: Float32Array[] (one per channel)
    Napi::Value Process(const Napi::CallbackInfo& info) {
        auto env = info.Env();
        if (!host_) return env.Null();

        auto input_obj = info[0].As<Napi::Object>();
        int channels = input_obj.Get("channels").As<Napi::Number>().Int32Value();
        int frames = input_obj.Get("frames").As<Napi::Number>().Int32Value();
        auto data_arr = input_obj.Get("data").As<Napi::Array>();

        // Build input buffer
        std::vector<std::vector<float>> in_data(static_cast<size_t>(channels));
        std::vector<const float*> in_ptrs(static_cast<size_t>(channels));
        for (int ch = 0; ch < channels; ++ch) {
            auto typed = data_arr.Get(static_cast<uint32_t>(ch)).As<Napi::Float32Array>();
            in_data[static_cast<size_t>(ch)].resize(static_cast<size_t>(frames));
            for (int i = 0; i < frames; ++i) {
                in_data[static_cast<size_t>(ch)][static_cast<size_t>(i)] = typed[static_cast<uint32_t>(i)];
            }
            in_ptrs[static_cast<size_t>(ch)] = in_data[static_cast<size_t>(ch)].data();
        }
        audio::BufferView<const float> in_view(in_ptrs.data(), channels, frames);

        // Build output buffer
        std::vector<std::vector<float>> out_data(static_cast<size_t>(channels),
            std::vector<float>(static_cast<size_t>(frames), 0.0f));
        std::vector<float*> out_ptrs(static_cast<size_t>(channels));
        for (int ch = 0; ch < channels; ++ch) {
            out_ptrs[static_cast<size_t>(ch)] = out_data[static_cast<size_t>(ch)].data();
        }
        audio::BufferView<float> out_view(out_ptrs.data(), channels, frames);

        host_->process(out_view, in_view);

        // Return Float32Arrays
        auto result = Napi::Array::New(env, static_cast<size_t>(channels));
        for (int ch = 0; ch < channels; ++ch) {
            auto buf = Napi::Float32Array::New(env, static_cast<size_t>(frames));
            for (int i = 0; i < frames; ++i) {
                buf[static_cast<uint32_t>(i)] = out_data[static_cast<size_t>(ch)][static_cast<size_t>(i)];
            }
            result.Set(static_cast<uint32_t>(ch), buf);
        }
        return result;
    }

    Napi::Value SaveState(const Napi::CallbackInfo& info) {
        auto env = info.Env();
        if (!host_) return env.Null();
        auto data = host_->save_state();
        auto buf = Napi::Buffer<uint8_t>::Copy(env, data.data(), data.size());
        return buf;
    }

    Napi::Value LoadState(const Napi::CallbackInfo& info) {
        if (!host_) return Napi::Boolean::New(info.Env(), false);
        auto buf = info[0].As<Napi::Buffer<uint8_t>>();
        bool ok = host_->load_state(std::span<const uint8_t>(buf.Data(), buf.Length()));
        return Napi::Boolean::New(info.Env(), ok);
    }
};

// ── Module initialization ────────────────────────────────────────────────

Napi::Object Init(Napi::Env env, Napi::Object exports) {
    MidiBufferWrap::Init(env, exports);
    HeadlessHostWrap::Init(env, exports);
    return exports;
}

NODE_API_MODULE(pulp, Init)
