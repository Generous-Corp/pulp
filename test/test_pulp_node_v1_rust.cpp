// Opt-in cross-language proof (PULP_BUILD_NATIVE_COMPONENT_RUST_TESTS): a Rust
// node implementing pulp_node_v1 loads through the SAME contract the always-built
// C node uses (test_pulp_node_v1.cpp). Proves the public node ABI is genuinely
// language-neutral.

#include <pulp/native_components/pulp_node_v1.h>
#include <pulp/native_components/pulp_node_v1.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <cstring>
#include <string_view>
#include <vector>

using namespace pulp::native_components;

// Exported by the Rust node staticlib (fixtures/native-components/node-rust).
extern "C" const pulp_node_entry_v1* pulp_node_v1_entry(void);

namespace {
std::vector<uint8_t> g_written;
void test_write(void*, const uint8_t* b, size_t n) {
    g_written.insert(g_written.end(), b, b + n);
}
}  // namespace

TEST_CASE("pulp_node_v1: a Rust node loads through the same contract as C",
          "[pulp-node-v1-rust]") {
    const pulp_node_entry_v1* e = pulp_node_v1_entry();
    REQUIRE(node_is_compatible(e));

    const pulp_node_descriptor_v1* d = e->descriptor();
    REQUIRE(std::string_view(d->stable_id, d->stable_id_len) ==
            "pulp.test.node.rust-gain");
    REQUIRE((d->capability_flags & PULP_NODE_CAP_STATE_V1) != 0);
    REQUIRE((d->capability_flags & PULP_NODE_CAP_RESET_V1) != 0);

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
    cfg.max_block_size = 4;
    REQUIRE(e->prepare(inst, &cfg) == PULP_NODE_OK_V1);

    // Load state (level = 0.25) in Rust, then process 1.0 input -> 0.25 output.
    const float level = 0.25f;
    uint8_t blob[sizeof(float)];
    std::memcpy(blob, &level, sizeof(float));
    REQUIRE(e->load_state(inst, blob, sizeof(blob)) == PULP_NODE_OK_V1);

    constexpr uint32_t frames = 4;
    float in[frames] = {1.0f, 1.0f, 1.0f, 1.0f};
    float out[frames] = {-1, -1, -1, -1};
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
    for (uint32_t s = 0; s < frames; ++s) REQUIRE(out[s] == 0.25f);

    // Save state through the host writer — Rust serializes the level.
    g_written.clear();
    pulp_node_writer_v1 writer{};
    writer.size = sizeof(writer);
    writer.abi_major = PULP_NODE_V1_ABI_MAJOR;
    writer.write = test_write;
    REQUIRE(e->save_state(inst, &writer) == PULP_NODE_OK_V1);
    REQUIRE(g_written.size() == sizeof(float));
    float saved = 0.0f;
    std::memcpy(&saved, g_written.data(), sizeof(float));
    REQUIRE(saved == 0.25f);

    // Malformed state rejected without unwinding across extern "C".
    const uint8_t bad[2] = {0, 0};
    REQUIRE(e->load_state(inst, bad, sizeof(bad)) ==
            PULP_NODE_ERR_MALFORMED_STATE_V1);

    e->reset(inst);
    g_written.clear();
    REQUIRE(e->save_state(inst, &writer) == PULP_NODE_OK_V1);
    REQUIRE(g_written.size() == sizeof(float));
    std::memcpy(&saved, g_written.data(), sizeof(float));
    REQUIRE(saved == 1.0f);

    e->release(inst);
    e->destroy(inst);
}
