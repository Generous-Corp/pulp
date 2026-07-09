#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <pulp/format/vst3_adapter.hpp>
#include <pulp/format/host_quirks.hpp>
#include <pulp/format/quirk_apply.hpp>
#include <pulp/format/detail/vst3_midi_mapping.hpp>
#include <pulp/midi/message.hpp>
#include <pulp/midi/mpe_buffer.hpp>
#include <pluginterfaces/vst/ivstnoteexpression.h>
#include <pluginterfaces/vst/ivsteditcontroller.h>
#include <pulp/state/parameter_event_queue.hpp>
#include <pulp/events/main_thread_dispatcher.hpp>
#include <pluginterfaces/vst/ivstmidicontrollers.h>
#include <public.sdk/source/vst/hosting/eventlist.h>
#include <public.sdk/source/vst/hosting/parameterchanges.h>

#include "harness/rt_allocation_probe.hpp"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <string>
#include <utility>
#include <atomic>
#include <thread>
#include <vector>

using Catch::Matchers::WithinAbs;

namespace {

using Steinberg::Vst::ParameterInfo;
namespace SpeakerArr = Steinberg::Vst::SpeakerArr;

constexpr pulp::state::ParamID kGainParamId = 1;
constexpr pulp::state::ParamID kBypassParamId = 2;
constexpr pulp::state::ParamID kResetParamId = 3;

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

// Controllable MainThreadDispatcher backend for deterministic testing of the
// adapter's paced restart poll. It registers itself as the active backend
// (last registration wins) and captures every posted / delayed task instead of
// running it on a live OS run loop, so a test can drive ticks by hand and
// observe re-scheduling, delivery, and post-teardown no-ops. `is_main_thread`
// reports true (the test thread acts as the main thread here).
class ScopedMainThreadBackend {
public:
    ScopedMainThreadBackend() {
        pulp::events::MainThreadDispatcher::Backend backend;
        backend.post = [this](pulp::events::Task task) {
            immediate_.push_back(std::move(task));
            return true;
        };
        backend.is_main_thread = [] { return true; };
        backend.post_after = [this](pulp::events::Task task, int) {
            delayed_.push_back(std::move(task));
            return true;
        };
        token_ = pulp::events::MainThreadDispatcher::register_backend(std::move(backend));
    }
    ~ScopedMainThreadBackend() {
        if (token_ != 0)
            pulp::events::MainThreadDispatcher::unregister_backend(token_);
    }

    ScopedMainThreadBackend(const ScopedMainThreadBackend&) = delete;
    ScopedMainThreadBackend& operator=(const ScopedMainThreadBackend&) = delete;

    std::size_t delayed_pending() const { return delayed_.size(); }
    std::size_t immediate_pending() const { return immediate_.size(); }

    // Run every currently-queued delayed task once. Tasks the running tasks
    // enqueue (the next poll tick) are NOT run in the same call — they land in
    // a fresh queue and require another run_delayed() to fire, matching a real
    // timer's one-tick-per-interval cadence.
    int run_delayed() {
        std::vector<pulp::events::Task> batch;
        batch.swap(delayed_);
        for (auto& t : batch)
            if (t) t();
        return static_cast<int>(batch.size());
    }
    int run_immediate() {
        std::vector<pulp::events::Task> batch;
        batch.swap(immediate_);
        for (auto& t : batch)
            if (t) t();
        return static_cast<int>(batch.size());
    }

private:
    pulp::events::MainThreadDispatcher::Token token_ = 0;
    std::vector<pulp::events::Task> immediate_;
    std::vector<pulp::events::Task> delayed_;
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
    // When set, the test processor calls push_output_param_event() twice from
    // process() — at output_offset_1 and output_offset_2 — exercising the
    // sample-accurate output-parameter drain path.
    bool push_output_param_events = false;
    pulp::state::ParamID output_param_id = kGainParamId;
    float output_value_1 = -12.0f;
    float output_value_2 = 6.0f;
    std::uint32_t output_offset_1 = 16;
    std::uint32_t output_offset_2 = 48;
    // When set, the test processor flags a latency / tail change from inside
    // process() (the audio-thread render callback), exercising the adapter's
    // RT-safe restart-publisher path. It flags once on the first block so the
    // edge is observable without flagging on every block.
    bool flag_latency_in_process = false;
    bool flag_tail_in_process = false;
    bool veto_bus_layout = false;  // is_bus_layout_supported() always returns false
    bool capture_param_event_vector = true;
    int latency_samples = 0;
    // When set, declare a real plug-in parameter whose ID lands inside the
    // reserved MIDI-controller range, to exercise the collision-aware
    // diversion predicate.
    bool add_colliding_param = false;
    pulp::state::ParamID colliding_param_id = 0;
    // When set, declare a Bypass-DESIGNATED parameter with a non-boolean
    // (continuous) range and a name that is NOT "Bypass" — exercises the
    // designation-first path: the adapter must tag kIsBypass and FORCE a
    // two-state stepCount regardless of the continuous range.
    bool add_designated_continuous_bypass = false;
    // When set, declare a Reset-designated (momentary trigger) parameter so a
    // test can raise it and assert the adapter auto-resets it after the block,
    // even when the block is bypassed.
    bool add_reset_trigger_param = false;
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
        if (config_.add_colliding_param) {
            store.add_parameter({
                .id = config_.colliding_param_id,
                .name = "Collider",
                .range = {0.0f, 1.0f, 0.0f, 0.01f},
            });
        }
        if (config_.add_designated_continuous_bypass) {
            pulp::state::ParamInfo info;
            info.id = kBypassParamId;
            info.name = "Engine Active";  // deliberately not "Bypass"
            info.range = {0.0f, 4.0f, 0.0f, 0.25f};  // continuous, non-boolean
            info.designation = pulp::state::ParamDesignation::Bypass;
            store.add_parameter(info);
        }
        if (config_.add_reset_trigger_param) {
            pulp::state::ParamInfo info;
            info.id = kResetParamId;
            info.name = "Panic";
            info.range = {0.0f, 1.0f, 0.0f, 1.0f};  // default 0
            info.designation = pulp::state::ParamDesignation::Reset;
            store.add_parameter(info);
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
    std::vector<pulp::midi::MidiEvent> last_midi_in_events;
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
    bool flagged_restart_in_process_ = false;
    // Per-note expression (MPE) sidecar observation, captured each process().
    bool mpe_input_attached = false;
    std::size_t mpe_event_count = 0;
    std::size_t mpe_capacity = 0;
    std::uint32_t mpe_drops = 0;
    std::vector<pulp::midi::MpeExpressionEvent> last_mpe_events;
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
    last_midi_in_events.assign(midi_in.begin(), midi_in.end());
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

    // Snapshot the MPE sidecar the adapter handed us for this block.
    mpe_input_attached = (mpe_input() != nullptr);
    mpe_event_count = 0;
    mpe_capacity = 0;
    mpe_drops = 0;
    last_mpe_events.clear();
    if (const auto* mpe = mpe_input()) {
        mpe_event_count = mpe->size();
        mpe_capacity = mpe->capacity();
        mpe_drops = mpe->dropped_event_count();
        last_mpe_events.assign(mpe->begin(), mpe->end());
    }

    const auto channels = std::min(audio_output.num_channels(), audio_input.num_channels());
    for (std::size_t ch = 0; ch < channels; ++ch) {
        std::copy(audio_input.channel(ch).begin(), audio_input.channel(ch).end(),
                  audio_output.channel(ch).begin());
    }

    if (config_.mutate_gain_in_process) {
        state().set_value(kGainParamId, -6.0f);
    }
    if (config_.push_output_param_events) {
        // Two explicit sample-accurate output events for the same param, pushed
        // out of ascending order to prove the adapter globally sorts them into
        // one ascending-offset VST3 queue.
        push_output_param_event(config_.output_param_id, config_.output_value_2,
                                config_.output_offset_2);
        push_output_param_event(config_.output_param_id, config_.output_value_1,
                                config_.output_offset_1);
    }
    // Flag a latency / tail change from the audio thread exactly once, on the
    // first block. The adapter must marshal the resulting restartComponent call
    // to the main thread; it must NOT fire the host callback from here.
    if (!flagged_restart_in_process_) {
        if (config_.flag_latency_in_process) flag_latency_changed();
        if (config_.flag_tail_in_process) flag_tail_changed();
        if (config_.flag_latency_in_process || config_.flag_tail_in_process) {
            flagged_restart_in_process_ = true;
        }
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

// Capturing IComponentHandler: records every restartComponent call — the
// coalesced flags, a call count, and the std::thread::id it ran on — so a test
// can assert the host callback never fires on the audio (process) thread and
// does fire, with the right flags, on the main/poll thread.
class CapturingComponentHandler final : public Steinberg::Vst::IComponentHandler {
public:
    Steinberg::tresult PLUGIN_API beginEdit(Steinberg::Vst::ParamID) override {
        return Steinberg::kResultOk;
    }
    Steinberg::tresult PLUGIN_API performEdit(Steinberg::Vst::ParamID,
                                              Steinberg::Vst::ParamValue) override {
        return Steinberg::kResultOk;
    }
    Steinberg::tresult PLUGIN_API endEdit(Steinberg::Vst::ParamID) override {
        return Steinberg::kResultOk;
    }
    Steinberg::tresult PLUGIN_API restartComponent(Steinberg::int32 flags) override {
        ++restart_calls;
        last_flags = flags;
        accumulated_flags |= flags;
        last_thread = std::this_thread::get_id();
        return Steinberg::kResultOk;
    }

    Steinberg::tresult PLUGIN_API queryInterface(const Steinberg::TUID iid,
                                                 void** obj) override {
        if (Steinberg::FUnknownPrivate::iidEqual(iid, Steinberg::Vst::IComponentHandler::iid) ||
            Steinberg::FUnknownPrivate::iidEqual(iid, Steinberg::FUnknown::iid)) {
            *obj = static_cast<Steinberg::Vst::IComponentHandler*>(this);
            return Steinberg::kResultTrue;
        }
        *obj = nullptr;
        return Steinberg::kNoInterface;
    }
    Steinberg::uint32 PLUGIN_API addRef() override { return 1; }
    Steinberg::uint32 PLUGIN_API release() override { return 1; }

    int restart_calls = 0;
    Steinberg::int32 last_flags = 0;
    Steinberg::int32 accumulated_flags = 0;
    std::thread::id last_thread{};
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

TEST_CASE("VST3 processes kSample64 buffers through the f32 boundary path",
          "[vst3][audio][f64]") {
    reset_test_processor();

    HostApp host_app;
    pulp::format::vst3::PulpVst3Processor processor(create_test_processor);
    REQUIRE(processor.initialize(&host_app) == Steinberg::kResultOk);
    auto* test_processor = TestVst3Processor::g_last_processor;
    REQUIRE(test_processor != nullptr);

    REQUIRE(processor.canProcessSampleSize(Steinberg::Vst::kSample32) == Steinberg::kResultTrue);
    REQUIRE(processor.canProcessSampleSize(Steinberg::Vst::kSample64) == Steinberg::kResultTrue);
    REQUIRE(processor.canProcessSampleSize(-1) == Steinberg::kResultFalse);

    Steinberg::Vst::ProcessSetup setup{};
    setup.processMode = Steinberg::Vst::kRealtime;
    setup.symbolicSampleSize = Steinberg::Vst::kSample64;
    setup.maxSamplesPerBlock = 8;
    setup.sampleRate = 48000.0;
    REQUIRE(processor.setupProcessing(setup) == Steinberg::kResultOk);
    REQUIRE(processor.setActive(true) == Steinberg::kResultOk);

    constexpr int kFrames = 8;
    std::array<double, kFrames> in_l{0.1, -0.25, 0.5, -0.75, 1.0, -1.25, 1.5, -1.75};
    std::array<double, kFrames> in_r{-0.2, 0.3, -0.4, 0.6, -0.8, 1.2, -1.6, 2.4};
    std::array<double, kFrames> out_l{};
    std::array<double, kFrames> out_r{};
    double* main_inputs[2] = {in_l.data(), in_r.data()};
    double* main_outputs[2] = {out_l.data(), out_r.data()};

    Steinberg::Vst::AudioBusBuffers audio_inputs[1]{};
    audio_inputs[0].numChannels = 2;
    audio_inputs[0].channelBuffers64 = main_inputs;
    Steinberg::Vst::AudioBusBuffers audio_outputs[1]{};
    audio_outputs[0].numChannels = 2;
    audio_outputs[0].channelBuffers64 = main_outputs;

    Steinberg::Vst::ProcessData data{};
    data.symbolicSampleSize = Steinberg::Vst::kSample64;
    data.numSamples = kFrames;
    data.numInputs = 1;
    data.numOutputs = 1;
    data.inputs = audio_inputs;
    data.outputs = audio_outputs;

    REQUIRE(processor.process(data) == Steinberg::kResultOk);
    REQUIRE(test_processor->process_buffer_count == 1);
    REQUIRE(test_processor->process_count == 1);
    REQUIRE(test_processor->last_input_channels == 2);
    REQUIRE(test_processor->last_output_channels == 2);
    REQUIRE(test_processor->last_context.num_samples == kFrames);

    for (int i = 0; i < kFrames; ++i) {
        REQUIRE(out_l[static_cast<std::size_t>(i)] ==
                static_cast<double>(static_cast<float>(in_l[static_cast<std::size_t>(i)])));
        REQUIRE(out_r[static_cast<std::size_t>(i)] ==
                static_cast<double>(static_cast<float>(in_r[static_cast<std::size_t>(i)])));
    }

    REQUIRE(processor.setActive(false) == Steinberg::kResultOk);
    REQUIRE(processor.terminate() == Steinberg::kResultOk);
}

TEST_CASE("VST3 declared-Bypass designation emits a toggle kIsBypass even on a "
          "continuous range",
          "[vst3][param-designation][bypass]") {
    TestVst3Config config;
    config.add_designated_continuous_bypass = true;
    reset_test_processor(config);

    HostApp host_app;
    pulp::format::vst3::PulpVst3Processor processor(create_test_processor);
    REQUIRE(processor.initialize(&host_app) == Steinberg::kResultOk);

    // Gain (index 0) + the designated continuous bypass (index 1).
    REQUIRE(processor.getParameterCount() == 2);

    ParameterInfo bypass{};
    REQUIRE(processor.getParameterInfo(1, bypass) == Steinberg::kResultOk);
    REQUIRE(bypass.id == kBypassParamId);
    // Detected by designation, not by name ("Engine Active"):
    REQUIRE((bypass.flags & ParameterInfo::kIsBypass) != 0);
    // The continuous range ({0,4,step 0.25}) must NOT leak through as a
    // continuous control — kIsBypass is forced to a two-state toggle.
    REQUIRE(bypass.stepCount == 1);
    REQUIRE((bypass.flags & ParameterInfo::kCanAutomate) != 0);

    REQUIRE(processor.terminate() == Steinberg::kResultOk);
}

TEST_CASE("VST3 trigger param raised on a bypassed block still auto-resets",
          "[vst3][param-designation][rt-safety]") {
    TestVst3Config config;
    config.add_bypass_param = true;       // boolean Bypass at kBypassParamId
    config.add_reset_trigger_param = true; // Reset trigger at kResetParamId
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
    setup.sampleRate = 48000.0;
    REQUIRE(processor.setupProcessing(setup) == Steinberg::kResultOk);
    REQUIRE(processor.setActive(true) == Steinberg::kResultOk);

    constexpr int kFrames = 8;
    std::array<float, kFrames> in_l{}, in_r{}, out_l{}, out_r{};
    float* main_inputs[2] = {in_l.data(), in_r.data()};
    float* main_outputs[2] = {out_l.data(), out_r.data()};
    Steinberg::Vst::AudioBusBuffers ab_in[1]{};
    ab_in[0].numChannels = 2;
    ab_in[0].channelBuffers32 = main_inputs;
    Steinberg::Vst::AudioBusBuffers ab_out[1]{};
    ab_out[0].numChannels = 2;
    ab_out[0].channelBuffers32 = main_outputs;
    Steinberg::Vst::ProcessData data{};
    data.numSamples = kFrames;
    data.numInputs = 1;
    data.numOutputs = 1;
    data.inputs = ab_in;
    data.outputs = ab_out;

    // Engage bypass AND raise the Reset trigger before the block. The adapter
    // short-circuits to pass-through (Processor::process is NOT called), but the
    // trigger must still settle on this block — otherwise a panic raised while
    // bypassed would fire late on the next non-bypassed block.
    test_processor->state().set_value(kBypassParamId, 1.0f);
    test_processor->state().set_value(kResetParamId, 1.0f);
    REQUIRE_THAT(test_processor->state().get_value(kResetParamId),
                 WithinAbs(1.0f, 1e-6f));

    REQUIRE(processor.process(data) == Steinberg::kResultOk);

    // After the bypassed block, the trigger has auto-reset to its default.
    REQUIRE_THAT(test_processor->state().get_value(kResetParamId),
                 WithinAbs(0.0f, 1e-6f));
    // Bypass itself is a normal latch — it stays engaged.
    REQUIRE_THAT(test_processor->state().get_value(kBypassParamId),
                 WithinAbs(1.0f, 1e-6f));

    REQUIRE(processor.setActive(false) == Steinberg::kResultOk);
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

TEST_CASE("VST3 restartComponent is marshaled off the audio thread",
          "[vst3][latency][realtime][rt-safety]") {
    // IComponentHandler::restartComponent is a host callback that may lock,
    // allocate, and synchronously re-enter the plug-in, so it must never run on
    // the real-time audio thread. When the processor flags a latency/tail change
    // during process(), the adapter only accumulates RT-safe atomic flags; the
    // host callback fires later on the main thread.
    using Steinberg::Vst::kLatencyChanged;
    using Steinberg::Vst::kReloadComponent;

    TestVst3Config config;
    config.flag_latency_in_process = true;
    config.flag_tail_in_process = true;
    reset_test_processor(config);

    HostApp host_app;
    CapturingComponentHandler handler;
    pulp::format::vst3::PulpVst3Processor processor(create_test_processor);
    REQUIRE(processor.initialize(&host_app) == Steinberg::kResultOk);
    REQUIRE(processor.setComponentHandler(&handler) == Steinberg::kResultOk);

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
    Steinberg::Vst::ProcessData data{};
    data.numSamples = kFrames;
    data.numInputs = 1;
    data.numOutputs = 1;
    data.inputs = audio_inputs;
    data.outputs = audio_outputs;

    const std::thread::id main_thread_id = std::this_thread::get_id();

    SECTION("process() flags a restart without calling the host callback") {
        // Run process() on a DEDICATED thread so its identity is provably
        // distinct from the main thread. The host callback must not fire here.
        // Catch2 assertions are not thread-safe: record the render result and
        // assert on the test thread after join() (see thread_assert_check.py).
        std::atomic<bool> process_ok{false};
        std::thread audio_thread([&] {
            process_ok.store(processor.process(data) == Steinberg::kResultOk,
                             std::memory_order_relaxed);
        });
        audio_thread.join();
        REQUIRE(process_ok.load());

        // No restartComponent during the audio render — only the atomic flag.
        REQUIRE(handler.restart_calls == 0);
        REQUIRE(processor.restart_dispatch_armed_for_test());

        // Draining on the main thread (here, the test thread) delivers exactly
        // one restartComponent with the coalesced flags.
        const Steinberg::int32 delivered = processor.poll_pending_restart_for_test();
        REQUIRE(delivered == (kLatencyChanged | kReloadComponent));
        REQUIRE(handler.restart_calls == 1);
        REQUIRE(handler.last_flags == (kLatencyChanged | kReloadComponent));
        REQUIRE(handler.last_thread == main_thread_id);
        REQUIRE_FALSE(processor.restart_dispatch_armed_for_test());

        // A second drain with nothing pending is a no-op (no duplicate notify).
        REQUIRE(processor.poll_pending_restart_for_test() == 0);
        REQUIRE(handler.restart_calls == 1);
    }

    SECTION("multiple flagged blocks coalesce into a single host notification") {
        // The test processor flags only on its first block, but flag the
        // processor again directly to simulate a multi-block burst before any
        // drain. Both edges must collapse to ONE restartComponent call.
        std::atomic<bool> process_ok{true};
        std::thread audio_thread([&] {
            if (processor.process(data) != Steinberg::kResultOk)  // flags
                process_ok.store(false, std::memory_order_relaxed);
            if (processor.process(data) != Steinberg::kResultOk)  // no flag
                process_ok.store(false, std::memory_order_relaxed);
        });
        audio_thread.join();
        REQUIRE(process_ok.load());
        REQUIRE(handler.restart_calls == 0);

        processor.poll_pending_restart_for_test();
        REQUIRE(handler.restart_calls == 1);
    }

    SECTION("a main-thread host entrypoint drains the pending restart") {
        // getLatencySamples() runs on the main thread; a host re-queries it
        // after a latency change. It must flush the pending restartComponent.
        std::atomic<bool> process_ok{false};
        std::thread audio_thread([&] {
            process_ok.store(processor.process(data) == Steinberg::kResultOk,
                             std::memory_order_relaxed);
        });
        audio_thread.join();
        REQUIRE(process_ok.load());
        REQUIRE(handler.restart_calls == 0);

        (void)processor.getLatencySamples();
        REQUIRE(handler.restart_calls == 1);
        REQUIRE(handler.last_flags == (kLatencyChanged | kReloadComponent));
        REQUIRE(handler.last_thread == main_thread_id);
    }

    SECTION("flagging a restart adds no allocation to the process() path") {
        // process() has unrelated per-block allocations (e.g. the test
        // processor's bookkeeping), so the RT-safety claim is differential: a
        // block that flags a restart must allocate NO MORE than an otherwise-
        // identical block that does not — i.e. note_pending() itself is
        // allocation-free. (It is a pure atomic OR + exchange.)
        auto* tp = TestVst3Processor::g_last_processor;
        REQUIRE(tp != nullptr);

        auto allocs_for_block = [&](bool flag) -> std::size_t {
            // Drain any prior pending restart and arm/disarm flagging.
            processor.poll_pending_restart_for_test();
            tp->flagged_restart_in_process_ = !flag;  // true => won't flag
            REQUIRE(processor.process(data) == Steinberg::kResultOk);  // warm
            processor.poll_pending_restart_for_test();
            tp->flagged_restart_in_process_ = !flag;
            pulp::test::RtAllocationProbe probe;
            REQUIRE(processor.process(data) == Steinberg::kResultOk);
            return probe.allocation_count();
        };

        const std::size_t without_flag = allocs_for_block(false);
        const std::size_t with_flag = allocs_for_block(true);
        INFO("process() allocations: without_flag=" << without_flag
             << ", with_flag=" << with_flag);
        REQUIRE(with_flag <= without_flag);
        // And the flagged block did arm a pending restart.
        REQUIRE(processor.restart_dispatch_armed_for_test());
    }

    REQUIRE(processor.terminate() == Steinberg::kResultOk);
}

TEST_CASE("VST3 paced main-thread poll delivers a restart without a host query",
          "[vst3][latency][realtime][rt-safety]") {
    // Liveness: a mid-stream latency change must reach the host even if the
    // host never calls another main-thread query (getLatency/Tail/getState).
    // The adapter's paced poll on the main-thread dispatcher delivers it.
    using Steinberg::Vst::kLatencyChanged;

    TestVst3Config config;
    config.flag_latency_in_process = true;
    reset_test_processor(config);

    HostApp host_app;
    CapturingComponentHandler handler;
    pulp::format::vst3::PulpVst3Processor processor(create_test_processor);
    REQUIRE(processor.initialize(&host_app) == Steinberg::kResultOk);
    // Install the controllable backend AFTER initialize() so it is the active
    // one (last registration wins over the adapter's plugin backend).
    ScopedMainThreadBackend backend;
    REQUIRE(processor.setComponentHandler(&handler) == Steinberg::kResultOk);

    Steinberg::Vst::ProcessSetup setup{};
    setup.processMode = Steinberg::Vst::kRealtime;
    setup.symbolicSampleSize = Steinberg::Vst::kSample32;
    setup.maxSamplesPerBlock = 8;
    setup.sampleRate = 48000.0;
    REQUIRE(processor.setupProcessing(setup) == Steinberg::kResultOk);

    // Activating starts the paced poll: exactly one delayed tick is scheduled.
    REQUIRE(processor.setActive(true) == Steinberg::kResultOk);
    REQUIRE(backend.delayed_pending() == 1);

    // A render block flags a latency change on the audio thread.
    std::array<float, 8> in_l{}, in_r{}, out_l{}, out_r{};
    float* ins[2] = {in_l.data(), in_r.data()};
    float* outs[2] = {out_l.data(), out_r.data()};
    Steinberg::Vst::AudioBusBuffers ab_in[1]{};
    ab_in[0].numChannels = 2;
    ab_in[0].channelBuffers32 = ins;
    Steinberg::Vst::AudioBusBuffers ab_out[1]{};
    ab_out[0].numChannels = 2;
    ab_out[0].channelBuffers32 = outs;
    Steinberg::Vst::ProcessData data{};
    data.numSamples = 8;
    data.numInputs = 1;
    data.numOutputs = 1;
    data.inputs = ab_in;
    data.outputs = ab_out;

    std::atomic<bool> process_ok{false};
    std::thread audio_thread([&] {
        process_ok.store(processor.process(data) == Steinberg::kResultOk,
                         std::memory_order_relaxed);
    });
    audio_thread.join();
    REQUIRE(process_ok.load());

    // No host query was made — the restart is only delivered by the poll tick.
    REQUIRE(handler.restart_calls == 0);
    REQUIRE(processor.restart_dispatch_armed_for_test());

    // Run the scheduled tick: it drains the publisher (delivering the restart)
    // AND re-schedules the next tick (still active).
    REQUIRE(backend.run_delayed() == 1);
    REQUIRE(handler.restart_calls == 1);
    REQUIRE(handler.last_flags == kLatencyChanged);
    REQUIRE_FALSE(processor.restart_dispatch_armed_for_test());
    REQUIRE(backend.delayed_pending() == 1);  // poll keeps running while active

    // A subsequent tick with nothing pending delivers nothing but keeps polling.
    REQUIRE(backend.run_delayed() == 1);
    REQUIRE(handler.restart_calls == 1);
    REQUIRE(backend.delayed_pending() == 1);

    // Deactivating stops the poll: the next tick is a no-op and does not re-post.
    REQUIRE(processor.setActive(false) == Steinberg::kResultOk);
    REQUIRE(backend.run_delayed() == 1);
    REQUIRE(backend.delayed_pending() == 0);

    REQUIRE(processor.terminate() == Steinberg::kResultOk);
}

TEST_CASE("VST3 a restart callback queued past terminate() is a safe no-op",
          "[vst3][latency][realtime][rt-safety]") {
    // P0 lifetime safety: a main-thread lambda the adapter posted before
    // terminate() must not touch the (destroyed) instance when it finally runs.
    // The shared liveness flag, cleared in terminate(), makes it a no-op.
    using Steinberg::Vst::kLatencyChanged;

    TestVst3Config config;
    config.flag_latency_in_process = true;
    reset_test_processor(config);

    HostApp host_app;
    CapturingComponentHandler handler;
    // Declared before the processor scope so it outlives the processor and can
    // run the stale tick after the processor is destroyed. Registered as the
    // ACTIVE backend below, after initialize() installs the adapter's plugin
    // backend (last registration wins).
    std::optional<ScopedMainThreadBackend> backend;

    {
        auto processor =
            std::make_unique<pulp::format::vst3::PulpVst3Processor>(create_test_processor);
        REQUIRE(processor->initialize(&host_app) == Steinberg::kResultOk);
        backend.emplace();
        REQUIRE(processor->setComponentHandler(&handler) == Steinberg::kResultOk);

        Steinberg::Vst::ProcessSetup setup{};
        setup.processMode = Steinberg::Vst::kRealtime;
        setup.symbolicSampleSize = Steinberg::Vst::kSample32;
        setup.maxSamplesPerBlock = 8;
        setup.sampleRate = 48000.0;
        REQUIRE(processor->setupProcessing(setup) == Steinberg::kResultOk);

        // Activate (schedules a poll tick), then arm a restart from a render
        // block — so the queued tick has real work to deliver — and terminate
        // WITHOUT running the tick. The tick now references a soon-to-be-
        // destroyed instance and a still-armed restart.
        REQUIRE(processor->setActive(true) == Steinberg::kResultOk);
        REQUIRE(backend->delayed_pending() >= 1);

        std::array<float, 8> in_l{}, in_r{}, out_l{}, out_r{};
        float* ins[2] = {in_l.data(), in_r.data()};
        float* outs[2] = {out_l.data(), out_r.data()};
        Steinberg::Vst::AudioBusBuffers ab_in[1]{};
        ab_in[0].numChannels = 2;
        ab_in[0].channelBuffers32 = ins;
        Steinberg::Vst::AudioBusBuffers ab_out[1]{};
        ab_out[0].numChannels = 2;
        ab_out[0].channelBuffers32 = outs;
        Steinberg::Vst::ProcessData data{};
        data.numSamples = 8;
        data.numInputs = 1;
        data.numOutputs = 1;
        data.inputs = ab_in;
        data.outputs = ab_out;
        REQUIRE(processor->process(data) == Steinberg::kResultOk);
        REQUIRE(processor->restart_dispatch_armed_for_test());

        // terminate() directly (no setActive(false), which would drain
        // synchronously on this main thread). The pending poll tick survives.
        REQUIRE(processor->terminate() == Steinberg::kResultOk);
        // processor destroyed here.
    }

    // Run the stale tick(s). With the liveness flag cleared, they must not call
    // restartComponent and must not crash (no use-after-free).
    backend->run_delayed();
    backend->run_immediate();
    REQUIRE(handler.restart_calls == 0);
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
    // The adapter now routes both declared output buses (main + aux) to the
    // Processor's richer surface instead of zero-filling the secondary bus.
    REQUIRE(test_processor->last_process_buffer_output_buses == 2);
    REQUIRE(test_processor->last_process_buffer_active_inputs == 2);
    REQUIRE(test_processor->last_process_buffer_active_outputs == 2);
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

TEST_CASE("VST3 push_output_param_event emits two sample-accurate offset points",
          "[vst3][params][out-events][sample-accurate]") {
    // The processor pushes two explicit output events for the same param (out of
    // order: offset 48 then 16). The adapter must sort them and drain them into
    // ONE IParamValueQueue with two points in ascending sample-offset order, and
    // the snapshot-diff fallback must NOT add a third offset-0 point for a param
    // that already emitted explicit events.
    TestVst3Config config;              // Gain-only processor
    config.push_output_param_events = true;
    reset_test_processor(config);

    HostApp host_app;
    pulp::format::vst3::PulpVst3Processor processor(create_test_processor);
    REQUIRE(processor.initialize(&host_app) == Steinberg::kResultOk);
    auto* test_processor = TestVst3Processor::g_last_processor;
    REQUIRE(test_processor != nullptr);
    // Gain + the adapter's synthesized Bypass (the synthesized Bypass is
    // controller-side only and is not in the StateStore the output drain walks).
    REQUIRE(processor.getParameterCount() == 2);

    Steinberg::Vst::ProcessSetup setup{};
    setup.processMode = Steinberg::Vst::kRealtime;
    setup.symbolicSampleSize = Steinberg::Vst::kSample32;
    setup.maxSamplesPerBlock = 64;
    setup.sampleRate = 48000.0;
    REQUIRE(processor.setupProcessing(setup) == Steinberg::kResultOk);
    REQUIRE(processor.setActive(true) == Steinberg::kResultOk);

    constexpr int kFrames = 64;
    std::array<float, kFrames> in_l{}, in_r{}, out_l{}, out_r{};
    float* ins[2] = {in_l.data(), in_r.data()};
    float* outs[2] = {out_l.data(), out_r.data()};
    Steinberg::Vst::AudioBusBuffers ab_in[1]{};
    ab_in[0].numChannels = 2;
    ab_in[0].channelBuffers32 = ins;
    Steinberg::Vst::AudioBusBuffers ab_out[1]{};
    ab_out[0].numChannels = 2;
    ab_out[0].channelBuffers32 = outs;

    Steinberg::Vst::ParameterChanges output_params(4);
    Steinberg::Vst::ProcessData data{};
    data.numSamples = kFrames;
    data.numInputs = 1;
    data.numOutputs = 1;
    data.inputs = ab_in;
    data.outputs = ab_out;
    data.outputParameterChanges = &output_params;

    REQUIRE(processor.process(data) == Steinberg::kResultOk);

    // Exactly ONE output queue for the Gain param.
    REQUIRE(output_params.getParameterCount() == 1);
    auto* queue = output_params.getParameterData(0);
    REQUIRE(queue != nullptr);
    REQUIRE(queue->getParameterId() == kGainParamId);
    // TWO sample-accurate points — and no spurious offset-0 fallback point
    // (a third point would fail this count).
    REQUIRE(queue->getPointCount() == 2);

    Steinberg::int32 off0 = -1, off1 = -1;
    Steinberg::Vst::ParamValue val0 = 0.0, val1 = 0.0;
    REQUIRE(queue->getPoint(0, off0, val0) == Steinberg::kResultTrue);
    REQUIRE(queue->getPoint(1, off1, val1) == Steinberg::kResultTrue);
    // Points land at the exact pushed offsets, in ascending order (the adapter
    // sorted the out-of-order pushes).
    REQUIRE(off0 == 16);
    REQUIRE(off1 == 48);
    REQUIRE(off0 < off1);
    // VST3 output uses NORMALIZED values: range.normalize(plain).
    // Gain range = [-60, 24] (span 84); normalize(v) = (v + 60) / 84.
    REQUIRE_THAT(val0, WithinAbs((-12.0 + 60.0) / 84.0, 1e-5));  // offset 16
    REQUIRE_THAT(val1, WithinAbs((6.0 + 60.0) / 84.0, 1e-5));    // offset 48

    REQUIRE(processor.setActive(false) == Steinberg::kResultOk);
    REQUIRE(processor.terminate() == Steinberg::kResultOk);
}

TEST_CASE("VST3 store-only param change still yields a single offset-0 point",
          "[vst3][params][out-events][regression]") {
    // Regression for the legacy fallback: a plugin that changes a param through
    // the store WITHOUT pushing an explicit event must still be reported once, at
    // offset 0, via the before/after snapshot diff.
    TestVst3Config config;
    config.mutate_gain_in_process = true;   // sets Gain to -6 dB in process()
    reset_test_processor(config);

    HostApp host_app;
    pulp::format::vst3::PulpVst3Processor processor(create_test_processor);
    REQUIRE(processor.initialize(&host_app) == Steinberg::kResultOk);
    // Gain + the adapter's synthesized (controller-side) Bypass.
    REQUIRE(processor.getParameterCount() == 2);

    Steinberg::Vst::ProcessSetup setup{};
    setup.processMode = Steinberg::Vst::kRealtime;
    setup.symbolicSampleSize = Steinberg::Vst::kSample32;
    setup.maxSamplesPerBlock = 64;
    setup.sampleRate = 48000.0;
    REQUIRE(processor.setupProcessing(setup) == Steinberg::kResultOk);
    REQUIRE(processor.setActive(true) == Steinberg::kResultOk);

    constexpr int kFrames = 64;
    std::array<float, kFrames> in_l{}, in_r{}, out_l{}, out_r{};
    float* ins[2] = {in_l.data(), in_r.data()};
    float* outs[2] = {out_l.data(), out_r.data()};
    Steinberg::Vst::AudioBusBuffers ab_in[1]{};
    ab_in[0].numChannels = 2;
    ab_in[0].channelBuffers32 = ins;
    Steinberg::Vst::AudioBusBuffers ab_out[1]{};
    ab_out[0].numChannels = 2;
    ab_out[0].channelBuffers32 = outs;

    Steinberg::Vst::ParameterChanges output_params(4);
    Steinberg::Vst::ProcessData data{};
    data.numSamples = kFrames;
    data.numInputs = 1;
    data.numOutputs = 1;
    data.inputs = ab_in;
    data.outputs = ab_out;
    data.outputParameterChanges = &output_params;

    REQUIRE(processor.process(data) == Steinberg::kResultOk);

    REQUIRE(output_params.getParameterCount() == 1);
    auto* queue = output_params.getParameterData(0);
    REQUIRE(queue != nullptr);
    REQUIRE(queue->getParameterId() == kGainParamId);
    REQUIRE(queue->getPointCount() == 1);
    Steinberg::int32 offset = -1;
    Steinberg::Vst::ParamValue value = 0.0;
    REQUIRE(queue->getPoint(0, offset, value) == Steinberg::kResultTrue);
    REQUIRE(offset == 0);
    REQUIRE_THAT(value, WithinAbs((-6.0 + 60.0) / 84.0, 1e-5));  // 54/84

    REQUIRE(processor.setActive(false) == Steinberg::kResultOk);
    REQUIRE(processor.terminate() == Steinberg::kResultOk);
}

namespace {
// Declares two output buses and writes a distinct constant to each via the
// richer process(ProcessBuffers&) surface. Proves the VST3 adapter routes a
// declared secondary output bus to the Processor instead of zero-filling it.
class Vst3MultiOutProcessor final : public pulp::format::Processor {
public:
    static constexpr float kMainValue = 0.5f;
    static constexpr float kAuxValue = -0.25f;
    static Vst3MultiOutProcessor* g_last;
    // Set before constructing the adapter to control the declared aux channel
    // count (e.g. declare mono and negotiate stereo via setBusArrangements).
    static int g_next_aux_declared_channels;

    Vst3MultiOutProcessor() : aux_declared_channels_(g_next_aux_declared_channels) {
        g_last = this;
    }

    pulp::format::PluginDescriptor descriptor() const override {
        pulp::format::PluginDescriptor d;
        d.name = "Vst3MultiOut";
        d.manufacturer = "PulpTest";
        d.bundle_id = "com.pulp.test.vst3.multiout";
        d.version = "1.0.0";
        d.category = pulp::format::PluginCategory::Instrument;
        d.input_buses = {{"Main In", 2}};
        d.output_buses = {{"Main Out", 2}, {"Aux Out", aux_declared_channels_}};
        return d;
    }
    void define_parameters(pulp::state::StateStore&) override {}
    void prepare(const pulp::format::PrepareContext&) override {}

    using Processor::process;
    void process(pulp::audio::BufferView<float>& out,
                 const pulp::audio::BufferView<const float>&,
                 pulp::midi::MidiBuffer&, pulp::midi::MidiBuffer&,
                 const pulp::format::ProcessContext&) override {
        fill(out, kMainValue);
    }
    void process(pulp::format::ProcessBuffers& audio,
                 pulp::midi::MidiBuffer&, pulp::midi::MidiBuffer&,
                 const pulp::format::ProcessContext&) override {
        ++process_buffer_calls;
        output_bus_count = static_cast<int>(audio.outputs.size());
        if (auto* main = audio.outputs.main(); main && main->active()) {
            fill(main->buffer, kMainValue);
            wrote_main = true;
        }
        if (auto* aux = audio.outputs.find(pulp::format::BusRole::Aux); aux) {
            aux_declared_seen = aux->info.declared_channels;
            if (aux->active()) {
                fill(aux->buffer, kAuxValue);
                wrote_aux = true;
                aux_channels = static_cast<int>(aux->num_channels());
            }
        }
    }

    int process_buffer_calls = 0;
    int output_bus_count = 0;
    int aux_channels = 0;
    int aux_declared_seen = -1;
    bool wrote_main = false;
    bool wrote_aux = false;

private:
    int aux_declared_channels_;
    static void fill(pulp::audio::BufferView<float>& view, float value) {
        for (std::size_t ch = 0; ch < view.num_channels(); ++ch) {
            auto data = view.channel(ch);
            for (std::size_t n = 0; n < view.num_samples(); ++n) data[n] = value;
        }
    }
};
Vst3MultiOutProcessor* Vst3MultiOutProcessor::g_last = nullptr;
int Vst3MultiOutProcessor::g_next_aux_declared_channels = 2;

std::unique_ptr<pulp::format::Processor> create_vst3_multi_out() {
    return std::make_unique<Vst3MultiOutProcessor>();
}
}  // namespace

TEST_CASE("VST3 adapter routes a declared secondary output bus to the Processor",
          "[vst3][process][multi-out]") {
    Vst3MultiOutProcessor::g_last = nullptr;
    Vst3MultiOutProcessor::g_next_aux_declared_channels = 2;
    HostApp host_app;
    pulp::format::vst3::PulpVst3Processor processor(create_vst3_multi_out);
    REQUIRE(processor.initialize(&host_app) == Steinberg::kResultOk);
    auto* proc = Vst3MultiOutProcessor::g_last;
    REQUIRE(proc != nullptr);

    Steinberg::Vst::SpeakerArrangement inputs[1] = {SpeakerArr::kStereo};
    Steinberg::Vst::SpeakerArrangement outputs[2] = {SpeakerArr::kStereo,
                                                     SpeakerArr::kStereo};
    REQUIRE(processor.setBusArrangements(inputs, 1, outputs, 2) ==
            Steinberg::kResultTrue);

    Steinberg::Vst::ProcessSetup setup{};
    setup.processMode = Steinberg::Vst::kRealtime;
    setup.symbolicSampleSize = Steinberg::Vst::kSample32;
    setup.maxSamplesPerBlock = 8;
    setup.sampleRate = 48000.0;
    REQUIRE(processor.setupProcessing(setup) == Steinberg::kResultOk);

    constexpr int kFrames = 8;
    std::array<float, kFrames> in_l{}, in_r{};
    std::array<float, kFrames> main_l{}, main_r{}, aux_l{}, aux_r{};
    // Pre-seed outputs with a sentinel that must be overwritten.
    main_l.fill(99.0f); main_r.fill(99.0f);
    aux_l.fill(99.0f);  aux_r.fill(99.0f);

    float* main_inputs[2] = {in_l.data(), in_r.data()};
    float* main_outputs[2] = {main_l.data(), main_r.data()};
    float* aux_outputs[2] = {aux_l.data(), aux_r.data()};

    Steinberg::Vst::AudioBusBuffers audio_inputs[1]{};
    audio_inputs[0].numChannels = 2;
    audio_inputs[0].channelBuffers32 = main_inputs;

    Steinberg::Vst::AudioBusBuffers audio_outputs[2]{};
    audio_outputs[0].numChannels = 2;
    audio_outputs[0].channelBuffers32 = main_outputs;
    audio_outputs[1].numChannels = 2;
    audio_outputs[1].channelBuffers32 = aux_outputs;

    Steinberg::Vst::ProcessData data{};
    data.numSamples = kFrames;
    data.numInputs = 1;
    data.numOutputs = 2;
    data.inputs = audio_inputs;
    data.outputs = audio_outputs;

    REQUIRE(processor.process(data) == Steinberg::kResultOk);

    REQUIRE(proc->process_buffer_calls == 1);
    REQUIRE(proc->output_bus_count == 2);
    REQUIRE(proc->wrote_main);
    REQUIRE(proc->wrote_aux);
    REQUIRE(proc->aux_channels == 2);
    REQUIRE(proc->aux_declared_seen == 2);  // declared == routed here

    for (int i = 0; i < kFrames; ++i) {
        REQUIRE_THAT(main_l[i], WithinAbs(Vst3MultiOutProcessor::kMainValue, 1e-6f));
        REQUIRE_THAT(main_r[i], WithinAbs(Vst3MultiOutProcessor::kMainValue, 1e-6f));
        REQUIRE_THAT(aux_l[i], WithinAbs(Vst3MultiOutProcessor::kAuxValue, 1e-6f));
        REQUIRE_THAT(aux_r[i], WithinAbs(Vst3MultiOutProcessor::kAuxValue, 1e-6f));
    }
    REQUIRE(Vst3MultiOutProcessor::kMainValue != Vst3MultiOutProcessor::kAuxValue);

    REQUIRE(processor.terminate() == Steinberg::kResultOk);
}

TEST_CASE("VST3 routes the full aux channel count negotiated by setBusArrangements",
          "[vst3][process][multi-out]") {
    // P1: the processor declares a MONO aux bus, but the host negotiates STEREO
    // on it via setBusArrangements (accepted because the default
    // is_bus_layout_supported permits mono/stereo). The adapter must size + route
    // the aux storage from the ACCEPTED arrangement (2 channels), not the
    // descriptor default (1), so the processor receives both negotiated channels.
    Vst3MultiOutProcessor::g_last = nullptr;
    Vst3MultiOutProcessor::g_next_aux_declared_channels = 1;  // declare mono aux
    HostApp host_app;
    pulp::format::vst3::PulpVst3Processor processor(create_vst3_multi_out);
    REQUIRE(processor.initialize(&host_app) == Steinberg::kResultOk);
    auto* proc = Vst3MultiOutProcessor::g_last;
    REQUIRE(proc != nullptr);

    // Negotiate stereo on BOTH output buses; the descriptor declared the aux as
    // mono, so this exercises the mono→stereo acceptance path.
    Steinberg::Vst::SpeakerArrangement inputs[1] = {SpeakerArr::kStereo};
    Steinberg::Vst::SpeakerArrangement outputs[2] = {SpeakerArr::kStereo,
                                                     SpeakerArr::kStereo};
    REQUIRE(processor.setBusArrangements(inputs, 1, outputs, 2) ==
            Steinberg::kResultTrue);

    Steinberg::Vst::ProcessSetup setup{};
    setup.processMode = Steinberg::Vst::kRealtime;
    setup.symbolicSampleSize = Steinberg::Vst::kSample32;
    setup.maxSamplesPerBlock = 8;
    setup.sampleRate = 48000.0;
    REQUIRE(processor.setupProcessing(setup) == Steinberg::kResultOk);

    constexpr int kFrames = 8;
    std::array<float, kFrames> in_l{}, in_r{};
    std::array<float, kFrames> main_l{}, main_r{}, aux_l{}, aux_r{};
    main_l.fill(99.0f); main_r.fill(99.0f);
    aux_l.fill(99.0f);  aux_r.fill(99.0f);

    float* main_inputs[2] = {in_l.data(), in_r.data()};
    float* main_outputs[2] = {main_l.data(), main_r.data()};
    float* aux_outputs[2] = {aux_l.data(), aux_r.data()};

    Steinberg::Vst::AudioBusBuffers audio_inputs[1]{};
    audio_inputs[0].numChannels = 2;
    audio_inputs[0].channelBuffers32 = main_inputs;

    Steinberg::Vst::AudioBusBuffers audio_outputs[2]{};
    audio_outputs[0].numChannels = 2;
    audio_outputs[0].channelBuffers32 = main_outputs;
    audio_outputs[1].numChannels = 2;  // host drives 2 channels on the aux bus
    audio_outputs[1].channelBuffers32 = aux_outputs;

    Steinberg::Vst::ProcessData data{};
    data.numSamples = kFrames;
    data.numInputs = 1;
    data.numOutputs = 2;
    data.inputs = audio_inputs;
    data.outputs = audio_outputs;

    REQUIRE(processor.process(data) == Steinberg::kResultOk);

    REQUIRE(proc->wrote_aux);
    // The processor received BOTH negotiated channels (not clamped to the mono
    // descriptor default), and declared_channels still reports the descriptor.
    REQUIRE(proc->aux_channels == 2);
    REQUIRE(proc->aux_declared_seen == 1);
    for (int i = 0; i < kFrames; ++i) {
        REQUIRE_THAT(aux_l[i], WithinAbs(Vst3MultiOutProcessor::kAuxValue, 1e-6f));
        REQUIRE_THAT(aux_r[i], WithinAbs(Vst3MultiOutProcessor::kAuxValue, 1e-6f));
    }

    REQUIRE(processor.terminate() == Steinberg::kResultOk);
}

TEST_CASE("VST3 leaves a secondary output bus silent for a single-output processor",
          "[vst3][process][multi-out]") {
    // Regression: the existing TestVst3Processor (single legacy process()) with a
    // declared aux bus — the adapter pre-zeros the aux and the default
    // process(ProcessBuffers&) writes only main, so the aux reads silence.
    TestVst3Config config;
    config.descriptor.input_buses = {{"Main In", 2}};
    config.descriptor.output_buses = {{"Main Out", 2}, {"Aux Out", 2}};
    reset_test_processor(config);

    HostApp host_app;
    pulp::format::vst3::PulpVst3Processor processor(create_test_processor);
    REQUIRE(processor.initialize(&host_app) == Steinberg::kResultOk);

    Steinberg::Vst::SpeakerArrangement inputs[1] = {SpeakerArr::kStereo};
    Steinberg::Vst::SpeakerArrangement outputs[2] = {SpeakerArr::kStereo,
                                                     SpeakerArr::kStereo};
    REQUIRE(processor.setBusArrangements(inputs, 1, outputs, 2) ==
            Steinberg::kResultTrue);

    Steinberg::Vst::ProcessSetup setup{};
    setup.processMode = Steinberg::Vst::kRealtime;
    setup.symbolicSampleSize = Steinberg::Vst::kSample32;
    setup.maxSamplesPerBlock = 8;
    setup.sampleRate = 48000.0;
    REQUIRE(processor.setupProcessing(setup) == Steinberg::kResultOk);

    constexpr int kFrames = 8;
    std::array<float, kFrames> in_l{{0.1f, 0.2f, 0.3f, 0.4f, 0.5f, 0.6f, 0.7f, 0.8f}};
    std::array<float, kFrames> in_r{{0.1f, 0.2f, 0.3f, 0.4f, 0.5f, 0.6f, 0.7f, 0.8f}};
    std::array<float, kFrames> main_l{}, main_r{}, aux_l{}, aux_r{};
    aux_l.fill(42.0f); aux_r.fill(-42.0f);

    float* main_inputs[2] = {in_l.data(), in_r.data()};
    float* main_outputs[2] = {main_l.data(), main_r.data()};
    float* aux_outputs[2] = {aux_l.data(), aux_r.data()};

    Steinberg::Vst::AudioBusBuffers audio_inputs[1]{};
    audio_inputs[0].numChannels = 2;
    audio_inputs[0].channelBuffers32 = main_inputs;
    Steinberg::Vst::AudioBusBuffers audio_outputs[2]{};
    audio_outputs[0].numChannels = 2;
    audio_outputs[0].channelBuffers32 = main_outputs;
    audio_outputs[1].numChannels = 2;
    audio_outputs[1].channelBuffers32 = aux_outputs;

    Steinberg::Vst::ProcessData data{};
    data.numSamples = kFrames;
    data.numInputs = 1;
    data.numOutputs = 2;
    data.inputs = audio_inputs;
    data.outputs = audio_outputs;

    REQUIRE(processor.process(data) == Steinberg::kResultOk);

    for (int i = 0; i < kFrames; ++i) {
        REQUIRE_THAT(aux_l[i], WithinAbs(0.0f, 1e-6f));
        REQUIRE_THAT(aux_r[i], WithinAbs(0.0f, 1e-6f));
    }
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

TEST_CASE("VST3 bypass pass-through delays the dry signal by the reported latency",
          "[vst3][bypass][latency][pdc]") {
    // A plugin that reports latency gets host plugin-delay-compensation on its
    // path. The bypassed dry pass-through must be delayed by exactly that many
    // samples so it stays sample-aligned with the host's PDC; otherwise the
    // bypassed output arrives `latency` samples early and comb-filters against
    // parallel tracks. Drive a known ramp through the real adapter process path
    // with bypass engaged across enough blocks to exceed the latency, then
    // assert steady-state output[n] == input[n - latency].
    constexpr int kLatency = 16;
    TestVst3Config config;
    config.add_bypass_param = true;
    config.latency_samples = kLatency;
    reset_test_processor(config);

    HostApp host_app;
    pulp::format::vst3::PulpVst3Processor processor(create_test_processor);
    REQUIRE(processor.initialize(&host_app) == Steinberg::kResultOk);
    REQUIRE(processor.bypass_parameter_id() == kBypassParamId);

    Steinberg::Vst::SpeakerArrangement io[1] = {SpeakerArr::kStereo};
    REQUIRE(processor.setBusArrangements(io, 1, io, 1) == Steinberg::kResultTrue);

    constexpr int kBlock = 32;
    Steinberg::Vst::ProcessSetup setup{};
    setup.processMode = Steinberg::Vst::kRealtime;
    setup.symbolicSampleSize = Steinberg::Vst::kSample32;
    setup.maxSamplesPerBlock = kBlock;
    setup.sampleRate = 48000.0;
    REQUIRE(processor.setupProcessing(setup) == Steinberg::kResultOk);

    // A monotonically rising input makes any misalignment obvious: a 0-delay
    // copy would yield output[n] == input[n], not input[n - kLatency].
    constexpr int kBlocks = 6;
    constexpr int kTotal = kBlock * kBlocks;
    std::vector<float> source(kTotal);
    for (int n = 0; n < kTotal; ++n) source[n] = static_cast<float>(n + 1);

    std::vector<float> captured_l(kTotal, 0.0f);
    std::vector<float> captured_r(kTotal, 0.0f);

    for (int b = 0; b < kBlocks; ++b) {
        std::array<float, kBlock> in_l{};
        std::array<float, kBlock> in_r{};
        std::array<float, kBlock> out_l{};
        std::array<float, kBlock> out_r{};
        for (int i = 0; i < kBlock; ++i) {
            in_l[i] = source[b * kBlock + i];
            in_r[i] = -source[b * kBlock + i];
        }
        out_l.fill(99.0f);
        out_r.fill(99.0f);
        float* ins[2]  = {in_l.data(), in_r.data()};
        float* outs[2] = {out_l.data(), out_r.data()};
        Steinberg::Vst::AudioBusBuffers ab_in[1]{};
        ab_in[0].numChannels = 2; ab_in[0].channelBuffers32 = ins;
        Steinberg::Vst::AudioBusBuffers ab_out[1]{};
        ab_out[0].numChannels = 2; ab_out[0].channelBuffers32 = outs;

        // Engage bypass at sample 0 of every block.
        Steinberg::Vst::ParameterChanges params(1);
        Steinberg::int32 q_index = 0;
        auto* q = params.addParameterData(kBypassParamId, q_index);
        REQUIRE(q != nullptr);
        Steinberg::int32 pt = 0;
        REQUIRE(q->addPoint(0, 1.0, pt) == Steinberg::kResultTrue);

        Steinberg::Vst::ProcessData data{};
        data.numSamples = kBlock;
        data.numInputs = 1;
        data.numOutputs = 1;
        data.inputs = ab_in;
        data.outputs = ab_out;
        data.inputParameterChanges = &params;
        REQUIRE(processor.process(data) == Steinberg::kResultOk);

        for (int i = 0; i < kBlock; ++i) {
            captured_l[b * kBlock + i] = out_l[i];
            captured_r[b * kBlock + i] = out_r[i];
        }
    }

    // Steady state (past the warm-up): output[n] == input[n - kLatency].
    for (int n = kLatency; n < kTotal; ++n) {
        REQUIRE_THAT(captured_l[n], WithinAbs(source[n - kLatency], 1e-6f));
        REQUIRE_THAT(captured_r[n], WithinAbs(-source[n - kLatency], 1e-6f));
    }
    // Warm-up: the first kLatency samples carry the pre-bypass (silent) delay
    // contents, not the input — confirming the delay is real, not a no-op copy.
    for (int n = 0; n < kLatency; ++n) {
        REQUIRE_THAT(captured_l[n], WithinAbs(0.0f, 1e-6f));
    }
}

TEST_CASE("VST3 bypass pass-through is a zero-delay copy when latency is zero",
          "[vst3][bypass][latency][pdc]") {
    // The bug only exists for latency > 0; a 0-latency plugin must keep the
    // original zero-copy pass-through (output[n] == input[n], no warm-up).
    TestVst3Config config;
    config.add_bypass_param = true;
    config.latency_samples = 0;
    reset_test_processor(config);

    HostApp host_app;
    pulp::format::vst3::PulpVst3Processor processor(create_test_processor);
    REQUIRE(processor.initialize(&host_app) == Steinberg::kResultOk);
    REQUIRE(processor.bypass_parameter_id() == kBypassParamId);

    Steinberg::Vst::SpeakerArrangement io[1] = {SpeakerArr::kStereo};
    REQUIRE(processor.setBusArrangements(io, 1, io, 1) == Steinberg::kResultTrue);

    constexpr int kBlock = 8;
    Steinberg::Vst::ProcessSetup setup{};
    setup.processMode = Steinberg::Vst::kRealtime;
    setup.symbolicSampleSize = Steinberg::Vst::kSample32;
    setup.maxSamplesPerBlock = kBlock;
    setup.sampleRate = 48000.0;
    REQUIRE(processor.setupProcessing(setup) == Steinberg::kResultOk);

    std::array<float, kBlock> in_l{{1, 2, 3, 4, 5, 6, 7, 8}};
    std::array<float, kBlock> in_r{{-1, -2, -3, -4, -5, -6, -7, -8}};
    std::array<float, kBlock> out_l{};
    std::array<float, kBlock> out_r{};
    out_l.fill(99.0f);
    out_r.fill(99.0f);
    float* ins[2]  = {in_l.data(), in_r.data()};
    float* outs[2] = {out_l.data(), out_r.data()};
    Steinberg::Vst::AudioBusBuffers ab_in[1]{};
    ab_in[0].numChannels = 2; ab_in[0].channelBuffers32 = ins;
    Steinberg::Vst::AudioBusBuffers ab_out[1]{};
    ab_out[0].numChannels = 2; ab_out[0].channelBuffers32 = outs;

    Steinberg::Vst::ParameterChanges params(1);
    Steinberg::int32 q_index = 0;
    auto* q = params.addParameterData(kBypassParamId, q_index);
    REQUIRE(q != nullptr);
    Steinberg::int32 pt = 0;
    REQUIRE(q->addPoint(0, 1.0, pt) == Steinberg::kResultTrue);

    Steinberg::Vst::ProcessData data{};
    data.numSamples = kBlock;
    data.numInputs = 1;
    data.numOutputs = 1;
    data.inputs = ab_in;
    data.outputs = ab_out;
    data.inputParameterChanges = &params;
    REQUIRE(processor.process(data) == Steinberg::kResultOk);

    for (int i = 0; i < kBlock; ++i) {
        REQUIRE_THAT(out_l[i], WithinAbs(in_l[i], 1e-6f));
        REQUIRE_THAT(out_r[i], WithinAbs(in_r[i], 1e-6f));
    }
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

// ─────────────────────────────────────────────────────────────────────
// IMidiMapping — MIDI controller input for VST3 instruments.
//
// VST3 has no raw MIDI CC / pitch-bend / aftertouch events. The host
// queries getMidiControllerAssignment for the ParamID each controller maps
// to, then delivers those controllers as ordinary parameter changes. The
// adapter reserves a private ParamID range, registers the controllers as
// hidden parameters, and decodes inbound parameter changes in that range
// back into MIDI messages on midi_in.
// ─────────────────────────────────────────────────────────────────────

namespace {

// Feed a single controller parameter-change point through process() and
// return the MIDI events the processor saw on midi_in. The plug-in must
// accept MIDI for the controller mapping to be active.
struct MidiControllerSetup {
    pulp::format::vst3::PulpVst3Processor processor{create_test_processor};
    TestVst3Processor* test_processor = nullptr;
    HostApp host_app;

    explicit MidiControllerSetup() {
        TestVst3Config config;
        config.descriptor.accepts_midi = true;
        reset_test_processor(config);
        REQUIRE(processor.initialize(&host_app) == Steinberg::kResultOk);
        test_processor = TestVst3Processor::g_last_processor;
        REQUIRE(test_processor != nullptr);

        Steinberg::Vst::ProcessSetup setup{};
        setup.processMode = Steinberg::Vst::kRealtime;
        setup.symbolicSampleSize = Steinberg::Vst::kSample32;
        setup.maxSamplesPerBlock = 16;
        setup.sampleRate = 48000.0;
        REQUIRE(processor.setupProcessing(setup) == Steinberg::kResultOk);
    }

    // Drive one parameter change for `param_id` at `offset` with `normalized`
    // value, and run process() over a small stereo block.
    void run_one(Steinberg::Vst::ParamID param_id, Steinberg::int32 offset,
                 double normalized) {
        Steinberg::Vst::ParameterChanges input_params(1);
        Steinberg::int32 param_index = 0;
        auto* queue = input_params.addParameterData(param_id, param_index);
        REQUIRE(queue != nullptr);
        Steinberg::int32 point_index = 0;
        REQUIRE(queue->addPoint(offset, normalized, point_index) ==
                Steinberg::kResultTrue);

        constexpr int kFrames = 16;
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

        Steinberg::Vst::ProcessData data{};
        data.numSamples = kFrames;
        data.numInputs = 1;
        data.numOutputs = 1;
        data.inputs = audio_inputs;
        data.outputs = audio_outputs;
        data.inputParameterChanges = &input_params;

        REQUIRE(processor.process(data) == Steinberg::kResultOk);
    }
};

}  // namespace

TEST_CASE("VST3 MIDI event buses advertise all 16 channels",
          "[vst3][midi][bus]") {
    TestVst3Config config;
    config.descriptor.accepts_midi = true;
    config.descriptor.produces_midi = true;
    reset_test_processor(config);

    HostApp host_app;
    pulp::format::vst3::PulpVst3Processor processor(create_test_processor);
    REQUIRE(processor.initialize(&host_app) == Steinberg::kResultOk);

    Steinberg::Vst::BusInfo input_info{};
    REQUIRE(processor.getBusInfo(Steinberg::Vst::kEvent,
                                 Steinberg::Vst::kInput,
                                 0,
                                 input_info) == Steinberg::kResultOk);
    REQUIRE(input_info.channelCount ==
            pulp::format::detail::kVst3MidiChannels);

    Steinberg::Vst::BusInfo output_info{};
    REQUIRE(processor.getBusInfo(Steinberg::Vst::kEvent,
                                 Steinberg::Vst::kOutput,
                                 0,
                                 output_info) == Steinberg::kResultOk);
    REQUIRE(output_info.channelCount ==
            pulp::format::detail::kVst3MidiChannels);

    REQUIRE(processor.terminate() == Steinberg::kResultOk);
}

TEST_CASE("VST3 IMidiMapping assigns stable, non-colliding controller ParamIDs",
          "[vst3][midi][midimapping]") {
    namespace VstCtrl = Steinberg::Vst;
    TestVst3Config config;
    config.descriptor.accepts_midi = true;
    reset_test_processor(config);

    HostApp host_app;
    pulp::format::vst3::PulpVst3Processor processor(create_test_processor);
    REQUIRE(processor.initialize(&host_app) == Steinberg::kResultOk);

    // The plug-in declares one Gain param (id 1). The controller IDs must all
    // differ from it, and from each other.
    auto assignment = [&](Steinberg::int16 channel,
                          VstCtrl::CtrlNumber cc) -> std::optional<VstCtrl::ParamID> {
        VstCtrl::ParamID id = 0;
        if (processor.getMidiControllerAssignment(0, channel, cc, id) ==
            Steinberg::kResultTrue) {
            return id;
        }
        return std::nullopt;
    };

    // Mod wheel (CC1), sustain (CC64), pitch bend, channel aftertouch on
    // channel 0 — each present, distinct, and not equal to a plug-in param.
    const auto cc1   = assignment(0, VstCtrl::kCtrlModWheel);
    const auto cc64  = assignment(0, VstCtrl::kCtrlSustainOnOff);
    const auto pitch = assignment(0, VstCtrl::kPitchBend);
    const auto after = assignment(0, VstCtrl::kAfterTouch);
    REQUIRE(cc1.has_value());
    REQUIRE(cc64.has_value());
    REQUIRE(pitch.has_value());
    REQUIRE(after.has_value());

    std::array<VstCtrl::ParamID, 4> ids{*cc1, *cc64, *pitch, *after};
    for (auto id : ids) {
        REQUIRE(id != kGainParamId);
        REQUIRE(pulp::format::detail::is_vst3_midi_cc_param(id));
    }
    std::sort(ids.begin(), ids.end());
    REQUIRE(std::adjacent_find(ids.begin(), ids.end()) == ids.end());

    // Per-channel uniqueness: the same controller on a different channel maps
    // to a different ParamID, and channel 0 differs from channels 1 and 7.
    REQUIRE(assignment(1, VstCtrl::kCtrlModWheel) != cc1);
    REQUIRE(assignment(7, VstCtrl::kPitchBend) != pitch);

    // Stable: a second query returns the same ID.
    REQUIRE(assignment(0, VstCtrl::kCtrlModWheel) == cc1);

    // Out-of-range controller numbers and buses are declined.
    VstCtrl::ParamID dummy = 0;
    REQUIRE(processor.getMidiControllerAssignment(
                0, 0, VstCtrl::kCountCtrlNumber, dummy) == Steinberg::kResultFalse);
    REQUIRE(processor.getMidiControllerAssignment(
                1, 0, VstCtrl::kCtrlModWheel, dummy) == Steinberg::kResultFalse);
    REQUIRE(processor.getMidiControllerAssignment(
                0, 16, VstCtrl::kCtrlModWheel, dummy) == Steinberg::kResultFalse);

    // Every reserved ParamID the mapping can return is a registered hidden
    // parameter (VST3 requires this for the host to honor the mapping).
    ParameterInfo info{};
    REQUIRE(processor.getParameterInfo(static_cast<Steinberg::int32>(0), info) ==
            Steinberg::kResultOk);
    bool found_hidden_controller = false;
    const Steinberg::int32 param_count = processor.getParameterCount();
    for (Steinberg::int32 i = 0; i < param_count; ++i) {
        ParameterInfo pi{};
        REQUIRE(processor.getParameterInfo(i, pi) == Steinberg::kResultOk);
        if (pi.id == *cc1) {
            found_hidden_controller = true;
            REQUIRE((pi.flags & ParameterInfo::kIsHidden) != 0);
            REQUIRE((pi.flags & ParameterInfo::kCanAutomate) == 0);
        }
    }
    REQUIRE(found_hidden_controller);

    REQUIRE(processor.terminate() == Steinberg::kResultOk);
}

TEST_CASE("VST3 IMidiMapping is inert for plug-ins that do not accept MIDI",
          "[vst3][midi][midimapping]") {
    // An effect with accepts_midi == false registers no controller params and
    // declines every assignment query, so its parameter set is exactly what it
    // declared (no host-visible inflation, no state-format change). Disable the
    // bypass-synthesis quirk so the only parameter is the plug-in's own Gain.
    pulp::format::set_host_quirk_policy(pulp::format::kQuirkFilterOff);
    reset_test_processor();  // default config: accepts_midi == false
    HostApp host_app;
    pulp::format::vst3::PulpVst3Processor processor(create_test_processor);
    REQUIRE(processor.initialize(&host_app) == Steinberg::kResultOk);

    REQUIRE(processor.getParameterCount() == 1);  // Gain only — no controllers

    Steinberg::Vst::ParamID id = 0xdead;
    REQUIRE(processor.getMidiControllerAssignment(
                0, 0, Steinberg::Vst::kCtrlModWheel, id) == Steinberg::kResultFalse);

    REQUIRE(processor.terminate() == Steinberg::kResultOk);
    pulp::format::set_host_quirk_policy(std::nullopt);
}

TEST_CASE("VST3 IMidiMapping decodes a CC parameter change into a MIDI CC",
          "[vst3][midi][midimapping][process]") {
    namespace VstCtrl = Steinberg::Vst;
    MidiControllerSetup s;

    VstCtrl::ParamID cc1_id = 0;
    REQUIRE(s.processor.getMidiControllerAssignment(
                0, 0, VstCtrl::kCtrlModWheel, cc1_id) == Steinberg::kResultTrue);

    // Mod wheel to ~half-scale at sample offset 5 on channel 0.
    s.run_one(cc1_id, 5, 0.5);

    REQUIRE(s.test_processor->last_midi_in_size == 1);
    const auto& me = s.test_processor->last_midi_in_events.at(0);
    REQUIRE(me.is_cc());
    REQUIRE(me.channel() == 0);
    REQUIRE(me.cc_number() == 1);
    REQUIRE(me.cc_value() == static_cast<uint8_t>(0.5 * 127.0 + 0.5));  // 64
    REQUIRE(me.sample_offset == 5);

    // The controller did NOT leak into the parameter stream or the store.
    REQUIRE(s.processor.last_input_param_events().size() == 0);

    REQUIRE(s.processor.terminate() == Steinberg::kResultOk);
}

TEST_CASE("VST3 IMidiMapping decodes pitch bend to a 14-bit message",
          "[vst3][midi][midimapping][process]") {
    namespace VstCtrl = Steinberg::Vst;
    MidiControllerSetup s;

    VstCtrl::ParamID pb_id = 0;
    REQUIRE(s.processor.getMidiControllerAssignment(
                0, 3, VstCtrl::kPitchBend, pb_id) == Steinberg::kResultTrue);

    // Centre (0.5) → 8192 (half of 16383, rounded).
    s.run_one(pb_id, 9, 0.5);
    REQUIRE(s.test_processor->last_midi_in_size == 1);
    {
        const auto& me = s.test_processor->last_midi_in_events.at(0);
        REQUIRE(me.is_pitch_bend());
        REQUIRE(me.channel() == 3);
        REQUIRE(me.message.getPitchWheelValue() ==
                static_cast<uint32_t>(0.5 * 16383.0 + 0.5));  // 8192
        REQUIRE(me.sample_offset == 9);
    }

    // Full scale (1.0) → 16383.
    s.run_one(pb_id, 0, 1.0);
    {
        const auto& me = s.test_processor->last_midi_in_events.at(0);
        REQUIRE(me.is_pitch_bend());
        REQUIRE(me.message.getPitchWheelValue() == 16383u);
    }

    REQUIRE(s.processor.terminate() == Steinberg::kResultOk);
}

TEST_CASE("VST3 IMidiMapping decodes channel aftertouch to channel pressure",
          "[vst3][midi][midimapping][process]") {
    namespace VstCtrl = Steinberg::Vst;
    MidiControllerSetup s;

    VstCtrl::ParamID at_id = 0;
    REQUIRE(s.processor.getMidiControllerAssignment(
                0, 2, VstCtrl::kAfterTouch, at_id) == Steinberg::kResultTrue);

    s.run_one(at_id, 4, 1.0);

    REQUIRE(s.test_processor->last_midi_in_size == 1);
    const auto& me = s.test_processor->last_midi_in_events.at(0);
    REQUIRE(me.message.isChannelPressure());
    REQUIRE(me.channel() == 2);
    REQUIRE(me.message.getChannelPressureValue() == 127);
    REQUIRE(me.sample_offset == 4);

    REQUIRE(s.processor.terminate() == Steinberg::kResultOk);
}

TEST_CASE("VST3 real plug-in parameter changes still reach the store, not MIDI",
          "[vst3][midi][midimapping][process]") {
    // With MIDI mapping active, a change to a REAL parameter (Gain) must still
    // flow to the store and the param-event queue — only the reserved
    // controller range is diverted to MIDI.
    MidiControllerSetup s;

    s.run_one(kGainParamId, 0, 1.0);  // full-scale gain

    REQUIRE(s.test_processor->last_midi_in_size == 0);
    REQUIRE(s.processor.last_input_param_events().size() == 1);
    REQUIRE(s.processor.last_input_param_events().events()[0].param_id ==
            kGainParamId);
    // Gain range is [-60, 24]; normalized 1.0 denormalizes to 24 dB.
    REQUIRE_THAT(s.test_processor->gain_seen_in_process, WithinAbs(24.0f, 1e-5f));

    REQUIRE(s.processor.terminate() == Steinberg::kResultOk);
}

TEST_CASE("VST3 IMidiMapping controller decode does not allocate on the audio thread",
          "[vst3][midi][midimapping][realtime][perf]") {
    namespace VstCtrl = Steinberg::Vst;
    MidiControllerSetup s;

    VstCtrl::ParamID cc1_id = 0;
    REQUIRE(s.processor.getMidiControllerAssignment(
                0, 0, VstCtrl::kCtrlModWheel, cc1_id) == Steinberg::kResultTrue);

    // Build the param-change structure once (its ctor allocates), then measure
    // only process().
    VstCtrl::ParameterChanges input_params(1);
    Steinberg::int32 param_index = 0;
    auto* queue = input_params.addParameterData(cc1_id, param_index);
    REQUIRE(queue != nullptr);
    Steinberg::int32 point_index = 0;
    REQUIRE(queue->addPoint(2, 0.75, point_index) == Steinberg::kResultTrue);

    constexpr int kFrames = 16;
    std::array<float, kFrames> in_l{};
    std::array<float, kFrames> in_r{};
    std::array<float, kFrames> out_l{};
    std::array<float, kFrames> out_r{};
    float* main_inputs[2] = {in_l.data(), in_r.data()};
    float* main_outputs[2] = {out_l.data(), out_r.data()};
    VstCtrl::AudioBusBuffers audio_inputs[1]{};
    audio_inputs[0].numChannels = 2;
    audio_inputs[0].channelBuffers32 = main_inputs;
    VstCtrl::AudioBusBuffers audio_outputs[1]{};
    audio_outputs[0].numChannels = 2;
    audio_outputs[0].channelBuffers32 = main_outputs;

    VstCtrl::ProcessData data{};
    data.numSamples = kFrames;
    data.numInputs = 1;
    data.numOutputs = 1;
    data.inputs = audio_inputs;
    data.outputs = audio_outputs;
    data.inputParameterChanges = &input_params;

    REQUIRE(s.processor.process(data) == Steinberg::kResultOk);  // warm
    {
        pulp::test::RtAllocationProbe probe;
        REQUIRE(s.processor.process(data) == Steinberg::kResultOk);
        REQUIRE(probe.allocation_count() == 0);
    }

    REQUIRE(s.processor.terminate() == Steinberg::kResultOk);
}

// ── P0: collision-aware diversion ────────────────────────────────────
// A real plug-in parameter whose ID lands inside the reserved controller
// range must NOT be hijacked as a MIDI controller: it is registered as a
// real parameter, getMidiControllerAssignment declines the colliding
// controller, and a host parameter-change for that ID reaches store_
// rather than being decoded to MIDI. A non-colliding controller in the
// same plug-in still decodes normally.
TEST_CASE("VST3 IMidiMapping never diverts a real param that collides with the reserved range",
          "[vst3][midi][midimapping][collision]") {
    namespace VstCtrl = Steinberg::Vst;

    // Channel 0 / controller 1 (mod wheel) → base + 1.
    const auto collide_id =
        pulp::format::detail::vst3_midi_cc_param_id(0, 1);
    REQUIRE(collide_id == pulp::format::detail::kVst3MidiCcParamBase + 1);

    TestVst3Config config;
    config.descriptor.accepts_midi = true;
    config.add_colliding_param = true;
    config.colliding_param_id = collide_id;
    reset_test_processor(config);

    HostApp host_app;
    pulp::format::vst3::PulpVst3Processor processor(create_test_processor);
    REQUIRE(processor.initialize(&host_app) == Steinberg::kResultOk);
    auto* test_processor = TestVst3Processor::g_last_processor;
    REQUIRE(test_processor != nullptr);

    // The colliding controller (channel 0, CC1) is declined — the host owns
    // that ID as a real parameter, not a controller.
    VstCtrl::ParamID assigned = 0;
    REQUIRE(processor.getMidiControllerAssignment(
                0, 0, VstCtrl::kCtrlModWheel, assigned) == Steinberg::kResultFalse);

    // A different controller (channel 0, sustain CC64) is unaffected.
    VstCtrl::ParamID cc64_id = 0;
    REQUIRE(processor.getMidiControllerAssignment(
                0, 0, VstCtrl::kCtrlSustainOnOff, cc64_id) == Steinberg::kResultTrue);
    REQUIRE(cc64_id != collide_id);

    Steinberg::Vst::ProcessSetup setup{};
    setup.processMode = Steinberg::Vst::kRealtime;
    setup.symbolicSampleSize = Steinberg::Vst::kSample32;
    setup.maxSamplesPerBlock = 16;
    setup.sampleRate = 48000.0;
    REQUIRE(processor.setupProcessing(setup) == Steinberg::kResultOk);

    constexpr int kFrames = 16;
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

    // Drive BOTH the colliding real-param ID and the non-colliding controller
    // in the same block.
    Steinberg::Vst::ParameterChanges input_params(2);
    Steinberg::int32 idx = 0;
    auto* collide_queue = input_params.addParameterData(collide_id, idx);
    REQUIRE(collide_queue != nullptr);
    Steinberg::int32 pidx = 0;
    REQUIRE(collide_queue->addPoint(0, 1.0, pidx) == Steinberg::kResultTrue);
    auto* cc64_queue = input_params.addParameterData(cc64_id, idx);
    REQUIRE(cc64_queue != nullptr);
    REQUIRE(cc64_queue->addPoint(4, 1.0, pidx) == Steinberg::kResultTrue);

    Steinberg::Vst::ProcessData data{};
    data.numSamples = kFrames;
    data.numInputs = 1;
    data.numOutputs = 1;
    data.inputs = audio_inputs;
    data.outputs = audio_outputs;
    data.inputParameterChanges = &input_params;

    REQUIRE(processor.process(data) == Steinberg::kResultOk);

    // The colliding ID flowed to store_ as a real parameter (value 1.0), NOT
    // to MIDI — only the non-colliding CC64 appears on midi_in.
    REQUIRE_THAT(test_processor->state().get_value(collide_id),
                 WithinAbs(1.0f, 1e-6f));
    REQUIRE(test_processor->last_midi_in_size == 1);
    const auto& me = test_processor->last_midi_in_events.at(0);
    REQUIRE(me.is_cc());
    REQUIRE(me.cc_number() == 64);
    REQUIRE(me.sample_offset == 4);

    // The colliding ID surfaced as a real parameter event (reached store_ path).
    bool saw_collider_event = false;
    for (const auto& ev : test_processor->last_param_events) {
        if (ev.param_id == collide_id) saw_collider_event = true;
    }
    REQUIRE(saw_collider_event);

    REQUIRE(processor.terminate() == Steinberg::kResultOk);
}

// ── should-fix: defensive value / sample-offset hardening ────────────
TEST_CASE("VST3 IMidiMapping clamps out-of-range controller values and offsets",
          "[vst3][midi][midimapping][robust]") {
    namespace VstCtrl = Steinberg::Vst;
    MidiControllerSetup s;

    VstCtrl::ParamID cc1_id = 0;
    REQUIRE(s.processor.getMidiControllerAssignment(
                0, 0, VstCtrl::kCtrlModWheel, cc1_id) == Steinberg::kResultTrue);

    // Above 1.0 clamps to full scale (127), not a wrapped/garbage byte.
    s.run_one(cc1_id, 0, 1.7);
    REQUIRE(s.test_processor->last_midi_in_size == 1);
    REQUIRE(s.test_processor->last_midi_in_events.at(0).cc_value() == 127);

    // Below 0.0 clamps to 0.
    s.run_one(cc1_id, 0, -0.5);
    REQUIRE(s.test_processor->last_midi_in_size == 1);
    REQUIRE(s.test_processor->last_midi_in_events.at(0).cc_value() == 0);

    // A sample offset outside [0, numSamples) is dropped, not emitted.
    s.run_one(cc1_id, 9999, 0.5);
    REQUIRE(s.test_processor->last_midi_in_size == 0);

    REQUIRE(s.processor.terminate() == Steinberg::kResultOk);
}

// ── should-fix: bounded, allocation-free, drop-on-overflow ───────────
TEST_CASE("VST3 IMidiMapping controller overflow drops without allocating",
          "[vst3][midi][midimapping][realtime][perf]") {
    namespace VstCtrl = Steinberg::Vst;
    MidiControllerSetup s;

    VstCtrl::ParamID cc1_id = 0;
    REQUIRE(s.processor.getMidiControllerAssignment(
                0, 0, VstCtrl::kCtrlModWheel, cc1_id) == Steinberg::kResultTrue);

    // Far more controller points than the reserved per-block MIDI capacity.
    constexpr Steinberg::int32 kFrames = 4096;
    constexpr int kPoints = 4096;
    VstCtrl::ParameterChanges input_params(1);
    Steinberg::int32 idx = 0;
    auto* queue = input_params.addParameterData(cc1_id, idx);
    REQUIRE(queue != nullptr);
    Steinberg::int32 pidx = 0;
    for (int i = 0; i < kPoints; ++i) {
        REQUIRE(queue->addPoint(i % kFrames,
                                static_cast<double>(i % 128) / 127.0,
                                pidx) == Steinberg::kResultTrue);
    }

    std::vector<float> in_l(kFrames, 0.0f), in_r(kFrames, 0.0f);
    std::vector<float> out_l(kFrames, 0.0f), out_r(kFrames, 0.0f);
    float* main_inputs[2] = {in_l.data(), in_r.data()};
    float* main_outputs[2] = {out_l.data(), out_r.data()};
    VstCtrl::AudioBusBuffers audio_inputs[1]{};
    audio_inputs[0].numChannels = 2;
    audio_inputs[0].channelBuffers32 = main_inputs;
    VstCtrl::AudioBusBuffers audio_outputs[1]{};
    audio_outputs[0].numChannels = 2;
    audio_outputs[0].channelBuffers32 = main_outputs;

    VstCtrl::ProcessData data{};
    data.numSamples = kFrames;
    data.numInputs = 1;
    data.numOutputs = 1;
    data.inputs = audio_inputs;
    data.outputs = audio_outputs;
    data.inputParameterChanges = &input_params;

    // Reconfigure prepared block size to the big frame count so the run is
    // legal, then warm + measure: the overflowing controller decode must not
    // allocate, must not crash, and the buffer caps at its reserved size.
    VstCtrl::ProcessSetup big{};
    big.processMode = VstCtrl::kRealtime;
    big.symbolicSampleSize = VstCtrl::kSample32;
    big.maxSamplesPerBlock = kFrames;
    big.sampleRate = 48000.0;
    REQUIRE(s.processor.setupProcessing(big) == Steinberg::kResultOk);

    REQUIRE(s.processor.process(data) == Steinberg::kResultOk);  // warm
    {
        pulp::test::RtAllocationProbe probe;
        REQUIRE(s.processor.process(data) == Steinberg::kResultOk);
        REQUIRE(probe.allocation_count() == 0);
    }
    // Capacity-bounded: dropped past the reserved worst-case, never grew.
    REQUIRE(s.test_processor->last_midi_in_size <= 2048u);

    REQUIRE(s.processor.terminate() == Steinberg::kResultOk);
}

// ── should-fix: deterministic same-offset ordering ───────────────────
TEST_CASE("VST3 IMidiMapping controller and note at same offset sort deterministically",
          "[vst3][midi][midimapping][order]") {
    namespace VstCtrl = Steinberg::Vst;
    MidiControllerSetup s;

    VstCtrl::ParamID pb_id = 0;
    REQUIRE(s.processor.getMidiControllerAssignment(
                0, 0, VstCtrl::kPitchBend, pb_id) == Steinberg::kResultTrue);

    constexpr int kFrames = 16;
    std::array<float, kFrames> in_l{};
    std::array<float, kFrames> in_r{};
    std::array<float, kFrames> out_l{};
    std::array<float, kFrames> out_r{};
    float* main_inputs[2] = {in_l.data(), in_r.data()};
    float* main_outputs[2] = {out_l.data(), out_r.data()};
    VstCtrl::AudioBusBuffers audio_inputs[1]{};
    audio_inputs[0].numChannels = 2;
    audio_inputs[0].channelBuffers32 = main_inputs;
    VstCtrl::AudioBusBuffers audio_outputs[1]{};
    audio_outputs[0].numChannels = 2;
    audio_outputs[0].channelBuffers32 = main_outputs;

    // A note-on AND a pitch-bend, both at sample offset 8.
    VstCtrl::EventList events(2);
    VstCtrl::Event note_on{};
    note_on.type = VstCtrl::Event::kNoteOnEvent;
    note_on.sampleOffset = 8;
    note_on.noteOn.channel = 0;
    note_on.noteOn.pitch = 60;
    note_on.noteOn.velocity = 0.5f;
    REQUIRE(events.addEvent(note_on) == Steinberg::kResultOk);

    auto run_capture = [&]() {
        VstCtrl::ParameterChanges input_params(1);
        Steinberg::int32 idx = 0;
        auto* q = input_params.addParameterData(pb_id, idx);
        REQUIRE(q != nullptr);
        Steinberg::int32 pidx = 0;
        REQUIRE(q->addPoint(8, 1.0, pidx) == Steinberg::kResultTrue);

        VstCtrl::ProcessData data{};
        data.numSamples = kFrames;
        data.numInputs = 1;
        data.numOutputs = 1;
        data.inputs = audio_inputs;
        data.outputs = audio_outputs;
        data.inputParameterChanges = &input_params;
        data.inputEvents = &events;
        REQUIRE(s.processor.process(data) == Steinberg::kResultOk);

        std::vector<std::pair<int, bool>> seq;  // (offset, is_pitch_bend)
        for (const auto& m : s.test_processor->last_midi_in_events) {
            seq.emplace_back(m.sample_offset, m.is_pitch_bend());
        }
        return seq;
    };

    const auto first = run_capture();
    const auto second = run_capture();
    REQUIRE(first.size() == 2);
    // Both events at offset 8, identical relative order run-to-run.
    REQUIRE(first[0].first == 8);
    REQUIRE(first[1].first == 8);
    REQUIRE(first == second);
    // Insertion-order semantics: the adapter decodes controllers in the
    // parameter-change loop, which runs BEFORE the note/SysEx event loop, so
    // the pitch-bend was add()'ed before the note-on and must stay first after
    // the insertion-stable sort. (A byte tie-break would have put the note-on —
    // status 0x90 — before the pitch-bend — status 0xE0.)
    REQUIRE(first[0].second == true);   // pitch-bend first
    REQUIRE(first[1].second == false);  // note-on second

    REQUIRE(s.processor.terminate() == Steinberg::kResultOk);
}

// Direct MidiBuffer contract: sort() is insertion-stable at equal offsets.
// This is where the same-offset musical semantics live (every adapter + the
// synth consume it), so assert the order primitives directly rather than only
// through one adapter.
TEST_CASE("MidiBuffer sort is insertion-stable for equal sample offsets",
          "[midi][buffer][order]") {
    pulp::midi::MidiBuffer buf;
    buf.reserve(8);
    buf.set_realtime_capacity_limit(true);

    // All at offset 4, inserted in a deliberate order:
    //   CC (0xB0) → note-off (0x80) → note-on (0x90) → pitch-bend (0xE0)
    // A byte/status tie-break would reorder these (0x80 < 0x90 < 0xB0 < 0xE0);
    // insertion order must preserve the sequence as inserted.
    auto cc      = pulp::midi::MidiEvent::cc(0, 64, 100);  cc.sample_offset = 4;
    auto noteoff = pulp::midi::MidiEvent::note_off(0, 60, 0); noteoff.sample_offset = 4;
    auto noteon  = pulp::midi::MidiEvent::note_on(0, 60, 100); noteon.sample_offset = 4;
    auto bend    = pulp::midi::MidiEvent::pitch_bend(0, 12000); bend.sample_offset = 4;
    REQUIRE(buf.add(cc));
    REQUIRE(buf.add(noteoff));
    REQUIRE(buf.add(noteon));
    REQUIRE(buf.add(bend));

    // An earlier-offset event added LAST must sort to the front; a later one
    // to the back — the primary key is still sample_offset.
    auto early = pulp::midi::MidiEvent::cc(0, 1, 5); early.sample_offset = 0;
    auto late  = pulp::midi::MidiEvent::cc(0, 1, 9); late.sample_offset = 9;
    REQUIRE(buf.add(early));
    REQUIRE(buf.add(late));

    buf.sort();

    std::vector<std::pair<int, uint8_t>> got;  // (offset, status byte)
    for (const auto& m : buf) {
        got.emplace_back(m.sample_offset, m.data()[0] & 0xF0);
    }
    // offset 0 first, offset 9 last; the four offset-4 events keep insertion
    // order: CC(0xB0), note-off(0x80), note-on(0x90), pitch-bend(0xE0).
    REQUIRE(got.size() == 6);
    REQUIRE(got[0] == std::make_pair(0, static_cast<uint8_t>(0xB0)));
    REQUIRE(got[1] == std::make_pair(4, static_cast<uint8_t>(0xB0)));  // CC
    REQUIRE(got[2] == std::make_pair(4, static_cast<uint8_t>(0x80)));  // note-off
    REQUIRE(got[3] == std::make_pair(4, static_cast<uint8_t>(0x90)));  // note-on
    REQUIRE(got[4] == std::make_pair(4, static_cast<uint8_t>(0xE0)));  // pitch-bend
    REQUIRE(got[5] == std::make_pair(9, static_cast<uint8_t>(0xB0)));

    // Re-sorting an already-sorted buffer is idempotent (stable).
    buf.sort();
    std::vector<std::pair<int, uint8_t>> again;
    for (const auto& m : buf) {
        again.emplace_back(m.sample_offset, m.data()[0] & 0xF0);
    }
    REQUIRE(again == got);
}

// ── should-fix: parameter count delta, state exclusion, flags ────────
TEST_CASE("VST3 IMidiMapping adds exactly 2080 hidden controller params and keeps state clean",
          "[vst3][midi][midimapping][params][state]") {
    namespace VstCtrl = Steinberg::Vst;
    constexpr int kControllers =
        pulp::format::detail::kVst3MidiChannels *
        pulp::format::detail::kVst3ControllersPerChannel;  // 2080

    // Disable the bypass-synthesis quirk so the count delta is exactly the
    // controller set, with no synthesized extras to subtract.
    pulp::format::set_host_quirk_policy(pulp::format::kQuirkFilterOff);

    Steinberg::int32 audio_only_count = 0;
    {
        reset_test_processor();  // accepts_midi == false
        HostApp host_app;
        pulp::format::vst3::PulpVst3Processor processor(create_test_processor);
        REQUIRE(processor.initialize(&host_app) == Steinberg::kResultOk);
        audio_only_count = processor.getParameterCount();
        REQUIRE(audio_only_count == 1);  // Gain only
        REQUIRE(processor.terminate() == Steinberg::kResultOk);
    }

    TestVst3Config config;
    config.descriptor.accepts_midi = true;
    reset_test_processor(config);
    HostApp host_app;
    pulp::format::vst3::PulpVst3Processor processor(create_test_processor);
    REQUIRE(processor.initialize(&host_app) == Steinberg::kResultOk);
    auto* test_processor = TestVst3Processor::g_last_processor;
    REQUIRE(test_processor != nullptr);

    // Exactly +2080 over the audio-only set.
    REQUIRE(processor.getParameterCount() == audio_only_count + kControllers);

    // Every controller param is kIsHidden AND NOT kCanAutomate.
    VstCtrl::ParamID cc1_id = 0;
    REQUIRE(processor.getMidiControllerAssignment(
                0, 0, VstCtrl::kCtrlModWheel, cc1_id) == Steinberg::kResultTrue);
    bool checked = false;
    const Steinberg::int32 count = processor.getParameterCount();
    for (Steinberg::int32 i = 0; i < count; ++i) {
        ParameterInfo pi{};
        REQUIRE(processor.getParameterInfo(i, pi) == Steinberg::kResultOk);
        if (pi.id == cc1_id) {
            checked = true;
            REQUIRE((pi.flags & ParameterInfo::kIsHidden) != 0);
            REQUIRE((pi.flags & ParameterInfo::kCanAutomate) == 0);
        }
    }
    REQUIRE(checked);

    // Saved state contains ONLY real/store params: the serialized blob must
    // not contain the controller ID's little-endian bytes.
    test_processor->state().set_value(kGainParamId, -12.0f);
    VectorStream out_stream;
    REQUIRE(processor.getState(&out_stream) == Steinberg::kResultOk);
    const auto blob = out_stream.take();
    const std::array<uint8_t, 4> needle{
        static_cast<uint8_t>(cc1_id & 0xFF),
        static_cast<uint8_t>((cc1_id >> 8) & 0xFF),
        static_cast<uint8_t>((cc1_id >> 16) & 0xFF),
        static_cast<uint8_t>((cc1_id >> 24) & 0xFF)};
    const bool present = std::search(blob.begin(), blob.end(),
                                     needle.begin(), needle.end()) != blob.end();
    REQUIRE_FALSE(present);

    REQUIRE(processor.terminate() == Steinberg::kResultOk);
    pulp::format::set_host_quirk_policy(std::nullopt);
}

// ── should-fix: queryInterface regression guard ──────────────────────
TEST_CASE("VST3 queryInterface exposes base interfaces and IMidiMapping with one AddRef",
          "[vst3][midi][midimapping][queryinterface]") {
    reset_test_processor();
    HostApp host_app;
    pulp::format::vst3::PulpVst3Processor processor(create_test_processor);
    REQUIRE(processor.initialize(&host_app) == Steinberg::kResultOk);

    auto query = [&](const Steinberg::TUID iid) -> void* {
        void* obj = nullptr;
        if (processor.queryInterface(iid, &obj) == Steinberg::kResultOk) {
            return obj;
        }
        return nullptr;
    };

    // Base interfaces still resolve through the override.
    void* comp = query(Steinberg::Vst::IComponent::iid);
    REQUIRE(comp != nullptr);
    static_cast<Steinberg::FUnknown*>(comp)->release();

    void* proc = query(Steinberg::Vst::IAudioProcessor::iid);
    REQUIRE(proc != nullptr);
    static_cast<Steinberg::FUnknown*>(proc)->release();

    void* edit = query(Steinberg::Vst::IEditController::iid);
    REQUIRE(edit != nullptr);
    static_cast<Steinberg::FUnknown*>(edit)->release();

    // The newly added interface resolves and is a valid IMidiMapping.
    void* mm = nullptr;
    REQUIRE(processor.queryInterface(Steinberg::Vst::IMidiMapping::iid, &mm) ==
            Steinberg::kResultOk);
    REQUIRE(mm != nullptr);
    auto* midi_mapping = static_cast<Steinberg::Vst::IMidiMapping*>(mm);
    Steinberg::Vst::ParamID dummy = 0;
    // The plug-in does not accept MIDI, so the query is declined — but the
    // interface pointer itself is callable (no crash), proving the cast is
    // sound and queryInterface returned the right vtable.
    REQUIRE(midi_mapping->getMidiControllerAssignment(
                0, 0, Steinberg::Vst::kCtrlModWheel, dummy) ==
            Steinberg::kResultFalse);
    static_cast<Steinberg::FUnknown*>(mm)->release();

    REQUIRE(processor.terminate() == Steinberg::kResultOk);
}

// ── INoteExpressionController — per-note expression (MPE) input ─────────────
//
// VST3 delivers per-note pitch / pressure / timbre as
// Event::kNoteExpressionValueEvent keyed by the originating note-on's noteId.
// The host first queries getNoteExpressionInfo to learn which expression types
// the plug-in accepts; the adapter declares them only when the descriptor opts
// into MPE. process() routes each value event through the shared MpeVoiceTracker
// / MpeBuffer sidecar, mapping VST3 tuning -> per-note pitch bend, volume ->
// per-note pressure, and brightness -> per-note timbre (CC74). A note-on on an
// MPE member channel (lower zone: channels 1-15) creates the voice the
// expression then narrows to.
// ─────────────────────────────────────────────────────────────────────

namespace {

// Drive note-on / note-expression events for an MPE-capable plug-in and expose
// the per-note expression buffer the processor saw. Lower-zone MPE: channel 0
// is the manager, channels 1-15 are member channels.
struct NoteExpressionSetup {
    pulp::format::vst3::PulpVst3Processor processor{create_test_processor};
    TestVst3Processor* test_processor = nullptr;
    HostApp host_app;

    explicit NoteExpressionSetup(bool supports_mpe = true) {
        TestVst3Config config;
        config.descriptor.accepts_midi = true;
        config.descriptor.supports_mpe = supports_mpe;
        reset_test_processor(config);
        REQUIRE(processor.initialize(&host_app) == Steinberg::kResultOk);
        test_processor = TestVst3Processor::g_last_processor;
        REQUIRE(test_processor != nullptr);

        Steinberg::Vst::ProcessSetup setup{};
        setup.processMode = Steinberg::Vst::kRealtime;
        setup.symbolicSampleSize = Steinberg::Vst::kSample32;
        setup.maxSamplesPerBlock = 32;
        setup.sampleRate = 48000.0;
        REQUIRE(processor.setupProcessing(setup) == Steinberg::kResultOk);
    }

    // Run a process() block over the given event list (and optional parameter
    // changes, e.g. IMidiMapping controllers) with a small stereo buffer. The
    // lists are owned by the caller.
    void run(Steinberg::Vst::IEventList* events,
             Steinberg::Vst::IParameterChanges* params = nullptr) {
        constexpr int kFrames = 32;
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

        Steinberg::Vst::ProcessData data{};
        data.numSamples = kFrames;
        data.numInputs = 1;
        data.numOutputs = 1;
        data.inputs = audio_inputs;
        data.outputs = audio_outputs;
        data.inputEvents = events;
        data.inputParameterChanges = params;

        REQUIRE(processor.process(data) == Steinberg::kResultOk);
    }
};

// Build a note-on event on an MPE member channel with the given noteId.
Steinberg::Vst::Event make_note_on(Steinberg::int16 channel,
                                   Steinberg::int16 pitch, float velocity,
                                   Steinberg::int32 note_id,
                                   Steinberg::int32 offset) {
    Steinberg::Vst::Event e{};
    e.type = Steinberg::Vst::Event::kNoteOnEvent;
    e.sampleOffset = offset;
    e.noteOn.channel = channel;
    e.noteOn.pitch = pitch;
    e.noteOn.velocity = velocity;
    e.noteOn.noteId = note_id;
    return e;
}

Steinberg::Vst::Event make_note_off(Steinberg::int16 channel,
                                    Steinberg::int16 pitch,
                                    Steinberg::int32 note_id,
                                    Steinberg::int32 offset) {
    Steinberg::Vst::Event e{};
    e.type = Steinberg::Vst::Event::kNoteOffEvent;
    e.sampleOffset = offset;
    e.noteOff.channel = channel;
    e.noteOff.pitch = pitch;
    e.noteOff.velocity = 0.0f;
    e.noteOff.noteId = note_id;
    return e;
}

Steinberg::Vst::Event make_note_expr(
    Steinberg::Vst::NoteExpressionTypeID type_id, Steinberg::int32 note_id,
    double value, Steinberg::int32 offset) {
    Steinberg::Vst::Event e{};
    e.type = Steinberg::Vst::Event::kNoteExpressionValueEvent;
    e.sampleOffset = offset;
    e.noteExpressionValue.typeId = type_id;
    e.noteExpressionValue.noteId = note_id;
    e.noteExpressionValue.value = value;
    return e;
}

// Find the newest expression event of a given kind in the captured buffer.
const pulp::midi::MpeExpressionEvent* find_mpe(
    const std::vector<pulp::midi::MpeExpressionEvent>& events,
    pulp::midi::MpeExpressionEvent::Kind kind) {
    const pulp::midi::MpeExpressionEvent* found = nullptr;
    for (const auto& e : events) {
        if (e.kind == kind) found = &e;
    }
    return found;
}

}  // namespace

TEST_CASE("VST3 INoteExpressionController declares types only for an MPE plug-in",
          "[vst3][midi][noteexpression][mpe]") {
    namespace V = Steinberg::Vst;

    SECTION("MPE plug-in declares the supported note-expression types") {
        NoteExpressionSetup s(/*supports_mpe=*/true);
        const Steinberg::int32 count = s.processor.getNoteExpressionCount(0, 1);
        REQUIRE(count > 0);

        // Every declared type must resolve to valid info, and the set must
        // include the three MPE axes: tuning, volume (pressure), brightness.
        bool has_tuning = false, has_volume = false, has_brightness = false;
        for (Steinberg::int32 i = 0; i < count; ++i) {
            V::NoteExpressionTypeInfo info{};
            REQUIRE(s.processor.getNoteExpressionInfo(0, 1, i, info) ==
                    Steinberg::kResultTrue);
            if (info.typeId == V::kTuningTypeID) has_tuning = true;
            if (info.typeId == V::kVolumeTypeID) has_volume = true;
            if (info.typeId == V::kBrightnessTypeID) has_brightness = true;
            // Declared range is the full normalized [0,1] window.
            REQUIRE(info.valueDesc.minimum == 0.0);
            REQUIRE(info.valueDesc.maximum == 1.0);
        }
        REQUIRE(has_tuning);
        REQUIRE(has_volume);
        REQUIRE(has_brightness);

        // Tuning is bipolar (centered at 0.5).
        V::NoteExpressionTypeInfo tuning{};
        bool found_tuning_info = false;
        for (Steinberg::int32 i = 0; i < count; ++i) {
            V::NoteExpressionTypeInfo info{};
            REQUIRE(s.processor.getNoteExpressionInfo(0, 1, i, info) ==
                    Steinberg::kResultTrue);
            if (info.typeId == V::kTuningTypeID) {
                tuning = info;
                found_tuning_info = true;
            }
        }
        REQUIRE(found_tuning_info);
        REQUIRE((tuning.flags & V::NoteExpressionTypeInfo::kIsBipolar) != 0);
        REQUIRE(tuning.valueDesc.defaultValue == 0.5);

        // Out-of-range index / wrong bus / wrong channel are declined.
        V::NoteExpressionTypeInfo dummy{};
        REQUIRE(s.processor.getNoteExpressionInfo(0, 1, count, dummy) ==
                Steinberg::kResultFalse);
        REQUIRE(s.processor.getNoteExpressionCount(1, 1) == 0);
        REQUIRE(s.processor.getNoteExpressionCount(0, 16) == 0);

        REQUIRE(s.processor.terminate() == Steinberg::kResultOk);
    }

    SECTION("non-MPE plug-in declares zero note-expression types") {
        NoteExpressionSetup s(/*supports_mpe=*/false);
        REQUIRE(s.processor.getNoteExpressionCount(0, 1) == 0);
        V::NoteExpressionTypeInfo dummy{};
        REQUIRE(s.processor.getNoteExpressionInfo(0, 1, 0, dummy) ==
                Steinberg::kResultFalse);
        REQUIRE(s.processor.terminate() == Steinberg::kResultOk);
    }
}

TEST_CASE("VST3 note-expression tuning routes to per-note pitch bend",
          "[vst3][midi][noteexpression][mpe][process]") {
    namespace V = Steinberg::Vst;
    NoteExpressionSetup s;

    // Note-on (noteId 42) on member channel 1, then a tuning expression for
    // that noteId. norm 0.5 + (1/240) == one half-semitone up.
    constexpr Steinberg::int32 kNoteId = 42;
    constexpr Steinberg::int16 kChannel = 1;
    constexpr Steinberg::int16 kPitch = 60;
    // +1 semitone up: norm = plain/240 + 0.5, plain = 1.0 -> norm = 0.5 + 1/240.
    const double tuning_norm = 0.5 + 1.0 / 240.0;

    Steinberg::Vst::EventList events(4);
    auto on = make_note_on(kChannel, kPitch, 0.8f, kNoteId, 0);
    auto expr = make_note_expr(V::kTuningTypeID, kNoteId, tuning_norm, 4);
    REQUIRE(events.addEvent(on) == Steinberg::kResultOk);
    REQUIRE(events.addEvent(expr) == Steinberg::kResultOk);
    s.run(&events);

    REQUIRE(s.test_processor->mpe_input_attached);
    // The tracker must have created the note and applied a per-note pitch bend.
    const auto* pb = find_mpe(s.test_processor->last_mpe_events,
                              pulp::midi::MpeExpressionEvent::Kind::PitchBend);
    REQUIRE(pb != nullptr);
    REQUIRE(pb->state.channel == static_cast<uint8_t>(kChannel));
    REQUIRE(pb->state.note == static_cast<uint8_t>(kPitch));
    // +1 semitone up, within the tracker's quantization tolerance.
    REQUIRE_THAT(pb->state.pitch_bend_semitones, WithinAbs(1.0f, 0.05f));

    REQUIRE(s.processor.terminate() == Steinberg::kResultOk);
}

TEST_CASE("VST3 note-expression volume routes to per-note pressure",
          "[vst3][midi][noteexpression][mpe][process]") {
    namespace V = Steinberg::Vst;
    NoteExpressionSetup s;

    constexpr Steinberg::int32 kNoteId = 7;
    constexpr Steinberg::int16 kChannel = 2;
    constexpr Steinberg::int16 kPitch = 64;

    Steinberg::Vst::EventList events(4);
    auto on = make_note_on(kChannel, kPitch, 0.5f, kNoteId, 0);
    auto expr = make_note_expr(V::kVolumeTypeID, kNoteId, 1.0, 8);  // full
    REQUIRE(events.addEvent(on) == Steinberg::kResultOk);
    REQUIRE(events.addEvent(expr) == Steinberg::kResultOk);
    s.run(&events);

    const auto* pr = find_mpe(s.test_processor->last_mpe_events,
                              pulp::midi::MpeExpressionEvent::Kind::Pressure);
    REQUIRE(pr != nullptr);
    REQUIRE(pr->state.channel == static_cast<uint8_t>(kChannel));
    REQUIRE(pr->state.note == static_cast<uint8_t>(kPitch));
    REQUIRE_THAT(pr->state.pressure, WithinAbs(1.0f, 0.01f));

    REQUIRE(s.processor.terminate() == Steinberg::kResultOk);
}

TEST_CASE("VST3 note-expression brightness routes to per-note timbre",
          "[vst3][midi][noteexpression][mpe][process]") {
    namespace V = Steinberg::Vst;
    NoteExpressionSetup s;

    constexpr Steinberg::int32 kNoteId = 99;
    constexpr Steinberg::int16 kChannel = 3;
    constexpr Steinberg::int16 kPitch = 67;

    Steinberg::Vst::EventList events(4);
    auto on = make_note_on(kChannel, kPitch, 0.6f, kNoteId, 0);
    auto expr = make_note_expr(V::kBrightnessTypeID, kNoteId, 0.5, 2);  // mid
    REQUIRE(events.addEvent(on) == Steinberg::kResultOk);
    REQUIRE(events.addEvent(expr) == Steinberg::kResultOk);
    s.run(&events);

    const auto* tb = find_mpe(s.test_processor->last_mpe_events,
                              pulp::midi::MpeExpressionEvent::Kind::Timbre);
    REQUIRE(tb != nullptr);
    REQUIRE(tb->state.channel == static_cast<uint8_t>(kChannel));
    REQUIRE(tb->state.note == static_cast<uint8_t>(kPitch));
    // CC74 64/127 ~= 0.5.
    REQUIRE_THAT(tb->state.timbre, WithinAbs(64.0f / 127.0f, 0.02f));

    REQUIRE(s.processor.terminate() == Steinberg::kResultOk);
}

TEST_CASE("VST3 note-expression for an unknown noteId is ignored",
          "[vst3][midi][noteexpression][mpe][process]") {
    namespace V = Steinberg::Vst;
    NoteExpressionSetup s;

    // A tuning expression referencing a noteId that was never opened must not
    // synthesize any per-note state (no matching voice to route it to).
    Steinberg::Vst::EventList events(2);
    auto expr = make_note_expr(V::kTuningTypeID, /*note_id=*/1234, 0.7, 0);
    REQUIRE(events.addEvent(expr) == Steinberg::kResultOk);
    s.run(&events);

    REQUIRE(s.test_processor->mpe_input_attached);
    const auto* pb = find_mpe(s.test_processor->last_mpe_events,
                              pulp::midi::MpeExpressionEvent::Kind::PitchBend);
    REQUIRE(pb == nullptr);

    REQUIRE(s.processor.terminate() == Steinberg::kResultOk);
}

TEST_CASE("VST3 note-off releases the noteId so later expressions don't route",
          "[vst3][midi][noteexpression][mpe][process]") {
    namespace V = Steinberg::Vst;
    NoteExpressionSetup s;

    constexpr Steinberg::int32 kNoteId = 5;
    constexpr Steinberg::int16 kChannel = 4;
    constexpr Steinberg::int16 kPitch = 62;

    // Block 1: note-on then note-off for the same noteId.
    {
        Steinberg::Vst::EventList events(4);
        auto on = make_note_on(kChannel, kPitch, 0.7f, kNoteId, 0);
        auto off = make_note_off(kChannel, kPitch, kNoteId, 16);
        REQUIRE(events.addEvent(on) == Steinberg::kResultOk);
        REQUIRE(events.addEvent(off) == Steinberg::kResultOk);
        s.run(&events);
    }

    // Block 2: a tuning expression for the now-released noteId. With the
    // mapping erased, the adapter does not synthesize a bend for it.
    {
        Steinberg::Vst::EventList events(2);
        auto expr = make_note_expr(V::kTuningTypeID, kNoteId, 0.75, 0);
        REQUIRE(events.addEvent(expr) == Steinberg::kResultOk);
        s.run(&events);
    }

    const auto* pb = find_mpe(s.test_processor->last_mpe_events,
                              pulp::midi::MpeExpressionEvent::Kind::PitchBend);
    REQUIRE(pb == nullptr);

    REQUIRE(s.processor.terminate() == Steinberg::kResultOk);
}

TEST_CASE("VST3 non-MPE plug-in ignores note expressions and gets no MPE input",
          "[vst3][midi][noteexpression][mpe][process]") {
    namespace V = Steinberg::Vst;
    NoteExpressionSetup s(/*supports_mpe=*/false);

    Steinberg::Vst::EventList events(4);
    auto on = make_note_on(1, 60, 0.8f, /*note_id=*/1, 0);
    auto expr = make_note_expr(V::kTuningTypeID, /*note_id=*/1, 0.75, 4);
    REQUIRE(events.addEvent(on) == Steinberg::kResultOk);
    REQUIRE(events.addEvent(expr) == Steinberg::kResultOk);
    s.run(&events);

    // No MPE sidecar is attached, so the processor sees mpe_input() == nullptr.
    REQUIRE_FALSE(s.test_processor->mpe_input_attached);
    REQUIRE(s.test_processor->mpe_event_count == 0);

    REQUIRE(s.processor.terminate() == Steinberg::kResultOk);
}

TEST_CASE("VST3 note-expression decode does not allocate on the audio thread",
          "[vst3][midi][noteexpression][mpe][realtime][perf]") {
    namespace V = Steinberg::Vst;
    NoteExpressionSetup s;

    // Build the event list once outside the probe (EventList's ctor allocates),
    // then measure only process(): a note-on plus all three expression axes.
    constexpr Steinberg::int32 kNoteId = 21;
    Steinberg::Vst::EventList events(8);
    auto on = make_note_on(1, 60, 0.8f, kNoteId, 0);
    auto tun = make_note_expr(V::kTuningTypeID, kNoteId, 0.6, 1);
    auto vol = make_note_expr(V::kVolumeTypeID, kNoteId, 0.5, 2);
    auto brt = make_note_expr(V::kBrightnessTypeID, kNoteId, 0.4, 3);
    REQUIRE(events.addEvent(on) == Steinberg::kResultOk);
    REQUIRE(events.addEvent(tun) == Steinberg::kResultOk);
    REQUIRE(events.addEvent(vol) == Steinberg::kResultOk);
    REQUIRE(events.addEvent(brt) == Steinberg::kResultOk);

    s.run(&events);  // warm: capacity established
    {
        pulp::test::RtAllocationProbe probe;
        s.run(&events);
        REQUIRE(probe.allocation_count() == 0);
    }

    REQUIRE(s.processor.terminate() == Steinberg::kResultOk);
}

TEST_CASE("VST3 INoteExpressionController resolves via queryInterface",
          "[vst3][midi][noteexpression][mpe]") {
    namespace V = Steinberg::Vst;
    TestVst3Config config;
    config.descriptor.accepts_midi = true;
    config.descriptor.supports_mpe = true;
    reset_test_processor(config);

    HostApp host_app;
    pulp::format::vst3::PulpVst3Processor processor(create_test_processor);
    REQUIRE(processor.initialize(&host_app) == Steinberg::kResultOk);

    void* nec = nullptr;
    REQUIRE(processor.queryInterface(V::INoteExpressionController::iid, &nec) ==
            Steinberg::kResultOk);
    REQUIRE(nec != nullptr);
    auto* controller = static_cast<V::INoteExpressionController*>(nec);
    REQUIRE(controller->getNoteExpressionCount(0, 1) > 0);
    static_cast<Steinberg::FUnknown*>(nec)->release();

    REQUIRE(processor.terminate() == Steinberg::kResultOk);
}

// ── INoteExpressionController — adversarial / documented-semantics cases ────
//
// These pin the ACCEPTED behavior (they document, they do not change it): the
// channel-wide MPE bridge, the bounded noteId map's drop observability, and the
// IMidiMapping ∩ note-expression interleave on midi_in_.
// ─────────────────────────────────────────────────────────────────────

namespace {

std::size_t count_mpe(const std::vector<pulp::midi::MpeExpressionEvent>& events,
                      pulp::midi::MpeExpressionEvent::Kind kind) {
    std::size_t n = 0;
    for (const auto& e : events) {
        if (e.kind == kind) ++n;
    }
    return n;
}

}  // namespace

TEST_CASE("VST3 note-expression on a shared channel is channel-wide by design (matches CLAP)",
          "[vst3][midi][noteexpression][mpe][process]") {
    namespace V = Steinberg::Vst;
    NoteExpressionSetup s;

    // Two notes share the SAME MPE member channel (atypical for MPE, where one
    // note owns a member channel). A tuning expression for noteId A is bridged
    // to a channel-wide pitch bend; the MpeVoiceTracker applies channel pitch
    // bend to EVERY active note on that channel. This is the documented,
    // CLAP-consistent collapse — pinned here, not "fixed".
    constexpr Steinberg::int16 kChannel = 1;
    Steinberg::Vst::EventList events(4);
    auto onA = make_note_on(kChannel, 60, 0.8f, /*note_id=*/1, 0);
    auto onB = make_note_on(kChannel, 64, 0.8f, /*note_id=*/2, 0);
    // +1 semitone up tuning for note A only (noteId 1).
    auto expr = make_note_expr(V::kTuningTypeID, /*note_id=*/1,
                               0.5 + 1.0 / 240.0, 4);
    REQUIRE(events.addEvent(onA) == Steinberg::kResultOk);
    REQUIRE(events.addEvent(onB) == Steinberg::kResultOk);
    REQUIRE(events.addEvent(expr) == Steinberg::kResultOk);
    s.run(&events);

    // BOTH notes on the channel see the bend (channel-wide by design). The
    // tracker holds two active notes on the member channel, and the pitch-bend
    // callback fires for each. Assert both reached +1 st.
    std::size_t bent_notes = 0;
    for (const auto& e : s.test_processor->last_mpe_events) {
        if (e.kind == pulp::midi::MpeExpressionEvent::Kind::PitchBend) {
            REQUIRE(e.state.channel == static_cast<uint8_t>(kChannel));
            REQUIRE_THAT(e.state.pitch_bend_semitones, WithinAbs(1.0f, 0.05f));
            ++bent_notes;
        }
    }
    // At least the two notes share the channel-wide bend (one PitchBend event
    // per active note on the channel). Documented semantics, identical to CLAP.
    REQUIRE(bent_notes >= 2);

    REQUIRE(s.processor.terminate() == Steinberg::kResultOk);
}

TEST_CASE("VST3 noteId-map overflow bumps the drop counter without allocating",
          "[vst3][midi][noteexpression][mpe][realtime]") {
    NoteExpressionSetup s;

    // kMaxLiveNoteIds == 128 live noteId slots. Open 128 notes (fills the map),
    // then a 129th whose mapping must be dropped. Spread across member channels
    // 1..15 (channel 0 is the MPE manager and creates no voice). Build the event
    // list outside the probe; the only allocation in process() we care about is
    // none.
    constexpr int kCapacity = 128;
    Steinberg::Vst::EventList events(kCapacity + 1);
    for (int i = 0; i < kCapacity + 1; ++i) {
        const Steinberg::int16 channel =
            static_cast<Steinberg::int16>(1 + (i % 15));  // 1..15
        const Steinberg::int16 pitch =
            static_cast<Steinberg::int16>(36 + (i % 60));
        auto on = make_note_on(channel, pitch, 0.7f, /*note_id=*/i, 0);
        REQUIRE(events.addEvent(on) == Steinberg::kResultOk);
    }

    // Warm once outside the probe so the adapter's reserved buffers AND the test
    // processor's capture vector reach steady-state capacity; the probe then
    // measures only the adapter's process() path (which must not allocate even
    // when the noteId map overflows). The overflow drop is RT-safe — the only
    // "allocation" a naive impl could hit is the bounded map, which we don't
    // grow. setActive(false)/setupProcessing reset the map + drop counter, so
    // re-warm via a second setup-free run instead: run twice, measure the
    // second.
    s.run(&events);
    const auto drops_after_warm = s.processor.note_expression_drop_count();
    REQUIRE(drops_after_warm >= 1);  // overflow already observed on warm run

    {
        pulp::test::RtAllocationProbe probe;
        s.run(&events);
        REQUIRE(probe.allocation_count() == 0);
    }

    // The drop counter is monotonic within an activation (saturating, not reset
    // per block), so after two overflowing blocks it is >= 2.
    REQUIRE(s.processor.note_expression_drop_count() >= 2);
    // The processor still ran and saw the MPE sidecar.
    REQUIRE(s.test_processor->mpe_input_attached);

    REQUIRE(s.processor.terminate() == Steinberg::kResultOk);
}

TEST_CASE("VST3 expression for an unmapped noteId bumps the drop counter",
          "[vst3][midi][noteexpression][mpe][process]") {
    namespace V = Steinberg::Vst;
    NoteExpressionSetup s;

    // No note-on for this noteId — the expression has nowhere to route, which
    // the adapter records as an observable drop (not silently ignored).
    Steinberg::Vst::EventList events(2);
    auto expr = make_note_expr(V::kTuningTypeID, /*note_id=*/9999, 0.7, 0);
    REQUIRE(events.addEvent(expr) == Steinberg::kResultOk);
    s.run(&events);

    REQUIRE(s.processor.note_expression_drop_count() >= 1);

    REQUIRE(s.processor.terminate() == Steinberg::kResultOk);
}

TEST_CASE("VST3 IMidiMapping pitch bend and note-expression tuning coexist on midi_in_",
          "[vst3][midi][noteexpression][mpe][midimapping][process]") {
    namespace V = Steinberg::Vst;
    NoteExpressionSetup s;

    // A note-on (creates the voice) + a note-expression tuning, AND an
    // IMidiMapping pitch-bend controller change, all on the same channel at the
    // same offset. Both reach midi_in_, are sorted deterministically, and the
    // MpeVoiceTracker consumes both without crashing — the last pitch bend in
    // sample order wins. This pins that the two per-note input paths interleave
    // cleanly.
    constexpr Steinberg::int16 kChannel = 1;
    constexpr Steinberg::int32 kNoteId = 11;

    // Resolve the pitch-bend controller ParamID for channel 1.
    V::ParamID pb_id = 0;
    REQUIRE(s.processor.getMidiControllerAssignment(
                0, kChannel, V::kPitchBend, pb_id) == Steinberg::kResultTrue);

    Steinberg::Vst::EventList events(4);
    auto on = make_note_on(kChannel, 60, 0.8f, kNoteId, 0);
    // Note-expression tuning: +1 semitone up.
    auto expr = make_note_expr(V::kTuningTypeID, kNoteId, 0.5 + 1.0 / 240.0, 5);
    REQUIRE(events.addEvent(on) == Steinberg::kResultOk);
    REQUIRE(events.addEvent(expr) == Steinberg::kResultOk);

    // IMidiMapping pitch bend to centre (0.5 -> 8192 -> 0 semitones) at a LATER
    // offset, so in sorted order it is the last bend the tracker sees.
    Steinberg::Vst::ParameterChanges params(1);
    Steinberg::int32 pidx = 0;
    auto* q = params.addParameterData(pb_id, pidx);
    REQUIRE(q != nullptr);
    Steinberg::int32 ptidx = 0;
    REQUIRE(q->addPoint(10, 0.5, ptidx) == Steinberg::kResultTrue);

    s.run(&events, &params);

    // Both per-note input sources were consumed: there is at least one pitch
    // bend, and the final bend state is the IMidiMapping centre (0 st), proving
    // deterministic sample-ordered interleave (the later controller wins).
    REQUIRE(count_mpe(s.test_processor->last_mpe_events,
                      pulp::midi::MpeExpressionEvent::Kind::PitchBend) >= 1);
    const auto* last_pb = find_mpe(s.test_processor->last_mpe_events,
                                   pulp::midi::MpeExpressionEvent::Kind::PitchBend);
    REQUIRE(last_pb != nullptr);
    REQUIRE(last_pb->state.channel == static_cast<uint8_t>(kChannel));
    REQUIRE_THAT(last_pb->state.pitch_bend_semitones, WithinAbs(0.0f, 0.05f));

    REQUIRE(s.processor.terminate() == Steinberg::kResultOk);
}

TEST_CASE("VST3 expression on the MPE manager channel creates no per-note voice",
          "[vst3][midi][noteexpression][mpe][process]") {
    namespace V = Steinberg::Vst;
    NoteExpressionSetup s;

    // Lower-zone MPE: channel 0 is the manager. A note-on on channel 0 does NOT
    // create a member voice, so a tuning expression for its noteId produces no
    // per-note pitch bend (no mis-route onto a member voice). The noteId still
    // maps (so it is not an unmapped-drop), but the tracker holds no member note.
    constexpr Steinberg::int16 kManager = 0;
    constexpr Steinberg::int32 kNoteId = 3;
    Steinberg::Vst::EventList events(4);
    auto on = make_note_on(kManager, 60, 0.8f, kNoteId, 0);
    auto expr = make_note_expr(V::kTuningTypeID, kNoteId, 0.75, 4);
    REQUIRE(events.addEvent(on) == Steinberg::kResultOk);
    REQUIRE(events.addEvent(expr) == Steinberg::kResultOk);
    s.run(&events);

    // No member-channel note exists, so no per-note PitchBend event is emitted.
    const auto* pb = find_mpe(s.test_processor->last_mpe_events,
                              pulp::midi::MpeExpressionEvent::Kind::PitchBend);
    REQUIRE(pb == nullptr);

    REQUIRE(s.processor.terminate() == Steinberg::kResultOk);
}

TEST_CASE("VST3 expression before its note-on at the same offset is dropped (event order)",
          "[vst3][midi][noteexpression][mpe][process]") {
    namespace V = Steinberg::Vst;
    NoteExpressionSetup s;

    // VST3's IEventList is host-sorted and an expression for a noteId can only
    // occur AFTER its NoteOnEvent (ivstnoteexpression.h). The adapter relies on
    // that ordering: the note-on populates note_id_map_ as events are walked in
    // list order, so an expression that the host (incorrectly) places before the
    // note-on at the same offset finds no mapping yet and is dropped. This pins
    // the documented dependency on VST3 event ordering.
    constexpr Steinberg::int16 kChannel = 1;
    constexpr Steinberg::int32 kNoteId = 8;
    Steinberg::Vst::EventList events(4);
    // Expression FIRST (same offset 0), note-on SECOND — the host contract
    // forbids this, but we assert the adapter degrades safely (drop, no route).
    auto expr = make_note_expr(V::kTuningTypeID, kNoteId, 0.75, 0);
    auto on = make_note_on(kChannel, 60, 0.8f, kNoteId, 0);
    REQUIRE(events.addEvent(expr) == Steinberg::kResultOk);
    REQUIRE(events.addEvent(on) == Steinberg::kResultOk);
    s.run(&events);

    // The expression was processed before the note-on populated the map, so it
    // routed nothing and was counted as a drop.
    REQUIRE(s.processor.note_expression_drop_count() >= 1);
    const auto* pb = find_mpe(s.test_processor->last_mpe_events,
                              pulp::midi::MpeExpressionEvent::Kind::PitchBend);
    REQUIRE(pb == nullptr);

    REQUIRE(s.processor.terminate() == Steinberg::kResultOk);
}

// VST3 puts the obligation to declare output silence on the plugin: a host is
// free to hand us AudioBusBuffers whose silenceFlags carry its inbound
// silence, and to act on whatever we leave there. The adapter never wrote the
// field, so a generator / oscillator / DC-control-voltage source that
// synthesizes output from a silent input returned a full buffer still labelled
// silent. Assert we retract the claim on every rendered output bus.
TEST_CASE("VST3 clears output silenceFlags after an active render",
          "[vst3][audio][silence][dc]") {
    reset_test_processor();

    HostApp host_app;
    pulp::format::vst3::PulpVst3Processor processor(create_test_processor);
    REQUIRE(processor.initialize(&host_app) == Steinberg::kResultOk);

    Steinberg::Vst::ProcessSetup setup{};
    setup.processMode = Steinberg::Vst::kRealtime;
    setup.symbolicSampleSize = Steinberg::Vst::kSample32;
    setup.maxSamplesPerBlock = 8;
    setup.sampleRate = 48000.0;
    REQUIRE(processor.setupProcessing(setup) == Steinberg::kResultOk);
    REQUIRE(processor.setActive(true) == Steinberg::kResultOk);

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
    // The host tells us both input channels are digital silence.
    audio_inputs[0].silenceFlags = 0x3;

    Steinberg::Vst::AudioBusBuffers audio_outputs[1]{};
    audio_outputs[0].numChannels = 2;
    audio_outputs[0].channelBuffers32 = main_outputs;
    // ...and propagates that claim into the output buffers it hands us, which
    // is exactly what a silence-optimising host does.
    audio_outputs[0].silenceFlags = 0x3;

    Steinberg::Vst::ProcessData data{};
    data.symbolicSampleSize = Steinberg::Vst::kSample32;
    data.numSamples = kFrames;
    data.numInputs = 1;
    data.numOutputs = 1;
    data.inputs = audio_inputs;
    data.outputs = audio_outputs;

    REQUIRE(processor.process(data) == Steinberg::kResultOk);
    REQUIRE(data.outputs[0].silenceFlags == 0);

    // Sticky across blocks: a host that keeps asserting silence keeps having it
    // retracted, rather than the flag surviving from the first block onward.
    data.outputs[0].silenceFlags = 0x3;
    REQUIRE(processor.process(data) == Steinberg::kResultOk);
    REQUIRE(data.outputs[0].silenceFlags == 0);

    REQUIRE(processor.setActive(false) == Steinberg::kResultOk);
    REQUIRE(processor.terminate() == Steinberg::kResultOk);
}
