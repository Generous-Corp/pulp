// Tests for bus-layout validation, the processBlock precision contract,
// and the cross-adapter RT-safe latency / tail change notification
// pattern.
//
// These tests exercise the Processor-side API surface only. Adapter-
// specific wiring (VST3 setBusArrangements, AU PropertyChanged, CLAP
// host_latency->changed()) is covered separately in
// test_vst3_plugin_state.cpp and test_au_plugin_state.mm; here we pin
// the contract those adapters depend on so refactors can't silently
// break the bridge.

#include <catch2/catch_test_macros.hpp>
#include <pulp/format/processor.hpp>
#include <pulp/format/process_block.hpp>
#include <pulp/audio/buffer.hpp>
#include <pulp/midi/buffer.hpp>

#include <atomic>
#include <algorithm>
#include <array>
#include <thread>
#include <type_traits>

using namespace pulp::format;

namespace {

// Minimal Processor concrete: descriptor with one stereo main bus.
class StereoEffect : public Processor {
public:
    PluginDescriptor descriptor() const override {
        PluginDescriptor d;
        d.name = "StereoEffect";
        d.input_buses  = {{"Main In",  2, false}};
        d.output_buses = {{"Main Out", 2, false}};
        return d;
    }
    void define_parameters(pulp::state::StateStore&) override {}
    void prepare(const PrepareContext&) override {}
    void process(pulp::audio::BufferView<float>&,
                 const pulp::audio::BufferView<const float>&,
                 pulp::midi::MidiBuffer&,
                 pulp::midi::MidiBuffer&,
                 const ProcessContext&) override {}
};

class CopyEffect : public StereoEffect {
public:
    void process(pulp::audio::BufferView<float>& output,
                 const pulp::audio::BufferView<const float>& input,
                 pulp::midi::MidiBuffer&,
                 pulp::midi::MidiBuffer&,
                 const ProcessContext&) override {
        const auto channels = std::min(output.num_channels(), input.num_channels());
        for (std::size_t ch = 0; ch < channels; ++ch) {
            auto dst = output.channel(ch);
            const auto src = input.channel(ch);
            const auto frames = std::min(dst.size(), src.size());
            for (std::size_t i = 0; i < frames; ++i) dst[i] = src[i];
        }
    }
};

class FrameCountingCopyEffect : public CopyEffect {
public:
    void process(pulp::audio::BufferView<float>& output,
                 const pulp::audio::BufferView<const float>& input,
                 pulp::midi::MidiBuffer& midi_in,
                 pulp::midi::MidiBuffer& midi_out,
                 const ProcessContext& context) override {
        observed_frames = output.num_samples();
        CopyEffect::process(output, input, midi_in, midi_out, context);
    }

    std::size_t observed_frames = 0;
};

// Effect with a sidechain bus: main + sidechain in, main out.
class SidechainEffect : public StereoEffect {
public:
    PluginDescriptor descriptor() const override {
        PluginDescriptor d = StereoEffect::descriptor();
        d.name = "SidechainEffect";
        d.input_buses  = {{"Main In", 2, false}, {"Sidechain", 2, true}};
        return d;
    }
};

// Override that requires sidechain channels == main channels (the
// classic "linked sidechain" contract).
class LinkedSidechainEffect : public SidechainEffect {
public:
    bool is_bus_layout_supported(const BusesLayout& l) const override {
        if (l.inputs.size() != 2) return false;
        if (l.outputs.size() != 1) return false;
        // Main and sidechain must agree.
        if (l.inputs[0] != l.inputs[1]) return false;
        // Output mirrors main.
        if (l.outputs[0] != l.inputs[0]) return false;
        return true;
    }
};

class ProjectingProcessor : public SidechainEffect {
public:
    void process(pulp::audio::BufferView<float>& output,
                 const pulp::audio::BufferView<const float>& input,
                 pulp::midi::MidiBuffer&,
                 pulp::midi::MidiBuffer&,
                 const ProcessContext&) override {
        ++simple_process_calls;
        saw_sidechain = sidechain_input() != nullptr;
        observed_input_channels = input.num_channels();
        observed_output_channels = output.num_channels();
        if (!input.empty() && !output.empty()) {
            output.channel(0)[0] = input.channel(0)[0] + 1.0f;
        }
    }

    int simple_process_calls = 0;
    std::size_t observed_input_channels = 0;
    std::size_t observed_output_channels = 0;
    bool saw_sidechain = false;
};

} // namespace

// ── Bus layout validation ─────────────────────────────────────────────────

TEST_CASE("Processor::is_bus_layout_supported default policy accepts mono/stereo "
          "matching the descriptor's bus count",
          "[processor][bus-layout]") {
    StereoEffect p;

    // Matches descriptor (1 in / 1 out, both stereo).
    Processor::BusesLayout ok{{2}, {2}};
    REQUIRE(p.is_bus_layout_supported(ok));

    // Mono main is fine.
    Processor::BusesLayout mono{{1}, {1}};
    REQUIRE(p.is_bus_layout_supported(mono));

    // Stereo in, mono out is allowed by the default policy — the
    // adapter is the one that enforces matching counts via the
    // descriptor; the default validator just confirms channel counts
    // are in {1, 2}.
    Processor::BusesLayout mixed{{2}, {1}};
    REQUIRE(p.is_bus_layout_supported(mixed));
}

TEST_CASE("Processor::is_bus_layout_supported default policy rejects "
          "non-mono/stereo channel counts and bus-count mismatches",
          "[processor][bus-layout]") {
    StereoEffect p;

    // 6-channel (5.1) is out of scope for the default validator.
    Processor::BusesLayout surround{{6}, {6}};
    REQUIRE_FALSE(p.is_bus_layout_supported(surround));

    // Wrong input bus count (descriptor declares 1, host proposes 2).
    Processor::BusesLayout wrong_count{{2, 2}, {2}};
    REQUIRE_FALSE(p.is_bus_layout_supported(wrong_count));

    // Empty per-side means "no opinion" — must be accepted so adapters
    // that only pass the side they care about don't trip the validator.
    Processor::BusesLayout empty_in{{}, {2}};
    REQUIRE(p.is_bus_layout_supported(empty_in));
    Processor::BusesLayout empty_out{{2}, {}};
    REQUIRE(p.is_bus_layout_supported(empty_out));
}

TEST_CASE("Processor::is_bus_layout_supported override can enforce a "
          "linked-sidechain contract",
          "[processor][bus-layout]") {
    LinkedSidechainEffect p;

    // Stereo main + stereo sidechain + stereo out — all linked.
    Processor::BusesLayout linked_stereo{{2, 2}, {2}};
    REQUIRE(p.is_bus_layout_supported(linked_stereo));

    // Mono everywhere — also linked.
    Processor::BusesLayout linked_mono{{1, 1}, {1}};
    REQUIRE(p.is_bus_layout_supported(linked_mono));

    // Stereo main / mono sidechain — rejected, sidechain must match main.
    Processor::BusesLayout mismatched_sc{{2, 1}, {2}};
    REQUIRE_FALSE(p.is_bus_layout_supported(mismatched_sc));

    // Stereo main / stereo sidechain / mono out — rejected by the
    // mirror-output rule the plugin's override added.
    Processor::BusesLayout mismatched_out{{2, 2}, {1}};
    REQUIRE_FALSE(p.is_bus_layout_supported(mismatched_out));
}

// ── processBlock precision contract ───────────────────────────────────────

TEST_CASE("Processor keeps the f32 process signature and exposes an additive f64 fallback",
          "[processor][precision]") {
    using ProcessSig = void (Processor::*)(
        pulp::audio::BufferView<float>&,
        const pulp::audio::BufferView<const float>&,
        pulp::midi::MidiBuffer&,
        pulp::midi::MidiBuffer&,
        const ProcessContext&);
    static_assert(std::is_same_v<decltype(static_cast<ProcessSig>(&Processor::process)),
                                 ProcessSig>,
                  "Processor::process() must remain the f32 adapter-facing virtual.");
    using ProcessF64Sig = void (Processor::*)(
        pulp::audio::BufferView<double>&,
        const pulp::audio::BufferView<const double>&,
        pulp::midi::MidiBuffer&,
        pulp::midi::MidiBuffer&,
        const ProcessContext&);
    static_assert(std::is_same_v<decltype(static_cast<ProcessF64Sig>(&Processor::process_f64)),
                                 ProcessF64Sig>,
                  "Processor::process_f64() must stay an additive f64 overload.");
    using RichProcessF64Sig = void (Processor::*)(
        ProcessBuffers64&,
        pulp::midi::MidiBuffer&,
        pulp::midi::MidiBuffer&,
        const ProcessContext&);
    static_assert(std::is_same_v<decltype(static_cast<RichProcessF64Sig>(&Processor::process_f64)),
                                 RichProcessF64Sig>,
                  "Processor::process_f64(ProcessBuffers64&) must stay additive.");

    CopyEffect p;
    float buf_in[2][8] = {};
    float buf_out[2][8] = {};
    const float* in_ptrs[2]  = {buf_in[0],  buf_in[1]};
    float*       out_ptrs[2] = {buf_out[0], buf_out[1]};
    pulp::audio::BufferView<const float> in_view(in_ptrs, 2, 8);
    pulp::audio::BufferView<float>       out_view(out_ptrs, 2, 8);
    pulp::midi::MidiBuffer mi, mo;
    ProcessContext ctx; ctx.sample_rate = 48000.0; ctx.num_samples = 8;
    p.process(out_view, in_view, mi, mo, ctx);

    PrepareContext prepare;
    prepare.sample_rate = 48000.0;
    prepare.max_buffer_size = 8;
    prepare.input_channels = 2;
    prepare.output_channels = 2;
    p.prepare_f64_fallback_scratch(prepare);

    double buf64_in[2][8] = {
        {0.125, -0.25, 0.5, -0.75, 1.0, -1.25, 1.5, -1.75},
        {2.0, -2.25, 2.5, -2.75, 3.0, -3.25, 3.5, -3.75},
    };
    double buf64_out[2][8] = {};
    const double* in64_ptrs[2] = {buf64_in[0], buf64_in[1]};
    double* out64_ptrs[2] = {buf64_out[0], buf64_out[1]};
    pulp::audio::BufferView<const double> in64_view(in64_ptrs, 2, 8);
    pulp::audio::BufferView<double> out64_view(out64_ptrs, 2, 8);
    p.process_f64(out64_view, in64_view, mi, mo, ctx);

    for (std::size_t ch = 0; ch < 2; ++ch) {
        for (std::size_t i = 0; i < 8; ++i) {
            REQUIRE(buf64_out[ch][i] ==
                    static_cast<double>(static_cast<float>(buf64_in[ch][i])));
        }
    }
}

TEST_CASE("Processor f64 fallback slices prepared scratch to the active block",
          "[processor][precision]") {
    FrameCountingCopyEffect p;

    PrepareContext prepare;
    prepare.sample_rate = 48000.0;
    prepare.max_buffer_size = 8;
    prepare.input_channels = 2;
    prepare.output_channels = 2;
    p.prepare_f64_fallback_scratch(prepare);

    double in_l[4] = {1.0, 2.0, 3.0, 4.0};
    double in_r[4] = {};
    double out_l[4] = {};
    double out_r[4] = {};
    const double* in_ptrs[2] = {in_l, in_r};
    double* out_ptrs[2] = {out_l, out_r};
    pulp::audio::BufferView<const double> in_view(in_ptrs, 2, 4);
    pulp::audio::BufferView<double> out_view(out_ptrs, 2, 4);

    pulp::midi::MidiBuffer midi_in, midi_out;
    ProcessContext ctx;
    ctx.num_samples = 4;
    p.process_f64(out_view, in_view, midi_in, midi_out, ctx);

    REQUIRE(p.observed_frames == 4);
    REQUIRE(out_l[0] == 1.0);
    REQUIRE(out_l[3] == 4.0);
}

// ── Additive multi-bus process surface ───────────────────────────────────

TEST_CASE("ProcessBuffers exposes main and sidechain buses without owning audio",
          "[processor][process-buffers]") {
    float main_in_l[8] = {};
    float main_in_r[8] = {};
    float sidechain_l[8] = {};
    float sidechain_r[8] = {};
    float main_out_l[8] = {};
    float main_out_r[8] = {};

    const float* main_in_ptrs[2] = {main_in_l, main_in_r};
    const float* sidechain_ptrs[2] = {sidechain_l, sidechain_r};
    float* main_out_ptrs[2] = {main_out_l, main_out_r};

    std::array<ProcessBusBufferView<const float>, 2> inputs{{
        {
            .info = {"Main In", 0, BusDirection::Input, BusRole::Main, 2, false, true},
            .buffer = pulp::audio::BufferView<const float>(main_in_ptrs, 2, 8),
        },
        {
            .info = {"Sidechain", 1, BusDirection::Input, BusRole::Sidechain, 2, true, true},
            .buffer = pulp::audio::BufferView<const float>(sidechain_ptrs, 2, 8),
        },
    }};
    std::array<ProcessBusBufferView<float>, 1> outputs{{
        {
            .info = {"Main Out", 0, BusDirection::Output, BusRole::Main, 2, false, true},
            .buffer = pulp::audio::BufferView<float>(main_out_ptrs, 2, 8),
        },
    }};

    ProcessBuffers buffers{
        .inputs = ProcessBusBufferSet<const float>(inputs),
        .outputs = ProcessBusBufferSet<float>(outputs),
    };

    REQUIRE(buffers.inputs.size() == 2);
    REQUIRE(buffers.inputs.active_count() == 2);
    REQUIRE(buffers.outputs.active_count() == 1);
    REQUIRE(buffers.main_input() == &inputs[0].buffer);
    REQUIRE(buffers.sidechain_input() == &inputs[1].buffer);
    REQUIRE(buffers.main_output() == &outputs[0].buffer);
    REQUIRE(buffers.layouts_match_descriptors());
    REQUIRE(buffers.active_buses_have_storage());
}

TEST_CASE("BusBufferSet looks up indexed and named aux buses",
          "[processor][process-buffers]") {
    float main_l[4] = {};
    float main_r[4] = {};
    float cue_l[4] = {};
    float* main_ptrs[2] = {main_l, main_r};
    float* cue_ptrs[1] = {cue_l};

    std::array<ProcessBusBufferView<float>, 3> outputs{{
        {
            .info = {"Main Out", 0, BusDirection::Output, BusRole::Main, 2, false, true},
            .buffer = pulp::audio::BufferView<float>(main_ptrs, 2, 4),
        },
        {
            .info = {"Cue", 1, BusDirection::Output, BusRole::Aux, 1, true, true},
            .buffer = pulp::audio::BufferView<float>(cue_ptrs, 1, 4),
        },
        {
            .info = {"Stem", 2, BusDirection::Output, BusRole::Aux, 2, true, false},
            .buffer = {},
        },
    }};

    ProcessBusBufferSet<float> buses(outputs);
    const auto& const_buses = buses;

    REQUIRE(buses.count(BusRole::Main) == 1);
    REQUIRE(buses.count(BusRole::Aux) == 2);
    REQUIRE(buses.active_count(BusRole::Aux) == 1);
    REQUIRE(buses.find(BusRole::Aux, 0) == &outputs[1]);
    REQUIRE(buses.find(BusRole::Aux, 1) == &outputs[2]);
    REQUIRE(buses.find(BusRole::Aux, 2) == nullptr);
    REQUIRE(buses.find_by_index(2) == &outputs[2]);
    REQUIRE(buses.find_by_index(42) == nullptr);
    REQUIRE(buses.find_by_name("Cue") == &outputs[1]);
    REQUIRE(buses.find_by_name("Missing") == nullptr);
    REQUIRE(const_buses.find(BusRole::Aux, 1) == &outputs[2]);
    REQUIRE(const_buses.find_by_index(1) == &outputs[1]);
    REQUIRE(const_buses.find_by_name("Stem") == &outputs[2]);
}

TEST_CASE("ProcessBuffers accepts inactive optional buses with empty views",
          "[processor][process-buffers]") {
    float main_in_l[4] = {};
    float main_in_r[4] = {};
    float main_out_l[4] = {};
    float main_out_r[4] = {};
    const float* main_in_ptrs[2] = {main_in_l, main_in_r};
    float* main_out_ptrs[2] = {main_out_l, main_out_r};

    std::array<ProcessBusBufferView<const float>, 2> inputs{{
        {
            .info = {"Main In", 0, BusDirection::Input, BusRole::Main, 2, false, true},
            .buffer = pulp::audio::BufferView<const float>(main_in_ptrs, 2, 4),
        },
        {
            .info = {"Sidechain", 1, BusDirection::Input, BusRole::Sidechain, 2, true, false},
            .buffer = {},
        },
    }};
    std::array<ProcessBusBufferView<float>, 1> outputs{{
        {
            .info = {"Main Out", 0, BusDirection::Output, BusRole::Main, 2, false, true},
            .buffer = pulp::audio::BufferView<float>(main_out_ptrs, 2, 4),
        },
    }};

    ProcessBuffers buffers{
        .inputs = ProcessBusBufferSet<const float>(inputs),
        .outputs = ProcessBusBufferSet<float>(outputs),
    };

    REQUIRE(buffers.inputs.active_count() == 1);
    REQUIRE(buffers.sidechain_input() == nullptr);
    REQUIRE(buffers.layouts_match_descriptors());
    REQUIRE(buffers.active_buses_have_storage());
}

TEST_CASE("ProcessBuffers reports layout and storage mismatches before process",
          "[processor][process-buffers]") {
    float left[4] = {};
    const float* missing_channel[2] = {left, nullptr};
    std::array<ProcessBusBufferView<const float>, 1> inputs{{
        {
            .info = {"Main In", 0, BusDirection::Input, BusRole::Main, 2, false, true},
            .buffer = pulp::audio::BufferView<const float>(missing_channel, 2, 4),
        },
    }};

    float out_l[4] = {};
    float* mono_out[1] = {out_l};
    std::array<ProcessBusBufferView<float>, 1> outputs{{
        {
            .info = {"Main Out", 0, BusDirection::Output, BusRole::Main, 2, false, true},
            .buffer = pulp::audio::BufferView<float>(mono_out, 1, 4),
        },
    }};

    ProcessBuffers buffers{
        .inputs = ProcessBusBufferSet<const float>(inputs),
        .outputs = ProcessBusBufferSet<float>(outputs),
    };

    REQUIRE_FALSE(buffers.active_buses_have_storage());
    REQUIRE_FALSE(buffers.layouts_match_descriptors());
}

TEST_CASE("Processor multi-bus overload projects to the legacy process callback",
          "[processor][process-buffers]") {
    ProjectingProcessor processor;
    float in_l[4] = {2.0f, 0.0f, 0.0f, 0.0f};
    float in_r[4] = {};
    float side_l[4] = {};
    float side_r[4] = {};
    float out_l[4] = {};
    float out_r[4] = {};
    const float* in_ptrs[2] = {in_l, in_r};
    const float* side_ptrs[2] = {side_l, side_r};
    float* out_ptrs[2] = {out_l, out_r};

    std::array<ProcessBusBufferView<const float>, 2> inputs{{
        {
            .info = {"Main In", 0, BusDirection::Input, BusRole::Main, 2, false, true},
            .buffer = pulp::audio::BufferView<const float>(in_ptrs, 2, 4),
        },
        {
            .info = {"Sidechain", 1, BusDirection::Input, BusRole::Sidechain, 2, true, true},
            .buffer = pulp::audio::BufferView<const float>(side_ptrs, 2, 4),
        },
    }};
    std::array<ProcessBusBufferView<float>, 1> outputs{{
        {
            .info = {"Main Out", 0, BusDirection::Output, BusRole::Main, 2, false, true},
            .buffer = pulp::audio::BufferView<float>(out_ptrs, 2, 4),
        },
    }};

    ProcessBuffers buffers{
        .inputs = ProcessBusBufferSet<const float>(inputs),
        .outputs = ProcessBusBufferSet<float>(outputs),
    };
    pulp::midi::MidiBuffer midi_in, midi_out;
    ProcessContext ctx;

    static_cast<Processor&>(processor).process(buffers, midi_in, midi_out, ctx);

    REQUIRE(processor.simple_process_calls == 1);
    REQUIRE(processor.observed_input_channels == 2);
    REQUIRE(processor.observed_output_channels == 2);
    REQUIRE(processor.saw_sidechain);
    REQUIRE(out_l[0] == 3.0f);
    REQUIRE(processor.sidechain_input() == nullptr);
}

TEST_CASE("Processor rich f64 fallback preserves sidechain projection",
          "[processor][precision][process-buffers]") {
    ProjectingProcessor processor;

    PrepareContext prepare;
    prepare.sample_rate = 48000.0;
    prepare.max_buffer_size = 8;
    prepare.input_channels = 2;
    prepare.output_channels = 2;
    processor.prepare_f64_fallback_scratch(prepare);

    double in_l[4] = {2.0, 0.0, 0.0, 0.0};
    double in_r[4] = {};
    double side_l[4] = {7.0, 0.0, 0.0, 0.0};
    double side_r[4] = {};
    double out_l[4] = {};
    double out_r[4] = {};
    const double* in_ptrs[2] = {in_l, in_r};
    const double* side_ptrs[2] = {side_l, side_r};
    double* out_ptrs[2] = {out_l, out_r};

    std::array<ProcessBusBufferView<const double>, 2> inputs{{
        {
            .info = {"Main In", 0, BusDirection::Input, BusRole::Main, 2, false, true},
            .buffer = pulp::audio::BufferView<const double>(in_ptrs, 2, 4),
        },
        {
            .info = {"Sidechain", 1, BusDirection::Input, BusRole::Sidechain, 2, true, true},
            .buffer = pulp::audio::BufferView<const double>(side_ptrs, 2, 4),
        },
    }};
    std::array<ProcessBusBufferView<double>, 1> outputs{{
        {
            .info = {"Main Out", 0, BusDirection::Output, BusRole::Main, 2, false, true},
            .buffer = pulp::audio::BufferView<double>(out_ptrs, 2, 4),
        },
    }};

    ProcessBuffers64 buffers{
        .inputs = ProcessBusBufferSet<const double>(inputs),
        .outputs = ProcessBusBufferSet<double>(outputs),
    };

    pulp::midi::MidiBuffer midi_in, midi_out;
    ProcessContext ctx;
    ctx.num_samples = 4;
    static_cast<Processor&>(processor).process_f64(buffers, midi_in, midi_out, ctx);

    REQUIRE(processor.simple_process_calls == 1);
    REQUIRE(processor.observed_input_channels == 2);
    REQUIRE(processor.observed_output_channels == 2);
    REQUIRE(processor.saw_sidechain);
    REQUIRE(out_l[0] == 3.0);
    REQUIRE(processor.sidechain_input() == nullptr);
}

TEST_CASE("Processor rich f64 fallback silences every active output when scratch is too small",
          "[processor][precision][process-buffers]") {
    ProjectingProcessor processor;

    PrepareContext prepare;
    prepare.sample_rate = 48000.0;
    prepare.max_buffer_size = 4;
    prepare.input_channels = 2;
    prepare.output_channels = 2;
    processor.prepare_f64_fallback_scratch(prepare);

    double in_l[4] = {};
    double in_r[4] = {};
    double main_l[4] = {1.0, 1.0, 1.0, 1.0};
    double main_r[4] = {1.0, 1.0, 1.0, 1.0};
    double aux_l[4] = {2.0, 2.0, 2.0, 2.0};
    double aux_r[4] = {2.0, 2.0, 2.0, 2.0};
    const double* in_ptrs[2] = {in_l, in_r};
    double* main_ptrs[2] = {main_l, main_r};
    double* aux_ptrs[2] = {aux_l, aux_r};

    std::array<ProcessBusBufferView<const double>, 1> inputs{{
        {
            .info = {"Main In", 0, BusDirection::Input, BusRole::Main, 2, false, true},
            .buffer = pulp::audio::BufferView<const double>(in_ptrs, 2, 4),
        },
    }};
    std::array<ProcessBusBufferView<double>, 2> outputs{{
        {
            .info = {"Main Out", 0, BusDirection::Output, BusRole::Main, 2, false, true},
            .buffer = pulp::audio::BufferView<double>(main_ptrs, 2, 4),
        },
        {
            .info = {"Aux Out", 1, BusDirection::Output, BusRole::Aux, 2, true, true},
            .buffer = pulp::audio::BufferView<double>(aux_ptrs, 2, 4),
        },
    }};

    ProcessBuffers64 buffers{
        .inputs = ProcessBusBufferSet<const double>(inputs),
        .outputs = ProcessBusBufferSet<double>(outputs),
    };

    pulp::midi::MidiBuffer midi_in, midi_out;
    ProcessContext ctx;
    ctx.num_samples = 4;
    static_cast<Processor&>(processor).process_f64(buffers, midi_in, midi_out, ctx);

    REQUIRE(processor.simple_process_calls == 0);
    for (double sample : main_l) REQUIRE(sample == 0.0);
    for (double sample : main_r) REQUIRE(sample == 0.0);
    for (double sample : aux_l) REQUIRE(sample == 0.0);
    for (double sample : aux_r) REQUIRE(sample == 0.0);
}

TEST_CASE("Processor multi-bus overload no-ops when no active main output exists",
          "[processor][process-buffers]") {
    ProjectingProcessor processor;
    float in_l[4] = {};
    const float* in_ptrs[1] = {in_l};
    std::array<ProcessBusBufferView<const float>, 1> inputs{{
        {
            .info = {"Main In", 0, BusDirection::Input, BusRole::Main, 1, false, true},
            .buffer = pulp::audio::BufferView<const float>(in_ptrs, 1, 4),
        },
    }};
    std::array<ProcessBusBufferView<float>, 1> outputs{{
        {
            .info = {"Main Out", 0, BusDirection::Output, BusRole::Main, 2, false, false},
            .buffer = {},
        },
    }};

    ProcessBuffers buffers{
        .inputs = ProcessBusBufferSet<const float>(inputs),
        .outputs = ProcessBusBufferSet<float>(outputs),
    };
    pulp::midi::MidiBuffer midi_in, midi_out;
    ProcessContext ctx;

    static_cast<Processor&>(processor).process(buffers, midi_in, midi_out, ctx);

    REQUIRE(processor.simple_process_calls == 0);
}

// ── Runtime mode contract ─────────────────────────────────────────────────

TEST_CASE("ProcessContext defaults to realtime mode with explicit helper predicates",
          "[processor][process-context]") {
    ProcessContext ctx;

    REQUIRE(ctx.process_mode == ProcessMode::Realtime);
    REQUIRE(ctx.render_speed_hint == RenderSpeedHint::Unknown);
    REQUIRE(ctx.is_realtime());
    REQUIRE_FALSE(ctx.is_offline());
    REQUIRE_FALSE(ctx.is_bypassed);
    REQUIRE_FALSE(ctx.is_tail_drain);
    REQUIRE_FALSE(ctx.reset_requested);
    REQUIRE_FALSE(ctx.transport_jump);
    REQUIRE_FALSE(ctx.allows_offline_quality_work());
    REQUIRE_FALSE(ctx.should_reset_dsp_state());
    REQUIRE_FALSE(ctx.is_maintenance_render());
    REQUIRE_FALSE(ctx.should_render_tail_only());

    ctx.process_mode = ProcessMode::Offline;
    ctx.render_speed_hint = RenderSpeedHint::FasterThanRealtime;
    REQUIRE_FALSE(ctx.is_realtime());
    REQUIRE(ctx.is_offline());
    REQUIRE(ctx.allows_offline_quality_work());

    ctx.render_speed_hint = RenderSpeedHint::Realtime;
    REQUIRE_FALSE(ctx.allows_offline_quality_work());
}

TEST_CASE("ProcessContext reset and maintenance helpers compose explicit flags",
          "[processor][process-context]") {
    ProcessContext ctx;

    ctx.transport_jump = true;
    REQUIRE(ctx.should_reset_dsp_state());
    ctx.transport_jump = false;

    ctx.reset_requested = true;
    REQUIRE(ctx.should_reset_dsp_state());
    ctx.reset_requested = false;

    ctx.is_bypassed = true;
    REQUIRE(ctx.is_maintenance_render());
    REQUIRE_FALSE(ctx.should_render_tail_only());
    ctx.is_bypassed = false;

    ctx.is_tail_drain = true;
    REQUIRE(ctx.is_maintenance_render());
    REQUIRE(ctx.should_render_tail_only());
}

// ── Latency / tail change notifications (RT-safe) ─────────────────────────

TEST_CASE("flag_latency_changed / consume_latency_changed_flag round-trip "
          "yields exactly one consume per flag",
          "[processor][latency]") {
    StereoEffect p;
    REQUIRE_FALSE(p.consume_latency_changed_flag());
    p.flag_latency_changed();
    REQUIRE(p.consume_latency_changed_flag());
    // Edge has been drained.
    REQUIRE_FALSE(p.consume_latency_changed_flag());
}

TEST_CASE("flag_tail_changed / consume_tail_changed_flag round-trip "
          "yields exactly one consume per flag",
          "[processor][latency]") {
    StereoEffect p;
    REQUIRE_FALSE(p.consume_tail_changed_flag());
    p.flag_tail_changed();
    REQUIRE(p.consume_tail_changed_flag());
    REQUIRE_FALSE(p.consume_tail_changed_flag());
}

TEST_CASE("latency_change_pending / tail_change_pending peek does not "
          "drain the flag",
          "[processor][latency]") {
    StereoEffect p;
    p.flag_latency_changed();
    p.flag_tail_changed();
    REQUIRE(p.latency_change_pending());
    REQUIRE(p.tail_change_pending());
    // Peek twice — still pending, still pending.
    REQUIRE(p.latency_change_pending());
    REQUIRE(p.tail_change_pending());
    // Consume drains.
    REQUIRE(p.consume_latency_changed_flag());
    REQUIRE(p.consume_tail_changed_flag());
    REQUIRE_FALSE(p.latency_change_pending());
    REQUIRE_FALSE(p.tail_change_pending());
}

TEST_CASE("flag_*_changed -> consume_*_changed_flag is data-race-free "
          "when the flag is set from one thread and drained from another",
          "[processor][latency][threading]") {
    // Hammer the flag from an "audio" thread and drain from a "main"
    // thread. The contract: every drain that observes `true` corresponds
    // to at least one preceding set, and we never observe undefined
    // behaviour. This is the minimal smoke we need to gate the
    // RT-safety claim; a TSan run extends it.
    StereoEffect p;
    std::atomic<bool> stop{false};
    std::atomic<int> seen{0};

    std::thread audio([&]{
        for (int i = 0; i < 100000 && !stop.load(); ++i) {
            p.flag_latency_changed();
        }
    });
    std::thread main_t([&]{
        // Drain until audio thread is done plus a tail drain.
        while (!stop.load()) {
            if (p.consume_latency_changed_flag()) ++seen;
        }
        // Final drain to catch any remaining edge.
        if (p.consume_latency_changed_flag()) ++seen;
    });

    audio.join();
    stop.store(true);
    main_t.join();

    // At least one set was observed.
    REQUIRE(seen.load() >= 1);
    REQUIRE_FALSE(p.consume_latency_changed_flag());
}
