#include <pulp/audio/buffer.hpp>
#include <pulp/format/processor.hpp>
#include <pulp/format/reload/reload_abi.hpp>
#include <pulp/host/baked_graph_processor.hpp>
#include <pulp/host/graph_serializer.hpp>
#include <pulp/host/scanner.hpp>
#include <pulp/host/signal_graph.hpp>
#include <pulp/midi/buffer.hpp>
#include <pulp/native_components/pulp_node_v1.h>
#include <pulp/runtime/crypto.hpp>
#include <pulp/runtime/node_abi.hpp>

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <type_traits>
#include <vector>

namespace fs = std::filesystem;
using namespace pulp;
using namespace pulp::host;

PULP_RELOAD_LOGIC(nullptr)

namespace {
enum Exit : int { ok = 0, drift = 1, harness_failure = 2 };

using NodeEntryFn = const pulp_node_entry_v1* (*)();
using NodeAllocFn = void* (*)(void*, std::size_t);
using NodeFreeFn = void (*)(void*, void*);
using NodeLogFn = void (*)(void*, std::int32_t, const char*, std::size_t);
using NodeNowFn = std::uint64_t (*)(void*);
using NodeWriteFn = void (*)(void*, const std::uint8_t*, std::size_t);
using NodeDescriptorFn = const pulp_node_descriptor_v1* (*)();
using NodeCreateFn = pulp_node_status_v1 (*)(const pulp_node_host_services_v1*,
                                             pulp_node_instance_v1**);
using NodeInstanceFn = void (*)(pulp_node_instance_v1*);
using NodePrepareFn = pulp_node_status_v1 (*)(pulp_node_instance_v1*, const pulp_node_prepare_v1*);
using NodeProcessFn = pulp_node_status_v1 (*)(pulp_node_instance_v1*, const pulp_node_audio_v1*);
using NodeSaveFn = pulp_node_status_v1 (*)(pulp_node_instance_v1*, const pulp_node_writer_v1*);
using NodeLoadFn = pulp_node_status_v1 (*)(pulp_node_instance_v1*, const std::uint8_t*,
                                           std::size_t);
using NodeLatencyFn = std::uint32_t (*)(pulp_node_instance_v1*);

static_assert(std::is_same_v<decltype(&pulp_node_v1_entry), NodeEntryFn>);
static_assert(std::is_same_v<decltype(&pulp_node_v1_abi_major), std::uint32_t (*)()>);
static_assert(std::is_same_v<pulp_node_status_v1, std::int32_t>);
static_assert(std::is_same_v<pulp_node_caps_v1, std::uint32_t>);
#define ASSERT_C_ABI_FIELD(type, field, expected)                                                  \
    static_assert(std::is_same_v<decltype(type::field), expected>)
ASSERT_C_ABI_FIELD(pulp_node_host_services_v1, size, std::uint32_t);
ASSERT_C_ABI_FIELD(pulp_node_host_services_v1, abi_major, std::uint32_t);
ASSERT_C_ABI_FIELD(pulp_node_host_services_v1, host_context, void*);
ASSERT_C_ABI_FIELD(pulp_node_descriptor_v1, size, std::uint32_t);
ASSERT_C_ABI_FIELD(pulp_node_descriptor_v1, abi_major, std::uint32_t);
ASSERT_C_ABI_FIELD(pulp_node_descriptor_v1, stable_id, const char*);
ASSERT_C_ABI_FIELD(pulp_node_descriptor_v1, stable_id_len, std::size_t);
ASSERT_C_ABI_FIELD(pulp_node_descriptor_v1, display_name, const char*);
ASSERT_C_ABI_FIELD(pulp_node_descriptor_v1, display_name_len, std::size_t);
ASSERT_C_ABI_FIELD(pulp_node_descriptor_v1, node_version, std::uint32_t);
ASSERT_C_ABI_FIELD(pulp_node_descriptor_v1, capability_flags, pulp_node_caps_v1);
ASSERT_C_ABI_FIELD(pulp_node_descriptor_v1, audio_input_count, std::uint32_t);
ASSERT_C_ABI_FIELD(pulp_node_descriptor_v1, audio_output_count, std::uint32_t);
ASSERT_C_ABI_FIELD(pulp_node_audio_v1, size, std::uint32_t);
ASSERT_C_ABI_FIELD(pulp_node_audio_v1, abi_major, std::uint32_t);
ASSERT_C_ABI_FIELD(pulp_node_audio_v1, frame_count, std::uint32_t);
ASSERT_C_ABI_FIELD(pulp_node_audio_v1, input_count, std::uint32_t);
ASSERT_C_ABI_FIELD(pulp_node_audio_v1, output_count, std::uint32_t);
ASSERT_C_ABI_FIELD(pulp_node_audio_v1, reserved, std::uint32_t);
ASSERT_C_ABI_FIELD(pulp_node_audio_v1, inputs, const float* const*);
ASSERT_C_ABI_FIELD(pulp_node_audio_v1, outputs, float* const*);
ASSERT_C_ABI_FIELD(pulp_node_prepare_v1, size, std::uint32_t);
ASSERT_C_ABI_FIELD(pulp_node_prepare_v1, abi_major, std::uint32_t);
ASSERT_C_ABI_FIELD(pulp_node_prepare_v1, sample_rate, double);
ASSERT_C_ABI_FIELD(pulp_node_prepare_v1, max_block_size, std::uint32_t);
ASSERT_C_ABI_FIELD(pulp_node_prepare_v1, reserved, std::uint32_t);
ASSERT_C_ABI_FIELD(pulp_node_writer_v1, size, std::uint32_t);
ASSERT_C_ABI_FIELD(pulp_node_writer_v1, abi_major, std::uint32_t);
ASSERT_C_ABI_FIELD(pulp_node_writer_v1, writer_context, void*);
#undef ASSERT_C_ABI_FIELD
static_assert(std::is_same_v<decltype(pulp_node_host_services_v1::alloc), NodeAllocFn>);
static_assert(std::is_same_v<decltype(pulp_node_host_services_v1::free), NodeFreeFn>);
static_assert(std::is_same_v<decltype(pulp_node_host_services_v1::log), NodeLogFn>);
static_assert(std::is_same_v<decltype(pulp_node_host_services_v1::now_ns), NodeNowFn>);
static_assert(std::is_same_v<decltype(pulp_node_writer_v1::write), NodeWriteFn>);
static_assert(std::is_same_v<decltype(pulp_node_entry_v1::descriptor), NodeDescriptorFn>);
static_assert(std::is_same_v<decltype(pulp_node_entry_v1::create), NodeCreateFn>);
static_assert(std::is_same_v<decltype(pulp_node_entry_v1::destroy), NodeInstanceFn>);
static_assert(std::is_same_v<decltype(pulp_node_entry_v1::prepare), NodePrepareFn>);
static_assert(std::is_same_v<decltype(pulp_node_entry_v1::reset), NodeInstanceFn>);
static_assert(std::is_same_v<decltype(pulp_node_entry_v1::process), NodeProcessFn>);
static_assert(std::is_same_v<decltype(pulp_node_entry_v1::release), NodeInstanceFn>);
static_assert(std::is_same_v<decltype(pulp_node_entry_v1::save_state), NodeSaveFn>);
static_assert(std::is_same_v<decltype(pulp_node_entry_v1::load_state), NodeLoadFn>);
static_assert(std::is_same_v<decltype(pulp_node_entry_v1::report_latency), NodeLatencyFn>);
static_assert(std::is_same_v<format::reload::ReloadAbiVersionFn, int (*)()>);
static_assert(std::is_same_v<format::reload::ReloadFingerprintFn,
                             void (*)(format::reload::BuildFingerprint*)>);
static_assert(std::is_same_v<format::reload::ReloadCreateFn, format::Processor* (*)()>);
static_assert(std::is_same_v<format::reload::ReloadDestroyFn, void (*)(format::Processor*)>);
static_assert(
    std::is_same_v<decltype(&pulp_reload_abi_version), format::reload::ReloadAbiVersionFn>);
static_assert(
    std::is_same_v<decltype(&pulp_reload_fingerprint), format::reload::ReloadFingerprintFn>);
static_assert(std::is_same_v<decltype(&pulp_reload_create), format::reload::ReloadCreateFn>);
static_assert(std::is_same_v<decltype(&pulp_reload_destroy), format::reload::ReloadDestroyFn>);

std::string read_text(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    if (!in)
        throw std::runtime_error("cannot read " + p.string());
    return {std::istreambuf_iterator<char>(in), {}};
}
std::vector<std::uint8_t> read_bytes(const fs::path& p) {
    const auto s = read_text(p);
    return {s.begin(), s.end()};
}
void write_text(const fs::path& p, const std::string& s) {
    fs::create_directories(p.parent_path());
    std::ofstream out(p, std::ios::binary | std::ios::trunc);
    if (!out)
        throw std::runtime_error("cannot write " + p.string());
    out.write(s.data(), static_cast<std::streamsize>(s.size()));
}
void write_bytes(const fs::path& p, const std::vector<std::uint8_t>& b) {
    write_text(p, {reinterpret_cast<const char*>(b.data()), b.size()});
}

std::string hex_bytes(const std::vector<std::uint8_t>& bytes) {
    std::ostringstream out;
    for (auto b : bytes)
        out << std::hex << std::setw(2) << std::setfill('0') << unsigned(b);
    return out.str();
}
std::vector<std::uint8_t> unhex(std::string s) {
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r'))
        s.pop_back();
    if (s.size() % 2)
        throw std::runtime_error("odd hex fixture");
    std::vector<std::uint8_t> out;
    for (std::size_t i = 0; i < s.size(); i += 2)
        out.push_back(static_cast<std::uint8_t>(std::stoul(s.substr(i, 2), nullptr, 16)));
    return out;
}

std::vector<float> stimulus() {
    std::vector<float> v(48);
    for (std::size_t i = 0; i < v.size(); ++i)
        v[i] = static_cast<float>((static_cast<int>(i * 17 % 23) - 11) / 16.0);
    return v;
}

std::vector<float> render(SignalGraph& graph, const std::vector<int>& schedule) {
    constexpr int max_block = 16;
    if (!graph.prepare(48000.0, max_block))
        throw std::runtime_error("graph prepare refused");
    auto input = stimulus();
    std::vector<float> output(input.size(), -99.0f);
    int offset = 0;
    std::size_t turn = 0;
    while (offset < static_cast<int>(input.size())) {
        const int n =
            std::min(schedule[turn++ % schedule.size()], static_cast<int>(input.size()) - offset);
        const float* ip[] = {input.data() + offset};
        float* op[] = {output.data() + offset};
        audio::BufferView<const float> in(ip, 1, static_cast<std::uint32_t>(n));
        audio::BufferView<float> out(op, 1, static_cast<std::uint32_t>(n));
        graph.process(out, in, n);
        offset += n;
    }
    return output;
}

std::vector<float> render_baked(format::Processor& processor) {
    constexpr int block = 8;
    format::PrepareContext prep;
    prep.sample_rate = 48000.0;
    prep.max_buffer_size = block;
    prep.input_channels = 1;
    prep.output_channels = 1;
    processor.prepare(prep);
    const auto input = stimulus();
    std::vector<float> output(input.size(), -99.0f);
    midi::MidiBuffer mi, mo;
    format::ProcessContext ctx;
    ctx.sample_rate = 48000.0;
    for (int off = 0; off < static_cast<int>(input.size()); off += block) {
        const float* ip[] = {input.data() + off};
        float* op[] = {output.data() + off};
        audio::BufferView<const float> in(ip, 1, block);
        audio::BufferView<float> out(op, 1, block);
        processor.process(out, in, mi, mo, ctx);
    }
    return output;
}

std::string floats(const std::vector<float>& values) {
    std::ostringstream out;
    for (float value : values)
        out << std::hex << std::setw(8) << std::setfill('0') << std::bit_cast<std::uint32_t>(value)
            << '\n';
    return out.str();
}

void load_graph(SignalGraph& graph, const fs::path& path) {
    auto result = GraphSerializer::from_json(graph, read_text(path));
    if (!result.ok)
        throw std::runtime_error("graph load refused: " + result.error);
}

std::string abi_facts() {
    auto member_pointer = [](auto pointer) {
        std::array<std::uint8_t, sizeof(pointer)> bytes{};
        std::memcpy(bytes.data(), &pointer, sizeof(pointer));
        return hex_bytes(std::vector<std::uint8_t>(bytes.begin(), bytes.end()));
    };
    using LegacyProcess = void (format::Processor::*)(
        audio::BufferView<float>&, const audio::BufferView<const float>&, midi::MidiBuffer&,
        midi::MidiBuffer&, const format::ProcessContext&);
    std::ostringstream out;
    out << "processor_size=" << sizeof(format::Processor) << '\n'
        << "processor_align=" << alignof(format::Processor) << '\n'
        << "custom_node_type_size=" << sizeof(CustomNodeType) << '\n'
        << "node_type_audio_input=" << static_cast<int>(NodeType::AudioInput) << '\n'
        << "node_type_audio_output=" << static_cast<int>(NodeType::AudioOutput) << '\n'
        << "node_type_plugin=" << static_cast<int>(NodeType::Plugin) << '\n'
        << "node_type_gain=" << static_cast<int>(NodeType::Gain) << '\n'
        << "node_type_midi_input=" << static_cast<int>(NodeType::MidiInput) << '\n'
        << "node_type_midi_output=" << static_cast<int>(NodeType::MidiOutput) << '\n'
        << "node_type_custom=" << static_cast<int>(NodeType::Custom) << '\n'
        << "automation_mix_replace=" << static_cast<int>(AutomationMix::Replace) << '\n'
        << "automation_mix_add=" << static_cast<int>(AutomationMix::Add) << '\n'
        << "plugin_format_vst3=" << static_cast<int>(PluginFormat::VST3) << '\n'
        << "plugin_format_auv2=" << static_cast<int>(PluginFormat::AudioUnit) << '\n'
        << "plugin_format_auv3=" << static_cast<int>(PluginFormat::AudioUnitV3) << '\n'
        << "plugin_format_clap=" << static_cast<int>(PluginFormat::CLAP) << '\n'
        << "plugin_format_lv2=" << static_cast<int>(PluginFormat::LV2) << '\n'
        << "plugin_format_builtin=" << static_cast<int>(PluginFormat::BuiltIn) << '\n'
        << "param_rate_control=" << static_cast<int>(state::ParamRate::ControlRate) << '\n'
        << "param_rate_audio=" << static_cast<int>(state::ParamRate::AudioRate) << '\n'
        << "plugin_category_effect=" << static_cast<int>(format::PluginCategory::Effect) << '\n'
        << "plugin_category_instrument=" << static_cast<int>(format::PluginCategory::Instrument)
        << '\n'
        << "plugin_category_midi_effect=" << static_cast<int>(format::PluginCategory::MidiEffect)
        << '\n'
        << "node_abi_version=" << PULP_NODE_ABI_VERSION << '\n'
        << "reload_abi_version=" << format::reload::kReloadAbiVersion << '\n'
        << "reload_symbol_abi=" << format::reload::kAbiVersionSymbol << '\n'
        << "reload_symbol_fingerprint=" << format::reload::kFingerprintSymbol << '\n'
        << "reload_symbol_create=" << format::reload::kCreateSymbol << '\n'
        << "reload_symbol_destroy=" << format::reload::kDestroySymbol << '\n'
        << "node_v1_abi_minor=" << PULP_NODE_V1_ABI_MINOR << '\n'
        << "node_status_ok=" << PULP_NODE_OK_V1 << '\n'
        << "node_status_unsupported=" << PULP_NODE_ERR_UNSUPPORTED_V1 << '\n'
        << "node_status_invalid_argument=" << PULP_NODE_ERR_INVALID_ARGUMENT_V1 << '\n'
        << "node_status_out_of_memory=" << PULP_NODE_ERR_OUT_OF_MEMORY_V1 << '\n'
        << "node_status_invalid_state=" << PULP_NODE_ERR_INVALID_STATE_V1 << '\n'
        << "node_status_malformed_state=" << PULP_NODE_ERR_MALFORMED_STATE_V1 << '\n'
        << "node_status_version_mismatch=" << PULP_NODE_ERR_VERSION_MISMATCH_V1 << '\n'
        << "node_status_internal=" << PULP_NODE_ERR_INTERNAL_V1 << '\n'
        << "node_cap_state=" << PULP_NODE_CAP_STATE_V1 << '\n'
        << "node_cap_reset=" << PULP_NODE_CAP_RESET_V1 << '\n'
        << "node_cap_events=" << PULP_NODE_CAP_EVENTS_V1 << '\n'
        << "node_cap_latency=" << PULP_NODE_CAP_LATENCY_V1
        << '\n'
#define C_ABI_TYPE(type)                                                                           \
    << "c_abi." #type ".sizeof=" << sizeof(type) << '\n'                                           \
    << "c_abi." #type ".alignof=" << alignof(type) << '\n'
#define C_ABI_FIELD(type, field) << "c_abi." #type "." #field "=" << offsetof(type, field) << '\n'
        C_ABI_TYPE(pulp_node_host_services_v1) C_ABI_FIELD(
            pulp_node_host_services_v1,
            size) C_ABI_FIELD(pulp_node_host_services_v1,
                              abi_major) C_ABI_FIELD(pulp_node_host_services_v1, host_context)
            C_ABI_FIELD(pulp_node_host_services_v1, alloc) C_ABI_FIELD(
                pulp_node_host_services_v1,
                free) C_ABI_FIELD(pulp_node_host_services_v1,
                                  log) C_ABI_FIELD(pulp_node_host_services_v1,
                                                   now_ns) C_ABI_TYPE(pulp_node_descriptor_v1)
                C_ABI_FIELD(pulp_node_descriptor_v1, size) C_ABI_FIELD(
                    pulp_node_descriptor_v1,
                    abi_major) C_ABI_FIELD(pulp_node_descriptor_v1,
                                           stable_id) C_ABI_FIELD(pulp_node_descriptor_v1,
                                                                  stable_id_len)
                    C_ABI_FIELD(pulp_node_descriptor_v1, display_name) C_ABI_FIELD(
                        pulp_node_descriptor_v1,
                        display_name_len) C_ABI_FIELD(pulp_node_descriptor_v1,
                                                      node_version) C_ABI_FIELD(pulp_node_descriptor_v1,
                                                                                capability_flags)
                        C_ABI_FIELD(pulp_node_descriptor_v1, audio_input_count) C_ABI_FIELD(
                            pulp_node_descriptor_v1,
                            audio_output_count) C_ABI_TYPE(pulp_node_audio_v1)
                            C_ABI_FIELD(pulp_node_audio_v1, size) C_ABI_FIELD(
                                pulp_node_audio_v1,
                                abi_major) C_ABI_FIELD(pulp_node_audio_v1,
                                                       frame_count) C_ABI_FIELD(pulp_node_audio_v1,
                                                                                input_count)
                                C_ABI_FIELD(pulp_node_audio_v1, output_count) C_ABI_FIELD(
                                    pulp_node_audio_v1,
                                    reserved) C_ABI_FIELD(pulp_node_audio_v1,
                                                          inputs) C_ABI_FIELD(pulp_node_audio_v1,
                                                                              outputs)
                                    C_ABI_TYPE(pulp_node_prepare_v1) C_ABI_FIELD(
                                        pulp_node_prepare_v1,
                                        size) C_ABI_FIELD(pulp_node_prepare_v1,
                                                          abi_major) C_ABI_FIELD(pulp_node_prepare_v1,
                                                                                 sample_rate)
                                        C_ABI_FIELD(pulp_node_prepare_v1, max_block_size) C_ABI_FIELD(
                                            pulp_node_prepare_v1,
                                            reserved) C_ABI_TYPE(pulp_node_writer_v1)
                                            C_ABI_FIELD(pulp_node_writer_v1, size) C_ABI_FIELD(
                                                pulp_node_writer_v1,
                                                abi_major) C_ABI_FIELD(pulp_node_writer_v1,
                                                                       writer_context)
                                                C_ABI_FIELD(pulp_node_writer_v1,
                                                            write) C_ABI_TYPE(pulp_node_entry_v1)
                                                    C_ABI_FIELD(pulp_node_entry_v1, size) C_ABI_FIELD(
                                                        pulp_node_entry_v1,
                                                        abi_major) C_ABI_FIELD(pulp_node_entry_v1,
                                                                               descriptor)
                                                        C_ABI_FIELD(
                                                            pulp_node_entry_v1,
                                                            create) C_ABI_FIELD(pulp_node_entry_v1,
                                                                                destroy)
                                                            C_ABI_FIELD(
                                                                pulp_node_entry_v1,
                                                                prepare) C_ABI_FIELD(pulp_node_entry_v1,
                                                                                     reset)
                                                                C_ABI_FIELD(pulp_node_entry_v1,
                                                                            process)
                                                                    C_ABI_FIELD(pulp_node_entry_v1,
                                                                                release)
                                                                        C_ABI_FIELD(
                                                                            pulp_node_entry_v1,
                                                                            save_state)
                                                                            C_ABI_FIELD(
                                                                                pulp_node_entry_v1,
                                                                                load_state)
                                                                                C_ABI_FIELD(
                                                                                    pulp_node_entry_v1,
                                                                                    report_latency)
#undef C_ABI_FIELD
#undef C_ABI_TYPE
        << "virtual.descriptor=" << member_pointer(&format::Processor::descriptor) << '\n'
        << "virtual.define_parameters=" << member_pointer(&format::Processor::define_parameters)
        << '\n'
        << "virtual.prepare=" << member_pointer(&format::Processor::prepare) << '\n'
        << "virtual.release=" << member_pointer(&format::Processor::release) << '\n'
        << "virtual.suspend=" << member_pointer(&format::Processor::suspend) << '\n'
        << "virtual.resume=" << member_pointer(&format::Processor::resume) << '\n'
        << "virtual.process="
        << member_pointer(static_cast<LegacyProcess>(&format::Processor::process)) << '\n';
    return out.str();
}

void build_feedback_api_graph(SignalGraph& graph) {
    const auto input = graph.add_input_node(1, "Input");
    const auto gain = graph.add_gain_node("Legacy feedback gain");
    const auto output = graph.add_output_node(1, "Output");
    if (!graph.connect(input, 0, gain, 0) || !graph.connect_feedback(gain, 0, gain, 1) ||
        !graph.connect(gain, 0, output, 0) || !graph.set_node_gain(gain, 0.375f)) {
        throw std::runtime_error("connect_feedback API graph construction refused");
    }
}

std::string render_feedback_api_facts() {
    std::ostringstream out;
    SignalGraph fixed;
    build_feedback_api_graph(fixed);
    out << "[feedback_fixed]\n" << floats(render(fixed, {8}));
    SignalGraph varying;
    build_feedback_api_graph(varying);
    out << "[feedback_varying]\n" << floats(render(varying, {3, 7, 2, 11, 5}));
    return out.str();
}

std::string render_facts(const fs::path& root) {
    std::ostringstream out;
    for (const auto* file : {"legacy-v1.pulpgraph", "legacy-v2.pulpgraph"}) {
        for (const auto& [schedule_name, schedule] :
             std::array<std::pair<const char*, std::vector<int>>, 2>{
                 {{"fixed", {8}}, {"varying", {3, 7, 2, 11, 5}}}}) {
            SignalGraph graph;
            load_graph(graph, root / "graphs" / file);
            out << '[' << file << '.' << schedule_name << "]\n" << floats(render(graph, schedule));
        }
    }
    SignalGraph fixed;
    load_graph(fixed, root / "graphs" / "legacy-feedback-v1.pulpgraph");
    out << "[feedback_fixed]\n" << floats(render(fixed, {8}));
    SignalGraph varying;
    load_graph(varying, root / "graphs" / "legacy-feedback-v1.pulpgraph");
    out << "[feedback_varying]\n" << floats(render(varying, {3, 7, 2, 11, 5}));
    return out.str();
}

int emit(const fs::path& root) {
    SignalGraph empty;
    write_text(root / "expected" / "no-region-v2.pulpgraph", GraphSerializer::to_json(empty));
    write_text(root / "expected" / "render-f32-bits.txt", render_facts(root));
    write_text(root / "expected" / "abi.txt", abi_facts());

    SignalGraph graph;
    load_graph(graph, root / "graphs" / "legacy-v2.pulpgraph");
    if (!graph.prepare(48000.0, 16))
        throw std::runtime_error("bake source prepare refused");
    auto plan = bake_to_plan(graph);
    if (!plan.accepted || !plan.plan)
        throw std::runtime_error("bake_to_plan refused");
    std::array<std::uint8_t, 32> seed{};
    for (std::size_t i = 0; i < seed.size(); ++i)
        seed[i] = static_cast<std::uint8_t>(i + 1);
    auto kp = runtime::ed25519_keypair_from_seed(seed.data(), seed.size());
    if (!kp)
        throw std::runtime_error("fixture key generation failed");
    auto artifact = write_baked_signed(*plan.plan, kp->private_key);
    if (artifact.empty())
        throw std::runtime_error("fixture signing failed");
    write_bytes(root / "bake" / "legacy-v1.pulpbake", artifact);
    write_text(root / "bake" / "legacy-v1-public-key.hex", hex_bytes(kp->public_key) + "\n");
    BakedTrust trust{{kp->public_key}};
    auto loaded = load_baked(artifact, trust, {});
    if (!loaded.accepted || !loaded.processor)
        throw std::runtime_error("fixture bake load refused");
    write_text(root / "expected" / "bake-render-f32-bits.txt",
               floats(render_baked(*loaded.processor)));
    return ok;
}

int verify(const fs::path& root) {
    bool changed = false;
    auto compare = [&](const char* id, const std::string& actual, const fs::path& expected) {
        if (actual != read_text(expected)) {
            std::cerr << "compatibility_drift=" << id << '\n';
            changed = true;
        } else
            std::cout << "compatibility_ok=" << id << '\n';
    };
    SignalGraph empty;
    compare("COMP-01.no-region-bytes", GraphSerializer::to_json(empty),
            root / "expected" / "no-region-v2.pulpgraph");
    const auto rendered = render_facts(root);
    compare("COMP-01.COMP-05.graph-renders", rendered, root / "expected" / "render-f32-bits.txt");
    const auto feedback = rendered.find("[feedback_fixed]");
    if (feedback == std::string::npos || render_feedback_api_facts() != rendered.substr(feedback)) {
        std::cerr << "compatibility_drift=COMP-05.connect-feedback-api\n";
        changed = true;
    } else {
        std::cout << "compatibility_ok=COMP-05.connect-feedback-api\n";
    }
    compare("COMP-07.abi", abi_facts(), root / "expected" / "abi.txt");

    const auto artifact = read_bytes(root / "bake" / "legacy-v1.pulpbake");
    const auto public_key = unhex(read_text(root / "bake" / "legacy-v1-public-key.hex"));
    BakedTrust trust{{public_key}};
    auto loaded = load_baked(artifact, trust, {});
    if (!loaded.accepted || !loaded.processor) {
        std::cerr << "compatibility_drift=legacy-signed-bake-load\n";
        changed = true;
    } else {
        compare("legacy-signed-bake-render", floats(render_baked(*loaded.processor)),
                root / "expected" / "bake-render-f32-bits.txt");
    }
    if (!changed) {
        std::cout << "COMP-01=pass\nCOMP-05=pass\nCOMP-07=pass\nPKT-C0-01=pass\n";
        return ok;
    }
    return drift;
}
} // namespace

int main(int argc, char** argv) {
    try {
        if (argc != 3 || (std::string(argv[1]) != "--verify" && std::string(argv[1]) != "--emit")) {
            std::cerr << "usage: compat_runner (--verify|--emit) FIXTURE_ROOT\n";
            return harness_failure;
        }
        return std::string(argv[1]) == "--emit" ? emit(argv[2]) : verify(argv[2]);
    } catch (const std::exception& e) {
        std::cerr << "harness_failure=" << e.what() << '\n';
        return harness_failure;
    }
}
