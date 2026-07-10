// A CLAP output buffer carries a `constant_mask`: bit N set means "every frame
// of channel N holds the value in sample 0", and a host is free to act on that
// by reading one sample instead of the block. The plugin owns the mask — the
// host does not clear it — and CLAP explicitly permits in-place buffers, so the
// `clap_audio_buffer_t` handed to an output bus can arrive still carrying the
// mask an upstream plugin set on the input it aliases.
//
// An adapter that never writes the mask therefore inherits whatever was there.
// The visible failure is a plugin whose output varies being read by the host as
// a single held sample — silence, or a frozen DC level. It is worst for CV-rate
// outputs, where the variation IS the signal.
//
// These tests hand the adapter output buses with every bit of the mask pre-set,
// exactly as an in-place host that had just processed a silent block would, and
// pin that the adapter hands them back cleared — on the main bus, on aux buses,
// through the f64 path, and through the bypass short-circuit.

#include <catch2/catch_test_macros.hpp>

#include <pulp/format/clap_adapter.hpp>
#include <pulp/format/processor.hpp>
#include <pulp/state/store.hpp>

#include <cstdint>
#include <memory>
#include <vector>

using namespace pulp;
using namespace pulp::format;

namespace {

constexpr uint32_t kFrames = 64;

// Writes a per-channel ramp, so every output channel genuinely varies and a set
// constant_mask bit would be a lie the host is entitled to believe.
class RampProcessor : public Processor {
public:
    PluginDescriptor descriptor() const override {
        return {
            .name = "ConstantMask",
            .manufacturer = "PulpTest",
            .bundle_id = "com.pulp.test.constant-mask",
            .version = "1.0.0",
            .category = PluginCategory::Effect,
            .input_buses = {{"In", 2}},
            .output_buses = {{"Main", 2}, {"Aux", 2}},
        };
    }
    void define_parameters(state::StateStore& store) override {
        store.add_parameter({.id = 1, .name = "Gain", .range = {0.0f, 1.0f, 1.0f, 0.0f}});
    }
    void prepare(const PrepareContext&) override {}
    void process(audio::BufferView<float>& out,
                 const audio::BufferView<const float>&,
                 midi::MidiBuffer&, midi::MidiBuffer&,
                 const ProcessContext&) override {
        for (std::size_t ch = 0; ch < out.num_channels(); ++ch) {
            auto oc = out.channel(ch);
            for (std::size_t i = 0; i < out.num_samples(); ++i) {
                oc[i] = static_cast<float>(i + 1) * static_cast<float>(ch + 1);
            }
        }
    }
};

std::unique_ptr<Processor> make_ramp() { return std::make_unique<RampProcessor>(); }

// The adapter through its public surface, activated the way a host loader does.
struct ClapInstance {
    clap_adapter::PulpClapPlugin plugin;
    bool active = false;

    ClapInstance() {
        plugin.factory = make_ramp;
        plugin.plugin.plugin_data = &plugin;
        REQUIRE(clap_adapter::clap_init(&plugin.plugin));
        REQUIRE(clap_adapter::clap_activate(&plugin.plugin, 48000.0, 32, kFrames));
        active = true;
    }
    ~ClapInstance() {
        if (active) clap_adapter::clap_deactivate(&plugin.plugin);
    }
};

// Everything a host must own to call clap_process once: two output buses (main
// + aux) and one input bus, either f32 or f64, with every mask bit pre-set.
struct HostBuffers {
    explicit HostBuffers(bool f64) : is_f64(f64) {
        for (int ch = 0; ch < 2; ++ch) {
            in32[ch].assign(kFrames, 0.0f);
            in64[ch].assign(kFrames, 0.0);
            main32[ch].assign(kFrames, 0.0f);
            main64[ch].assign(kFrames, 0.0);
            aux32[ch].assign(kFrames, 0.0f);
            aux64[ch].assign(kFrames, 0.0);
            in32_ptrs[ch] = in32[ch].data();
            in64_ptrs[ch] = in64[ch].data();
            main32_ptrs[ch] = main32[ch].data();
            main64_ptrs[ch] = main64[ch].data();
            aux32_ptrs[ch] = aux32[ch].data();
            aux64_ptrs[ch] = aux64[ch].data();
        }
        for (auto* bus : {&input, &outputs[0], &outputs[1]}) bus->channel_count = 2;
        if (is_f64) {
            input.data64 = in64_ptrs;
            outputs[0].data64 = main64_ptrs;
            outputs[1].data64 = aux64_ptrs;
        } else {
            input.data32 = in32_ptrs;
            outputs[0].data32 = main32_ptrs;
            outputs[1].data32 = aux32_ptrs;
        }
        // What an in-place host leaves behind: both channels of both output
        // buses claimed constant, because the silent input they alias was.
        input.constant_mask = 0b11;
        outputs[0].constant_mask = 0b11;
        outputs[1].constant_mask = 0b11;
    }

    clap_process_t make_process() {
        clap_process_t proc{};
        proc.frames_count = kFrames;
        proc.audio_inputs = &input;
        proc.audio_inputs_count = 1;
        proc.audio_outputs = outputs;
        proc.audio_outputs_count = 2;
        return proc;
    }

    // Reads the main bus back through whichever precision it was set up with.
    double main_sample(int ch, uint32_t frame) const {
        return is_f64 ? main64[ch][frame] : static_cast<double>(main32[ch][frame]);
    }
    void fill_input_ramp() {
        for (int ch = 0; ch < 2; ++ch) {
            for (uint32_t i = 0; i < kFrames; ++i) {
                const auto v = static_cast<double>(i + 1) * (ch + 1);
                in32[ch][i] = static_cast<float>(v);
                in64[ch][i] = v;
            }
        }
    }

    bool is_f64;
    std::vector<float> in32[2], main32[2], aux32[2];
    std::vector<double> in64[2], main64[2], aux64[2];
    float* in32_ptrs[2];
    double* in64_ptrs[2];
    float* main32_ptrs[2];
    double* main64_ptrs[2];
    float* aux32_ptrs[2];
    double* aux64_ptrs[2];
    clap_audio_buffer_t input{};
    clap_audio_buffer_t outputs[2]{};
};

}  // namespace

TEST_CASE("The CLAP adapter clears a stale constant_mask on every output bus",
          "[clap][constant-mask]") {
    ClapInstance inst;
    HostBuffers bufs(/*f64=*/false);
    auto proc = bufs.make_process();

    REQUIRE(clap_adapter::clap_process(&inst.plugin.plugin, &proc) ==
            CLAP_PROCESS_CONTINUE);

    CHECK(bufs.outputs[0].constant_mask == 0);
    CHECK(bufs.outputs[1].constant_mask == 0);

    // The mask mattered: the main bus does vary, so a host honoring the stale
    // bits would have read one sample and held it for the block.
    CHECK(bufs.main_sample(0, 0) != bufs.main_sample(0, kFrames - 1));
    CHECK(bufs.main_sample(1, 0) != bufs.main_sample(1, kFrames - 1));

    // The adapter leaves the host's input mask alone — it is the host's to own.
    CHECK(bufs.input.constant_mask == 0b11);
}

TEST_CASE("The CLAP adapter clears a stale constant_mask on an f64 output bus",
          "[clap][constant-mask]") {
    ClapInstance inst;
    HostBuffers bufs(/*f64=*/true);
    auto proc = bufs.make_process();

    REQUIRE(clap_adapter::clap_process(&inst.plugin.plugin, &proc) ==
            CLAP_PROCESS_CONTINUE);

    CHECK(bufs.outputs[0].constant_mask == 0);
    CHECK(bufs.outputs[1].constant_mask == 0);
    CHECK(bufs.main_sample(0, 0) != bufs.main_sample(0, kFrames - 1));
}

TEST_CASE("The CLAP adapter clears a stale constant_mask while bypassed",
          "[clap][constant-mask]") {
    ClapInstance inst;
    // Bypass short-circuits the Processor entirely and copies input to output
    // itself, which is a second path that must not leave the mask standing.
    REQUIRE(inst.plugin.bypass_param_id != 0);
    inst.plugin.store.set_value(inst.plugin.bypass_param_id, 1.0f);

    HostBuffers bufs(/*f64=*/false);
    bufs.fill_input_ramp();
    auto proc = bufs.make_process();

    REQUIRE(clap_adapter::clap_process(&inst.plugin.plugin, &proc) ==
            CLAP_PROCESS_CONTINUE);

    CHECK(bufs.outputs[0].constant_mask == 0);
    CHECK(bufs.outputs[1].constant_mask == 0);
    // The pass-through carried the varying input, so the stale bits were wrong
    // here too.
    CHECK(bufs.main_sample(0, 0) != bufs.main_sample(0, kFrames - 1));
}

TEST_CASE("The CLAP adapter clears a stale constant_mask on a bus it does not route",
          "[clap][constant-mask]") {
    // A host may present more output buses than the adapter routes to the
    // Processor (kMaxOutputBuses). Those extra buses are zero-filled rather than
    // left uninitialised, so they too are described by a mask the plugin owns —
    // and clearing every bus, not just the routed ones, is what makes the
    // adapter's answer independent of where that ceiling happens to sit.
    constexpr uint32_t kBuses = clap_adapter::kMaxOutputBuses + 1;
    ClapInstance inst;

    std::vector<float> storage[kBuses];
    float* ptrs[kBuses];
    clap_audio_buffer_t outputs[kBuses]{};
    for (uint32_t b = 0; b < kBuses; ++b) {
        storage[b].assign(kFrames, 0.0f);
        ptrs[b] = storage[b].data();
        outputs[b].data32 = &ptrs[b];
        outputs[b].channel_count = 1;
        outputs[b].constant_mask = 0b1;
    }

    clap_process_t proc{};
    proc.frames_count = kFrames;
    proc.audio_outputs = outputs;
    proc.audio_outputs_count = kBuses;

    REQUIRE(clap_adapter::clap_process(&inst.plugin.plugin, &proc) ==
            CLAP_PROCESS_CONTINUE);
    for (uint32_t b = 0; b < kBuses; ++b) {
        INFO("output bus " << b);
        CHECK(outputs[b].constant_mask == 0);
    }
}
