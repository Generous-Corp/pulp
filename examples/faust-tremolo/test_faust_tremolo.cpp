#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include "faust_tremolo.hpp"
#include <cmath>

using namespace pulp;
using namespace pulp::examples;
using Catch::Matchers::WithinAbs;

struct FaustTremoloFixture {
    state::StateStore store;
    std::unique_ptr<format::Processor> processor;

    FaustTremoloFixture() {
        processor = create_faust_tremolo();
        processor->set_state_store(&store);
        processor->define_parameters(store);
        processor->prepare({48000.0, 512, 2, 2});
    }

    void process(audio::Buffer<float>& in, audio::Buffer<float>& out) {
        auto out_view = out.view();
        const float* in_ptrs[2] = {in.channel(0).data(), in.channel(1).data()};
        audio::BufferView<const float> const_in(in_ptrs, 2, in.num_samples());
        midi::MidiBuffer midi_in, midi_out;
        format::ProcessContext ctx;
        ctx.sample_rate = 48000.0;
        ctx.num_samples = static_cast<int>(in.num_samples());
        processor->process(out_view, const_in, midi_in, midi_out, ctx);
    }
};

TEST_CASE("FaustTremolo descriptor", "[faust][tremolo]") {
    FaustTremoloFixture fx;
    auto desc = fx.processor->descriptor();
    REQUIRE(desc.name == "FaustTremolo");
    REQUIRE(desc.category == format::PluginCategory::Effect);
    REQUIRE(desc.default_input_channels() == 2);
    REQUIRE(desc.default_output_channels() == 2);
}

TEST_CASE("FaustTremolo parameter count and names", "[faust][tremolo]") {
    FaustTremoloFixture fx;
    REQUIRE(fx.store.param_count() == 2);

    auto* rate_info = fx.store.info(1);
    REQUIRE(rate_info != nullptr);
    REQUIRE(rate_info->name == "Rate");
    REQUIRE(rate_info->unit == "Hz");

    auto* depth_info = fx.store.info(2);
    REQUIRE(depth_info != nullptr);
    REQUIRE(depth_info->name == "Depth");
}

TEST_CASE("FaustTremolo at zero depth is unity", "[faust][tremolo]") {
    FaustTremoloFixture fx;
    fx.store.set_value(2, 0.0f);  // Depth = 0 → no modulation

    audio::Buffer<float> in(2, 512), out(2, 512);
    for (std::size_t ch = 0; ch < 2; ++ch)
        for (std::size_t i = 0; i < 512; ++i)
            in.channel(ch)[i] = 1.0f;

    fx.process(in, out);

    // With zero depth, output should equal input
    for (std::size_t i = 0; i < 512; ++i)
        REQUIRE_THAT(out.channel(0)[i], WithinAbs(1.0, 0.001));
}

TEST_CASE("FaustTremolo modulates signal", "[faust][tremolo]") {
    FaustTremoloFixture fx;
    fx.store.set_value(1, 4.0f);   // 4 Hz rate
    fx.store.set_value(2, 1.0f);   // Full depth

    audio::Buffer<float> in(2, 48000), out(2, 48000);
    for (std::size_t ch = 0; ch < 2; ++ch)
        for (std::size_t i = 0; i < 48000; ++i)
            in.channel(ch)[i] = 1.0f;

    fx.process(in, out);

    // With full depth at 4 Hz over 1 second, output should vary between 0 and 1
    float min_val = 1.0f, max_val = 0.0f;
    for (std::size_t i = 0; i < 48000; ++i) {
        min_val = std::min(min_val, out.channel(0)[i]);
        max_val = std::max(max_val, out.channel(0)[i]);
    }

    // Full-depth tremolo: min should approach 0, max should approach 1
    REQUIRE(min_val < 0.05f);
    REQUIRE(max_val > 0.95f);
}

TEST_CASE("FaustTremolo state round-trip", "[faust][tremolo]") {
    FaustTremoloFixture fx;
    fx.store.set_value(1, 8.0f);
    fx.store.set_value(2, 0.75f);

    auto data = fx.store.serialize();

    FaustTremoloFixture fx2;
    REQUIRE(fx2.store.deserialize(data));
    REQUIRE_THAT(fx2.store.get_value(1), WithinAbs(8.0, 0.01));
    REQUIRE_THAT(fx2.store.get_value(2), WithinAbs(0.75, 0.01));
}

TEST_CASE("FaustTremolo DSL reflection", "[faust][tremolo]") {
    auto proc = create_faust_tremolo();
    auto* dsl_proc = dynamic_cast<dsl::DslProcessor*>(proc.get());
    REQUIRE(dsl_proc != nullptr);
    REQUIRE(dsl_proc->dsl_name() == "faust");
    REQUIRE(dsl_proc->dsl_params().size() == 2);
    REQUIRE(dsl_proc->bus_layout().num_inputs == 2);
    REQUIRE(dsl_proc->bus_layout().num_outputs == 2);
}
