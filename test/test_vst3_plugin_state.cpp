#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <pulp/format/vst3_adapter.hpp>
#include <pulp/format/host_quirks.hpp>
#include <pulp/format/quirk_apply.hpp>
#include <pulp/state/parameter_event_queue.hpp>
#include <public.sdk/source/vst/hosting/eventlist.h>
#include <public.sdk/source/vst/hosting/parameterchanges.h>

#include "harness/rt_allocation_probe.hpp"

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
    bool veto_bus_layout = false;  // is_bus_layout_supported() always returns false
    bool capture_param_event_vector = true;
    int latency_samples = 0;
};

class TestVst3Processor : public pulp::format::Processor {
public:
    TestVst3Processor() : config_(g_next_config) { g_last_processor = this; }

    pulp::format::PluginDescriptor descriptor() const override {
        return config_.descriptor;
    }

    bool is_bus_layout_supported(const BusesLayout&) const override {
        // Simulate a processor that enforces a layout contract (e.g. linked
        // main/sidechain counts). When set, EVERY proposal is vetoed.
        return !config_.veto_bus_layout;
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
    void process(pulp::format::ProcessBuffers& audio,
                 pulp::midi::MidiBuffer& midi_in,
                 pulp::midi::MidiBuffer& midi_out,
                 const pulp::format::ProcessContext& context) override;

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
    int process_buffer_count = 0;
    pulp::format::PrepareContext last_prepare;
    pulp::format::ProcessContext last_context;
    std::size_t last_input_channels = 0;
    std::size_t last_output_channels = 0;
    std::size_t last_sidechain_channels = 0;
    std::size_t last_process_buffer_input_buses = 0;
    std::size_t last_process_buffer_output_buses = 0;
    std::size_t last_process_buffer_active_inputs = 0;
    std::size_t last_process_buffer_active_outputs = 0;
    bool last_process_buffer_had_sidechain = false;
    bool last_process_buffer_layouts_match = false;
    bool last_process_buffer_storage_valid = false;
    std::size_t last_midi_in_size = 0;
    std::size_t last_sysex_size = 0;
    std::vector<uint8_t> last_sysex_payload;
    std::vector<pulp::state::ParameterEvent> last_param_events;
    bool had_param_events = false;
    std::size_t last_param_event_count = 0;
    std::size_t last_param_event_capacity = 0;
    bool last_param_event_overflowed = false;
    std::uint32_t last_param_event_drops = 0;
    int32_t first_param_event_offset = -1;
    int32_t last_param_event_offset = -1;
    float first_param_event_value = 0.0f;
    float last_param_event_value = 0.0f;
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
        last_sysex_payload = midi_in.sysex()[0].data.to_vector();
    }
    had_param_events = (param_events() != nullptr);
    last_param_events.clear();
    last_param_event_count = 0;
    last_param_event_capacity = 0;
    last_param_event_overflowed = false;
    last_param_event_drops = 0;
    first_param_event_offset = -1;
    last_param_event_offset = -1;
    first_param_event_value = 0.0f;
    last_param_event_value = 0.0f;
    if (auto* events = param_events()) {
        last_param_event_count = events->size();
        last_param_event_capacity = events->capacity();
        last_param_event_overflowed = events->overflowed();
        last_param_event_drops = events->dropped_event_count();
        if (!events->empty()) {
            const auto first = events->begin();
            const auto last = events->end() - 1;
            first_param_event_offset = first->sample_offset;
            first_param_event_value = first->value;
            last_param_event_offset = last->sample_offset;
            last_param_event_value = last->value;
        }
        if (config_.capture_param_event_vector) {
            for (const auto& event : *events) last_param_events.push_back(event);
        }
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

void TestVst3Processor::process(
    pulp::format::ProcessBuffers& audio,
    pulp::midi::MidiBuffer& midi_in,
    pulp::midi::MidiBuffer& midi_out,
    const pulp::format::ProcessContext& context) {
    ++process_buffer_count;
    last_process_buffer_input_buses = audio.inputs.size();
    last_process_buffer_output_buses = audio.outputs.size();
    last_process_buffer_active_inputs = audio.inputs.active_count();
    last_process_buffer_active_outputs = audio.outputs.active_count();
    last_process_buffer_had_sidechain =
        audio.inputs.sidechain() != nullptr && audio.inputs.sidechain()->active();
    last_process_buffer_layouts_match = audio.layouts_match_descriptors();
    last_process_buffer_storage_valid = audio.active_buses_have_storage();

    pulp::format::Processor::process(audio, midi_in, midi_out, context);
}

std::unique_ptr<pulp::format::Processor> create_test_processor() {
    return std::make_unique<TestVst3Processor>();
}

std::unique_ptr<pulp::format::Processor> create_null_processor() {
    return {};
}

// A processor that stages each block through per-channel scratch sized to the
// prepared max in prepare(). An oversized render block overruns that scratch —
// which is the corruption the adapter's clamp guard prevents (the host output
// buffer alone is large enough, so internal scratch is required to exercise
// the real overrun under ASan).
class ScratchStagingProcessor final : public pulp::format::Processor {
public:
    static ScratchStagingProcessor* g_last;
    int observed_num_samples = 0;
    int prepared_max = 0;
    std::vector<std::vector<float>> scratch;

    ScratchStagingProcessor() { g_last = this; }

    pulp::format::PluginDescriptor descriptor() const override {
        return {
            .name = "Vst3ScratchStaging",
            .manufacturer = "PulpTest",
            .bundle_id = "com.pulp.test.vst3.scratch",
            .version = "1.0.0",
            .category = pulp::format::PluginCategory::Effect,
            .input_buses = {{"Audio In", 2}},
            .output_buses = {{"Audio Out", 2}},
        };
    }
    void define_parameters(pulp::state::StateStore&) override {}
    void prepare(const pulp::format::PrepareContext& context) override {
        prepared_max = context.max_buffer_size;
        scratch.assign(2,
            std::vector<float>(static_cast<std::size_t>(prepared_max), 0.0f));
    }
    void process(pulp::audio::BufferView<float>& audio_output,
                 const pulp::audio::BufferView<const float>& audio_input,
                 pulp::midi::MidiBuffer&,
                 pulp::midi::MidiBuffer&,
                 const pulp::format::ProcessContext& context) override {
        observed_num_samples = context.num_samples;
        const auto channels =
            std::min(audio_output.num_channels(), audio_input.num_channels());
        const auto frames = static_cast<std::size_t>(context.num_samples);
        for (std::size_t ch = 0; ch < channels && ch < scratch.size(); ++ch) {
            const auto* in = audio_input.channel_ptr(ch);
            auto* out = audio_output.channel_ptr(ch);
            auto& sc = scratch[ch];
            // Without the adapter's clamp, frames > prepared_max overruns sc.
            for (std::size_t i = 0; i < frames; ++i) sc[i] = in[i];
            for (std::size_t i = 0; i < frames; ++i) out[i] = sc[i];
        }
    }
};

ScratchStagingProcessor* ScratchStagingProcessor::g_last = nullptr;

std::unique_ptr<pulp::format::Processor> create_scratch_staging_processor() {
    return std::make_unique<ScratchStagingProcessor>();
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
          "[vst3][issue-493]") {
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

TEST_CASE("VST3 latency and tail report processor runtime contract",
          "[vst3][latency][tail][phase2]") {
    HostApp host_app;

    SECTION("finite latency and tail samples are reported directly") {
        TestVst3Config config;
        config.latency_samples = 192;
        config.descriptor.tail_samples = 4096;
        reset_test_processor(config);

        pulp::format::vst3::PulpVst3Processor processor(create_test_processor);
        REQUIRE(processor.initialize(&host_app) == Steinberg::kResultOk);
        REQUIRE(processor.getLatencySamples() == 192u);
        REQUIRE(processor.getTailSamples() == 4096u);
        REQUIRE(processor.terminate() == Steinberg::kResultOk);
    }

    SECTION("zero latency and tail remain zero") {
        reset_test_processor();

        pulp::format::vst3::PulpVst3Processor processor(create_test_processor);
        REQUIRE(processor.initialize(&host_app) == Steinberg::kResultOk);
        REQUIRE(processor.getLatencySamples() == 0u);
        REQUIRE(processor.getTailSamples() == 0u);
        REQUIRE(processor.terminate() == Steinberg::kResultOk);
    }

    SECTION("negative latency clamps to zero and infinite tail maps to VST3 sentinel") {
        TestVst3Config config;
        config.latency_samples = -256;
        config.descriptor.tail_samples = -1;
        reset_test_processor(config);

        pulp::format::vst3::PulpVst3Processor processor(create_test_processor);
        REQUIRE(processor.initialize(&host_app) == Steinberg::kResultOk);
        REQUIRE(processor.getLatencySamples() == 0u);
        REQUIRE(processor.getTailSamples() == Steinberg::Vst::kInfiniteTail);
        REQUIRE(processor.terminate() == Steinberg::kResultOk);
    }
}

TEST_CASE("VST3 transport jumps request processor reset through ProcessContext",
          "[vst3][transport][reset][phase2]") {
    reset_test_processor();

    HostApp host_app;
    pulp::format::vst3::PulpVst3Processor processor(create_test_processor);
    REQUIRE(processor.initialize(&host_app) == Steinberg::kResultOk);
    auto* test_processor = TestVst3Processor::g_last_processor;
    REQUIRE(test_processor != nullptr);

    Steinberg::Vst::ProcessSetup setup{};
    setup.processMode = Steinberg::Vst::kRealtime;
    setup.symbolicSampleSize = Steinberg::Vst::kSample32;
    setup.maxSamplesPerBlock = 8;
    setup.sampleRate = 48000.0;
    REQUIRE(processor.setupProcessing(setup) == Steinberg::kResultOk);

    constexpr int kFrames = 8;
    std::array<float, kFrames> in_l{};
    std::array<float, kFrames> in_r{};
    std::array<float, kFrames> out_l{};
    std::array<float, kFrames> out_r{};
    float* main_inputs[2] = {in_l.data(), in_r.data()};
    float* main_outputs[2] = {out_l.data(), out_r.data()};

    Steinberg::Vst::AudioBusBuffers audio_inputs[1]{};
    audio_inputs[0].numChannels = 2;
    audio_inputs[0].channelBuffers32 = main_inputs;
    Steinberg::Vst::AudioBusBuffers audio_outputs[1]{};
    audio_outputs[0].numChannels = 2;
    audio_outputs[0].channelBuffers32 = main_outputs;

    Steinberg::Vst::ParameterChanges input_params;
    Steinberg::Vst::ParameterChanges output_params;
    Steinberg::Vst::EventList input_events(1);
    Steinberg::Vst::EventList output_events(1);
    Steinberg::Vst::ProcessContext process_context{};
    process_context.state = Steinberg::Vst::ProcessContext::kPlaying;

    Steinberg::Vst::ProcessData data{};
    data.numSamples = kFrames;
    data.numInputs = 1;
    data.numOutputs = 1;
    data.inputs = audio_inputs;
    data.outputs = audio_outputs;
    data.inputParameterChanges = &input_params;
    data.outputParameterChanges = &output_params;
    data.inputEvents = &input_events;
    data.outputEvents = &output_events;
    data.processContext = &process_context;

    auto run_at = [&](Steinberg::int64 sample_position) {
        process_context.projectTimeSamples = sample_position;
        REQUIRE(processor.process(data) == Steinberg::kResultOk);
        return test_processor->last_context;
    };

    const auto first = run_at(1000);
    REQUIRE_FALSE(first.transport_jump);
    REQUIRE_FALSE(first.should_reset_dsp_state());

    const auto continuous = run_at(1008);
    REQUIRE_FALSE(continuous.transport_jump);
    REQUIRE_FALSE(continuous.should_reset_dsp_state());

    const auto jumped = run_at(4096);
    REQUIRE(jumped.transport_jump);
    REQUIRE(jumped.should_reset_dsp_state());

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
          "[vst3][process][issue-493]") {
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
    setup.processMode = Steinberg::Vst::kOffline;
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
    REQUIRE(test_processor->process_buffer_count == 1);
    REQUIRE(test_processor->last_process_buffer_input_buses == 2);
    REQUIRE(test_processor->last_process_buffer_output_buses == 1);
    REQUIRE(test_processor->last_process_buffer_active_inputs == 2);
    REQUIRE(test_processor->last_process_buffer_active_outputs == 1);
    REQUIRE(test_processor->last_process_buffer_had_sidechain);
    REQUIRE(test_processor->last_process_buffer_layouts_match);
    REQUIRE(test_processor->last_process_buffer_storage_valid);
    REQUIRE(test_processor->last_input_channels == 2);
    REQUIRE(test_processor->last_output_channels == 2);
    REQUIRE(test_processor->last_sidechain_channels == 1);
    REQUIRE(test_processor->last_midi_in_size == 2);
    REQUIRE(test_processor->last_sysex_size == 1);
    REQUIRE(test_processor->last_sysex_payload == std::vector<uint8_t>(sysex.begin(), sysex.end()));
    REQUIRE_THAT(test_processor->gain_seen_in_process, WithinAbs(24.0f, 1e-5f));
    REQUIRE(test_processor->last_context.is_playing);
    REQUIRE(test_processor->last_context.process_mode ==
            pulp::format::ProcessMode::Offline);
    REQUIRE(test_processor->last_context.render_speed_hint ==
            pulp::format::RenderSpeedHint::FasterThanRealtime);
    REQUIRE(test_processor->last_context.is_offline());
    REQUIRE(test_processor->last_context.allows_offline_quality_work());
    REQUIRE_FALSE(test_processor->last_context.is_maintenance_render());
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

TEST_CASE("VST3 adapter process() translates MIDI without heap allocation",
          "[vst3][process][realtime][perf]") {
    // A1: the adapter's per-block MidiBuffers are reused members reserved +
    // realtime-capacity-limited in setupProcessing(), so translating note and
    // SysEx events into them on the audio thread must not allocate. Previously
    // the buffers were block-local, allocating on the first add() of any block
    // carrying MIDI.
    TestVst3Config config;
    config.descriptor.accepts_midi = true;
    config.descriptor.produces_midi = true;
    reset_test_processor(config);

    HostApp host_app;
    pulp::format::vst3::PulpVst3Processor processor(create_test_processor);
    REQUIRE(processor.initialize(&host_app) == Steinberg::kResultOk);

    Steinberg::Vst::ProcessSetup setup{};
    setup.processMode = Steinberg::Vst::kRealtime;
    setup.symbolicSampleSize = Steinberg::Vst::kSample32;
    setup.maxSamplesPerBlock = 8;
    setup.sampleRate = 44100.0;
    REQUIRE(processor.setupProcessing(setup) == Steinberg::kResultOk);

    constexpr int kFrames = 8;
    std::array<float, kFrames> in_l{};
    std::array<float, kFrames> in_r{};
    std::array<float, kFrames> out_l{};
    std::array<float, kFrames> out_r{};
    float* main_inputs[2] = {in_l.data(), in_r.data()};
    float* main_outputs[2] = {out_l.data(), out_r.data()};
    Steinberg::Vst::AudioBusBuffers audio_inputs[1]{};
    audio_inputs[0].numChannels = 2;
    audio_inputs[0].channelBuffers32 = main_inputs;
    Steinberg::Vst::AudioBusBuffers audio_outputs[1]{};
    audio_outputs[0].numChannels = 2;
    audio_outputs[0].channelBuffers32 = main_outputs;

    std::array<uint8_t, 4> sysex{{0xF0, 0x7D, 0x01, 0xF7}};

    // Build the event list + ProcessData ONCE, outside the probe scope, so the
    // only thing measured is process() itself (EventList's ctor allocates). The
    // adapter reads input events read-only via getEvent(), so the same data is
    // safe to reuse across blocks. A block carries a note pair + a SysEx payload
    // — the allocation-prone path before A1.
    Steinberg::Vst::EventList input_events(8);
    Steinberg::Vst::Event note_on{};
    note_on.type = Steinberg::Vst::Event::kNoteOnEvent;
    note_on.sampleOffset = 1;
    note_on.noteOn.channel = 0;
    note_on.noteOn.pitch = 60;
    note_on.noteOn.velocity = 0.8f;
    REQUIRE(input_events.addEvent(note_on) == Steinberg::kResultOk);
    Steinberg::Vst::Event note_off{};
    note_off.type = Steinberg::Vst::Event::kNoteOffEvent;
    note_off.sampleOffset = 5;
    note_off.noteOff.channel = 0;
    note_off.noteOff.pitch = 60;
    note_off.noteOff.velocity = 0.0f;
    REQUIRE(input_events.addEvent(note_off) == Steinberg::kResultOk);

    // A second list that adds a SysEx payload on top of the notes.
    Steinberg::Vst::EventList midi_with_sysex(8);
    REQUIRE(midi_with_sysex.addEvent(note_on) == Steinberg::kResultOk);
    REQUIRE(midi_with_sysex.addEvent(note_off) == Steinberg::kResultOk);
    Steinberg::Vst::Event sysex_event{};
    sysex_event.type = Steinberg::Vst::Event::kDataEvent;
    sysex_event.sampleOffset = 3;
    sysex_event.data.type = Steinberg::Vst::DataEvent::kMidiSysEx;
    sysex_event.data.bytes = sysex.data();
    sysex_event.data.size = static_cast<Steinberg::uint32>(sysex.size());
    REQUIRE(midi_with_sysex.addEvent(sysex_event) == Steinberg::kResultOk);

    Steinberg::Vst::EventList empty_events(8);

    Steinberg::Vst::ProcessData data{};
    data.numSamples = kFrames;
    data.numInputs = 1;
    data.numOutputs = 1;
    data.inputs = audio_inputs;
    data.outputs = audio_outputs;

    // Differential measurement isolates the MIDI-translation cost from any
    // unrelated per-block allocation elsewhere in process() (or in the test
    // processor): a block carrying note + SysEx events must allocate no more
    // than an otherwise-identical block with no events. Before A1 the MIDI
    // block allocated (block-local MidiBuffers); after A1 the two are equal.
    auto allocs_for = [&](Steinberg::Vst::IEventList* events) -> std::size_t {
        data.inputEvents = events;
        REQUIRE(processor.process(data) == Steinberg::kResultOk);  // warm
        pulp::test::RtAllocationProbe probe;
        REQUIRE(processor.process(data) == Steinberg::kResultOk);
        return probe.allocation_count();
    };

    const std::size_t baseline = allocs_for(&empty_events);
    const std::size_t with_notes = allocs_for(&input_events);
    const std::size_t with_sysex = allocs_for(&midi_with_sysex);
    INFO("baseline=" << baseline << ", notes=" << with_notes
         << ", notes+sysex=" << with_sysex);

    // Core A1 win: note/CC translation into the reused, reserved MidiBuffer adds
    // ZERO allocations on the audio thread (was a per-block allocation when the
    // buffers were block-local). Notes/CC are the overwhelming majority of MIDI.
    REQUIRE(with_notes == baseline);

    // Known residual (tracked follow-up, NOT regressed by A1): the SysEx pooled-
    // copy path (MidiBuffer::add_sysex_copy realtime) still incurs one allocation
    // per block carrying SysEx — the cost lives inside MidiBuffer's SysexEvent
    // payload handling (core/midi/buffer.hpp), not this adapter. A1 already routes
    // SysEx through the pooled copy (no per-event std::vector ctor); fully
    // eliminating the residual is a separate core/midi slice. Asserted as a
    // no-regression upper bound so the contract is explicit, not silently ignored.
    REQUIRE(with_sysex <= baseline + 1);

    REQUIRE(processor.terminate() == Steinberg::kResultOk);
}

TEST_CASE("VST3 adapter clears SysEx between reused process blocks",
          "[vst3][process][regression]") {
    // The per-block MidiBuffers are reused members (A1). MidiBuffer::clear()
    // empties only the short-event store, so the adapter must ALSO clear_sysex()
    // each block — otherwise a SysEx payload from one block leaks into the next.
    // Process a SysEx block, then
    // an event-free block, and assert the processor sees no stale SysEx.
    TestVst3Config config;
    config.descriptor.accepts_midi = true;
    reset_test_processor(config);

    HostApp host_app;
    pulp::format::vst3::PulpVst3Processor processor(create_test_processor);
    REQUIRE(processor.initialize(&host_app) == Steinberg::kResultOk);
    auto* test_processor = TestVst3Processor::g_last_processor;
    REQUIRE(test_processor != nullptr);

    Steinberg::Vst::ProcessSetup setup{};
    setup.processMode = Steinberg::Vst::kRealtime;
    setup.symbolicSampleSize = Steinberg::Vst::kSample32;
    setup.maxSamplesPerBlock = 8;
    setup.sampleRate = 44100.0;
    REQUIRE(processor.setupProcessing(setup) == Steinberg::kResultOk);

    constexpr int kFrames = 8;
    std::array<float, kFrames> in_l{};
    std::array<float, kFrames> in_r{};
    std::array<float, kFrames> out_l{};
    std::array<float, kFrames> out_r{};
    float* main_inputs[2] = {in_l.data(), in_r.data()};
    float* main_outputs[2] = {out_l.data(), out_r.data()};
    Steinberg::Vst::AudioBusBuffers audio_inputs[1]{};
    audio_inputs[0].numChannels = 2;
    audio_inputs[0].channelBuffers32 = main_inputs;
    Steinberg::Vst::AudioBusBuffers audio_outputs[1]{};
    audio_outputs[0].numChannels = 2;
    audio_outputs[0].channelBuffers32 = main_outputs;

    std::array<uint8_t, 4> sysex{{0xF0, 0x7D, 0x01, 0xF7}};
    Steinberg::Vst::EventList sysex_events(4);
    Steinberg::Vst::Event sysex_event{};
    sysex_event.type = Steinberg::Vst::Event::kDataEvent;
    sysex_event.sampleOffset = 2;
    sysex_event.data.type = Steinberg::Vst::DataEvent::kMidiSysEx;
    sysex_event.data.bytes = sysex.data();
    sysex_event.data.size = static_cast<Steinberg::uint32>(sysex.size());
    REQUIRE(sysex_events.addEvent(sysex_event) == Steinberg::kResultOk);
    Steinberg::Vst::EventList empty_events(4);

    Steinberg::Vst::ProcessData data{};
    data.numSamples = kFrames;
    data.numInputs = 1;
    data.numOutputs = 1;
    data.inputs = audio_inputs;
    data.outputs = audio_outputs;

    // Block 1: carries one SysEx event.
    data.inputEvents = &sysex_events;
    REQUIRE(processor.process(data) == Steinberg::kResultOk);
    REQUIRE(test_processor->last_sysex_size == 1);

    // Block 2: no events. Without clear_sysex() the stale payload would persist.
    data.inputEvents = &empty_events;
    REQUIRE(processor.process(data) == Steinberg::kResultOk);
    REQUIRE(test_processor->last_sysex_size == 0);

    REQUIRE(processor.terminate() == Steinberg::kResultOk);
}

TEST_CASE("VST3 parameter automation drops past realtime event capacity without growing",
          "[vst3][params][realtime][capacity]") {
    static constexpr Steinberg::int32 kLargeBlockFrames = 4096;

    TestVst3Config config;
    config.capture_param_event_vector = false;
    reset_test_processor(config);

    HostApp host_app;
    pulp::format::vst3::PulpVst3Processor processor(create_test_processor);
    REQUIRE(processor.initialize(&host_app) == Steinberg::kResultOk);
    auto* test_processor = TestVst3Processor::g_last_processor;
    REQUIRE(test_processor != nullptr);

    Steinberg::Vst::ProcessSetup setup{};
    setup.processMode = Steinberg::Vst::kRealtime;
    setup.symbolicSampleSize = Steinberg::Vst::kSample32;
    setup.maxSamplesPerBlock = kLargeBlockFrames;
    setup.sampleRate = 48000.0;
    REQUIRE(processor.setupProcessing(setup) == Steinberg::kResultOk);

    Steinberg::Vst::ParameterChanges input_params(1);
    Steinberg::int32 param_index = 0;
    auto* gain_queue = input_params.addParameterData(kGainParamId, param_index);
    REQUIRE(gain_queue != nullptr);
    Steinberg::int32 point_index = 0;
    for (std::size_t i = 0; i < pulp::state::ParameterEventQueue::kCapacity + 1; ++i) {
        const double normalized = (i == pulp::state::ParameterEventQueue::kCapacity)
            ? 1.0
            : 0.5;
        REQUIRE(gain_queue->addPoint(static_cast<Steinberg::int32>(i),
                                     normalized,
                                     point_index) == Steinberg::kResultTrue);
    }

    Steinberg::Vst::ParameterChanges output_params(1);
    Steinberg::Vst::EventList input_events(0);
    Steinberg::Vst::EventList output_events(0);

    std::vector<float> in_l(kLargeBlockFrames, 0.0f);
    std::vector<float> in_r(kLargeBlockFrames, 0.0f);
    std::vector<float> out_l(kLargeBlockFrames, 0.0f);
    std::vector<float> out_r(kLargeBlockFrames, 0.0f);

    float* main_inputs[2] = {in_l.data(), in_r.data()};
    float* main_outputs[2] = {out_l.data(), out_r.data()};

    Steinberg::Vst::AudioBusBuffers audio_inputs[1]{};
    audio_inputs[0].numChannels = 2;
    audio_inputs[0].channelBuffers32 = main_inputs;

    Steinberg::Vst::AudioBusBuffers audio_outputs[1]{};
    audio_outputs[0].numChannels = 2;
    audio_outputs[0].channelBuffers32 = main_outputs;

    Steinberg::Vst::ProcessData data{};
    data.numSamples = kLargeBlockFrames;
    data.numInputs = 1;
    data.numOutputs = 1;
    data.inputs = audio_inputs;
    data.outputs = audio_outputs;
    data.inputParameterChanges = &input_params;
    data.outputParameterChanges = &output_params;
    data.inputEvents = &input_events;
    data.outputEvents = &output_events;

    REQUIRE(processor.process(data) == Steinberg::kResultOk);

    REQUIRE(test_processor->process_count == 1);
    REQUIRE(test_processor->last_context.num_samples == kLargeBlockFrames);
    REQUIRE(test_processor->had_param_events);
    REQUIRE(test_processor->last_param_event_count
            == pulp::state::ParameterEventQueue::kCapacity);
    REQUIRE(test_processor->last_param_event_capacity
            == pulp::state::ParameterEventQueue::kCapacity);
    REQUIRE(test_processor->last_param_event_overflowed);
    REQUIRE(test_processor->last_param_event_drops == 1);
    REQUIRE(test_processor->first_param_event_offset == 0);
    REQUIRE_THAT(test_processor->first_param_event_value, WithinAbs(-18.0f, 1e-5f));
    REQUIRE(test_processor->last_param_event_offset
            == static_cast<int32_t>(pulp::state::ParameterEventQueue::kCapacity - 1));
    REQUIRE_THAT(test_processor->last_param_event_value, WithinAbs(-18.0f, 1e-5f));
    REQUIRE_THAT(test_processor->gain_seen_in_process, WithinAbs(24.0f, 1e-5f));

    REQUIRE(processor.last_input_param_events().size()
            == pulp::state::ParameterEventQueue::kCapacity);
    REQUIRE(processor.last_input_param_events().capacity()
            == pulp::state::ParameterEventQueue::kCapacity);
    REQUIRE(processor.last_input_param_events().overflowed());
    REQUIRE(processor.last_input_param_events().dropped_event_count() == 1);

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

// ── Item 3.2 — VST3 processBlockBypassed pass-through ──────────────────────
//
// Pins three contract points:
//   * initialize() caches the StateStore ParamID of the "Bypass"
//     parameter (visible via bypass_parameter_id()).
//   * When the host sets that parameter to >= 0.5 (denormalized) before
//     process(), the adapter short-circuits to in→out copy and does NOT
//     call Processor::process().
//   * Plugins without a Bypass parameter see the short-circuit only when
//     the synthesize_bypass_parameter host-quirk is enforced (P3b), which
//     injects an automatable Bypass param the detection pass then adopts.
//     With PULP_HOST_QUIRKS=off no param is synthesized.

TEST_CASE("VST3 processBlockBypassed copies input to output without calling Processor::process",
          "[vst3][bypass][item-3.2]") {
    TestVst3Config config;
    config.add_bypass_param = true;
    reset_test_processor(config);

    HostApp host_app;
    pulp::format::vst3::PulpVst3Processor processor(create_test_processor);
    REQUIRE(processor.initialize(&host_app) == Steinberg::kResultOk);
    auto* test_processor = TestVst3Processor::g_last_processor;
    REQUIRE(test_processor != nullptr);

    // The adapter should have noticed the Bypass parameter and routed
    // its kIsBypass surface to it.
    REQUIRE(processor.bypass_parameter_id() == kBypassParamId);

    Steinberg::Vst::SpeakerArrangement inputs[1]  = {SpeakerArr::kStereo};
    Steinberg::Vst::SpeakerArrangement outputs[1] = {SpeakerArr::kStereo};
    REQUIRE(processor.setBusArrangements(inputs, 1, outputs, 1) ==
            Steinberg::kResultTrue);

    Steinberg::Vst::ProcessSetup setup{};
    setup.processMode = Steinberg::Vst::kRealtime;
    setup.symbolicSampleSize = Steinberg::Vst::kSample32;
    setup.maxSamplesPerBlock = 4;
    setup.sampleRate = 48000.0;
    REQUIRE(processor.setupProcessing(setup) == Steinberg::kResultOk);

    constexpr int kFrames = 4;
    std::array<float, kFrames> in_l{{0.1f, 0.2f, 0.3f, 0.4f}};
    std::array<float, kFrames> in_r{{-0.1f, -0.2f, -0.3f, -0.4f}};
    std::array<float, kFrames> out_l{};
    std::array<float, kFrames> out_r{};
    out_l.fill(99.0f); // sentinel — must be overwritten by pass-through copy
    out_r.fill(99.0f);

    float* main_inputs[2]  = {in_l.data(), in_r.data()};
    float* main_outputs[2] = {out_l.data(), out_r.data()};

    Steinberg::Vst::AudioBusBuffers audio_inputs[1]{};
    audio_inputs[0].numChannels = 2;
    audio_inputs[0].channelBuffers32 = main_inputs;
    Steinberg::Vst::AudioBusBuffers audio_outputs[1]{};
    audio_outputs[0].numChannels = 2;
    audio_outputs[0].channelBuffers32 = main_outputs;

    // Engage bypass via an input parameter change at sample 0.
    // VST3 hosts deliver bypass via the kIsBypass parameter on the
    // normalized lane (0..1).
    Steinberg::Vst::ParameterChanges input_params(1);
    Steinberg::int32 q_index = 0;
    auto* bypass_queue = input_params.addParameterData(kBypassParamId, q_index);
    REQUIRE(bypass_queue != nullptr);
    Steinberg::int32 pt_index = 0;
    REQUIRE(bypass_queue->addPoint(0, 1.0, pt_index) == Steinberg::kResultTrue);

    Steinberg::Vst::ProcessData data{};
    data.numSamples = kFrames;
    data.numInputs = 1;
    data.numOutputs = 1;
    data.inputs = audio_inputs;
    data.outputs = audio_outputs;
    data.inputParameterChanges = &input_params;

    const int before = test_processor->process_count;
    REQUIRE(processor.process(data) == Steinberg::kResultOk);

    // Pass-through must have copied input → output verbatim.
    for (int i = 0; i < kFrames; ++i) {
        REQUIRE_THAT(out_l[i], WithinAbs(in_l[i], 1e-6f));
        REQUIRE_THAT(out_r[i], WithinAbs(in_r[i], 1e-6f));
    }
    // The Processor must NOT have been called.
    REQUIRE(test_processor->process_count == before);

    // Releasing bypass restores the normal process() path. Reset
    // outputs and run again with bypass = 0.
    out_l.fill(99.0f);
    out_r.fill(99.0f);
    Steinberg::Vst::ParameterChanges release_params(1);
    auto* release_queue = release_params.addParameterData(kBypassParamId, q_index);
    REQUIRE(release_queue != nullptr);
    pt_index = 0;
    REQUIRE(release_queue->addPoint(0, 0.0, pt_index) == Steinberg::kResultTrue);
    data.inputParameterChanges = &release_params;

    REQUIRE(processor.process(data) == Steinberg::kResultOk);
    REQUIRE(test_processor->process_count == before + 1);
    // TestVst3Processor::process copies input → output, so out should
    // also match in — but importantly, the Processor::process counter
    // moved.
}

TEST_CASE("VST3 adapter without a Bypass parameter never short-circuits",
          "[vst3][bypass][item-3.2]") {
    // Since host-quirks P3b the adapter SYNTHESIZES a Bypass param by
    // default, so the "no bypass surface at all" scenario this test pins
    // only exists when synthesize_bypass_parameter is disabled.
    pulp::format::set_host_quirk_policy(pulp::format::kQuirkFilterOff);

    TestVst3Config config; // add_bypass_param defaults to false
    reset_test_processor(config);

    HostApp host_app;
    pulp::format::vst3::PulpVst3Processor processor(create_test_processor);
    REQUIRE(processor.initialize(&host_app) == Steinberg::kResultOk);

    // No "Bypass" parameter declared by the plugin AND none synthesized —
    // the adapter reports the no-op sentinel ID 0 so process() never
    // short-circuits.
    REQUIRE(processor.bypass_parameter_id() == 0u);

    pulp::format::set_host_quirk_policy(std::nullopt);
}

// ─────────────────────────────────────────────────────────────────────
// host-quirks P3b — synthesize_bypass_parameter, end-to-end (VST3).
//
// A plugin that declares NO Bypass parameter: with the quirk enforced the
// adapter synthesizes an automatable "Bypass" param (reserved ID), the
// existing detection tags it kIsBypass, and process() honors it with the
// pass-through short-circuit. With PULP_HOST_QUIRKS=off nothing is
// synthesized (original behavior).
// ─────────────────────────────────────────────────────────────────────

TEST_CASE("VST3 synthesizes an automatable Bypass param when the plugin declares none",
          "[vst3][host-quirks][p3][bypass]") {
    pulp::format::set_host_quirk_policy(pulp::format::QuirkFilter{});  // quirk on

    TestVst3Config config;
    config.add_bypass_param = false;  // plugin declares ONLY Gain
    reset_test_processor(config);
    HostApp host_app;
    pulp::format::vst3::PulpVst3Processor processor(create_test_processor);
    REQUIRE(processor.initialize(&host_app) == Steinberg::kResultOk);
    auto* test_processor = TestVst3Processor::g_last_processor;
    REQUIRE(test_processor != nullptr);

    // A synthesized Bypass now exists, carrying the reserved ID + kIsBypass.
    REQUIRE(processor.getParameterCount() == 2);  // Gain + synthesized Bypass
    REQUIRE(processor.bypass_parameter_id() ==
            pulp::format::kSynthesizedBypassParamId);

    Steinberg::Vst::SpeakerArrangement io[1] = {SpeakerArr::kStereo};
    REQUIRE(processor.setBusArrangements(io, 1, io, 1) == Steinberg::kResultTrue);
    Steinberg::Vst::ProcessSetup setup{};
    setup.processMode = Steinberg::Vst::kRealtime;
    setup.symbolicSampleSize = Steinberg::Vst::kSample32;
    setup.maxSamplesPerBlock = 4;
    setup.sampleRate = 48000.0;
    REQUIRE(processor.setupProcessing(setup) == Steinberg::kResultOk);

    constexpr int kFrames = 4;
    std::array<float, kFrames> in_l{{0.1f, 0.2f, 0.3f, 0.4f}};
    std::array<float, kFrames> in_r{{-0.1f, -0.2f, -0.3f, -0.4f}};
    std::array<float, kFrames> out_l{};
    std::array<float, kFrames> out_r{};
    out_l.fill(99.0f);
    out_r.fill(99.0f);
    float* ins[2]  = {in_l.data(), in_r.data()};
    float* outs[2] = {out_l.data(), out_r.data()};
    Steinberg::Vst::AudioBusBuffers ab_in[1]{};
    ab_in[0].numChannels = 2; ab_in[0].channelBuffers32 = ins;
    Steinberg::Vst::AudioBusBuffers ab_out[1]{};
    ab_out[0].numChannels = 2; ab_out[0].channelBuffers32 = outs;

    // Engage the SYNTHESIZED bypass via its reserved ID → pass-through.
    Steinberg::Vst::ParameterChanges params(1);
    Steinberg::int32 q_index = 0;
    auto* queue = params.addParameterData(
        static_cast<Steinberg::Vst::ParamID>(pulp::format::kSynthesizedBypassParamId),
        q_index);
    REQUIRE(queue != nullptr);
    Steinberg::int32 pt = 0;
    REQUIRE(queue->addPoint(0, 1.0, pt) == Steinberg::kResultTrue);

    Steinberg::Vst::ProcessData data{};
    data.numSamples = kFrames;
    data.numInputs = 1; data.numOutputs = 1;
    data.inputs = ab_in; data.outputs = ab_out;
    data.inputParameterChanges = &params;

    const int before = test_processor->process_count;
    REQUIRE(processor.process(data) == Steinberg::kResultOk);
    // Synthesized bypass engaged → input copied through, Processor skipped.
    for (int i = 0; i < kFrames; ++i) {
        REQUIRE_THAT(out_l[i], WithinAbs(in_l[i], 1e-6f));
        REQUIRE_THAT(out_r[i], WithinAbs(in_r[i], 1e-6f));
    }
    REQUIRE(test_processor->process_count == before);

    pulp::format::set_host_quirk_policy(std::nullopt);
}

TEST_CASE("VST3 does NOT synthesize a Bypass param when the quirk is off",
          "[vst3][host-quirks][p3][bypass]") {
    pulp::format::set_host_quirk_policy(pulp::format::kQuirkFilterOff);

    TestVst3Config config;
    config.add_bypass_param = false;
    reset_test_processor(config);
    HostApp host_app;
    pulp::format::vst3::PulpVst3Processor processor(create_test_processor);
    REQUIRE(processor.initialize(&host_app) == Steinberg::kResultOk);

    // No synthesis: only the plugin's own Gain param, no bypass surface.
    REQUIRE(processor.getParameterCount() == 1);
    REQUIRE(processor.bypass_parameter_id() == 0);

    pulp::format::set_host_quirk_policy(std::nullopt);
}

// ─────────────────────────────────────────────────────────────────────
// host-quirks P3c — silence_unsupported_bus_arrangements, end-to-end.
//
// Empirical proof the VST3 adapter RESPECTS the quirk: with it enforced
// (default), setBusArrangements accepts an arrangement the processor does
// NOT natively support (6-ch 5.1) instead of failing, the processor still
// runs at its prepared (stereo) channel count, and the host's extra output
// channels are silenced. With PULP_HOST_QUIRKS=off the original
// reject-the-proposal behavior is preserved exactly.
// ─────────────────────────────────────────────────────────────────────

TEST_CASE("VST3 accepts an unsupported arrangement and silences extras when the quirk is enforced",
          "[vst3][host-quirks][p3][bus-arrangement]") {
    pulp::format::set_host_quirk_policy(pulp::format::QuirkFilter{});  // all tiers → quirk on

    TestVst3Config config;  // stereo in / stereo out
    reset_test_processor(config);
    HostApp host_app;
    pulp::format::vst3::PulpVst3Processor processor(create_test_processor);
    REQUIRE(processor.initialize(&host_app) == Steinberg::kResultOk);
    auto* test_processor = TestVst3Processor::g_last_processor;
    REQUIRE(test_processor != nullptr);

    Steinberg::Vst::ProcessSetup setup{};
    setup.processMode = Steinberg::Vst::kRealtime;
    setup.symbolicSampleSize = Steinberg::Vst::kSample32;
    setup.maxSamplesPerBlock = 64;
    setup.sampleRate = 48000.0;
    REQUIRE(processor.setupProcessing(setup) == Steinberg::kResultOk);

    // Host requests a 5.1 (6-channel) output — not mono/stereo, so the
    // processor cannot natively support it. With the quirk enforced the
    // adapter accepts rather than returning kResultFalse.
    Steinberg::Vst::SpeakerArrangement inputs[1]  = {SpeakerArr::kStereo};
    Steinberg::Vst::SpeakerArrangement outputs[1] = {SpeakerArr::k51};
    REQUIRE(processor.setBusArrangements(inputs, 1, outputs, 1) == Steinberg::kResultTrue);

    // Drive a process block with a 6-channel output buffer; pre-fill with a
    // sentinel so we can tell "silenced" (0) from "left untouched" (9).
    constexpr int kFrames = 8;
    std::array<float, kFrames> in_l{{0.1f, 0.2f, 0.3f, 0.4f, 0.5f, 0.6f, 0.7f, 0.8f}};
    std::array<float, kFrames> in_r{{-0.1f, -0.2f, -0.3f, -0.4f, -0.5f, -0.6f, -0.7f, -0.8f}};
    std::array<std::array<float, kFrames>, 6> outs{};
    for (auto& o : outs) o.fill(9.0f);

    float* main_inputs[2] = {in_l.data(), in_r.data()};
    float* main_outputs[6];
    for (int ch = 0; ch < 6; ++ch) main_outputs[ch] = outs[ch].data();

    Steinberg::Vst::AudioBusBuffers audio_inputs[1]{};
    audio_inputs[0].numChannels = 2;
    audio_inputs[0].channelBuffers32 = main_inputs;
    Steinberg::Vst::AudioBusBuffers audio_outputs[1]{};
    audio_outputs[0].numChannels = 6;
    audio_outputs[0].channelBuffers32 = main_outputs;

    Steinberg::Vst::ParameterChanges input_params;
    Steinberg::Vst::ParameterChanges output_params;
    Steinberg::Vst::EventList input_events(4);
    Steinberg::Vst::EventList output_events(4);
    Steinberg::Vst::ProcessContext process_context{};

    Steinberg::Vst::ProcessData data{};
    data.numSamples = kFrames;
    data.numInputs = 1;
    data.numOutputs = 1;
    data.inputs = audio_inputs;
    data.outputs = audio_outputs;
    data.inputParameterChanges = &input_params;
    data.outputParameterChanges = &output_params;
    data.inputEvents = &input_events;
    data.outputEvents = &output_events;
    data.processContext = &process_context;

    REQUIRE(processor.process(data) == Steinberg::kResultOk);

    // The processor saw only its prepared (stereo) channel count — not 6.
    REQUIRE(test_processor->last_output_channels == 2);

    // Channels 0..1: the processor copied the input through.
    REQUIRE_THAT(outs[0][0], WithinAbs(0.1, 1e-6));
    REQUIRE_THAT(outs[1][0], WithinAbs(-0.1, 1e-6));
    // Channels 2..5: silenced (0), NOT the 9.0 sentinel and NOT garbage.
    for (int ch = 2; ch < 6; ++ch) {
        for (int s = 0; s < kFrames; ++s) {
            REQUIRE_THAT(outs[ch][s], WithinAbs(0.0, 1e-9));
        }
    }

    pulp::format::set_host_quirk_policy(std::nullopt);
}

TEST_CASE("VST3 rejects an unsupported arrangement when silence accommodation is off",
          "[vst3][host-quirks][p3][bus-arrangement]") {
    pulp::format::set_host_quirk_policy(pulp::format::kQuirkFilterOff);

    TestVst3Config config;
    reset_test_processor(config);
    HostApp host_app;
    pulp::format::vst3::PulpVst3Processor processor(create_test_processor);
    REQUIRE(processor.initialize(&host_app) == Steinberg::kResultOk);

    Steinberg::Vst::ProcessSetup setup{};
    setup.processMode = Steinberg::Vst::kRealtime;
    setup.symbolicSampleSize = Steinberg::Vst::kSample32;
    setup.maxSamplesPerBlock = 64;
    setup.sampleRate = 48000.0;
    REQUIRE(processor.setupProcessing(setup) == Steinberg::kResultOk);

    Steinberg::Vst::SpeakerArrangement inputs[1]  = {SpeakerArr::kStereo};
    Steinberg::Vst::SpeakerArrangement outputs[1] = {SpeakerArr::k51};
    // Quirk off → original behavior: reject the unsupported proposal.
    REQUIRE(processor.setBusArrangements(inputs, 1, outputs, 1) == Steinberg::kResultFalse);

    pulp::format::set_host_quirk_policy(std::nullopt);
}

// Self-sweep hardening (2026-05-30): the bypass pass-through must null-check
// the destination channel pointer. A VST3 bus can report numChannels > 0
// while an individual channelBuffers32[ch] is null (#178); without the guard
// the bypass short-circuit dereferenced null on the audio thread — a crash
// P3b widened by making the short-circuit reachable for synthesized bypass.
TEST_CASE("VST3 bypass pass-through tolerates a null output channel pointer",
          "[vst3][bypass][regression]") {
    TestVst3Config config;
    config.add_bypass_param = true;  // declared bypass — policy-independent
    reset_test_processor(config);
    HostApp host_app;
    pulp::format::vst3::PulpVst3Processor processor(create_test_processor);
    REQUIRE(processor.initialize(&host_app) == Steinberg::kResultOk);
    REQUIRE(processor.bypass_parameter_id() == kBypassParamId);

    Steinberg::Vst::SpeakerArrangement io[1] = {SpeakerArr::kStereo};
    REQUIRE(processor.setBusArrangements(io, 1, io, 1) == Steinberg::kResultTrue);
    Steinberg::Vst::ProcessSetup setup{};
    setup.processMode = Steinberg::Vst::kRealtime;
    setup.symbolicSampleSize = Steinberg::Vst::kSample32;
    setup.maxSamplesPerBlock = 4;
    setup.sampleRate = 48000.0;
    REQUIRE(processor.setupProcessing(setup) == Steinberg::kResultOk);

    constexpr int kFrames = 4;
    std::array<float, kFrames> in_l{{0.1f, 0.2f, 0.3f, 0.4f}};
    std::array<float, kFrames> in_r{{-0.1f, -0.2f, -0.3f, -0.4f}};
    std::array<float, kFrames> out_l{};
    out_l.fill(99.0f);
    float* ins[2]  = {in_l.data(), in_r.data()};
    // Channel 1's output buffer is NULL — the host reports 2 channels but
    // only provides one live pointer.
    float* outs[2] = {out_l.data(), nullptr};
    Steinberg::Vst::AudioBusBuffers ab_in[1]{};
    ab_in[0].numChannels = 2; ab_in[0].channelBuffers32 = ins;
    Steinberg::Vst::AudioBusBuffers ab_out[1]{};
    ab_out[0].numChannels = 2; ab_out[0].channelBuffers32 = outs;

    Steinberg::Vst::ParameterChanges params(1);
    Steinberg::int32 q_index = 0;
    auto* queue = params.addParameterData(kBypassParamId, q_index);
    REQUIRE(queue != nullptr);
    Steinberg::int32 pt = 0;
    REQUIRE(queue->addPoint(0, 1.0, pt) == Steinberg::kResultTrue);  // engage bypass

    Steinberg::Vst::ProcessData data{};
    data.numSamples = kFrames;
    data.numInputs = 1; data.numOutputs = 1;
    data.inputs = ab_in; data.outputs = ab_out;
    data.inputParameterChanges = &params;

    // Must not dereference the null channel-1 pointer.
    REQUIRE(processor.process(data) == Steinberg::kResultOk);
    // The live channel 0 still got the pass-through copy.
    for (int i = 0; i < kFrames; ++i) {
        REQUIRE_THAT(out_l[i], WithinAbs(in_l[i], 1e-6f));
    }
}

// Regression (#3235): the silence accommodation must NOT override a
// processor's veto of a mono/stereo layout (a real contract, e.g. linked
// main/sidechain counts) — there are no extra channels to silence, so
// running process() under it would be a correctness bug. The veto is
// honored even with the quirk enforced (pre-P3c behavior for mono/stereo).
TEST_CASE("VST3 honors a processor mono/stereo bus-layout veto even with the quirk on",
          "[vst3][host-quirks][p3][bus-arrangement]") {
    pulp::format::set_host_quirk_policy(pulp::format::QuirkFilter{});  // quirk on
    TestVst3Config config;
    config.veto_bus_layout = true;  // processor rejects every proposed layout
    reset_test_processor(config);
    HostApp host_app;
    pulp::format::vst3::PulpVst3Processor processor(create_test_processor);
    REQUIRE(processor.initialize(&host_app) == Steinberg::kResultOk);

    // Stereo in/out is mono/stereo-expressible, so is_bus_layout_supported()
    // is consulted — the processor vetoes it. With the quirk on this is now
    // REJECTED (no silence accommodation for vetoed mono/stereo layouts).
    Steinberg::Vst::SpeakerArrangement io[1] = {SpeakerArr::kStereo};
    REQUIRE(processor.setBusArrangements(io, 1, io, 1) == Steinberg::kResultFalse);

    pulp::format::set_host_quirk_policy(std::nullopt);
}

// A spec-violating host that renders MORE frames than the prepared
// maxSamplesPerBlock must not overrun the processor's prepared scratch.
// The adapter clamps the processed region to the prepared max and zeros the
// un-processable tail so it reads back as clean silence. (Un-fixed, this
// path overruns the prepared buffers and trips ASan.)
TEST_CASE("VST3 clamps an oversized render block and zeros the tail",
          "[vst3][rt-safety][process]") {
    ScratchStagingProcessor::g_last = nullptr;

    HostApp host_app;
    pulp::format::vst3::PulpVst3Processor processor(create_scratch_staging_processor);
    REQUIRE(processor.initialize(&host_app) == Steinberg::kResultOk);
    auto* test_processor = ScratchStagingProcessor::g_last;
    REQUIRE(test_processor != nullptr);

    constexpr int kPreparedMax = 64;
    constexpr int kRenderFrames = 256;  // host exceeds the advertised max

    Steinberg::Vst::ProcessSetup setup{};
    setup.processMode = Steinberg::Vst::kRealtime;
    setup.symbolicSampleSize = Steinberg::Vst::kSample32;
    setup.maxSamplesPerBlock = kPreparedMax;
    setup.sampleRate = 48000.0;
    REQUIRE(processor.setupProcessing(setup) == Steinberg::kResultOk);

    // Host-provided buffers are sized to the LARGER render count. Input is a
    // sentinel value across the whole block; the unity test processor copies
    // input -> output for the frames it processes.
    std::array<float, kRenderFrames> in_l{};
    std::array<float, kRenderFrames> in_r{};
    std::array<float, kRenderFrames> out_l{};
    std::array<float, kRenderFrames> out_r{};
    in_l.fill(0.5f);
    in_r.fill(0.5f);
    // Pre-fill outputs with garbage so a clean tail proves the adapter zeroed
    // it rather than the buffer happening to be zero.
    out_l.fill(-9.0f);
    out_r.fill(-9.0f);

    float* main_inputs[2] = {in_l.data(), in_r.data()};
    float* main_outputs[2] = {out_l.data(), out_r.data()};

    Steinberg::Vst::AudioBusBuffers audio_inputs[1]{};
    audio_inputs[0].numChannels = 2;
    audio_inputs[0].channelBuffers32 = main_inputs;
    Steinberg::Vst::AudioBusBuffers audio_outputs[1]{};
    audio_outputs[0].numChannels = 2;
    audio_outputs[0].channelBuffers32 = main_outputs;

    Steinberg::Vst::ParameterChanges input_params;
    Steinberg::Vst::ParameterChanges output_params;
    Steinberg::Vst::ProcessData data{};
    data.numSamples = kRenderFrames;
    data.numInputs = 1;
    data.numOutputs = 1;
    data.inputs = audio_inputs;
    data.outputs = audio_outputs;
    data.inputParameterChanges = &input_params;
    data.outputParameterChanges = &output_params;

    // (a) No crash / no overrun — the core guarantee (would trip ASan unfixed).
    REQUIRE(processor.process(data) == Steinberg::kResultOk);

    // (b) The processor saw only the prepared-max count.
    REQUIRE(test_processor->observed_num_samples == kPreparedMax);

    // (c) The first kPreparedMax frames were processed (unity copy).
    for (int i = 0; i < kPreparedMax; ++i) {
        REQUIRE(out_l[i] == 0.5f);
        REQUIRE(out_r[i] == 0.5f);
    }
    // (d) The un-processable tail [kPreparedMax, kRenderFrames) is silence.
    for (int i = kPreparedMax; i < kRenderFrames; ++i) {
        REQUIRE(out_l[i] == 0.0f);
        REQUIRE(out_r[i] == 0.0f);
    }

    REQUIRE(processor.terminate() == Steinberg::kResultOk);
}
