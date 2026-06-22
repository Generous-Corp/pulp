// Opt-in Rust DSP proof (PULP_BUILD_NATIVE_COMPONENT_RUST_TESTS): a REAL Rust
// DSP core (a stereo gain) driven through the C++ NativeCoreProcessor adapter.
// This joins the Rust <-> C ABI with the adapter <-> Processor seam using real
// Rust audio.

#include <pulp/format/native_core_processor.hpp>

#include <pulp/native_components/native_core.h>

#include <pulp/audio/buffer.hpp>
#include <pulp/midi/buffer.hpp>
#include <pulp/state/parameter_event_queue.hpp>
#include <pulp/state/store.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <cstring>
#include <string_view>
#include <vector>

using namespace pulp;

// Exported by the Rust gain staticlib (fixtures/native-components/gain-rust-core).
extern "C" const pulp_native_core_v1* pulp_native_core_entry_v1(void);

namespace {
struct Block {
    std::vector<float> l, r;
    std::vector<float*> ptrs;
    Block(std::size_t n, float fill) : l(n, fill), r(n, fill) {
        ptrs = {l.data(), r.data()};
    }
};
}  // namespace

TEST_CASE("Rust gain core: descriptor + params through the adapter",
          "[rust-dsp-processor]") {
    format::NativeCoreProcessor proc(pulp_native_core_entry_v1());
    REQUIRE(proc.valid());
    REQUIRE(proc.descriptor().name == "Rust Gain");

    state::StateStore store;
    proc.define_parameters(store);
    REQUIRE(store.all_params().size() == 1);
    REQUIRE(store.all_params()[0].name == "gain");
}

TEST_CASE("Rust gain core: real audio processing through the adapter",
          "[rust-dsp-processor]") {
    format::NativeCoreProcessor proc(pulp_native_core_entry_v1());
    state::StateStore store;
    proc.define_parameters(store);

    format::PrepareContext ctx;
    ctx.sample_rate = 48000.0;
    ctx.max_buffer_size = 64;
    ctx.input_channels = 2;
    ctx.output_channels = 2;
    proc.prepare(ctx);

    constexpr std::size_t frames = 64;
    Block in(frames, 1.0f);
    Block out(frames, 0.0f);
    audio::BufferView<float> out_view(out.ptrs.data(), 2, frames);
    audio::BufferView<const float> in_view(
        const_cast<const float* const*>(in.ptrs.data()), 2, frames);

    // Automate the gain to 0.25.
    state::ParameterEventQueue queue;
    queue.push({/*param_id=*/0, /*offset=*/0, /*value=*/0.25f, /*ramp=*/0});
    proc.set_param_events(&queue);

    midi::MidiBuffer min, mout;
    format::ProcessContext pctx;
    pctx.sample_rate = 48000.0;
    pctx.num_samples = frames;
    proc.process(out_view, in_view, min, mout, pctx);

    // Rust applied gain 0.25 to the 1.0 input across both channels.
    REQUIRE(out.l[0] == 0.25f);
    REQUIRE(out.l[frames - 1] == 0.25f);
    REQUIRE(out.r[0] == 0.25f);
}

TEST_CASE("Rust gain core: non-RT editor_command returns Rust-built JSON",
          "[rust-dsp-processor]") {
    format::NativeCoreProcessor proc(pulp_native_core_entry_v1());
    const auto reply = proc.editor_command(R"({"cmd":"info"})");
    REQUIRE(reply.has_value());
    // The reply JSON is built in Rust off the audio thread.
    REQUIRE(reply->find("\"engine\":\"rust\"") != std::string::npos);
    // Empty request is unsupported -> nullopt, no unwinding across the boundary.
    REQUIRE_FALSE(proc.editor_command("").has_value());
}

TEST_CASE("Rust gain core: opaque state round-trips through the adapter",
          "[rust-dsp-processor]") {
    format::NativeCoreProcessor proc(pulp_native_core_entry_v1());

    float g = 1.5f;
    std::vector<uint8_t> blob(sizeof(float));
    std::memcpy(blob.data(), &g, sizeof(float));
    REQUIRE(proc.deserialize_plugin_state(blob));

    const auto saved = proc.serialize_plugin_state();
    REQUIRE(saved.size() == sizeof(float));
    float restored = 0.0f;
    std::memcpy(&restored, saved.data(), sizeof(float));
    REQUIRE(restored == 1.5f);

    std::vector<uint8_t> bad(2, 0);
    REQUIRE_FALSE(proc.deserialize_plugin_state(bad));
}
