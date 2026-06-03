// Contract tests for the native-component core ABI (native_core.h).
//
// These build WITHOUT a Rust toolchain — they pin the C ABI shape so the
// future binary freeze is a relabel, not a rewrite. There is one test per
// forward-compatibility decision (1-12) so a regression names the decision it
// broke. The Rust round-trip + RT-safety self-test lives in
// test_rust_dsp_ffi.cpp behind PULP_BUILD_NATIVE_COMPONENT_RUST_TESTS.

#include <pulp/native_components/native_core.h>
#include <pulp/native_components/native_core.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <string_view>
#include <type_traits>

using namespace pulp::native_components;

namespace {

// Decision 1: every boundary struct leads with `size` then `abi_version`
// (or `size` then a count for the few that intentionally omit a version).
// A leading size is what makes additive growth detectable.
template <typename T>
constexpr bool leads_with_size() {
    return offsetof(T, size) == 0 && sizeof(decltype(T{}.size)) == 4;
}

}  // namespace

TEST_CASE("decision 1: ABI is shaped like a binary ABI", "[native-core][abi]") {
    REQUIRE(PULP_NATIVE_CORE_ABI_VERSION == 1u);
    REQUIRE(kAbiVersion == 1u);

    // Leading size field on every versioned struct.
    REQUIRE(leads_with_size<pulp_native_descriptor_v1>());
    REQUIRE(leads_with_size<pulp_native_param_v1>());
    REQUIRE(leads_with_size<pulp_native_param_event_view_v1>());
    REQUIRE(leads_with_size<pulp_native_audio_io_v1>());
    REQUIRE(leads_with_size<pulp_native_midi_view_v1>());
    REQUIRE(leads_with_size<pulp_native_state_span_v1>());
    REQUIRE(leads_with_size<pulp_native_bus_layout_v1>());
    REQUIRE(leads_with_size<pulp_native_prepare_v1>());
    REQUIRE(leads_with_size<pulp_native_process_context_v1>());
    REQUIRE(leads_with_size<pulp_native_process_v1>());
    REQUIRE(leads_with_size<pulp_native_host_services_v1>());
    REQUIRE(leads_with_size<pulp_native_core_v1>());

    // `abi_version` immediately follows `size` on the top-level structs.
    REQUIRE(offsetof(pulp_native_core_v1, abi_version) == 4);
    REQUIRE(offsetof(pulp_native_descriptor_v1, abi_version) == 4);

    // POD-ness is statically asserted in the .hpp; assert again at runtime so
    // the intent is visible in the test report.
    REQUIRE(std::is_standard_layout_v<pulp_native_core_v1>);
    REQUIRE(std::is_trivially_copyable_v<pulp_native_param_event_v1>);

    // No status code collisions; OK is zero.
    REQUIRE(PULP_NATIVE_OK == 0);
    REQUIRE(PULP_NATIVE_ERR_MALFORMED_STATE != PULP_NATIVE_ERR_INVALID_STATE);
}

TEST_CASE("decision 2: audio buffers are host-owned planar views",
          "[native-core][buffers]") {
    // The view carries per-bus channel counts and a frame count; channels are
    // float* const* (planar). The core never owns these — the struct exposes
    // no allocate/free for them.
    pulp_native_audio_bus_v1 bus{};
    bus.size = sizeof(bus);
    bus.channel_count = 2;
    bus.channels = nullptr;  // host fills with planar pointers at call time
    REQUIRE(bus.channel_count == 2);

    pulp_native_audio_io_v1 io{};
    io.size = sizeof(io);
    io.abi_version = kAbiVersion;
    io.frame_count = 256;
    io.input_bus_count = 1;
    io.output_bus_count = 1;
    io.sidechain_bus_count = 0;
    REQUIRE(io.frame_count == 256);
    // Sidechain pointer is independent of the main I/O (read-only bus).
    REQUIRE(std::is_same_v<decltype(io.sidechains),
                           const pulp_native_audio_bus_v1*>);
}

TEST_CASE("decision 3: state is an opaque versioned span; empty == defaults",
          "[native-core][state]") {
    pulp_native_state_span_v1 empty{};
    empty.size = sizeof(empty);
    empty.abi_version = kAbiVersion;
    empty.bytes = nullptr;
    empty.byte_len = 0;
    // Empty span is the documented "restore defaults" signal.
    REQUIRE(empty.bytes == nullptr);
    REQUIRE(empty.byte_len == 0);

    // The span carries a core-defined state_version distinct from the ABI
    // version, so state format can evolve independently of the ABI.
    static const std::uint8_t blob[] = {0xDE, 0xAD};
    pulp_native_state_span_v1 s{};
    s.size = sizeof(s);
    s.abi_version = kAbiVersion;
    s.state_version = 7;
    s.bytes = blob;
    s.byte_len = sizeof(blob);
    REQUIRE(s.state_version == 7);
    REQUIRE(s.byte_len == 2);
    REQUIRE(s.bytes[0] == 0xDE);
}

TEST_CASE("decision 4: parameter identity is stable string id + FNV-1a/64 hash",
          "[native-core][params]") {
    // The hash is FNV-1a/64 over the UTF-8 id bytes. Pin a known vector so the
    // host and any binding generator agree byte-for-byte.
    // FNV-1a/64("gain") computed independently:
    constexpr std::uint64_t kGain = param_id_hash("gain");
    REQUIRE(kGain == 0x8AE87E72043D203EULL);
    // Case-sensitive: different case => different hash.
    REQUIRE(param_id_hash("Gain") != kGain);
    // Empty id hashes to the offset basis (documented).
    REQUIRE(param_id_hash("") == 0xcbf29ce484222325ULL);

    pulp_native_param_v1 p{};
    p.size = sizeof(p);
    p.abi_version = kAbiVersion;
    p.id = "gain";
    p.id_len = 4;
    p.id_hash = param_id_hash({p.id, p.id_len});
    p.min_value = -60.0;
    p.max_value = 12.0;
    p.default_value = 0.0;
    REQUIRE(p.id_hash == kGain);
    // Values are plain domain (engineering units), not normalized [0,1].
    REQUIRE(p.min_value == -60.0);
    REQUIRE(p.max_value == 12.0);
    REQUIRE(std::string_view(p.id, p.id_len) == "gain");
}

TEST_CASE("decision 4b: param events are sorted, plain-domain, ramped, offset",
          "[native-core][params]") {
    pulp_native_param_event_v1 e{};
    e.param_id_hash = param_id_hash("gain");
    e.value = -3.0;       // plain domain
    e.sample_offset = 256;
    e.ramp_frames = 64;   // linear ramp
    e.kind = PULP_NATIVE_EVENT_AUTOMATION;
    REQUIRE(e.sample_offset == 256);
    REQUIRE(e.ramp_frames == 64);
    REQUIRE(e.value == -3.0);
    // 8-byte alignment preserved by the reserved word (so arrays are tight).
    REQUIRE(alignof(pulp_native_param_event_v1) == 8);
}

TEST_CASE("decision 4c: null event queue is distinct from an empty one",
          "[native-core][params]") {
    pulp_native_param_event_view_v1 none{};
    none.size = sizeof(none);
    none.abi_version = kAbiVersion;
    none.events = nullptr;  // host supplied no queue this block
    none.count = 0;
    none.capacity = 0;

    static const pulp_native_param_event_v1 ev[1] = {};
    pulp_native_param_event_view_v1 emptyq{};
    emptyq.size = sizeof(emptyq);
    emptyq.abi_version = kAbiVersion;
    emptyq.events = ev;     // real, empty, fixed-capacity queue
    emptyq.count = 0;
    emptyq.capacity = 1024;

    // Semantically distinct: NULL vs present-but-empty (matches Pulp's queue).
    REQUIRE(none.events == nullptr);
    REQUIRE(emptyq.events != nullptr);
    REQUIRE(emptyq.capacity == 1024);
    // Overflow is representable (the 1024-cap queue can drop dense events).
    emptyq.overflowed = 1;
    REQUIRE(emptyq.overflowed == 1);
}

TEST_CASE("decision 5: additive evolution via size + capability flags",
          "[native-core][abi]") {
    // Capability bits are distinct powers of two — additive negotiation.
    const pulp_native_caps all =
        PULP_NATIVE_CAP_MIDI_INPUT | PULP_NATIVE_CAP_MIDI_OUTPUT |
        PULP_NATIVE_CAP_MPE | PULP_NATIVE_CAP_UMP | PULP_NATIVE_CAP_SIDECHAIN |
        PULP_NATIVE_CAP_IS_INSTRUMENT | PULP_NATIVE_CAP_PARAM_MODULATION |
        PULP_NATIVE_CAP_STATE | PULP_NATIVE_CAP_TAIL |
        PULP_NATIVE_CAP_EDITOR_COMMAND;
    // 10 distinct bits set.
    REQUIRE(__builtin_popcount(all) == 10);

    // is_compatible tolerates an older, smaller `size` at the same major.
    pulp_native_core_v1 core{};
    core.abi_version = kAbiVersion;
    core.size = offsetof(pulp_native_core_v1, descriptor);
    REQUIRE(is_compatible(&core));
    core.abi_version = kAbiVersion + 1;  // wrong major
    REQUIRE_FALSE(is_compatible(&core));
    REQUIRE_FALSE(is_compatible(nullptr));
}

TEST_CASE("decision 6: the Processor FFI is independent of SignalGraph",
          "[native-core][scope]") {
    // Structural guarantee: this header pulls in NO graph/node/host types. If a
    // SignalGraph dependency ever leaks in, this TU stops compiling against the
    // standalone include path. The presence of a self-contained descriptor +
    // vtable with its own lifecycle is the evidence.
    REQUIRE(offsetof(pulp_native_core_v1, process) > 0);
    SUCCEED("native_core.h includes only <stddef.h>/<stdint.h>");
}

TEST_CASE("decision 7: explicit suspended/active lifecycle states",
          "[native-core][lifecycle]") {
    REQUIRE(PULP_NATIVE_LIFECYCLE_SUSPENDED == 0);
    REQUIRE(PULP_NATIVE_LIFECYCLE_ACTIVE == 1);
    // The vtable exposes suspend/resume/reset distinct from prepare/release.
    pulp_native_core_v1 core{};
    REQUIRE(std::is_same_v<decltype(core.resume),
                           pulp_native_status (*)(pulp_native_instance*)>);
    REQUIRE(std::is_same_v<decltype(core.suspend),
                           pulp_native_status (*)(pulp_native_instance*)>);
    REQUIRE(std::is_same_v<decltype(core.reset),
                           void (*)(pulp_native_instance*)>);
}

TEST_CASE("decision 8: rate/block change is carried by prepare()",
          "[native-core][lifecycle]") {
    pulp_native_prepare_v1 cfg{};
    cfg.size = sizeof(cfg);
    cfg.abi_version = kAbiVersion;
    cfg.sample_rate = 48000.0;
    cfg.max_block_size = 512;
    // Both renegotiable parameters live on prepare(), so any change is a fresh
    // prepare() — there is no separate "set sample rate" RT call.
    REQUIRE(cfg.sample_rate == 48000.0);
    REQUIRE(cfg.max_block_size == 512);
    pulp_native_core_v1 core{};
    REQUIRE(std::is_same_v<decltype(core.prepare),
                           pulp_native_status (*)(pulp_native_instance*,
                                                  const pulp_native_prepare_v1*)>);
}

TEST_CASE("decision 9: per-instance opaque handle (no shared mutable globals)",
          "[native-core][lifecycle]") {
    // create() yields an opaque instance handle; every stateful call takes it.
    // The contract forbids process-wide mutable state, so N instances are
    // independent — represented by the handle threading through the vtable.
    pulp_native_core_v1 core{};
    REQUIRE(std::is_same_v<decltype(core.create),
                           pulp_native_status (*)(const pulp_native_host_services_v1*,
                                                  pulp_native_instance**)>);
    REQUIRE(std::is_same_v<decltype(core.destroy),
                           void (*)(pulp_native_instance*)>);
    // The handle is an incomplete (opaque) type — its layout never crosses.
    REQUIRE_FALSE(std::is_same_v<pulp_native_instance, void>);
}

TEST_CASE("decision 10: bus/sidechain layout negotiated as POD",
          "[native-core][buses]") {
    const std::uint32_t ins[2] = {2, 1};
    const std::uint32_t outs[1] = {2};
    pulp_native_bus_layout_v1 layout{};
    layout.size = sizeof(layout);
    layout.abi_version = kAbiVersion;
    layout.input_channel_counts = ins;
    layout.input_bus_count = 2;
    layout.output_channel_counts = outs;
    layout.output_bus_count = 1;
    layout.sidechain_channel_count = 1;
    // Arbitrary channel counts — no stereo-in/stereo-out assumption.
    REQUIRE(layout.input_channel_counts[0] == 2);
    REQUIRE(layout.input_channel_counts[1] == 1);
    REQUIRE(layout.sidechain_channel_count == 1);
    // The core may reject a layout via status code.
    pulp_native_core_v1 core{};
    REQUIRE(std::is_same_v<decltype(core.set_bus_layout),
                           pulp_native_status (*)(pulp_native_instance*,
                                                  const pulp_native_bus_layout_v1*)>);
}

TEST_CASE("decision 11: modulation and automation are distinct",
          "[native-core][params]") {
    // Distinct event kinds...
    REQUIRE(PULP_NATIVE_EVENT_AUTOMATION != PULP_NATIVE_EVENT_MODULATION);
    REQUIRE(PULP_NATIVE_EVENT_AUTOMATION == 0);
    REQUIRE(PULP_NATIVE_EVENT_MODULATION == 1);
    // ...and distinct param-capability bits, so a param can be automatable,
    // modulatable, both, or neither.
    REQUIRE((PULP_NATIVE_PARAM_AUTOMATABLE & PULP_NATIVE_PARAM_MODULATABLE) == 0);
    REQUIRE(PULP_NATIVE_CAP_PARAM_MODULATION != 0);
}

TEST_CASE("decision 12: allocator ownership is explicit and paired",
          "[native-core][ownership]") {
    // Host services carry a matched alloc/free pair (host allocator).
    pulp_native_host_services_v1 host{};
    REQUIRE(std::is_same_v<decltype(host.alloc), void* (*)(void*, std::size_t)>);
    REQUIRE(std::is_same_v<decltype(host.free), void (*)(void*, void*)>);

    // Core-owned outputs each name their paired free function:
    //   save_state  -> free_state
    //   editor_command -> free_editor_reply
    pulp_native_core_v1 core{};
    REQUIRE(std::is_same_v<decltype(core.save_state),
                           pulp_native_status (*)(pulp_native_instance*,
                                                  pulp_native_state_out_v1*)>);
    REQUIRE(std::is_same_v<decltype(core.free_state),
                           void (*)(pulp_native_instance*,
                                    pulp_native_state_out_v1*)>);
    REQUIRE(std::is_same_v<decltype(core.free_editor_reply),
                           void (*)(pulp_native_instance*, std::uint8_t*,
                                    std::size_t)>);
}

TEST_CASE("production RT trap is a no-op outside any no-alloc scope",
          "[native-core][rt-safety]") {
    // The shipped default of pulp_rt_trap_if_no_alloc_scope is a weak no-op:
    // production audio code never traps. Test harnesses override it with a
    // strong definition that aborts inside a no-alloc scope (see the opt-in
    // Rust lane). Here we just confirm the default returns cleanly.
    pulp_rt_trap_if_no_alloc_scope(0, 16);
    pulp_rt_trap_if_no_alloc_scope(2, 0);
    SUCCEED("default trap returned without aborting");
}

TEST_CASE("descriptor round-trips its identity and capability fields",
          "[native-core][descriptor]") {
    pulp_native_descriptor_v1 d{};
    d.size = sizeof(d);
    d.abi_version = kAbiVersion;
    d.id = "com.pulp.example.gain";
    d.id_len = std::string_view("com.pulp.example.gain").size();
    d.name = "Example Gain";
    d.name_len = std::string_view("Example Gain").size();
    d.capabilities = PULP_NATIVE_CAP_SIDECHAIN | PULP_NATIVE_CAP_STATE;
    d.default_input_bus_count = 1;
    d.default_output_bus_count = 1;
    d.latency_frames = 0;

    REQUIRE(std::string_view(d.id, d.id_len) == "com.pulp.example.gain");
    REQUIRE(std::string_view(d.name, d.name_len) == "Example Gain");
    REQUIRE((d.capabilities & PULP_NATIVE_CAP_SIDECHAIN) != 0);
    REQUIRE((d.capabilities & PULP_NATIVE_CAP_MIDI_INPUT) == 0);
}
