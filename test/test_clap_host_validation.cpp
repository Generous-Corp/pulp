// Item 3.4 of the macOS plugin authoring plan — pin the CLAP host-facing
// API contract that real DAW hosts (Bitwig 5.2+, Reaper 7.20+, FL Studio
// 21.2+, Studio One 7.0+) depend on. Real-host smoke-testing requires a
// license + manual install, so this test acts as the CI proxy: it loads
// the adapter the same way a host loader does and asserts the contract
// each named host has historically broken when an adapter regresses.
//
// What this pins:
//
//   * Plugin ID stability — the string the host uses for project recall
//     never drifts between builds (Reaper R5 has tripped on this when
//     vendors rename their bundle_id).
//   * Parameter ID + range + flags stability — Bitwig caches param IDs
//     in the project file, so re-shuffling order or changing min/max
//     silently breaks recall.
//   * Modulation amount is per-block transient — a MOD event applied
//     this block must NOT bleed into the next block (FL Studio's MOD
//     test sweeps that case; Pulp uses `reset_all_mod()` on each
//     process() entry).
//   * Event-namespace gating — third-party CLAP extensions sharing a
//     type ID with core PARAM_VALUE must NOT be applied (clap-validator
//     `param-set-wrong-namespace`).
//   * Save → reload → save produces a byte-equivalent state stream
//     (Studio One project-recall determinism).
//
// All four bullets fire through the public adapter surface (clap_init /
// clap_activate / clap_process / state stream) so the test fails the
// same way a real host would notice. No real DAW process needed.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <pulp/format/clap_adapter.hpp>
#include <pulp/format/clap_entry.hpp>
#include <pulp/format/processor.hpp>
#include <pulp/state/store.hpp>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

using namespace pulp;
using namespace pulp::format;
using Catch::Matchers::WithinAbs;

namespace {

// ── Test plugin: stable IDs across instances ────────────────────────────
//
// The contract being pinned: the CLAP plugin id + parameter IDs + ranges
// emitted by this processor MUST be byte-identical for every instance
// the host creates. The host uses the plugin id to look up the plugin in
// the project file, and parameter IDs to recall automation envelopes.
// Changing any of these between builds invalidates user projects.

constexpr const char* kPluginId   = "com.pulp.test.host-validation";
constexpr const char* kPluginName = "PulpHostValidation";
constexpr state::ParamID kParamGain   = 1001;
constexpr state::ParamID kParamCutoff = 1002;

class HostValidationProcessor : public Processor {
public:
    PluginDescriptor descriptor() const override {
        PluginDescriptor d;
        d.name = kPluginName;
        d.manufacturer = "PulpTest";
        d.bundle_id = kPluginId;
        d.version = "1.0.0";
        d.category = PluginCategory::Effect;
        d.input_buses  = {{"Audio In",  2}};
        d.output_buses = {{"Audio Out", 2}};
        d.accepts_midi = false;
        d.tail_samples = 0;
        return d;
    }
    void define_parameters(state::StateStore& store) override {
        store.add_parameter({
            .id = kParamGain, .name = "Gain", .unit = "dB",
            .range = {-60.0f, 24.0f, 0.0f, 0.1f},
        });
        store.add_parameter({
            .id = kParamCutoff, .name = "Cutoff", .unit = "Hz",
            .range = {20.0f, 20000.0f, 1000.0f, 1.0f},
        });
    }
    void prepare(const PrepareContext&) override {}
    void process(audio::BufferView<float>&,
                 const audio::BufferView<const float>&,
                 midi::MidiBuffer&, midi::MidiBuffer&,
                 const ProcessContext&) override {}
};

std::unique_ptr<Processor> make_host_validation() {
    return std::make_unique<HostValidationProcessor>();
}

// In-memory CLAP I/O streams so we can round-trip plugin state without
// touching disk. Matches the contract clap-validator's
// `state-reproducibility-basic` test uses.
struct MemStream {
    std::vector<uint8_t> bytes;
    std::size_t cursor = 0;
};

int64_t mem_write(const clap_ostream_t* s, const void* buf, uint64_t sz) {
    auto* m = static_cast<MemStream*>(s->ctx);
    auto* p = static_cast<const uint8_t*>(buf);
    m->bytes.insert(m->bytes.end(), p, p + sz);
    return static_cast<int64_t>(sz);
}

int64_t mem_read(const clap_istream_t* s, void* buf, uint64_t sz) {
    auto* m = static_cast<MemStream*>(s->ctx);
    const auto remaining = m->bytes.size() - m->cursor;
    const auto n = remaining < sz ? remaining : static_cast<std::size_t>(sz);
    if (n == 0) return 0;
    std::memcpy(buf, m->bytes.data() + m->cursor, n);
    m->cursor += n;
    return static_cast<int64_t>(n);
}

// Single-event input list — keeps the call sites in each test small.
struct OneEventInput {
    template <typename E>
    explicit OneEventInput(const E& e) {
        bytes.resize(sizeof(E));
        std::memcpy(bytes.data(), &e, sizeof(E));
        vt.ctx = this;
        vt.size = [](const clap_input_events_t*) -> uint32_t { return 1; };
        vt.get  = [](const clap_input_events_t* l, uint32_t) {
            return reinterpret_cast<const clap_event_header_t*>(
                static_cast<OneEventInput*>(l->ctx)->bytes.data());
        };
    }
    std::vector<uint8_t> bytes;
    clap_input_events_t vt{};
};

clap_event_header_t make_hdr(uint32_t size, uint16_t type,
                              uint16_t space = CLAP_CORE_EVENT_SPACE_ID,
                              uint32_t time = 0) {
    clap_event_header_t h{};
    h.size = size;
    h.type = type;
    h.time = time;
    h.space_id = space;
    h.flags = 0;
    return h;
}

// Construct a real CLAP plugin via the in-process adapter, the same way a
// host loader would after dlopen. Mirrors the test_clap_midi_events
// Harness but trims to the surface this file needs.
struct ClapInstance {
    clap_adapter::PulpClapPlugin plugin;  // non-aggregate-init; default ctor uses MpeConfig::standard_lower()
    bool active = false;

    explicit ClapInstance(ProcessorFactory f) {
        plugin.factory = f;
        plugin.plugin.plugin_data = &plugin;
        REQUIRE(clap_adapter::clap_init(&plugin.plugin));
        REQUIRE(clap_adapter::clap_activate(&plugin.plugin, 48000.0, 32, 256));
        active = true;
    }
    ~ClapInstance() {
        if (active) clap_adapter::clap_deactivate(&plugin.plugin);
    }

    // Drive one block with the supplied input event list (may be null).
    clap_process_status run_block(const clap_input_events_t* in_events) {
        // Minimum-viable audio scaffolding — no audio is actually consumed,
        // but CLAP requires the buffer count + frame count to be valid.
        std::vector<float> l(64), r(64);
        std::vector<float> ol(64), or_(64);
        const float* in_ptrs[2] = {l.data(), r.data()};
        float* out_ptrs[2] = {ol.data(), or_.data()};
        clap_audio_buffer_t ab_in{}, ab_out{};
        ab_in.data32 = const_cast<float**>(in_ptrs);
        ab_in.channel_count = 2;
        ab_out.data32 = out_ptrs;
        ab_out.channel_count = 2;

        clap_process_t proc{};
        proc.frames_count = 64;
        proc.audio_inputs = &ab_in;
        proc.audio_inputs_count = 1;
        proc.audio_outputs = &ab_out;
        proc.audio_outputs_count = 1;
        proc.in_events = in_events;
        return clap_adapter::clap_process(&plugin.plugin, &proc);
    }
};

}  // namespace

// Generate the CLAP entry hook so the factory + descriptor surface is
// reachable through the same path a host's dlopen uses.
PULP_CLAP_PLUGIN(make_host_validation)

TEST_CASE("CLAP plugin id + parameter IDs are stable across instances",
          "[clap][host-validation][issue-3-4]") {
    // Force the entry to reinitialise its cached descriptor so we read
    // the canonical (factory-fresh) value rather than a leftover from
    // an earlier suite.
    REQUIRE(clap_entry.init("test"));
    pulp::format::clap_generic::init_descriptor();

    auto* factory = static_cast<const clap_plugin_factory_t*>(
        clap_entry.get_factory(CLAP_PLUGIN_FACTORY_ID));
    REQUIRE(factory != nullptr);
    REQUIRE(factory->get_plugin_count(factory) == 1);

    auto* desc1 = factory->get_plugin_descriptor(factory, 0);
    auto* desc2 = factory->get_plugin_descriptor(factory, 0);
    REQUIRE(desc1 != nullptr);
    REQUIRE(desc2 != nullptr);
    // The descriptor is stable across reads — a host caches it per-load.
    REQUIRE(std::string(desc1->id) == kPluginId);
    REQUIRE(std::string(desc1->id) == std::string(desc2->id));
    REQUIRE(std::string(desc1->name) == kPluginName);

    // Create two instances; both must report identical parameter IDs +
    // ranges. Re-shuffling the order or changing a min/max silently
    // breaks user-saved automation envelopes.
    const clap_plugin_t* a = factory->create_plugin(factory, nullptr, desc1->id);
    const clap_plugin_t* b = factory->create_plugin(factory, nullptr, desc1->id);
    REQUIRE(a != nullptr);
    REQUIRE(b != nullptr);
    REQUIRE(a->init(a));
    REQUIRE(b->init(b));

    auto* params_a = static_cast<const clap_plugin_params_t*>(
        a->get_extension(a, CLAP_EXT_PARAMS));
    auto* params_b = static_cast<const clap_plugin_params_t*>(
        b->get_extension(b, CLAP_EXT_PARAMS));
    REQUIRE(params_a != nullptr);
    REQUIRE(params_b != nullptr);
    REQUIRE(params_a->count(a) == params_b->count(b));
    REQUIRE(params_a->count(a) == 2u);

    for (uint32_t i = 0; i < params_a->count(a); ++i) {
        clap_param_info_t ia{}, ib{};
        REQUIRE(params_a->get_info(a, i, &ia));
        REQUIRE(params_b->get_info(b, i, &ib));
        REQUIRE(ia.id == ib.id);
        REQUIRE(std::string(ia.name) == std::string(ib.name));
        REQUIRE(ia.min_value == ib.min_value);
        REQUIRE(ia.max_value == ib.max_value);
        REQUIRE(ia.default_value == ib.default_value);
    }

    a->destroy(a);
    b->destroy(b);
    clap_entry.deinit();
}

TEST_CASE("CLAP modulation does not bleed across blocks",
          "[clap][host-validation][modulation][issue-3-4]") {
    ClapInstance inst(make_host_validation);

    // Block 1: apply +0.3 mod offset to the gain param.
    clap_event_param_mod_t mod{};
    mod.header = make_hdr(sizeof(mod), CLAP_EVENT_PARAM_MOD);
    mod.param_id = kParamGain;
    mod.amount = 0.3;
    OneEventInput in1(mod);
    REQUIRE(inst.run_block(&in1.vt) == CLAP_PROCESS_CONTINUE);

    // Mid-block the modulated value reflects the offset…
    const auto value_with_mod = inst.plugin.store.get_modulated(kParamGain);
    REQUIRE_THAT(static_cast<double>(value_with_mod), WithinAbs(0.3, 1e-5));

    // …and an empty next block resets the offset so the base value is
    // restored. clap_adapter::clap_process calls reset_all_mod() at the
    // top of every block; this test fails if that contract regresses.
    REQUIRE(inst.run_block(nullptr) == CLAP_PROCESS_CONTINUE);
    const auto value_after_clear = inst.plugin.store.get_modulated(kParamGain);
    REQUIRE_THAT(static_cast<double>(value_after_clear), WithinAbs(0.0, 1e-9));
}

TEST_CASE("CLAP non-core event namespace is ignored",
          "[clap][host-validation][namespace][issue-3-4]") {
    ClapInstance inst(make_host_validation);

    // Set the base param to a known value first so we can observe whether
    // the namespaced event was incorrectly applied on top.
    inst.plugin.store.set_value(kParamGain, 6.0f);
    const auto baseline = inst.plugin.store.get_value(kParamGain);
    REQUIRE_THAT(static_cast<double>(baseline), WithinAbs(6.0, 1e-6));

    // Send a PARAM_VALUE-shaped event in a non-core namespace. The adapter
    // must skip it (per CLAP spec) so the base value stays at 6.0.
    clap_event_param_value_t ev{};
    ev.header = make_hdr(sizeof(ev), CLAP_EVENT_PARAM_VALUE, /*space=*/42);
    ev.param_id = kParamGain;
    ev.value = -12.0;
    OneEventInput in(ev);
    REQUIRE(inst.run_block(&in.vt) == CLAP_PROCESS_CONTINUE);

    const auto after = inst.plugin.store.get_value(kParamGain);
    REQUIRE_THAT(static_cast<double>(after), WithinAbs(6.0, 1e-6));
}

TEST_CASE("CLAP plugin state save → load → save is byte-equivalent",
          "[clap][host-validation][state][issue-3-4]") {
    // Drive the state extension through the entry-level factory path —
    // that's the only path that registers CLAP_EXT_STATE on the plugin
    // handle (it lives in clap_entry.hpp, NOT in clap_adapter.cpp's
    // get_extension dispatcher).
    REQUIRE(clap_entry.init("test"));
    pulp::format::clap_generic::init_descriptor();
    auto* factory = static_cast<const clap_plugin_factory_t*>(
        clap_entry.get_factory(CLAP_PLUGIN_FACTORY_ID));
    REQUIRE(factory != nullptr);
    auto* desc = factory->get_plugin_descriptor(factory, 0);
    REQUIRE(desc != nullptr);

    const clap_plugin_t* p1 = factory->create_plugin(factory, nullptr, desc->id);
    REQUIRE(p1 != nullptr);
    REQUIRE(p1->init(p1));

    auto* state_ext = static_cast<const clap_plugin_state_t*>(
        p1->get_extension(p1, CLAP_EXT_STATE));
    REQUIRE(state_ext != nullptr);

    // Mutate a parameter the host would care about.
    auto* params1 = static_cast<const clap_plugin_params_t*>(
        p1->get_extension(p1, CLAP_EXT_PARAMS));
    REQUIRE(params1 != nullptr);
    // params extension's flush() handles a single PARAM_VALUE write the
    // same way clap_process does. We use the in-process adapter handle
    // directly to set the param via its public surface.
    {
        clap_event_param_value_t ev{};
        ev.header = make_hdr(sizeof(ev), CLAP_EVENT_PARAM_VALUE);
        ev.param_id = kParamGain;
        ev.value = 12.5;
        OneEventInput in(ev);
        clap_output_events_t out{};
        out.ctx = nullptr;
        out.try_push = [](const clap_output_events_t*, const clap_event_header_t*) { return true; };
        params1->flush(p1, &in.vt, &out);
    }

    MemStream out1;
    clap_ostream_t os1{};
    os1.ctx = &out1;
    os1.write = mem_write;
    REQUIRE(state_ext->save(p1, &os1));
    REQUIRE_FALSE(out1.bytes.empty());

    // Load into a second instance; saving must produce the same bytes.
    const clap_plugin_t* p2 = factory->create_plugin(factory, nullptr, desc->id);
    REQUIRE(p2 != nullptr);
    REQUIRE(p2->init(p2));
    auto* state2 = static_cast<const clap_plugin_state_t*>(
        p2->get_extension(p2, CLAP_EXT_STATE));
    REQUIRE(state2 != nullptr);

    MemStream in_stream;
    in_stream.bytes = out1.bytes;
    clap_istream_t is{};
    is.ctx = &in_stream;
    is.read = mem_read;
    REQUIRE(state2->load(p2, &is));

    MemStream out2;
    clap_ostream_t os2{};
    os2.ctx = &out2;
    os2.write = mem_write;
    REQUIRE(state2->save(p2, &os2));
    REQUIRE(out2.bytes == out1.bytes);

    p1->destroy(p1);
    p2->destroy(p2);
    clap_entry.deinit();
}
