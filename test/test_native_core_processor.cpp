// Seam test for NativeCoreProcessor — proves the C-ABI <-> pulp::format::Processor
// adapter bridges descriptor, parameters, sample-accurate automation, audio
// processing, and opaque state. Built ALWAYS (no Rust): the native core here is a
// tiny in-test C++ gain implementing the same C ABI a Rust core would. The Rust
// round-trip is covered separately by the opt-in lane (test_rust_dsp_ffi.cpp).

#include <pulp/format/native_core_processor.hpp>

#include <pulp/native_components/native_core.h>
#include <pulp/native_components/native_core.hpp>

#include <pulp/audio/buffer.hpp>
#include <pulp/midi/buffer.hpp>
#include <pulp/state/parameter_event_queue.hpp>
#include <pulp/state/store.hpp>

#if PULP_NATIVE_CORE_PROCESS_RT_TRAP_TESTS
#include "native_components/rt_test_scope.hpp"
#endif

#include <catch2/catch_test_macros.hpp>

#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#if PULP_NATIVE_CORE_PROCESS_RT_TRAP_TESTS
#include <pthread.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

using namespace pulp;

namespace {

// ── A minimal C-ABI native core: a stereo gain whose gain is set by the "gain"
// parameter (plain domain) via automation events, and whose only state is that
// gain value (4 bytes). ───────────────────────────────────────────────────────

struct GainInstance {
    float gain = 1.0f;
    bool active = false;
};

const char kId[] = "com.pulp.test.gain";
const char kName[] = "Test Gain";
const char kParamId[] = "gain";

pulp_native_descriptor_v1 g_desc = [] {
    pulp_native_descriptor_v1 d{};
    d.size = sizeof(d);
    d.abi_version = PULP_NATIVE_CORE_ABI_VERSION;
    d.id = kId;
    d.id_len = std::strlen(kId);
    d.name = kName;
    d.name_len = std::strlen(kName);
    d.plugin_version = 2;
    d.capabilities = PULP_NATIVE_CAP_STATE;
    d.default_input_bus_count = 1;
    d.default_output_bus_count = 1;
    return d;
}();

pulp_native_param_v1 g_param = [] {
    pulp_native_param_v1 p{};
    p.size = sizeof(p);
    p.abi_version = PULP_NATIVE_CORE_ABI_VERSION;
    p.id = kParamId;
    p.id_len = std::strlen(kParamId);
    p.id_hash = native_components::param_id_hash("gain");
    p.min_value = 0.0;
    p.max_value = 2.0;
    p.default_value = 1.0;
    p.flags = PULP_NATIVE_PARAM_AUTOMATABLE;
    return p;
}();

uint32_t g_last_param_view_count = 0;
uint32_t g_last_param_view_capacity = 0;
uint32_t g_last_param_view_overflowed = 0;
// Captures the (unsigned) sample_offset / ramp_duration the adapter handed the
// native core for the FIRST and LAST event in the block — used to prove
// out-of-range int32 offsets (negative or beyond the block) are clamped into
// range before the cast rather than wrapping or pointing past the block.
uint32_t g_first_event_sample_offset = 0;
uint32_t g_first_event_ramp_duration = 0;
uint32_t g_last_event_sample_offset = 0;
uint32_t g_last_event_ramp_duration = 0;

const pulp_native_descriptor_v1* gain_descriptor() { return &g_desc; }
const pulp_native_param_v1* gain_parameters(uint32_t* count) {
    if (count) *count = 1;
    return &g_param;
}
pulp_native_status gain_create(const pulp_native_host_services_v1*,
                               pulp_native_instance** out) {
    *out = reinterpret_cast<pulp_native_instance*>(new GainInstance());
    return PULP_NATIVE_OK;
}
void gain_destroy(pulp_native_instance* i) {
    delete reinterpret_cast<GainInstance*>(i);
}
pulp_native_status gain_prepare(pulp_native_instance*, const pulp_native_prepare_v1*) {
    return PULP_NATIVE_OK;
}
void gain_release(pulp_native_instance*) {}
pulp_native_status gain_set_bus_layout(pulp_native_instance*,
                                       const pulp_native_bus_layout_v1*) {
    return PULP_NATIVE_OK;
}
pulp_native_status gain_resume(pulp_native_instance* i) {
    reinterpret_cast<GainInstance*>(i)->active = true;
    return PULP_NATIVE_OK;
}
pulp_native_status gain_suspend(pulp_native_instance* i) {
    reinterpret_cast<GainInstance*>(i)->active = false;
    return PULP_NATIVE_OK;
}
void gain_reset(pulp_native_instance*) {}

pulp_native_status gain_process(pulp_native_instance* inst,
                                const pulp_native_process_v1* io) {
    auto* self = reinterpret_cast<GainInstance*>(inst);
    // Apply the last automation event for our parameter (offset-aware handling
    // is a DSP concern; this fixture just takes the final value for the block).
    if (io->params != nullptr && io->params->events != nullptr) {
        g_last_param_view_count = io->params->count;
        g_last_param_view_capacity = io->params->capacity;
        g_last_param_view_overflowed = io->params->overflowed;
        if (io->params->count > 0) {
            const auto& first = io->params->events[0];
            const auto& last = io->params->events[io->params->count - 1];
            g_first_event_sample_offset = first.sample_offset;
            g_first_event_ramp_duration = first.ramp_frames;
            g_last_event_sample_offset = last.sample_offset;
            g_last_event_ramp_duration = last.ramp_frames;
        }
        for (uint32_t e = 0; e < io->params->count; ++e) {
            const auto& ev = io->params->events[e];
            if (ev.param_id_hash == g_param.id_hash) {
                self->gain = static_cast<float>(ev.value);
            }
        }
    } else {
        g_last_param_view_count = 0;
        g_last_param_view_capacity = 0;
        g_last_param_view_overflowed = 0;
    }
    const auto* audio = io->audio;
    for (uint32_t b = 0; b < audio->output_bus_count; ++b) {
        const auto& obus = audio->outputs[b];
        const auto& ibus = audio->inputs[b];
        for (uint32_t c = 0; c < obus.channel_count; ++c) {
            float* out = obus.channels[c];
            const float* in = ibus.channels[c];
            for (uint32_t s = 0; s < audio->frame_count; ++s) {
                out[s] = in[s] * self->gain;
            }
        }
    }
    return PULP_NATIVE_OK;
}

pulp_native_status gain_save_state(pulp_native_instance* i,
                                   pulp_native_state_out_v1* out) {
    auto* self = reinterpret_cast<GainInstance*>(i);
    auto* buf = static_cast<uint8_t*>(std::malloc(sizeof(float)));
    std::memcpy(buf, &self->gain, sizeof(float));
    out->bytes = buf;
    out->byte_len = sizeof(float);
    out->state_version = 1;
    return PULP_NATIVE_OK;
}
void gain_free_state(pulp_native_instance*, pulp_native_state_out_v1* out) {
    std::free(out->bytes);
    out->bytes = nullptr;
}
pulp_native_status gain_load_state(pulp_native_instance* i,
                                   const pulp_native_state_span_v1* span) {
    auto* self = reinterpret_cast<GainInstance*>(i);
    if (span->bytes == nullptr || span->byte_len == 0) {
        self->gain = 1.0f;  // empty == defaults
        return PULP_NATIVE_OK;
    }
    if (span->byte_len != sizeof(float)) return PULP_NATIVE_ERR_MALFORMED_STATE;
    std::memcpy(&self->gain, span->bytes, sizeof(float));
    return PULP_NATIVE_OK;
}
uint32_t gain_report_latency(pulp_native_instance*) { return 0; }
uint32_t gain_report_tail(pulp_native_instance*) { return 0; }
const char kPong[] = R"({"pong":true})";
pulp_native_status gain_editor_command(pulp_native_instance*, const uint8_t*,
                                       size_t req_len, uint8_t** r, size_t* n) {
    if (req_len == 0) {  // empty request is unsupported
        if (r) *r = nullptr;
        if (n) *n = 0;
        return PULP_NATIVE_ERR_UNSUPPORTED;
    }
    const size_t len = std::strlen(kPong);
    auto* buf = static_cast<uint8_t*>(std::malloc(len));
    std::memcpy(buf, kPong, len);
    if (r) *r = buf;
    if (n) *n = len;
    return PULP_NATIVE_OK;
}
void gain_free_editor_reply(pulp_native_instance*, uint8_t* r, size_t) {
    std::free(r);
}

pulp_native_core_v1 g_core = [] {
    pulp_native_core_v1 c{};
    c.size = sizeof(c);
    c.abi_version = PULP_NATIVE_CORE_ABI_VERSION;
    c.descriptor = gain_descriptor;
    c.parameters = gain_parameters;
    c.create = gain_create;
    c.destroy = gain_destroy;
    c.prepare = gain_prepare;
    c.release = gain_release;
    c.set_bus_layout = gain_set_bus_layout;
    c.resume = gain_resume;
    c.suspend = gain_suspend;
    c.reset = gain_reset;
    c.process = gain_process;
    c.save_state = gain_save_state;
    c.free_state = gain_free_state;
    c.load_state = gain_load_state;
    c.report_latency = gain_report_latency;
    c.report_tail = gain_report_tail;
    c.editor_command = gain_editor_command;
    c.free_editor_reply = gain_free_editor_reply;
    return c;
}();

// Helper: run one stereo block of `frames` constant-1.0 input through the
// adapter and return the first output sample of channel 0.
struct Block {
    std::vector<float> l, r;
    std::vector<float*> ptrs;
    Block(std::size_t frames, float fill) : l(frames, fill), r(frames, fill) {
        ptrs = {l.data(), r.data()};
    }
};

void prepare_gain_processor(format::NativeCoreProcessor& proc,
                            state::StateStore& store,
                            std::size_t frames) {
    proc.define_parameters(store);
    format::PrepareContext ctx;
    ctx.sample_rate = 48000.0;
    ctx.max_buffer_size = static_cast<int>(frames);
    ctx.input_channels = 2;
    ctx.output_channels = 2;
    proc.prepare(ctx);
}

#if PULP_NATIVE_CORE_PROCESS_RT_TRAP_TESTS
void require_child_traps_in_rt_scope(void (*body)()) {
    std::fflush(nullptr);
    const pid_t pid = ::fork();
    REQUIRE(pid >= 0);
    if (pid == 0) {
        body();
        _exit(0);
    }
    int status = 0;
    REQUIRE(::waitpid(pid, &status, 0) == pid);
    INFO("child status=" << status);
    REQUIRE(WIFSIGNALED(status));
    REQUIRE((WTERMSIG(status) == SIGABRT || WTERMSIG(status) == SIGTRAP));
}
#endif

}  // namespace

TEST_CASE("NativeCoreProcessor mirrors the native descriptor",
          "[native-core-processor]") {
    format::NativeCoreProcessor proc(&g_core);
    REQUIRE(proc.valid());
    const auto d = proc.descriptor();
    REQUIRE(d.name == "Test Gain");
    REQUIRE(d.category == format::PluginCategory::Effect);
    REQUIRE(d.input_buses.size() == 1);
    REQUIRE(d.output_buses.size() == 1);
}

TEST_CASE("NativeCoreProcessor registers native parameters in the StateStore",
          "[native-core-processor]") {
    format::NativeCoreProcessor proc(&g_core);
    state::StateStore store;
    proc.define_parameters(store);
    REQUIRE(store.all_params().size() == 1);
    const auto& info = store.all_params()[0];
    REQUIRE(info.name == "gain");
    REQUIRE(info.range.min == 0.0f);
    REQUIRE(info.range.max == 2.0f);
    REQUIRE(info.range.default_value == 1.0f);
}

TEST_CASE("NativeCoreProcessor bridges automation and processes audio",
          "[native-core-processor]") {
    format::NativeCoreProcessor proc(&g_core);
    state::StateStore store;

    constexpr std::size_t frames = 64;
    prepare_gain_processor(proc, store, frames);
    Block in(frames, 1.0f);
    Block out(frames, 0.0f);
    audio::BufferView<float> out_view(out.ptrs.data(), 2, frames);
    audio::BufferView<const float> in_view(
        const_cast<const float* const*>(in.ptrs.data()), 2, frames);

    // Automation: set gain to 0.5 at the start of the block.
    state::ParameterEventQueue queue;
    queue.push({/*param_id=*/0, /*offset=*/0, /*value=*/0.5f, /*ramp=*/0});
    proc.set_param_events(&queue);

    midi::MidiBuffer min, mout;
    format::ProcessContext pctx;
    pctx.sample_rate = 48000.0;
    pctx.num_samples = frames;
    proc.process(out_view, in_view, min, mout, pctx);

    // Output should be input (1.0) scaled by the automated gain (0.5).
    REQUIRE(out.l[0] == 0.5f);
    REQUIRE(out.r[frames - 1] == 0.5f);
}

TEST_CASE("NativeCoreProcessor forwards parameter-event overflow to native cores",
          "[native-core-processor][params][overflow]") {
    format::NativeCoreProcessor proc(&g_core);
    state::StateStore store;

    constexpr std::size_t frames = 16;
    prepare_gain_processor(proc, store, frames);
    Block in(frames, 1.0f);
    Block out(frames, 0.0f);
    audio::BufferView<float> out_view(out.ptrs.data(), 2, frames);
    audio::BufferView<const float> in_view(
        const_cast<const float* const*>(in.ptrs.data()), 2, frames);

    state::ParameterEventQueue queue;
    for (std::size_t i = 0; i < queue.capacity(); ++i) {
        REQUIRE(queue.push({/*param_id=*/0,
                            /*offset=*/0,
                            /*value=*/0.25f,
                            /*ramp=*/0}));
    }
    REQUIRE_FALSE(queue.push({/*param_id=*/0,
                              /*offset=*/0,
                              /*value=*/0.75f,
                              /*ramp=*/0}));
    REQUIRE(queue.overflowed());
    proc.set_param_events(&queue);

    midi::MidiBuffer min, mout;
    format::ProcessContext pctx;
    pctx.sample_rate = 48000.0;
    pctx.num_samples = frames;
    proc.process(out_view, in_view, min, mout, pctx);

    REQUIRE(g_last_param_view_count == queue.capacity());
    REQUIRE(g_last_param_view_capacity == queue.capacity());
    REQUIRE(g_last_param_view_overflowed == 1);
}

TEST_CASE("NativeCoreProcessor clamps out-of-range event offsets before the cast",
          "[native-core-processor][params]") {
    // ParameterEvent::sample_offset / ramp_duration_sample_frames are int32_t.
    // The native ABI requires sample_offset in [0, frame_count) and a ramp in
    // [0, frame_count - offset]. The adapter must clamp both into range before
    // the unsigned cast: a NEGATIVE value would wrap to ~4.29e9, and an
    // out-of-range POSITIVE value would point past the block — both are
    // out-of-block offsets (OOB read/write risk inside the core).
    format::NativeCoreProcessor proc(&g_core);
    state::StateStore store;

    constexpr std::size_t frames = 16;  // valid offsets are [0, 15]
    prepare_gain_processor(proc, store, frames);
    Block in(frames, 1.0f);
    Block out(frames, 0.0f);
    audio::BufferView<float> out_view(out.ptrs.data(), 2, frames);
    audio::BufferView<const float> in_view(
        const_cast<const float* const*>(in.ptrs.data()), 2, frames);

    g_first_event_sample_offset = 0xDEADBEEFu;  // sentinels; must be overwritten
    g_first_event_ramp_duration = 0xDEADBEEFu;
    g_last_event_sample_offset = 0xDEADBEEFu;
    g_last_event_ramp_duration = 0xDEADBEEFu;

    state::ParameterEventQueue queue;
    // Event 0: negative offset + negative ramp -> clamp both to 0.
    REQUIRE(queue.push({/*param_id=*/0,
                        /*offset=*/-5,
                        /*value=*/0.5f,
                        /*ramp=*/-3}));
    // Event 1: huge positive offset + huge ramp -> clamp offset to
    // frame_count-1 (15), then ramp to frame_count-offset (16-15 = 1).
    REQUIRE(queue.push({/*param_id=*/0,
                        /*offset=*/1000,
                        /*value=*/0.5f,
                        /*ramp=*/1000}));
    proc.set_param_events(&queue);

    midi::MidiBuffer min, mout;
    format::ProcessContext pctx;
    pctx.sample_rate = 48000.0;
    pctx.num_samples = frames;
    proc.process(out_view, in_view, min, mout, pctx);

    REQUIRE(g_last_param_view_count == 2);

    // Negative -> 0 (NOT the 0xfffffffb wrap a bare cast of -5 would produce).
    REQUIRE(g_first_event_sample_offset == 0u);
    REQUIRE(g_first_event_ramp_duration == 0u);

    // Out-of-range positive -> clamped to the in-block maximums.
    REQUIRE(g_last_event_sample_offset == 15u);  // frame_count - 1
    REQUIRE(g_last_event_ramp_duration == 1u);    // frame_count - offset
}

#if PULP_NATIVE_CORE_PROCESS_RT_TRAP_TESTS
TEST_CASE("NativeCoreProcessor process path is no-alloc and no-lock after prepare",
          "[native-core-processor][rt-safety]") {
    format::NativeCoreProcessor proc(&g_core);
    state::StateStore store;

    constexpr std::size_t frames = 64;
    prepare_gain_processor(proc, store, frames);
    Block in(frames, 1.0f);
    Block out(frames, 0.0f);
    audio::BufferView<float> out_view(out.ptrs.data(), 2, frames);
    audio::BufferView<const float> in_view(
        const_cast<const float* const*>(in.ptrs.data()), 2, frames);
    state::ParameterEventQueue queue;
    queue.push({/*param_id=*/0, /*offset=*/0, /*value=*/0.25f, /*ramp=*/0});
    proc.set_param_events(&queue);
    midi::MidiBuffer min, mout;
    format::ProcessContext pctx;
    pctx.sample_rate = 48000.0;
    pctx.num_samples = frames;

    {
        native_components::test::RtNoAllocScope guard;
        proc.process(out_view, in_view, min, mout, pctx);
    }

    REQUIRE(out.l[0] == 0.25f);
    REQUIRE(out.r[frames - 1] == 0.25f);
}

TEST_CASE("NativeCoreProcessor RT harness traps C++ allocation",
          "[native-core-processor][rt-safety]") {
    require_child_traps_in_rt_scope([] {
        native_components::test::RtNoAllocScope guard;
        void* leak = ::operator new(sizeof(int));
        (void)leak;
    });
}

TEST_CASE("NativeCoreProcessor RT harness traps blocking locks",
          "[native-core-processor][rt-safety]") {
    require_child_traps_in_rt_scope([] {
        pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
        native_components::test::RtNoAllocScope guard;
        (void)::pthread_mutex_lock(&mutex);
        (void)::pthread_mutex_unlock(&mutex);
        (void)::pthread_mutex_destroy(&mutex);
    });
}
#endif

TEST_CASE("NativeCoreProcessor drives suspend/resume/release/latency",
          "[native-core-processor]") {
    format::NativeCoreProcessor proc(&g_core);
    state::StateStore store;
    proc.define_parameters(store);
    format::PrepareContext ctx;
    ctx.sample_rate = 44100.0;
    ctx.max_buffer_size = 128;
    ctx.input_channels = 2;
    ctx.output_channels = 2;
    proc.prepare(ctx);
    proc.suspend();
    proc.resume();
    REQUIRE(proc.latency_samples() == 0);
    proc.release();
    SUCCEED("lifecycle transitions completed");
}

TEST_CASE("NativeCoreProcessor round-trips opaque state",
          "[native-core-processor]") {
    format::NativeCoreProcessor proc(&g_core);

    // Load a known gain (1.75) as 4 raw bytes, then save and confirm it survives.
    float g = 1.75f;
    std::vector<uint8_t> blob(sizeof(float));
    std::memcpy(blob.data(), &g, sizeof(float));
    REQUIRE(proc.deserialize_plugin_state(blob));

    const auto saved = proc.serialize_plugin_state();
    REQUIRE(saved.size() == sizeof(float));
    float restored = 0.0f;
    std::memcpy(&restored, saved.data(), sizeof(float));
    REQUIRE(restored == 1.75f);

    // Malformed state (wrong size) is rejected without unwinding.
    std::vector<uint8_t> bad(3, 0);
    REQUIRE_FALSE(proc.deserialize_plugin_state(bad));
}

TEST_CASE("NativeCoreProcessor editor_command round-trips JSON off the RT thread",
          "[native-core-processor]") {
    format::NativeCoreProcessor proc(&g_core);
    const auto reply = proc.editor_command(R"({"cmd":"ping"})");
    REQUIRE(reply.has_value());
    REQUIRE(*reply == R"({"pong":true})");

    // An empty / unsupported request returns nullopt without unwinding.
    REQUIRE_FALSE(proc.editor_command("").has_value());

    // Inert adapter: no instance, no command support.
    pulp_native_core_v1 bad = g_core;
    bad.abi_version = PULP_NATIVE_CORE_ABI_VERSION + 1;
    format::NativeCoreProcessor inert(&bad);
    REQUIRE_FALSE(inert.editor_command(R"({"cmd":"ping"})").has_value());
}

TEST_CASE("NativeCoreProcessor is inert (silent) for an incompatible core",
          "[native-core-processor]") {
    pulp_native_core_v1 bad = g_core;
    bad.abi_version = PULP_NATIVE_CORE_ABI_VERSION + 1;  // wrong major
    format::NativeCoreProcessor proc(&bad);
    REQUIRE_FALSE(proc.valid());

    format::PrepareContext ctx;
    ctx.input_channels = 2;
    ctx.output_channels = 2;
    proc.prepare(ctx);

    constexpr std::size_t frames = 16;
    Block in(frames, 1.0f);
    Block out(frames, 7.0f);  // sentinel
    audio::BufferView<float> out_view(out.ptrs.data(), 2, frames);
    audio::BufferView<const float> in_view(
        const_cast<const float* const*>(in.ptrs.data()), 2, frames);
    midi::MidiBuffer min, mout;
    format::ProcessContext pctx;
    proc.process(out_view, in_view, min, mout, pctx);
    // Inert adapter outputs silence.
    REQUIRE(out.l[0] == 0.0f);
}
