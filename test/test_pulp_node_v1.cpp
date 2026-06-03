// Conformance tests for the public node ABI (pulp_node_v1.h). Always built (no
// Rust): pins the ABI shape, drives an in-test C gain node through the full
// lifecycle, and exercises the version-negotiation contract. The cross-language
// (C + Rust) load-through-the-same-contract test is the opt-in lane
// (test_pulp_node_v1_rust.cpp).

#include <pulp/native_components/pulp_node_v1.h>
#include <pulp/native_components/pulp_node_v1.hpp>

#include <pulp/runtime/node_abi.hpp>

#include <catch2/catch_test_macros.hpp>

// Version continuity: the public binary node ABI major must equal the runtime
// node-ABI generation (one number across the source + binary surfaces).
static_assert(pulp::native_components::kNodeAbiMajor == pulp::PULP_NODE_ABI_VERSION,
              "pulp_node_v1 major must match pulp::PULP_NODE_ABI_VERSION");

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string_view>
#include <vector>

using namespace pulp::native_components;

namespace {

// ── An in-test C gain node implementing pulp_node_v1: instance holds a level,
// scales mono input -> output, and round-trips the level as 4 bytes of state. ──
struct GainNode {
    float level = 1.0f;
};

const char kId[] = "pulp.test.node.gain";
const char kName[] = "Gain Node";

pulp_node_descriptor_v1 g_desc = [] {
    pulp_node_descriptor_v1 d{};
    d.size = sizeof(d);
    d.abi_major = PULP_NODE_V1_ABI_MAJOR;
    d.stable_id = kId;
    d.stable_id_len = std::strlen(kId);
    d.display_name = kName;
    d.display_name_len = std::strlen(kName);
    d.node_version = 1;
    d.capability_flags = PULP_NODE_CAP_STATE_V1 | PULP_NODE_CAP_RESET_V1;
    d.audio_input_count = 1;
    d.audio_output_count = 1;
    return d;
}();

const pulp_node_descriptor_v1* node_descriptor() { return &g_desc; }

pulp_node_status_v1 node_create(const pulp_node_host_services_v1*,
                                pulp_node_instance_v1** out) {
    *out = reinterpret_cast<pulp_node_instance_v1*>(new GainNode());
    return PULP_NODE_OK_V1;
}
void node_destroy(pulp_node_instance_v1* i) {
    delete reinterpret_cast<GainNode*>(i);
}
pulp_node_status_v1 node_prepare(pulp_node_instance_v1*,
                                 const pulp_node_prepare_v1*) {
    return PULP_NODE_OK_V1;
}
void node_reset(pulp_node_instance_v1* i) {
    reinterpret_cast<GainNode*>(i)->level = 1.0f;
}
pulp_node_status_v1 node_process(pulp_node_instance_v1* i,
                                 const pulp_node_audio_v1* a) {
    const float lvl = reinterpret_cast<GainNode*>(i)->level;
    const uint32_t ch = a->input_count < a->output_count ? a->input_count
                                                         : a->output_count;
    for (uint32_t c = 0; c < ch; ++c) {
        const float* in = a->inputs[c];
        float* out = a->outputs[c];
        for (uint32_t s = 0; s < a->frame_count; ++s) out[s] = in[s] * lvl;
    }
    return PULP_NODE_OK_V1;
}
void node_release(pulp_node_instance_v1*) {}
pulp_node_status_v1 node_save_state(pulp_node_instance_v1* i,
                                    const pulp_node_writer_v1* w) {
    const float lvl = reinterpret_cast<GainNode*>(i)->level;
    uint8_t bytes[sizeof(float)];
    std::memcpy(bytes, &lvl, sizeof(float));
    w->write(w->writer_context, bytes, sizeof(bytes));
    return PULP_NODE_OK_V1;
}
pulp_node_status_v1 node_load_state(pulp_node_instance_v1* i,
                                    const uint8_t* bytes, size_t len) {
    if (len != sizeof(float)) return PULP_NODE_ERR_MALFORMED_STATE_V1;
    std::memcpy(&reinterpret_cast<GainNode*>(i)->level, bytes, sizeof(float));
    return PULP_NODE_OK_V1;
}
uint32_t node_report_latency(pulp_node_instance_v1*) { return 0; }

pulp_node_entry_v1 g_entry = [] {
    pulp_node_entry_v1 e{};
    e.size = sizeof(e);
    e.abi_major = PULP_NODE_V1_ABI_MAJOR;
    e.descriptor = node_descriptor;
    e.create = node_create;
    e.destroy = node_destroy;
    e.prepare = node_prepare;
    e.reset = node_reset;
    e.process = node_process;
    e.release = node_release;
    e.save_state = node_save_state;
    e.load_state = node_load_state;
    e.report_latency = node_report_latency;
    return e;
}();

// A minimal host: alloc/free + a state writer collecting into a vector.
std::vector<uint8_t> g_written;
void test_write(void*, const uint8_t* b, size_t n) {
    g_written.insert(g_written.end(), b, b + n);
}

}  // namespace

TEST_CASE("pulp_node_v1: ABI is POD-shaped with leading size/major",
          "[pulp-node-v1][abi]") {
    REQUIRE(PULP_NODE_V1_ABI_MAJOR == 1u);
    REQUIRE(kNodeAbiMajor == 1u);
    REQUIRE(pulp_node_v1_abi_major() == 1u);
    REQUIRE(offsetof(pulp_node_entry_v1, size) == 0);
    REQUIRE(offsetof(pulp_node_entry_v1, abi_major) == 4);
    REQUIRE(offsetof(pulp_node_descriptor_v1, size) == 0);
    REQUIRE(std::is_standard_layout_v<pulp_node_entry_v1>);
    REQUIRE(std::is_trivially_copyable_v<pulp_node_descriptor_v1>);
    // Status codes: OK is zero, errors distinct.
    REQUIRE(PULP_NODE_OK_V1 == 0);
    REQUIRE(PULP_NODE_ERR_MALFORMED_STATE_V1 != PULP_NODE_ERR_INVALID_STATE_V1);
    // Capability bits are distinct powers of two.
    REQUIRE((PULP_NODE_CAP_STATE_V1 & PULP_NODE_CAP_RESET_V1) == 0);
    REQUIRE(__builtin_popcount(PULP_NODE_CAP_STATE_V1 | PULP_NODE_CAP_RESET_V1 |
                               PULP_NODE_CAP_EVENTS_V1 |
                               PULP_NODE_CAP_LATENCY_V1) == 4);
}

TEST_CASE("pulp_node_v1: version negotiation accepts same-major, rejects others",
          "[pulp-node-v1][abi]") {
    REQUIRE(node_is_compatible(&g_entry));
    REQUIRE_FALSE(node_is_compatible(nullptr));

    // Additive evolution: an older, smaller `size` at the same major is accepted
    // (the host only reads through the fields it knows).
    pulp_node_entry_v1 older = g_entry;
    older.size = offsetof(pulp_node_entry_v1, descriptor);
    REQUIRE(node_is_compatible(&older));

    // A different major is rejected.
    pulp_node_entry_v1 wrong = g_entry;
    wrong.abi_major = PULP_NODE_V1_ABI_MAJOR + 1;
    REQUIRE_FALSE(node_is_compatible(&wrong));
}

TEST_CASE("pulp_node_v1: a C node drives the full lifecycle + state",
          "[pulp-node-v1][lifecycle]") {
    const pulp_node_entry_v1* e = &g_entry;
    REQUIRE(node_is_compatible(e));

    const pulp_node_descriptor_v1* d = e->descriptor();
    REQUIRE(std::string_view(d->stable_id, d->stable_id_len) ==
            "pulp.test.node.gain");
    REQUIRE((d->capability_flags & PULP_NODE_CAP_STATE_V1) != 0);
    REQUIRE(d->audio_input_count == 1);

    pulp_node_host_services_v1 host{};
    host.size = sizeof(host);
    host.abi_major = PULP_NODE_V1_ABI_MAJOR;

    pulp_node_instance_v1* inst = nullptr;
    REQUIRE(e->create(&host, &inst) == PULP_NODE_OK_V1);
    REQUIRE(inst != nullptr);

    pulp_node_prepare_v1 cfg{};
    cfg.size = sizeof(cfg);
    cfg.abi_major = PULP_NODE_V1_ABI_MAJOR;
    cfg.sample_rate = 48000.0;
    cfg.max_block_size = 8;
    REQUIRE(e->prepare(inst, &cfg) == PULP_NODE_OK_V1);

    // Load state (level = 0.5), then process: 1.0 input -> 0.5 output.
    const float level = 0.5f;
    uint8_t blob[sizeof(float)];
    std::memcpy(blob, &level, sizeof(float));
    REQUIRE(e->load_state(inst, blob, sizeof(blob)) == PULP_NODE_OK_V1);

    constexpr uint32_t frames = 8;
    float in[frames];
    float out[frames];
    for (uint32_t s = 0; s < frames; ++s) { in[s] = 1.0f; out[s] = -1.0f; }
    const float* in_ptrs[1] = {in};
    float* out_ptrs[1] = {out};
    pulp_node_audio_v1 audio{};
    audio.size = sizeof(audio);
    audio.abi_major = PULP_NODE_V1_ABI_MAJOR;
    audio.frame_count = frames;
    audio.input_count = 1;
    audio.output_count = 1;
    audio.inputs = in_ptrs;
    audio.outputs = out_ptrs;
    REQUIRE(e->process(inst, &audio) == PULP_NODE_OK_V1);
    for (uint32_t s = 0; s < frames; ++s) REQUIRE(out[s] == 0.5f);

    // Save state through the host writer; it must reflect the loaded level.
    g_written.clear();
    pulp_node_writer_v1 writer{};
    writer.size = sizeof(writer);
    writer.abi_major = PULP_NODE_V1_ABI_MAJOR;
    writer.write = test_write;
    REQUIRE(e->save_state(inst, &writer) == PULP_NODE_OK_V1);
    REQUIRE(g_written.size() == sizeof(float));
    float saved = 0.0f;
    std::memcpy(&saved, g_written.data(), sizeof(float));
    REQUIRE(saved == 0.5f);

    // Malformed state is rejected without unwinding.
    const uint8_t bad[3] = {0, 0, 0};
    REQUIRE(e->load_state(inst, bad, sizeof(bad)) ==
            PULP_NODE_ERR_MALFORMED_STATE_V1);

    e->reset(inst);
    e->release(inst);
    e->destroy(inst);
    SUCCEED("lifecycle completed");
}
