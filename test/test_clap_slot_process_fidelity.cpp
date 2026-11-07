// What a hosted CLAP plugin sees from Pulp's CLAP slot
// (core/host/src/plugin_slot_clap.cpp): host identity and extensions, host
// transport, offline rendering, every declared audio port, note/MIDI/sysex in
// both directions, parameter feedback, and restart/latency requests reaching
// SignalGraph's latency compensation.
//
// Driven against a fake clap_plugin_t through make_clap_slot, so the contract
// is pinned as C-ABI behaviour with no dlopen and no bundle on disk.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "plugin_slot_clap_internal.hpp"
#include <pulp/host/plugin_slot.hpp>
#include <pulp/host/signal_graph.hpp>

#include <clap/clap.h>

#include <array>
#include <cstring>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

using pulp::host::PluginFormat;
using pulp::host::PluginHostSettings;
using pulp::host::PluginInfo;
using pulp::host::PluginParameterEdit;
using pulp::host::PluginSlot;
namespace fmt = pulp::format;

namespace {

struct Port {
    uint32_t channels = 2;
    bool main = true;
    bool supports_64 = false;
};

// Everything the fake plugin observes, and everything it can be told to emit.
struct FakeClap {
    // Declared shape.
    std::vector<Port> in_ports{{2, true}};
    std::vector<Port> out_ports{{2, true}};
    bool declare_ports = true;
    bool declare_note_port = false;
    uint32_t note_dialects = CLAP_NOTE_DIALECT_MIDI;
    clap_note_dialect preferred_dialect = CLAP_NOTE_DIALECT_MIDI;
    bool declare_render = true;
    bool hard_realtime = false;
    uint32_t latency = 0;

    // Observed.
    const clap_host_t* host = nullptr;
    clap_plugin_render_mode render_mode = -1;
    int activate_calls = 0;
    bool saw_transport = false;
    clap_event_transport_t transport{};
    uint32_t audio_inputs_count = 0;
    uint32_t audio_outputs_count = 0;
    std::vector<uint32_t> input_channel_counts;
    float sidechain_sample = 0.0f;
    bool saw_data64 = false;
    std::vector<uint16_t> event_types;
    std::vector<uint8_t> last_midi;
    std::vector<uint8_t> last_sysex;
    double last_note_velocity = -1.0;
    double last_expression = -1.0;
    bool audio_thread_during_process = false;
    bool main_thread_during_process = true;
    int on_main_thread_calls = 0;

    // Emitted on the next process() call.
    bool emit_note = false;
    bool emit_sysex = false;
    bool emit_param = false;
    float aux_output_value = 0.0f;
    std::array<uint8_t, 4> sysex_out{0xF0, 0x42, 0x10, 0xF7};

    double param_value = 0.0;

    clap_plugin_t plugin{};
    clap_plugin_audio_ports_t audio_ports{};
    clap_plugin_note_ports_t note_ports{};
    clap_plugin_render_t render{};
    clap_plugin_latency_t latency_ext{};
    clap_plugin_params_t params{};
};

FakeClap* g = nullptr;

const void* host_ext(const char* id) {
    return g->host->get_extension(g->host, id);
}

uint32_t CLAP_ABI ports_count(const clap_plugin_t*, bool is_input) {
    return (uint32_t)(is_input ? g->in_ports.size() : g->out_ports.size());
}
bool CLAP_ABI ports_get(const clap_plugin_t*, uint32_t index, bool is_input,
                        clap_audio_port_info_t* info) {
    const auto& ports = is_input ? g->in_ports : g->out_ports;
    if (index >= ports.size())
        return false;
    *info = clap_audio_port_info_t{};
    info->id = index;
    info->channel_count = ports[index].channels;
    info->flags = (ports[index].main ? CLAP_AUDIO_PORT_IS_MAIN : 0u) |
                  (ports[index].supports_64 ? CLAP_AUDIO_PORT_SUPPORTS_64BITS : 0u);
    info->in_place_pair = CLAP_INVALID_ID;
    return true;
}

uint32_t CLAP_ABI note_count(const clap_plugin_t*, bool is_input) {
    return is_input ? 1u : 0u;
}
bool CLAP_ABI note_get(const clap_plugin_t*, uint32_t index, bool is_input,
                       clap_note_port_info_t* info) {
    if (!is_input || index != 0)
        return false;
    *info = clap_note_port_info_t{};
    info->supported_dialects = g->note_dialects;
    info->preferred_dialect = g->preferred_dialect;
    return true;
}

bool CLAP_ABI render_hard_rt(const clap_plugin_t*) {
    return g->hard_realtime;
}
bool CLAP_ABI render_set(const clap_plugin_t*, clap_plugin_render_mode mode) {
    g->render_mode = mode;
    return true;
}

uint32_t CLAP_ABI latency_get(const clap_plugin_t*) {
    return g->latency;
}

uint32_t CLAP_ABI params_count(const clap_plugin_t*) {
    return 1;
}
bool CLAP_ABI params_info(const clap_plugin_t*, uint32_t index, clap_param_info_t* info) {
    if (index != 0)
        return false;
    *info = clap_param_info_t{};
    info->id = 7;
    std::strncpy(info->name, "Gain", sizeof(info->name) - 1);
    info->min_value = 0.0;
    info->max_value = 1.0;
    info->default_value = 0.5;
    info->flags = CLAP_PARAM_IS_AUTOMATABLE;
    return true;
}
bool CLAP_ABI params_value(const clap_plugin_t*, clap_id, double* value) {
    *value = g->param_value;
    return true;
}
void CLAP_ABI params_flush(const clap_plugin_t*, const clap_input_events_t*,
                           const clap_output_events_t* out) {
    clap_event_param_value_t ev{};
    ev.header = {sizeof(ev), 0, CLAP_CORE_EVENT_SPACE_ID, CLAP_EVENT_PARAM_VALUE, 0};
    ev.param_id = 7;
    ev.note_id = -1;
    ev.port_index = -1;
    ev.channel = -1;
    ev.key = -1;
    ev.value = 0.125;
    out->try_push(out, &ev.header);
}

bool CLAP_ABI p_init(const clap_plugin_t*) {
    return true;
}
void CLAP_ABI p_destroy(const clap_plugin_t*) {}
bool CLAP_ABI p_activate(const clap_plugin_t*, double, uint32_t, uint32_t) {
    ++g->activate_calls;
    return true;
}
void CLAP_ABI p_deactivate(const clap_plugin_t*) {}
bool CLAP_ABI p_start(const clap_plugin_t*) {
    return true;
}
void CLAP_ABI p_stop(const clap_plugin_t*) {}
void CLAP_ABI p_reset(const clap_plugin_t*) {}
void CLAP_ABI p_on_main_thread(const clap_plugin_t*) {
    ++g->on_main_thread_calls;
}

clap_process_status CLAP_ABI p_process(const clap_plugin_t*, const clap_process_t* p) {
    auto* check = static_cast<const clap_host_thread_check_t*>(host_ext(CLAP_EXT_THREAD_CHECK));
    g->audio_thread_during_process = check && check->is_audio_thread(g->host);
    g->main_thread_during_process = check && check->is_main_thread(g->host);

    g->saw_transport = p->transport != nullptr;
    if (p->transport)
        g->transport = *p->transport;
    g->audio_inputs_count = p->audio_inputs_count;
    g->audio_outputs_count = p->audio_outputs_count;
    g->input_channel_counts.clear();
    for (uint32_t i = 0; i < p->audio_inputs_count; ++i)
        g->input_channel_counts.push_back(p->audio_inputs[i].channel_count);
    g->saw_data64 = p->audio_inputs_count > 0 && p->audio_inputs[0].data64 != nullptr;
    if (p->audio_inputs_count > 1 && p->audio_inputs[1].data32)
        g->sidechain_sample = p->audio_inputs[1].data32[0][0];

    g->event_types.clear();
    const uint32_t n = p->in_events->size(p->in_events);
    for (uint32_t i = 0; i < n; ++i) {
        const auto* h = p->in_events->get(p->in_events, i);
        g->event_types.push_back(h->type);
        if (h->type == CLAP_EVENT_MIDI) {
            const auto* m = reinterpret_cast<const clap_event_midi_t*>(h);
            g->last_midi.assign(m->data, m->data + 3);
        } else if (h->type == CLAP_EVENT_MIDI_SYSEX) {
            const auto* s = reinterpret_cast<const clap_event_midi_sysex_t*>(h);
            g->last_sysex.assign(s->buffer, s->buffer + s->size);
        } else if (h->type == CLAP_EVENT_NOTE_ON) {
            g->last_note_velocity = reinterpret_cast<const clap_event_note_t*>(h)->velocity;
        } else if (h->type == CLAP_EVENT_NOTE_EXPRESSION) {
            g->last_expression = reinterpret_cast<const clap_event_note_expression_t*>(h)->value;
        }
    }

    for (uint32_t o = 1; o < p->audio_outputs_count; ++o)
        for (uint32_t c = 0; c < p->audio_outputs[o].channel_count; ++c)
            for (uint32_t i = 0; i < p->frames_count; ++i)
                p->audio_outputs[o].data32[c][i] = g->aux_output_value;

    if (g->emit_note) {
        clap_event_note_t ev{};
        ev.header = {sizeof(ev), 3, CLAP_CORE_EVENT_SPACE_ID, CLAP_EVENT_NOTE_ON, 0};
        ev.note_id = -1;
        ev.port_index = 0;
        ev.channel = 2;
        ev.key = 64;
        ev.velocity = 1.0;
        p->out_events->try_push(p->out_events, &ev.header);
    }
    if (g->emit_sysex) {
        clap_event_midi_sysex_t ev{};
        ev.header = {sizeof(ev), 1, CLAP_CORE_EVENT_SPACE_ID, CLAP_EVENT_MIDI_SYSEX, 0};
        ev.buffer = g->sysex_out.data();
        ev.size = (uint32_t)g->sysex_out.size();
        p->out_events->try_push(p->out_events, &ev.header);
    }
    if (g->emit_param) {
        clap_event_param_gesture_t begin{};
        begin.header = {sizeof(begin), 0, CLAP_CORE_EVENT_SPACE_ID, CLAP_EVENT_PARAM_GESTURE_BEGIN,
                        0};
        begin.param_id = 7;
        p->out_events->try_push(p->out_events, &begin.header);
        clap_event_param_value_t value{};
        value.header = {sizeof(value), 0, CLAP_CORE_EVENT_SPACE_ID, CLAP_EVENT_PARAM_VALUE, 0};
        value.param_id = 7;
        value.note_id = -1;
        value.port_index = -1;
        value.channel = -1;
        value.key = -1;
        value.value = 0.875;
        p->out_events->try_push(p->out_events, &value.header);
        clap_event_param_gesture_t end{};
        end.header = {sizeof(end), 0, CLAP_CORE_EVENT_SPACE_ID, CLAP_EVENT_PARAM_GESTURE_END, 0};
        end.param_id = 7;
        p->out_events->try_push(p->out_events, &end.header);
    }
    return CLAP_PROCESS_CONTINUE;
}

const void* CLAP_ABI p_get_extension(const clap_plugin_t*, const char* id) {
    const std::string_view ext(id);
    if (ext == CLAP_EXT_AUDIO_PORTS && g->declare_ports)
        return &g->audio_ports;
    if (ext == CLAP_EXT_NOTE_PORTS && g->declare_note_port)
        return &g->note_ports;
    if (ext == CLAP_EXT_RENDER && g->declare_render)
        return &g->render;
    if (ext == CLAP_EXT_LATENCY)
        return &g->latency_ext;
    if (ext == CLAP_EXT_PARAMS)
        return &g->params;
    return nullptr;
}

std::unique_ptr<PluginSlot> make_slot(FakeClap& fake) {
    g = &fake;
    fake.audio_ports = {ports_count, ports_get};
    fake.note_ports = {note_count, note_get};
    fake.render = {render_hard_rt, render_set};
    fake.latency_ext = {latency_get};
    fake.params = {params_count, params_info, params_value, nullptr, nullptr, params_flush};
    fake.plugin.init = p_init;
    fake.plugin.destroy = p_destroy;
    fake.plugin.activate = p_activate;
    fake.plugin.deactivate = p_deactivate;
    fake.plugin.start_processing = p_start;
    fake.plugin.stop_processing = p_stop;
    fake.plugin.reset = p_reset;
    fake.plugin.process = p_process;
    fake.plugin.get_extension = p_get_extension;
    fake.plugin.on_main_thread = p_on_main_thread;
    PluginInfo info;
    info.name = "Fake Fidelity";
    info.format = PluginFormat::CLAP;
    return pulp::host::make_clap_slot(info, [&fake](const clap_host_t* host) {
        fake.host = host;
        return &fake.plugin;
    });
}

// One stereo block through the legacy main-in/main-out overload.
struct Block {
    static constexpr int kFrames = 32;
    std::vector<float> l = std::vector<float>(kFrames, 0.0f);
    std::vector<float> r = std::vector<float>(kFrames, 0.0f);
    std::vector<float> ol = std::vector<float>(kFrames, 0.0f);
    std::vector<float> or_ = std::vector<float>(kFrames, 0.0f);
    std::array<const float*, 2> in{l.data(), r.data()};
    std::array<float*, 2> out{ol.data(), or_.data()};
    pulp::midi::MidiBuffer midi_in;
    pulp::midi::MidiBuffer midi_out;
    pulp::host::ParameterEventQueue params;

    void run(PluginSlot& slot) {
        pulp::audio::BufferView<const float> input(in.data(), 2, kFrames);
        pulp::audio::BufferView<float> output(out.data(), 2, kFrames);
        slot.process(output, input, midi_in, midi_out, params, kFrames);
    }
};

} // namespace

TEST_CASE("CLAP host reports the configured identity and its host extensions",
          "[host][clap][host-context]") {
    pulp::host::PluginHostIdentity identity;
    identity.name = "Fidelity Host";
    identity.vendor = "Test Vendor";
    identity.version = "9.8.7";
    const auto previous = pulp::host::plugin_host_identity();
    pulp::host::set_plugin_host_identity(identity);

    FakeClap fake;
    auto slot = make_slot(fake);
    pulp::host::set_plugin_host_identity(previous);
    REQUIRE(slot);
    CHECK(std::string(fake.host->name) == "Fidelity Host");
    CHECK(std::string(fake.host->vendor) == "Test Vendor");
    CHECK(std::string(fake.host->version) == "9.8.7");
    for (const char* ext : {CLAP_EXT_GUI, CLAP_EXT_PARAMS, CLAP_EXT_LATENCY, CLAP_EXT_STATE,
                            CLAP_EXT_THREAD_CHECK, CLAP_EXT_LOG, CLAP_EXT_AUDIO_PORTS}) {
        INFO(ext);
        CHECK(fake.host->get_extension(fake.host, ext) != nullptr);
    }
}

TEST_CASE("CLAP thread check distinguishes the main thread from the audio thread",
          "[host][clap][host-context]") {
    FakeClap fake;
    auto slot = make_slot(fake);
    REQUIRE(slot->prepare(48000.0, 64));
    auto* check = static_cast<const clap_host_thread_check_t*>(
        fake.host->get_extension(fake.host, CLAP_EXT_THREAD_CHECK));
    REQUIRE(check);
    CHECK(check->is_main_thread(fake.host));
    CHECK_FALSE(check->is_audio_thread(fake.host));
    Block b;
    b.run(*slot);
    CHECK(fake.audio_thread_during_process);
    CHECK_FALSE(fake.main_thread_during_process);
}

TEST_CASE("CLAP slot selects the offline render mode when the host renders offline",
          "[host][clap][offline]") {
    FakeClap fake;
    auto slot = make_slot(fake);
    PluginHostSettings settings;
    settings.process_mode = fmt::ProcessMode::Offline;
    REQUIRE(slot->set_host_settings(settings));
    REQUIRE(slot->prepare(48000.0, 64));
    CHECK(fake.render_mode == CLAP_RENDER_OFFLINE);

    settings.process_mode = fmt::ProcessMode::Realtime;
    slot->set_host_settings(settings);
    REQUIRE(slot->prepare(48000.0, 64));
    CHECK(fake.render_mode == CLAP_RENDER_REALTIME);
}

TEST_CASE("CLAP plugin with a hard realtime requirement stays realtime offline",
          "[host][clap][offline]") {
    FakeClap fake;
    fake.hard_realtime = true;
    auto slot = make_slot(fake);
    PluginHostSettings settings;
    settings.process_mode = fmt::ProcessMode::Offline;
    slot->set_host_settings(settings);
    REQUIRE(slot->prepare(48000.0, 64));
    CHECK(fake.render_mode == CLAP_RENDER_REALTIME);
}

TEST_CASE("CLAP plugin is free-running without a host transport", "[host][clap][transport]") {
    FakeClap fake;
    auto slot = make_slot(fake);
    REQUIRE(slot->wants_transport());
    REQUIRE(slot->prepare(48000.0, 64));
    Block b;
    b.run(*slot);
    CHECK_FALSE(fake.saw_transport);
}

TEST_CASE("CLAP plugin receives the host transport", "[host][clap][transport]") {
    FakeClap fake;
    auto slot = make_slot(fake);
    REQUIRE(slot->prepare(48000.0, 64));

    fmt::ProcessContext ctx;
    ctx.sample_rate = 48000.0;
    ctx.is_playing = true;
    ctx.is_recording = true;
    ctx.is_looping = true;
    ctx.tempo_bpm = 140.0;
    ctx.position_beats = 9.5;
    ctx.position_samples = 96000;
    ctx.time_sig_numerator = 3;
    ctx.time_sig_denominator = 4;
    ctx.loop_start_beats = 8.0;
    ctx.loop_end_beats = 16.0;
    using F = fmt::TransportField;
    for (auto f : {F::Playing, F::Recording, F::Looping, F::Tempo, F::BeatPosition,
                   F::SamplePosition, F::TimeSignature, F::LoopRange})
        ctx.transport_validity.set(f);

    Block b;
    std::array<fmt::ProcessBusBufferView<const float>, 1> ins{};
    ins[0].buffer = pulp::audio::BufferView<const float>(b.in.data(), 2, Block::kFrames);
    std::array<fmt::ProcessBusBufferView<float>, 1> outs{};
    outs[0].buffer = pulp::audio::BufferView<float>(b.out.data(), 2, Block::kFrames);
    fmt::ProcessBuffers buffers{fmt::ProcessBusBufferSet<const float>{std::span(ins)},
                                fmt::ProcessBusBufferSet<float>{std::span(outs)}};
    slot->process(buffers, b.midi_in, b.midi_out, b.params, Block::kFrames, ctx);

    REQUIRE(fake.saw_transport);
    const auto& t = fake.transport;
    CHECK((t.flags & CLAP_TRANSPORT_IS_PLAYING) != 0);
    CHECK((t.flags & CLAP_TRANSPORT_IS_RECORDING) != 0);
    CHECK((t.flags & CLAP_TRANSPORT_IS_LOOP_ACTIVE) != 0);
    CHECK((t.flags & CLAP_TRANSPORT_HAS_TEMPO) != 0);
    CHECK(t.tempo == 140.0);
    CHECK((t.flags & CLAP_TRANSPORT_HAS_BEATS_TIMELINE) != 0);
    CHECK(t.song_pos_beats == (clap_beattime)(9.5 * CLAP_BEATTIME_FACTOR));
    CHECK((t.flags & CLAP_TRANSPORT_HAS_SECONDS_TIMELINE) != 0);
    CHECK(t.song_pos_seconds == (clap_sectime)(2.0 * CLAP_SECTIME_FACTOR));
    CHECK((t.flags & CLAP_TRANSPORT_HAS_TIME_SIGNATURE) != 0);
    CHECK(t.tsig_num == 3);
    CHECK(t.tsig_denom == 4);
    CHECK(t.loop_start_beats == (clap_beattime)(8.0 * CLAP_BEATTIME_FACTOR));
    CHECK(t.loop_end_beats == (clap_beattime)(16.0 * CLAP_BEATTIME_FACTOR));
    // 3/4: three quarter notes per bar, so beat 9.5 is in bar 3, which starts at 9.
    CHECK(t.bar_number == 3);
    CHECK(t.bar_start == (clap_beattime)(9.0 * CLAP_BEATTIME_FACTOR));
}

TEST_CASE("CLAP transport claims only what the host supplied", "[host][clap][transport]") {
    fmt::ProcessContext ctx;
    ctx.is_playing = true;
    ctx.transport_validity.set(fmt::TransportField::Playing);
    clap_event_transport_t t{};
    pulp::host::clap_transport_from_context(ctx, t);
    CHECK((t.flags & CLAP_TRANSPORT_IS_PLAYING) != 0);
    CHECK((t.flags & CLAP_TRANSPORT_HAS_TEMPO) == 0);
    CHECK((t.flags & CLAP_TRANSPORT_HAS_BEATS_TIMELINE) == 0);
    CHECK((t.flags & CLAP_TRANSPORT_HAS_TIME_SIGNATURE) == 0);
}

TEST_CASE("CLAP slot turns transport off when the host settings ask", "[host][clap][transport]") {
    FakeClap fake;
    auto slot = make_slot(fake);
    PluginHostSettings settings;
    settings.deliver_transport = false;
    slot->set_host_settings(settings);
    CHECK_FALSE(slot->wants_transport());
}

TEST_CASE("CLAP sidechain port receives the host sidechain bus and aux outputs reach the host",
          "[host][clap][buses]") {
    FakeClap fake;
    fake.in_ports = {{2, true}, {2, false}};
    fake.out_ports = {{2, true}, {2, false}};
    fake.aux_output_value = 0.625f;
    auto slot = make_slot(fake);
    REQUIRE(slot->prepare(48000.0, 64));

    constexpr int kFrames = 16;
    std::vector<std::vector<float>> store(8, std::vector<float>(kFrames, 0.0f));
    std::fill(store[2].begin(), store[2].end(), 0.5f); // sidechain L
    std::array<const float*, 2> main_in{store[0].data(), store[1].data()};
    std::array<const float*, 2> side_in{store[2].data(), store[3].data()};
    std::array<float*, 2> main_out{store[4].data(), store[5].data()};
    std::array<float*, 2> aux_out{store[6].data(), store[7].data()};
    std::array<fmt::ProcessBusBufferView<const float>, 2> ins{};
    ins[0].buffer = pulp::audio::BufferView<const float>(main_in.data(), 2, kFrames);
    ins[1].info.role = fmt::BusRole::Sidechain;
    ins[1].buffer = pulp::audio::BufferView<const float>(side_in.data(), 2, kFrames);
    std::array<fmt::ProcessBusBufferView<float>, 2> outs{};
    outs[0].buffer = pulp::audio::BufferView<float>(main_out.data(), 2, kFrames);
    outs[1].info.role = fmt::BusRole::Aux;
    outs[1].buffer = pulp::audio::BufferView<float>(aux_out.data(), 2, kFrames);
    fmt::ProcessBuffers buffers{fmt::ProcessBusBufferSet<const float>{std::span(ins)},
                                fmt::ProcessBusBufferSet<float>{std::span(outs)}};
    pulp::midi::MidiBuffer midi_in;
    pulp::midi::MidiBuffer midi_out;
    pulp::host::ParameterEventQueue params;
    slot->process(buffers, midi_in, midi_out, params, kFrames);

    CHECK(fake.audio_inputs_count == 2);
    CHECK(fake.audio_outputs_count == 2);
    CHECK(fake.sidechain_sample == 0.5f);
    CHECK(store[6][kFrames - 1] == 0.625f);
    CHECK(store[7][0] == 0.625f);
}

TEST_CASE("CLAP sidechain port without a host sidechain gets silence", "[host][clap][buses]") {
    FakeClap fake;
    fake.in_ports = {{2, true}, {1, false}};
    auto slot = make_slot(fake);
    REQUIRE(slot->prepare(48000.0, 64));
    fake.sidechain_sample = 9.0f;
    Block b;
    b.run(*slot);
    REQUIRE(fake.input_channel_counts.size() == 2);
    CHECK(fake.input_channel_counts[1] == 1);
    CHECK(fake.sidechain_sample == 0.0f);
}

TEST_CASE("CLAP slot processes in 64-bit when asked and every port supports it",
          "[host][clap][buses]") {
    FakeClap fake;
    fake.in_ports = {{2, true, true}};
    fake.out_ports = {{2, true, true}};
    auto slot = make_slot(fake);
    PluginHostSettings settings;
    settings.prefer_double_precision = true;
    slot->set_host_settings(settings);
    REQUIRE(slot->prepare(48000.0, 64));
    Block b;
    b.run(*slot);
    CHECK(fake.saw_data64);

    fake.out_ports = {{2, true, false}};
    REQUIRE(slot->prepare(48000.0, 64));
    b.run(*slot);
    CHECK_FALSE(fake.saw_data64);
}

TEST_CASE("CLAP note port in the CLAP dialect gets note events and pressure expressions",
          "[host][clap][midi]") {
    FakeClap fake;
    fake.declare_note_port = true;
    fake.note_dialects = CLAP_NOTE_DIALECT_CLAP | CLAP_NOTE_DIALECT_MIDI;
    fake.preferred_dialect = CLAP_NOTE_DIALECT_CLAP;
    auto slot = make_slot(fake);
    REQUIRE(slot->prepare(48000.0, 64));
    Block b;
    auto on = pulp::midi::MidiEvent::note_on(0, 60, 127);
    on.sample_offset = 2;
    b.midi_in.add(on);
    pulp::midi::MidiEvent pressure;
    pressure.sample_offset = 4;
    pressure.message = choc::midi::ShortMessage(0xA0, 60, 127);
    b.midi_in.add(pressure);
    pulp::midi::MidiEvent cc;
    cc.sample_offset = 5;
    cc.message = choc::midi::ShortMessage(0xB0, 1, 64);
    b.midi_in.add(cc);
    b.run(*slot);
    CHECK(fake.event_types ==
          std::vector<uint16_t>{CLAP_EVENT_NOTE_ON, CLAP_EVENT_NOTE_EXPRESSION, CLAP_EVENT_MIDI});
    CHECK(fake.last_note_velocity == 1.0);
    CHECK(fake.last_expression == 1.0);
    CHECK(fake.last_midi == std::vector<uint8_t>{0xB0, 1, 64});
}

TEST_CASE("CLAP plugin receives MIDI and sysex in time order", "[host][clap][midi]") {
    FakeClap fake;
    auto slot = make_slot(fake);
    REQUIRE(slot->prepare(48000.0, 64));
    Block b;
    pulp::midi::MidiEvent bend;
    bend.sample_offset = 8;
    bend.message = choc::midi::ShortMessage(0xE1, 0x00, 0x40);
    b.midi_in.add(bend);
    b.midi_in.add_sysex({0xF0, 0x7E, 0x7F, 0xF7}, 3);
    b.run(*slot);
    CHECK(fake.event_types == std::vector<uint16_t>{CLAP_EVENT_MIDI_SYSEX, CLAP_EVENT_MIDI});
    CHECK(fake.last_sysex == std::vector<uint8_t>{0xF0, 0x7E, 0x7F, 0xF7});
    CHECK(fake.last_midi == std::vector<uint8_t>{0xE1, 0x00, 0x40});
}

TEST_CASE("CLAP plugin notes and sysex come back as MIDI", "[host][clap][midi]") {
    FakeClap fake;
    fake.emit_note = true;
    fake.emit_sysex = true;
    auto slot = make_slot(fake);
    REQUIRE(slot->prepare(48000.0, 64));
    Block b;
    b.run(*slot);
    REQUIRE(b.midi_out.size() == 1);
    const auto& note = *b.midi_out.begin();
    CHECK(note.sample_offset == 3);
    CHECK(note.message.data()[0] == 0x92);
    CHECK(note.message.data()[1] == 64);
    CHECK(note.message.data()[2] == 127);
    REQUIRE(b.midi_out.sysex_size() == 1);
    CHECK(b.midi_out.sysex()[0].data == std::vector<uint8_t>{0xF0, 0x42, 0x10, 0xF7});
    CHECK(b.midi_out.sysex()[0].sample_offset == 1);
}

TEST_CASE("CLAP plugin parameter gestures reach the host and replace the cached value",
          "[host][clap][params]") {
    FakeClap fake;
    auto slot = make_slot(fake);
    REQUIRE(slot->prepare(48000.0, 64));
    slot->set_parameter(7, 0.25f);
    CHECK(slot->get_parameter(7) == 0.25f);

    fake.emit_param = true;
    Block b;
    b.run(*slot);
    std::array<PluginParameterEdit, 8> edits{};
    REQUIRE(slot->drain_parameter_edits(edits) == 3);
    CHECK(edits[0].kind == PluginParameterEdit::Kind::begin_gesture);
    CHECK(edits[1].kind == PluginParameterEdit::Kind::value);
    CHECK(edits[1].id == 7);
    CHECK(edits[1].plain_value == 0.875f);
    CHECK(edits[2].kind == PluginParameterEdit::Kind::end_gesture);
    // The plugin's own value wins over the host's earlier edit.
    CHECK(slot->get_parameter(7) == 0.875f);
}

TEST_CASE("CLAP flush and main-thread callback requests are serviced by the control pump",
          "[host][clap][params]") {
    FakeClap fake;
    auto slot = make_slot(fake);
    auto* params = static_cast<const clap_host_params_t*>(
        fake.host->get_extension(fake.host, CLAP_EXT_PARAMS));
    REQUIRE(params);
    params->request_flush(fake.host);
    fake.host->request_callback(fake.host);
    std::array<PluginParameterEdit, 4> edits{};
    // Not processing: the flush runs on the control thread.
    REQUIRE(slot->drain_parameter_edits(edits) == 1);
    CHECK(edits[0].plain_value == 0.125f);
    CHECK(fake.on_main_thread_calls == 1);
}

TEST_CASE("CLAP state dirty and restart requests are counted", "[host][clap][restart]") {
    FakeClap fake;
    auto slot = make_slot(fake);
    auto* state =
        static_cast<const clap_host_state_t*>(fake.host->get_extension(fake.host, CLAP_EXT_STATE));
    REQUIRE(state);
    state->mark_dirty(fake.host);
    CHECK(slot->state_change_count() == 1);
    const auto before = slot->restart_request_count();
    fake.host->request_restart(fake.host);
    CHECK(slot->restart_request_count() == before + 1);
}

TEST_CASE("CLAP latency change reaches SignalGraph latency compensation",
          "[host][clap][restart][pdc]") {
    FakeClap fake;
    fake.latency = 64;
    auto slot = make_slot(fake);
    auto* raw = slot.get();
    pulp::host::SignalGraph graph;
    const auto in = graph.add_input_node(2);
    const auto plug = graph.add_plugin_node(std::move(slot), 2, 2, "fake");
    const auto out = graph.add_output_node(2);
    REQUIRE(graph.connect(in, 0, plug, 0));
    REQUIRE(graph.connect(in, 1, plug, 1));
    REQUIRE(graph.connect(plug, 0, out, 0));
    REQUIRE(graph.connect(plug, 1, out, 1));
    REQUIRE(graph.prepare(48000.0, 128));
    CHECK(graph.latency_samples() == 64);
    CHECK_FALSE(graph.plugin_restart_pending());

    // The plugin's latency moves and it asks the host to restart it.
    fake.latency = 256;
    auto* latency = static_cast<const clap_host_latency_t*>(
        fake.host->get_extension(fake.host, CLAP_EXT_LATENCY));
    REQUIRE(latency);
    latency->changed(fake.host);
    CHECK(raw->restart_request_count() > 0);
    CHECK(graph.plugin_restart_pending());
    CHECK(graph.latency_samples() == 64);

    const int activations = fake.activate_calls;
    REQUIRE(graph.apply_plugin_restarts());
    CHECK(fake.activate_calls == activations + 1);
    CHECK(graph.latency_samples() == 256);
    CHECK_FALSE(graph.plugin_restart_pending());
}

TEST_CASE("SignalGraph hands its hosted process mode to hosted plugins at prepare",
          "[host][clap][offline]") {
    FakeClap fake;
    auto slot = make_slot(fake);
    pulp::host::SignalGraph graph;
    const auto in = graph.add_input_node(2);
    const auto plug = graph.add_plugin_node(std::move(slot), 2, 2, "fake");
    const auto out = graph.add_output_node(2);
    REQUIRE(graph.connect(in, 0, plug, 0));
    REQUIRE(graph.connect(plug, 0, out, 0));
    graph.set_hosted_plugin_process_mode(fmt::ProcessMode::Offline);
    REQUIRE(graph.prepare(48000.0, 128));
    CHECK(fake.render_mode == CLAP_RENDER_OFFLINE);
    graph.set_hosted_plugin_process_mode(fmt::ProcessMode::Realtime);
    REQUIRE(graph.prepare(48000.0, 128));
    CHECK(fake.render_mode == CLAP_RENDER_REALTIME);
}
