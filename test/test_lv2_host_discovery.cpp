#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <pulp/host/dl_shim.hpp>
#include <pulp/host/plugin_slot.hpp>

#include "lv2_discovery.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using Catch::Matchers::WithinAbs;

namespace {

#if defined(PULP_TEST_LV2_SLOT_PROBE_BUNDLE) && defined(PULP_TEST_LV2_SLOT_PROBE_BINARY)

constexpr const char* kLv2SlotProbeUri = "http://example.com/pulp/lv2-slot-probe";
constexpr uint32_t kLv2SlotProbeGain = 4;

#endif

struct ScratchDir {
    fs::path path;

    explicit ScratchDir(const char* stem) {
        const auto counter =
            std::chrono::steady_clock::now().time_since_epoch().count();
        path = fs::temp_directory_path()
             / (std::string("pulp-lv2-host-") + stem + "-"
                + std::to_string(counter));
        std::error_code ec;
        fs::remove_all(path, ec);
        fs::create_directories(path);
    }

    ~ScratchDir() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }

    ScratchDir(const ScratchDir&) = delete;
    ScratchDir& operator=(const ScratchDir&) = delete;
};

void write_file(const fs::path& path, const std::string& body) {
    fs::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary);
    REQUIRE(out.good());
    out << body;
}

#if defined(PULP_TEST_LV2_SLOT_PROBE_BUNDLE) && defined(PULP_TEST_LV2_SLOT_PROBE_BINARY)

const pulp::host::HostParamInfo* find_param(
    const std::vector<pulp::host::HostParamInfo>& params,
    uint32_t id) {
    auto it = std::find_if(params.begin(), params.end(),
                           [id](const auto& p) { return p.id == id; });
    return it == params.end() ? nullptr : &*it;
}

class Lv2ProbeLibrary {
public:
    using VoidFn = void (*)();
    using U32Fn = uint32_t (*)();
    using FloatFn = float (*)();

    explicit Lv2ProbeLibrary(const std::string& binary_path) {
        handle_ = dlopen(binary_path.c_str(), RTLD_NOW | RTLD_LOCAL);
        REQUIRE(handle_ != nullptr);
        reset = symbol<VoidFn>("pulp_lv2_slot_probe_reset");
        instantiate_count =
            symbol<U32Fn>("pulp_lv2_slot_probe_instantiate_count");
        decoy_instantiate_count =
            symbol<U32Fn>("pulp_lv2_slot_probe_decoy_instantiate_count");
        activate_count =
            symbol<U32Fn>("pulp_lv2_slot_probe_activate_count");
        run_count = symbol<U32Fn>("pulp_lv2_slot_probe_run_count");
        deactivate_count =
            symbol<U32Fn>("pulp_lv2_slot_probe_deactivate_count");
        cleanup_count =
            symbol<U32Fn>("pulp_lv2_slot_probe_cleanup_count");
        last_sample_count =
            symbol<U32Fn>("pulp_lv2_slot_probe_last_sample_count");
        last_gain = symbol<FloatFn>("pulp_lv2_slot_probe_last_gain");
        reset();
    }

    ~Lv2ProbeLibrary() {
        if (handle_) dlclose(handle_);
    }

    Lv2ProbeLibrary(const Lv2ProbeLibrary&) = delete;
    Lv2ProbeLibrary& operator=(const Lv2ProbeLibrary&) = delete;

    VoidFn reset = nullptr;
    U32Fn instantiate_count = nullptr;
    U32Fn decoy_instantiate_count = nullptr;
    U32Fn activate_count = nullptr;
    U32Fn run_count = nullptr;
    U32Fn deactivate_count = nullptr;
    U32Fn cleanup_count = nullptr;
    U32Fn last_sample_count = nullptr;
    FloatFn last_gain = nullptr;

private:
    template <typename Fn>
    Fn symbol(const char* name) const {
        auto* ptr = dlsym(handle_, name);
        REQUIRE(ptr != nullptr);
        return reinterpret_cast<Fn>(ptr);
    }

    void* handle_ = nullptr;
};

std::unique_ptr<pulp::host::PluginSlot> load_lv2_slot_probe(
    Lv2ProbeLibrary& probe) {
    const std::string bundle = PULP_TEST_LV2_SLOT_PROBE_BUNDLE;
    if (!fs::exists(bundle)) {
        WARN("LV2 slot probe bundle not built at " << bundle << " - skipping");
        return nullptr;
    }

    probe.reset();

    pulp::host::PluginInfo info;
    info.name = "LV2 Slot Probe";
    info.path = bundle;
    info.format = pulp::host::PluginFormat::LV2;
    info.unique_id = kLv2SlotProbeUri;

    auto slot = pulp::host::PluginSlot::load(info);
    REQUIRE(slot != nullptr);
    REQUIRE(slot->is_loaded());
    REQUIRE(slot->info().unique_id == kLv2SlotProbeUri);
    return slot;
}

std::unique_ptr<pulp::host::PluginSlot> try_load_lv2_slot_probe(
    Lv2ProbeLibrary& probe) {
    const std::string binary = PULP_TEST_LV2_SLOT_PROBE_BINARY;
    if (!fs::exists(binary)) {
        WARN("LV2 slot probe module not built at " << binary << " - skipping");
        return nullptr;
    }
    return load_lv2_slot_probe(probe);
}

#endif

} // namespace

TEST_CASE("LV2 host discovery parses audio and control port roles",
          "[host][lv2][issue-493]") {
    ScratchDir scratch("ports");
    const auto bundle = scratch.path / "Probe.lv2";
    fs::create_directories(bundle);

    write_file(bundle / "plugin.ttl", R"TTL(
@prefix lv2: <http://lv2plug.in/ns/lv2core#> .

<http://example.com/pulp/probe>
    a lv2:Plugin ;
    lv2:port
    [
        a lv2:InputPort , lv2:AudioPort ;
        lv2:index 0 ;
        lv2:name "Input L"
    ] ,
    [
        a lv2:OutputPort , lv2:AudioPort ;
        lv2:index 1 ;
        lv2:name "Output L"
    ] ,
    [
        a lv2:InputPort , lv2:ControlPort ;
        lv2:index 2 ;
        lv2:name "Gain" ;
        lv2:default 0.75 ;
        lv2:minimum -1.0 ;
        lv2:maximum 2.0
    ] ,
    [
        a lv2:OutputPort , lv2:ControlPort ;
        lv2:index 3 ;
        lv2:name "Meter" ;
        lv2:default 0.0 ;
        lv2:minimum 0.0 ;
        lv2:maximum 1.0
    ] .
)TTL");

    write_file(bundle / "ignored.txt",
               "[ a lv2:InputPort , lv2:AudioPort ; lv2:index 99 ] .");
    write_file(bundle / "malformed.ttl",
               "[ a lv2:InputPort , lv2:AudioPort ; lv2:name \"No index\" ] .");

    auto roles = pulp::host::detail::discover_lv2_ports(bundle.string());
    REQUIRE(roles.size() == 4);

    REQUIRE(roles[0].index == 0);
    REQUIRE(roles[0].is_audio);
    REQUIRE_FALSE(roles[0].is_control);
    REQUIRE(roles[0].is_input);

    REQUIRE(roles[1].index == 1);
    REQUIRE(roles[1].is_audio);
    REQUIRE_FALSE(roles[1].is_input);

    REQUIRE(roles[2].index == 2);
    REQUIRE(roles[2].is_control);
    REQUIRE(roles[2].is_input);
    REQUIRE(roles[2].name == "Gain");
    REQUIRE(roles[2].default_value == 0.75f);
    REQUIRE(roles[2].min_value == -1.0f);
    REQUIRE(roles[2].max_value == 2.0f);

    REQUIRE(roles[3].index == 3);
    REQUIRE(roles[3].is_control);
    REQUIRE_FALSE(roles[3].is_input);
    REQUIRE(roles[3].name == "Meter");
}

TEST_CASE("LV2 host discovery resolves shared objects from bundle roots",
          "[host][lv2][issue-493]") {
    ScratchDir scratch("binary");
    const auto bundle = scratch.path / "Binary.lv2";
    fs::create_directories(bundle);
    write_file(bundle / "README.txt", "not loadable");

    REQUIRE(pulp::host::detail::resolve_lv2_binary(bundle.string()).empty());

    const auto so = bundle / "pulp-probe.so";
    write_file(so, "not a real shared library");
    REQUIRE(pulp::host::detail::resolve_lv2_binary(bundle.string()) == so.string());
}

TEST_CASE("LV2 PluginSlot load fails cleanly for invalid bundles",
          "[host][lv2][slot][issue-493]") {
    using namespace pulp::host;

    PluginInfo info;
    info.format = PluginFormat::LV2;
    info.name = "Missing";
    info.path = "/path/that/does/not/exist.lv2";
    REQUIRE(PluginSlot::load(info) == nullptr);

    ScratchDir scratch("load-failures");
    const auto bundle = scratch.path / "Invalid.lv2";
    fs::create_directories(bundle);

    info.name = "NoBinary";
    info.path = bundle.string();
    REQUIRE(PluginSlot::load(info) == nullptr);

    write_file(bundle / "invalid.so", "not a real shared library");
    info.name = "InvalidBinary";
    REQUIRE(PluginSlot::load(info) == nullptr);
}

#if defined(PULP_TEST_LV2_SLOT_PROBE_BUNDLE) && defined(PULP_TEST_LV2_SLOT_PROBE_BINARY)

TEST_CASE("LV2 PluginSlot loads probe descriptor by URI and exposes TTL params",
          "[host][lv2][slot][coverage][issue-493]") {
    Lv2ProbeLibrary probe(PULP_TEST_LV2_SLOT_PROBE_BINARY);
    auto slot = try_load_lv2_slot_probe(probe);
    if (!slot) return;

    REQUIRE(probe.decoy_instantiate_count() == 0);
    REQUIRE(probe.instantiate_count() == 0);
    REQUIRE(slot->prepare(48000.0, 64));
    REQUIRE(probe.decoy_instantiate_count() == 0);
    REQUIRE(probe.instantiate_count() == 1);
    REQUIRE(probe.activate_count() == 1);

    const auto params = slot->parameters();
    REQUIRE(params.size() == 1);

    const auto* gain = find_param(params, kLv2SlotProbeGain);
    REQUIRE(gain != nullptr);
    REQUIRE(gain->name == "Probe Gain");
    REQUIRE_THAT(gain->default_value, WithinAbs(0.5f, 1e-6f));
    REQUIRE_THAT(gain->min_value, WithinAbs(-2.0f, 1e-6f));
    REQUIRE_THAT(gain->max_value, WithinAbs(4.0f, 1e-6f));
    REQUIRE(gain->flags.automatable);
    REQUIRE_FALSE(gain->flags.read_only);

    REQUIRE_THAT(slot->get_parameter(kLv2SlotProbeGain),
                 WithinAbs(0.5f, 1e-6f));

    slot->release();
    REQUIRE(probe.deactivate_count() == 1);
}

TEST_CASE("LV2 PluginSlot processes audio and applies control-port events",
          "[host][lv2][slot][coverage][issue-493]") {
    Lv2ProbeLibrary probe(PULP_TEST_LV2_SLOT_PROBE_BINARY);
    auto slot = try_load_lv2_slot_probe(probe);
    if (!slot) return;

    constexpr int kFrames = 16;
    REQUIRE(slot->prepare(48000.0, kFrames));

    std::vector<float> in_l(kFrames);
    std::vector<float> in_r(kFrames);
    std::vector<float> out_l(kFrames, 0.0f);
    std::vector<float> out_r(kFrames, 0.0f);
    for (int i = 0; i < kFrames; ++i) {
        in_l[static_cast<size_t>(i)] = 0.25f + 0.01f * static_cast<float>(i);
        in_r[static_cast<size_t>(i)] = -0.50f + 0.02f * static_cast<float>(i);
    }

    const float* in_ptrs[2] = {in_l.data(), in_r.data()};
    float* out_ptrs[2] = {out_l.data(), out_r.data()};
    pulp::audio::BufferView<const float> input(in_ptrs, 2, kFrames);
    pulp::audio::BufferView<float> output(out_ptrs, 2, kFrames);
    pulp::midi::MidiBuffer midi_in;
    pulp::midi::MidiBuffer midi_out;

    slot->set_parameter(kLv2SlotProbeGain, 1.25f);
    pulp::host::ParameterEventQueue empty_events;
    slot->process(output, input, midi_in, midi_out, empty_events, kFrames);

    REQUIRE(probe.run_count() == 1);
    REQUIRE(probe.last_sample_count() == kFrames);
    REQUIRE_THAT(probe.last_gain(), WithinAbs(1.25f, 1e-6f));
    REQUIRE_THAT(out_l[3], WithinAbs(in_l[3] * 1.25f, 1e-6f));
    REQUIRE_THAT(out_r[7], WithinAbs(in_r[7] * 1.25f, 1e-6f));
    REQUIRE_THAT(slot->get_parameter(kLv2SlotProbeGain),
                 WithinAbs(1.25f, 1e-6f));

    std::fill(out_l.begin(), out_l.end(), 0.0f);
    std::fill(out_r.begin(), out_r.end(), 0.0f);
    slot->set_parameter(kLv2SlotProbeGain, 0.75f);

    pulp::host::ParameterEventQueue param_events;
    param_events.push({kLv2SlotProbeGain, 0, 1.5f});
    param_events.push({kLv2SlotProbeGain, kFrames - 1, 2.0f});
    param_events.sort();
    slot->process(output, input, midi_in, midi_out, param_events, kFrames);

    REQUIRE(probe.run_count() == 2);
    REQUIRE_THAT(probe.last_gain(), WithinAbs(2.0f, 1e-6f));
    REQUIRE_THAT(out_l[3], WithinAbs(in_l[3] * 2.0f, 1e-6f));
    REQUIRE_THAT(out_r[7], WithinAbs(in_r[7] * 2.0f, 1e-6f));
    REQUIRE_THAT(slot->get_parameter(kLv2SlotProbeGain),
                 WithinAbs(2.0f, 1e-6f));

    slot->release();
}

TEST_CASE("LV2 PluginSlot bypass and released processing avoid LV2 run",
          "[host][lv2][slot][coverage][issue-493]") {
    Lv2ProbeLibrary probe(PULP_TEST_LV2_SLOT_PROBE_BINARY);
    auto slot = try_load_lv2_slot_probe(probe);
    if (!slot) return;

    constexpr int kFrames = 8;
    REQUIRE(slot->prepare(44100.0, kFrames));

    std::vector<float> in_l(kFrames);
    std::vector<float> in_r(kFrames);
    std::vector<float> out_l(kFrames, 9.0f);
    std::vector<float> out_r(kFrames, 9.0f);
    std::vector<float> out_extra(kFrames, 9.0f);
    for (int i = 0; i < kFrames; ++i) {
        in_l[static_cast<size_t>(i)] = 0.1f * static_cast<float>(i + 1);
        in_r[static_cast<size_t>(i)] = -0.2f * static_cast<float>(i + 1);
    }

    const float* in_ptrs[2] = {in_l.data(), in_r.data()};
    float* out_ptrs[3] = {out_l.data(), out_r.data(), out_extra.data()};
    pulp::audio::BufferView<const float> input(in_ptrs, 2, kFrames);
    pulp::audio::BufferView<float> output(out_ptrs, 3, kFrames);
    pulp::midi::MidiBuffer midi_in;
    pulp::midi::MidiBuffer midi_out;
    pulp::host::ParameterEventQueue events;

    slot->set_bypass(true);
    REQUIRE(slot->is_bypassed());
    const auto before_bypass = probe.run_count();
    slot->process(output, input, midi_in, midi_out, events, kFrames);
    REQUIRE(probe.run_count() == before_bypass);

    for (int i = 0; i < kFrames; ++i) {
        REQUIRE_THAT(out_l[static_cast<size_t>(i)],
                     WithinAbs(in_l[static_cast<size_t>(i)], 1e-6f));
        REQUIRE_THAT(out_r[static_cast<size_t>(i)],
                     WithinAbs(in_r[static_cast<size_t>(i)], 1e-6f));
        REQUIRE_THAT(out_extra[static_cast<size_t>(i)],
                     WithinAbs(0.0f, 1e-6f));
    }

    std::fill(out_l.begin(), out_l.end(), 8.0f);
    std::fill(out_r.begin(), out_r.end(), 8.0f);
    std::fill(out_extra.begin(), out_extra.end(), 8.0f);

    slot->set_bypass(false);
    slot->release();
    const auto before_released = probe.run_count();
    slot->process(output, input, midi_in, midi_out, events, kFrames);
    REQUIRE(probe.run_count() == before_released);

    for (int i = 0; i < kFrames; ++i) {
        REQUIRE_THAT(out_l[static_cast<size_t>(i)],
                     WithinAbs(in_l[static_cast<size_t>(i)], 1e-6f));
        REQUIRE_THAT(out_r[static_cast<size_t>(i)],
                     WithinAbs(in_r[static_cast<size_t>(i)], 1e-6f));
        REQUIRE_THAT(out_extra[static_cast<size_t>(i)],
                     WithinAbs(0.0f, 1e-6f));
    }
}

#endif
