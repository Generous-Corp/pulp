#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <pulp/format/vst3_adapter.hpp>
#include <public.sdk/source/vst/hosting/eventlist.h>
#include <public.sdk/source/vst/hosting/parameterchanges.h>

#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <string>
#include <vector>

using Catch::Matchers::WithinAbs;

namespace {

using Steinberg::Vst::ParameterInfo;
namespace SpeakerArr = Steinberg::Vst::SpeakerArr;

constexpr pulp::state::ParamID kGainParamId = 1;
constexpr pulp::state::ParamID kBypassParamId = 2;

class ScopedEnv {
public:
    explicit ScopedEnv(std::string name) : name_(std::move(name)) {
        if (const char* prev = std::getenv(name_.c_str())) {
            prev_ = std::string(prev);
        }
    }

    ~ScopedEnv() {
        if (prev_) {
#if defined(_WIN32)
            _putenv_s(name_.c_str(), prev_->c_str());
#else
            ::setenv(name_.c_str(), prev_->c_str(), /*overwrite=*/1);
#endif
        } else {
#if defined(_WIN32)
            _putenv_s(name_.c_str(), "");
#else
            ::unsetenv(name_.c_str());
#endif
        }
    }

    void set(const std::string& value) {
#if defined(_WIN32)
        _putenv_s(name_.c_str(), value.c_str());
#else
        ::setenv(name_.c_str(), value.c_str(), /*overwrite=*/1);
#endif
    }

    void unset() {
#if defined(_WIN32)
        _putenv_s(name_.c_str(), "");
#else
        ::unsetenv(name_.c_str());
#endif
    }

private:
    std::string name_;
    std::optional<std::string> prev_;
};

struct TestVst3Config {
    pulp::format::PluginDescriptor descriptor{
        .name = "Vst3PluginStateTest",
        .manufacturer = "PulpTest",
        .bundle_id = "com.pulp.test.vst3-plugin-state",
        .version = "1.0.0",
        .category = pulp::format::PluginCategory::Effect,
        .input_buses = {{"Audio In", 2}},
        .output_buses = {{"Audio Out", 2}},
    };
    bool add_bypass_param = false;
    bool mutate_gain_in_process = false;
    bool emit_midi_out = false;
    int latency_samples = 0;
};

class TestVst3Processor : public pulp::format::Processor {
public:
    TestVst3Processor() : config_(g_next_config) { g_last_processor = this; }

    pulp::format::PluginDescriptor descriptor() const override {
        return config_.descriptor;
    }

    void define_parameters(pulp::state::StateStore& store) override {
        store.add_parameter({
            .id = kGainParamId,
            .name = "Gain",
            .unit = "dB",
            .range = {-60.0f, 24.0f, 0.0f, 0.1f},
            .group_id = 7,
        });
        if (config_.add_bypass_param) {
            store.add_parameter({
                .id = kBypassParamId,
                .name = "Bypass",
                .range = {0.0f, 1.0f, 0.0f, 1.0f},
            });
        }
    }

    void prepare(const pulp::format::PrepareContext& context) override {
        ++prepare_count;
        last_prepare = context;
    }

    void release() override { ++release_count; }

    void process(pulp::audio::BufferView<float>&,
                 const pulp::audio::BufferView<const float>&,
                 pulp::midi::MidiBuffer&,
                 pulp::midi::MidiBuffer&,
                 const pulp::format::ProcessContext&) override;

    int latency_samples() const override { return config_.latency_samples; }

    std::vector<uint8_t> serialize_plugin_state() const override {
        return std::vector<uint8_t>(plugin_state.begin(), plugin_state.end());
    }

    bool deserialize_plugin_state(std::span<const uint8_t> data) override {
        plugin_state.assign(data.begin(), data.end());
        return true;
    }

    std::string plugin_state;
    int prepare_count = 0;
    int release_count = 0;
    int process_count = 0;
    pulp::format::PrepareContext last_prepare;
    pulp::format::ProcessContext last_context;
    std::size_t last_input_channels = 0;
    std::size_t last_output_channels = 0;
    std::size_t last_sidechain_channels = 0;
    std::size_t last_midi_in_size = 0;
    std::size_t last_sysex_size = 0;
    std::vector<uint8_t> last_sysex_payload;
    std::vector<pulp::state::ParameterEvent> last_param_events;
    bool had_param_events = false;
    float gain_seen_in_process = 0.0f;
    static TestVst3Processor* g_last_processor;
    static TestVst3Config g_next_config;

private:
    TestVst3Config config_;
};

TestVst3Processor* TestVst3Processor::g_last_processor = nullptr;
TestVst3Config TestVst3Processor::g_next_config{};

void reset_test_processor(TestVst3Config config = {}) {
    TestVst3Processor::g_next_config = std::move(config);
    TestVst3Processor::g_last_processor = nullptr;
}

void TestVst3Processor::process(
    pulp::audio::BufferView<float>& audio_output,
    const pulp::audio::BufferView<const float>& audio_input,
    pulp::midi::MidiBuffer& midi_in,
    pulp::midi::MidiBuffer& midi_out,
    const pulp::format::ProcessContext& context) {
    ++process_count;
    last_context = context;
    last_input_channels = audio_input.num_channels();
    last_output_channels = audio_output.num_channels();
    last_sidechain_channels = sidechain_input() ? sidechain_input()->num_channels() : 0;
    last_midi_in_size = midi_in.size();
    last_sysex_size = midi_in.sysex_size();
    if (last_sysex_size > 0) {
        last_sysex_payload = midi_in.sysex()[0].data;
    }
    had_param_events = (param_events() != nullptr);
    last_param_events.clear();
    if (auto* events = param_events()) {
        for (const auto& event : *events) last_param_events.push_back(event);
    }
    gain_seen_in_process = state().get_value(kGainParamId);

    const auto channels = std::min(audio_output.num_channels(), audio_input.num_channels());
    for (std::size_t ch = 0; ch < channels; ++ch) {
        std::copy(audio_input.channel(ch).begin(), audio_input.channel(ch).end(),
                  audio_output.channel(ch).begin());
    }

    if (config_.mutate_gain_in_process) {
        state().set_value(kGainParamId, -6.0f);
    }
    if (config_.emit_midi_out) {
        auto note = pulp::midi::MidiEvent::note_on(1, 64, 100);
        note.sample_offset = 7;
        midi_out.add(note);
    }
}

std::unique_ptr<pulp::format::Processor> create_test_processor() {
    return std::make_unique<TestVst3Processor>();
}

std::unique_ptr<pulp::format::Processor> create_null_processor() {
    return {};
}

class HostApp final : public Steinberg::Vst::IHostApplication {
public:
    Steinberg::tresult PLUGIN_API getName(Steinberg::Vst::String128 name) override {
        const char* kName = "PulpTest";
        for (int i = 0; i < 127 && kName[i]; ++i) {
            name[i] = static_cast<Steinberg::Vst::TChar>(kName[i]);
        }
        name[8] = 0;
        return Steinberg::kResultTrue;
    }

    Steinberg::tresult PLUGIN_API createInstance(Steinberg::TUID,
                                                 Steinberg::TUID,
                                                 void** obj) override {
        if (obj) *obj = nullptr;
        return Steinberg::kNotImplemented;
    }

    Steinberg::tresult PLUGIN_API queryInterface(const Steinberg::TUID iid,
                                                 void** obj) override {
        if (Steinberg::FUnknownPrivate::iidEqual(iid, Steinberg::Vst::IHostApplication::iid) ||
            Steinberg::FUnknownPrivate::iidEqual(iid, Steinberg::FUnknown::iid)) {
            *obj = static_cast<Steinberg::Vst::IHostApplication*>(this);
            return Steinberg::kResultTrue;
        }
        *obj = nullptr;
        return Steinberg::kNoInterface;
    }

    Steinberg::uint32 PLUGIN_API addRef() override { return 1; }
    Steinberg::uint32 PLUGIN_API release() override { return 1; }
};

class VectorStream final : public Steinberg::IBStream {
public:
    VectorStream() = default;
    explicit VectorStream(const std::vector<uint8_t>& src) : buf_(src) {}

    std::vector<uint8_t> take() { return std::move(buf_); }

    Steinberg::tresult PLUGIN_API read(void* buffer, Steinberg::int32 num_bytes,
                                       Steinberg::int32* num_bytes_read) override {
        if (num_bytes < 0) return Steinberg::kInvalidArgument;
        const Steinberg::int64 remaining =
            static_cast<Steinberg::int64>(buf_.size()) - pos_;
        const Steinberg::int64 count = num_bytes < remaining ? num_bytes : remaining;
        if (count > 0) {
            std::memcpy(buffer, buf_.data() + pos_, static_cast<std::size_t>(count));
        }
        pos_ += count;
        if (num_bytes_read) *num_bytes_read = static_cast<Steinberg::int32>(count);
        return Steinberg::kResultOk;
    }

    Steinberg::tresult PLUGIN_API write(void* buffer, Steinberg::int32 num_bytes,
                                        Steinberg::int32* num_bytes_written) override {
        if (num_bytes < 0) return Steinberg::kInvalidArgument;
        const auto* bytes = static_cast<const uint8_t*>(buffer);
        buf_.insert(buf_.end(), bytes, bytes + num_bytes);
        pos_ = static_cast<Steinberg::int64>(buf_.size());
        if (num_bytes_written) *num_bytes_written = num_bytes;
        return Steinberg::kResultOk;
    }

    Steinberg::tresult PLUGIN_API seek(Steinberg::int64 pos, Steinberg::int32 mode,
                                       Steinberg::int64* result) override {
        Steinberg::int64 new_pos = pos_;
        switch (mode) {
            case kIBSeekSet:
                new_pos = pos;
                break;
            case kIBSeekCur:
                new_pos = pos_ + pos;
                break;
            case kIBSeekEnd:
                new_pos = static_cast<Steinberg::int64>(buf_.size()) + pos;
                break;
            default:
                return Steinberg::kInvalidArgument;
        }
        if (new_pos < 0 || new_pos > static_cast<Steinberg::int64>(buf_.size())) {
            return Steinberg::kInvalidArgument;
        }
        pos_ = new_pos;
        if (result) *result = pos_;
        return Steinberg::kResultOk;
    }

    Steinberg::tresult PLUGIN_API tell(Steinberg::int64* pos) override {
        if (!pos) return Steinberg::kInvalidArgument;
        *pos = pos_;
        return Steinberg::kResultOk;
    }

    Steinberg::tresult PLUGIN_API queryInterface(const Steinberg::TUID iid,
                                                 void** obj) override {
        if (Steinberg::FUnknownPrivate::iidEqual(iid, Steinberg::IBStream::iid) ||
            Steinberg::FUnknownPrivate::iidEqual(iid, Steinberg::FUnknown::iid)) {
            *obj = static_cast<Steinberg::IBStream*>(this);
            return Steinberg::kResultTrue;
        }
        *obj = nullptr;
        return Steinberg::kNoInterface;
    }

    Steinberg::uint32 PLUGIN_API addRef() override { return 1; }
    Steinberg::uint32 PLUGIN_API release() override { return 1; }

private:
    std::vector<uint8_t> buf_;
    Steinberg::int64 pos_ = 0;
};

} // namespace

TEST_CASE("VST3 adapter exposes parameter metadata and lifecycle values",
          "[vst3][coverage][issue-493]") {
    TestVst3Config config;
    config.add_bypass_param = true;
    config.latency_samples = 128;
    config.descriptor.tail_samples = -1;
    reset_test_processor(config);

    HostApp host_app;
    pulp::format::vst3::PulpVst3Processor processor(create_test_processor);
    REQUIRE(processor.initialize(&host_app) == Steinberg::kResultOk);
    auto* test_processor = TestVst3Processor::g_last_processor;
    REQUIRE(test_processor != nullptr);

    REQUIRE(processor.getParameterCount() == 2);

    ParameterInfo gain{};
    REQUIRE(processor.getParameterInfo(0, gain) == Steinberg::kResultOk);
    REQUIRE(gain.id == kGainParamId);
    REQUIRE(gain.stepCount == 0);
    REQUIRE(gain.unitId == 7);
    REQUIRE((gain.flags & ParameterInfo::kCanAutomate) != 0);
    REQUIRE((gain.flags & ParameterInfo::kIsBypass) == 0);
    REQUIRE_THAT(gain.defaultNormalizedValue, WithinAbs(60.0 / 84.0, 1e-6));

    ParameterInfo bypass{};
    REQUIRE(processor.getParameterInfo(1, bypass) == Steinberg::kResultOk);
    REQUIRE(bypass.id == kBypassParamId);
    REQUIRE(bypass.stepCount == 1);
    REQUIRE((bypass.flags & ParameterInfo::kCanAutomate) != 0);
    REQUIRE((bypass.flags & ParameterInfo::kIsBypass) != 0);
    REQUIRE_THAT(bypass.defaultNormalizedValue, WithinAbs(0.0, 1e-6));

    Steinberg::Vst::ProcessSetup setup{};
    setup.processMode = Steinberg::Vst::kRealtime;
    setup.symbolicSampleSize = Steinberg::Vst::kSample32;
    setup.maxSamplesPerBlock = 64;
    setup.sampleRate = 96000.0;
    REQUIRE(processor.setupProcessing(setup) == Steinberg::kResultOk);
    REQUIRE(test_processor->prepare_count == 1);
    REQUIRE_THAT(test_processor->last_prepare.sample_rate, WithinAbs(96000.0, 1e-6));
    REQUIRE(test_processor->last_prepare.max_buffer_size == 64);
    REQUIRE(test_processor->last_prepare.input_channels == 2);
    REQUIRE(test_processor->last_prepare.output_channels == 2);

    REQUIRE(processor.getLatencySamples() == 128);
    REQUIRE(processor.getTailSamples() == Steinberg::Vst::kInfiniteTail);

    REQUIRE(processor.setActive(false) == Steinberg::kResultOk);
    REQUIRE(test_processor->release_count == 1);
    REQUIRE(processor.terminate() == Steinberg::kResultOk);
}

TEST_CASE("VST3 editor creation is disabled by automation env",
          "[vst3][editor][issue-2515]") {
    ScopedEnv disable_editor("PULP_DISABLE_PLUGIN_EDITOR");
    ScopedEnv headless("PULP_HEADLESS");
    ScopedEnv test_mode("PULP_TEST_MODE");
    ScopedEnv ci("CI");
#if defined(__linux__) || defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__)
    ScopedEnv display("DISPLAY");
    ScopedEnv wayland("WAYLAND_DISPLAY");
    display.set(":99");
    wayland.unset();
#endif
    disable_editor.unset();
    headless.unset();
    test_mode.unset();
    ci.unset();

    SECTION("creates an editor when automation guards are unset") {
        reset_test_processor();
        HostApp host_app;
        pulp::format::vst3::PulpVst3Processor processor(create_test_processor);
        REQUIRE(processor.initialize(&host_app) == Steinberg::kResultOk);
        auto* view = processor.createView(Steinberg::Vst::ViewType::kEditor);
        REQUIRE(view != nullptr);
        view->release();
        REQUIRE(processor.terminate() == Steinberg::kResultOk);
    }

    SECTION("blocks editor creation under the no-editor env") {
        disable_editor.set("1");

        reset_test_processor();
        HostApp host_app;
        pulp::format::vst3::PulpVst3Processor processor(create_test_processor);
        REQUIRE(processor.initialize(&host_app) == Steinberg::kResultOk);
        REQUIRE(processor.createView(Steinberg::Vst::ViewType::kEditor) == nullptr);
        REQUIRE(processor.terminate() == Steinberg::kResultOk);
    }
}

TEST_CASE("VST3 adapter process path maps host events, buses, and outputs",
          "[vst3][process][coverage][issue-493]") {
    TestVst3Config config;
    config.mutate_gain_in_process = true;
    config.emit_midi_out = true;
    config.descriptor.accepts_midi = true;
    config.descriptor.produces_midi = true;
    config.descriptor.input_buses = {{"Main In", 2}, {"Sidechain", 1, true}};
    config.descriptor.output_buses = {{"Main Out", 2}, {"Aux Out", 1, true}};
    reset_test_processor(config);

    HostApp host_app;
    pulp::format::vst3::PulpVst3Processor processor(create_test_processor);
    REQUIRE(processor.initialize(&host_app) == Steinberg::kResultOk);
    auto* test_processor = TestVst3Processor::g_last_processor;
    REQUIRE(test_processor != nullptr);

    Steinberg::Vst::SpeakerArrangement inputs[2] = {
        SpeakerArr::kStereo,
        SpeakerArr::kMono,
    };
    Steinberg::Vst::SpeakerArrangement outputs[2] = {
        SpeakerArr::kStereo,
        SpeakerArr::kMono,
    };
    REQUIRE(processor.setBusArrangements(inputs, 2, outputs, 2) ==
            Steinberg::kResultTrue);

    Steinberg::Vst::ProcessSetup setup{};
    setup.processMode = Steinberg::Vst::kRealtime;
    setup.symbolicSampleSize = Steinberg::Vst::kSample32;
    setup.maxSamplesPerBlock = 8;
    setup.sampleRate = 44100.0;
    REQUIRE(processor.setupProcessing(setup) == Steinberg::kResultOk);

    Steinberg::Vst::ParameterChanges input_params(1);
    Steinberg::int32 param_index = 0;
    auto* gain_queue = input_params.addParameterData(kGainParamId, param_index);
    REQUIRE(gain_queue != nullptr);
    Steinberg::int32 point_index = 0;
    REQUIRE(gain_queue->addPoint(0, 0.0, point_index) == Steinberg::kResultTrue);
    REQUIRE(gain_queue->addPoint(2, 0.5, point_index) == Steinberg::kResultTrue);
    REQUIRE(gain_queue->addPoint(4, 1.0, point_index) == Steinberg::kResultTrue);

    Steinberg::Vst::ParameterChanges output_params(2);

    Steinberg::Vst::EventList input_events(4);
    Steinberg::Vst::Event note_on{};
    note_on.type = Steinberg::Vst::Event::kNoteOnEvent;
    note_on.sampleOffset = 3;
    note_on.noteOn.channel = 2;
    note_on.noteOn.pitch = 60;
    note_on.noteOn.velocity = 0.5f;
    REQUIRE(input_events.addEvent(note_on) == Steinberg::kResultOk);

    Steinberg::Vst::Event note_off{};
    note_off.type = Steinberg::Vst::Event::kNoteOffEvent;
    note_off.sampleOffset = 6;
    note_off.noteOff.channel = 2;
    note_off.noteOff.pitch = 60;
    note_off.noteOff.velocity = 0.25f;
    REQUIRE(input_events.addEvent(note_off) == Steinberg::kResultOk);

    std::array<uint8_t, 4> sysex{{0xF0, 0x7D, 0x01, 0xF7}};
    Steinberg::Vst::Event sysex_event{};
    sysex_event.type = Steinberg::Vst::Event::kDataEvent;
    sysex_event.sampleOffset = 5;
    sysex_event.data.type = Steinberg::Vst::DataEvent::kMidiSysEx;
    sysex_event.data.bytes = sysex.data();
    sysex_event.data.size = static_cast<Steinberg::uint32>(sysex.size());
    REQUIRE(input_events.addEvent(sysex_event) == Steinberg::kResultOk);

    Steinberg::Vst::EventList output_events(4);

    constexpr int kFrames = 8;
    std::array<float, kFrames> in_l{{0.1f, 0.2f, 0.3f, 0.4f, 0.5f, 0.6f, 0.7f, 0.8f}};
    std::array<float, kFrames> in_r{{-0.1f, -0.2f, -0.3f, -0.4f, -0.5f, -0.6f, -0.7f, -0.8f}};
    std::array<float, kFrames> sidechain{{1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f}};
    std::array<float, kFrames> out_l{};
    std::array<float, kFrames> out_r{};
    std::array<float, kFrames> aux_out{};
    out_l.fill(9.0f);
    out_r.fill(9.0f);
    aux_out.fill(9.0f);

    float* main_inputs[2] = {in_l.data(), in_r.data()};
    float* sidechain_inputs[1] = {sidechain.data()};
    float* main_outputs[2] = {out_l.data(), out_r.data()};
    float* aux_outputs[1] = {aux_out.data()};

    Steinberg::Vst::AudioBusBuffers audio_inputs[2]{};
    audio_inputs[0].numChannels = 2;
    audio_inputs[0].channelBuffers32 = main_inputs;
    audio_inputs[1].numChannels = 1;
    audio_inputs[1].channelBuffers32 = sidechain_inputs;

    Steinberg::Vst::AudioBusBuffers audio_outputs[2]{};
    audio_outputs[0].numChannels = 2;
    audio_outputs[0].channelBuffers32 = main_outputs;
    audio_outputs[1].numChannels = 1;
    audio_outputs[1].channelBuffers32 = aux_outputs;

    Steinberg::Vst::ProcessContext process_context{};
    process_context.state = Steinberg::Vst::ProcessContext::kPlaying |
                            Steinberg::Vst::ProcessContext::kTempoValid |
                            Steinberg::Vst::ProcessContext::kTimeSigValid;
    process_context.tempo = 137.5;
    process_context.projectTimeSamples = 12345;
    process_context.timeSigNumerator = 7;
    process_context.timeSigDenominator = 8;

    Steinberg::Vst::ProcessData data{};
    data.numSamples = kFrames;
    data.numInputs = 2;
    data.numOutputs = 2;
    data.inputs = audio_inputs;
    data.outputs = audio_outputs;
    data.inputParameterChanges = &input_params;
    data.outputParameterChanges = &output_params;
    data.inputEvents = &input_events;
    data.outputEvents = &output_events;
    data.processContext = &process_context;

    REQUIRE(processor.process(data) == Steinberg::kResultOk);

    REQUIRE(test_processor->process_count == 1);
    REQUIRE(test_processor->last_input_channels == 2);
    REQUIRE(test_processor->last_output_channels == 2);
    REQUIRE(test_processor->last_sidechain_channels == 1);
    REQUIRE(test_processor->last_midi_in_size == 2);
    REQUIRE(test_processor->last_sysex_size == 1);
    REQUIRE(test_processor->last_sysex_payload == std::vector<uint8_t>(sysex.begin(), sysex.end()));
    REQUIRE_THAT(test_processor->gain_seen_in_process, WithinAbs(24.0f, 1e-5f));
    REQUIRE(test_processor->last_context.is_playing);
    REQUIRE_THAT(test_processor->last_context.tempo_bpm, WithinAbs(137.5, 1e-6));
    REQUIRE(test_processor->last_context.position_samples == 12345);
    REQUIRE(test_processor->last_context.time_sig_numerator == 7);
    REQUIRE(test_processor->last_context.time_sig_denominator == 8);
    REQUIRE(test_processor->had_param_events);
    REQUIRE(test_processor->last_param_events.size() == 3);
    REQUIRE(test_processor->last_param_events[0].param_id == kGainParamId);
    REQUIRE(test_processor->last_param_events[0].sample_offset == 0);
    REQUIRE_THAT(test_processor->last_param_events[0].value, WithinAbs(-60.0f, 1e-5f));
    REQUIRE(test_processor->last_param_events[1].sample_offset == 2);
    REQUIRE_THAT(test_processor->last_param_events[1].value, WithinAbs(-18.0f, 1e-5f));
    REQUIRE(test_processor->last_param_events[2].sample_offset == 4);
    REQUIRE_THAT(test_processor->last_param_events[2].value, WithinAbs(24.0f, 1e-5f));

    const auto& param_events = processor.last_input_param_events().events();
    REQUIRE(param_events.size() == 3);
    REQUIRE(param_events[0].param_id == kGainParamId);
    REQUIRE(param_events[0].sample_offset == 0);
    REQUIRE_THAT(param_events[0].value, WithinAbs(-60.0f, 1e-5f));
    REQUIRE(param_events[1].sample_offset == 2);
    REQUIRE_THAT(param_events[1].value, WithinAbs(-18.0f, 1e-5f));
    REQUIRE(param_events[2].sample_offset == 4);
    REQUIRE_THAT(param_events[2].value, WithinAbs(24.0f, 1e-5f));

    for (int i = 0; i < kFrames; ++i) {
        REQUIRE_THAT(out_l[i], WithinAbs(in_l[i], 1e-6f));
        REQUIRE_THAT(out_r[i], WithinAbs(in_r[i], 1e-6f));
        REQUIRE_THAT(aux_out[i], WithinAbs(0.0f, 1e-6f));
    }

    REQUIRE(output_params.getParameterCount() == 1);
    auto* output_queue = output_params.getParameterData(0);
    REQUIRE(output_queue != nullptr);
    REQUIRE(output_queue->getParameterId() == kGainParamId);
    REQUIRE(output_queue->getPointCount() == 1);
    Steinberg::int32 output_offset = -1;
    Steinberg::Vst::ParamValue output_value = 0.0;
    REQUIRE(output_queue->getPoint(0, output_offset, output_value) == Steinberg::kResultTrue);
    REQUIRE(output_offset == 0);
    REQUIRE_THAT(output_value, WithinAbs(54.0 / 84.0, 1e-5));

    REQUIRE(output_events.getEventCount() == 1);
    Steinberg::Vst::Event out_event{};
    REQUIRE(output_events.getEvent(0, out_event) == Steinberg::kResultOk);
    REQUIRE(out_event.type == Steinberg::Vst::Event::kNoteOnEvent);
    REQUIRE(out_event.sampleOffset == 7);
    REQUIRE(out_event.noteOn.channel == 1);
    REQUIRE(out_event.noteOn.pitch == 64);
    REQUIRE_THAT(out_event.noteOn.velocity, WithinAbs(100.0f / 127.0f, 1e-6f));

    REQUIRE(processor.terminate() == Steinberg::kResultOk);
}

TEST_CASE("VST3 getState/setState round-trip includes plugin-owned payload",
          "[vst3][state]") {
    reset_test_processor();
    HostApp host_app;

    pulp::format::vst3::PulpVst3Processor saver(create_test_processor);
    REQUIRE(saver.initialize(&host_app) == Steinberg::kResultOk);
    auto* saver_processor = TestVst3Processor::g_last_processor;
    REQUIRE(saver_processor != nullptr);
    saver_processor->state().set_value(kGainParamId, -15.0f);
    saver_processor->plugin_state = "layout=64";

    VectorStream out_stream;
    REQUIRE(saver.getState(&out_stream) == Steinberg::kResultOk);
    auto saved = out_stream.take();
    REQUIRE(saved.size() >= 4);
    REQUIRE(saved[0] == 'P');
    REQUIRE(saved[1] == 'L');
    REQUIRE(saved[2] == 'S');
    REQUIRE(saved[3] == 'T');

    pulp::format::vst3::PulpVst3Processor loader(create_test_processor);
    REQUIRE(loader.initialize(&host_app) == Steinberg::kResultOk);
    auto* loader_processor = TestVst3Processor::g_last_processor;
    REQUIRE(loader_processor != nullptr);
    loader_processor->state().set_value(kGainParamId, 9.0f);
    loader_processor->plugin_state = "stale";

    VectorStream in_stream(saved);
    REQUIRE(loader.setState(&in_stream) == Steinberg::kResultOk);
    REQUIRE_THAT(loader_processor->state().get_value(kGainParamId), WithinAbs(-15.0, 0.01));
    REQUIRE(loader_processor->plugin_state == "layout=64");

    REQUIRE(loader.terminate() == Steinberg::kResultOk);
    REQUIRE(saver.terminate() == Steinberg::kResultOk);
}

TEST_CASE("VST3 setState rejects invalid plugin payload",
          "[vst3][state]") {
    reset_test_processor();
    HostApp host_app;

    pulp::format::vst3::PulpVst3Processor loader(create_test_processor);
    REQUIRE(loader.initialize(&host_app) == Steinberg::kResultOk);
    auto* processor = TestVst3Processor::g_last_processor;
    REQUIRE(processor != nullptr);
    processor->state().set_value(kGainParamId, 7.0f);
    processor->plugin_state = "keep";

    VectorStream bad_stream(std::vector<uint8_t>{'N', 'O', 'P', 'E'});
    REQUIRE(loader.setState(&bad_stream) == Steinberg::kResultFalse);
    REQUIRE_THAT(processor->state().get_value(kGainParamId), WithinAbs(7.0, 0.01));
    REQUIRE(processor->plugin_state == "keep");

    REQUIRE(loader.terminate() == Steinberg::kResultOk);
}

TEST_CASE("VST3 getState/setState fail cleanly without a live processor",
          "[vst3][state]") {
    reset_test_processor();
    HostApp host_app;

    SECTION("after terminate") {
        pulp::format::vst3::PulpVst3Processor processor(create_test_processor);
        REQUIRE(processor.initialize(&host_app) == Steinberg::kResultOk);
        REQUIRE(processor.terminate() == Steinberg::kResultOk);

        VectorStream out_stream;
        REQUIRE(processor.getState(&out_stream) == Steinberg::kResultFalse);

        VectorStream in_stream(std::vector<uint8_t>{'N', 'O', 'P', 'E'});
        REQUIRE(processor.setState(&in_stream) == Steinberg::kResultFalse);
    }

    SECTION("null factory") {
        pulp::format::vst3::PulpVst3Processor processor(create_null_processor);
        REQUIRE(processor.initialize(&host_app) == Steinberg::kInternalError);

        VectorStream out_stream;
        REQUIRE(processor.getState(&out_stream) == Steinberg::kResultFalse);

        VectorStream in_stream(std::vector<uint8_t>{'N', 'O', 'P', 'E'});
        REQUIRE(processor.setState(&in_stream) == Steinberg::kResultFalse);
    }
}
