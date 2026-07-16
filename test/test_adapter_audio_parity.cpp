// Adapter-vs-direct audio null test.
//
// `test_golden_audio.cpp` asserts Processor-level properties through
// HeadlessHost, which never enters a format adapter at all.
// `test_adapter_boundary_parity.cpp` drives a real `clap_process()` but compares
// transport decode, f64 marshalling, and bypass timing — never the emitted
// samples of a normal (non-bypassed) block. So nothing pins the claim the whole
// adapter layer rests on: **the adapter is transparent**. A block that a
// Processor renders through HeadlessHost and the same block rendered through the
// real CLAP adapter must be the same bits.
//
// That claim is what makes an "extract a function" refactor of the adapter's
// process() safe. The adapter does not filter, resample, or round anything on
// the normal path, so there is no tolerance to spend: any reordering of the
// adapter's phases (transport before params, MIDI after the DSP call, a dropped
// or duplicated block) perturbs the output, and the direct render — which shares
// no adapter code — does not move with it. The null is therefore a real
// discriminator, not a self-consistency check.
//
// The processor below is built to make that discriminator sharp:
//   * it is STATEFUL (a one-pole per channel), so a dropped, duplicated, or
//     reordered block leaves a permanent trace instead of averaging out;
//   * its coefficient is derived from the PREPARED SAMPLE RATE, so an adapter
//     that plumbs the rate wrongly diverges;
//   * MIDI gates the signal AT THE EVENT'S SAMPLE OFFSET, so an off-by-one in
//     offset decode moves a sample;
//   * a parameter scales the output, so param decode + dual-write is in the
//     signal path;
//   * it writes one parameter and pushes one explicit output param event, so the
//     host-facing publication path runs on every block.
//
// The sweep is (sample rate × block size): the adapter's per-block function is
// re-entered every block, so a phase-ordering fault shows up as a block-boundary
// artifact and is loudest at the extreme partitions (block=1 and a ragged tail).

#include <catch2/catch_test_macros.hpp>

#include "support/audio_signal_generators.hpp"
#include "support/render_scenario.hpp"

#include <pulp/audio/buffer.hpp>
#include <pulp/format/clap_adapter.hpp>
#include <pulp/format/processor.hpp>
#include <pulp/midi/message.hpp>
#include <pulp/state/store.hpp>

#include <clap/clap.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

using namespace pulp;
using namespace pulp::format;
using pulp::test::audio::MidiScriptEvent;
using pulp::test::audio::ParamStep;
using pulp::test::audio::RenderScenario;

namespace {

// ---------------------------------------------------------------------------
// The processor under both harnesses.
// ---------------------------------------------------------------------------

constexpr state::ParamID kGainParam = 1;
constexpr state::ParamID kOffsetParam = 2;
constexpr state::ParamID kMeterParam = 3;   // plugin-written (snapshot-diff path)
constexpr state::ParamID kEchoParam = 4;    // plugin-written (explicit-event path)

/// Deterministic, stateful, sample-rate-dependent, MIDI- and parameter-driven.
/// Every arithmetic step is plain float arithmetic evaluated in a fixed order,
/// so two renders of the same stimulus at the same block size are bit-identical
/// by construction — which is exactly what lets the null test below assert bits
/// rather than a tolerance.
class ParityProcessor : public Processor {
public:
    PluginDescriptor descriptor() const override {
        PluginDescriptor d;
        d.name = "PulpAdapterAudioParity";
        d.manufacturer = "PulpTest";
        d.bundle_id = "com.pulp.test.adapter-audio-parity";
        d.version = "1.0.0";
        d.category = PluginCategory::Effect;
        d.input_buses = {{"In", 2}};
        d.output_buses = {{"Out", 2}};
        d.accepts_midi = true;
        return d;
    }

    void define_parameters(state::StateStore& store) override {
        store.add_parameter(
            {.id = kGainParam, .name = "Gain", .range = {0.0f, 2.0f, 1.0f, 0.0f}});
        store.add_parameter(
            {.id = kOffsetParam, .name = "Offset", .range = {-1.0f, 1.0f, 0.0f, 0.0f}});
        store.add_parameter(
            {.id = kMeterParam, .name = "Meter", .range = {0.0f, 1.0f, 0.0f, 0.0f}});
        store.add_parameter(
            {.id = kEchoParam, .name = "Echo", .range = {0.0f, 2.0f, 0.0f, 0.0f}});
    }

    void prepare(const PrepareContext& ctx) override {
        // Sample-rate-dependent coefficient: an adapter that hands the processor
        // the wrong rate produces a different filter and fails the null.
        coeff_ = static_cast<float>(
            1.0 - std::exp(-2.0 * 3.14159265358979323846 /
                           (0.002 * ctx.sample_rate)));
        for (auto& z : z_) z = 0.0f;
        gate_ = 0.0f;
    }

    void process(audio::BufferView<float>& out,
                 const audio::BufferView<const float>& in,
                 midi::MidiBuffer& midi_in, midi::MidiBuffer&,
                 const ProcessContext&) override {
        const float gain = state().get_value(kGainParam);
        const float offset = state().get_value(kOffsetParam);
        const auto frames = out.num_samples();

        for (std::size_t ch = 0; ch < out.num_channels(); ++ch) {
            auto oc = out.channel(ch);
            const bool have_in = ch < in.num_channels();
            float z = z_[ch];
            float gate = gate_;
            for (std::size_t i = 0; i < frames; ++i) {
                // MIDI is applied at the event's own sample offset, so an
                // off-by-one in an adapter's offset decode moves a sample.
                for (const auto& me : midi_in) {
                    if (static_cast<std::size_t>(me.sample_offset) != i) continue;
                    if (me.is_note_on()) gate = 1.0f;
                    else if (me.is_note_off()) gate = 0.0f;
                }
                const float x = have_in ? in.channel(ch)[i] : 0.0f;
                z += coeff_ * (x - z);                 // stateful across blocks
                const float y = (z + gate * 0.25f + offset) * gain;
                oc[i] = y;
            }
            z_[ch] = z;
            if (ch + 1 == out.num_channels()) gate_ = gate;
        }

        // Plugin-side parameter writes. Both are pure bookkeeping — they never
        // reach the audio — but they make the host-facing publication path run
        // on every block, on both of its branches:
        //   * `Meter` is written to the store only, so it can only reach the
        //     host through the snapshot-diff fallback. It carries a per-block
        //     counter rather than the block peak: the peak can legitimately
        //     repeat between blocks (it clamps at 1.0), and a repeated value
        //     produces no diff and so no event -- which would force the test to
        //     settle for "some blocks published", a bar a broken publication
        //     path clears. A value that is distinct every block makes "exactly
        //     one per block" true, so a dropped publication fails;
        //   * `Echo` is BOTH pushed as an explicit sample-accurate event AND
        //     written to the store. That combination is what exercises the
        //     skip-set: the explicit event must suppress the snapshot fallback
        //     for the same parameter, or the host records the change twice.
        //     Only a parameter on both branches can catch a skip-set that marks
        //     the wrong index.
        meter_block_ = (meter_block_ + 1u) % 64u;
        state().set_value_rt(kMeterParam, static_cast<float>(meter_block_) / 64.0f);
        state().set_value_rt(kEchoParam, gain);
        push_output_param_event(kEchoParam, gain,
                                frames > 0 ? static_cast<std::uint32_t>(frames - 1) : 0);
    }

private:
    float coeff_ = 0.0f;
    float z_[2] = {0.0f, 0.0f};
    float gate_ = 0.0f;
    std::uint32_t meter_block_ = 0;
};

std::unique_ptr<Processor> make_parity_processor() {
    return std::make_unique<ParityProcessor>();
}

// ---------------------------------------------------------------------------
// Shared, deterministic stimulus. Both harnesses render THIS.
// ---------------------------------------------------------------------------

/// Seeded value-noise plus a sine, mixed per channel. Deterministic (a fixed
/// LCG seeded from a literal, never a clock or `random_device`) and broadband,
/// so a spectral or timing perturbation anywhere in the adapter has something to
/// perturb. Regenerated per sample rate so the sweep's stimulus tracks its cell.
pulp::audio::Buffer<float> make_parity_stimulus(double sample_rate, int channels,
                                                std::int64_t frames) {
    pulp::audio::Buffer<float> buf(static_cast<std::size_t>(channels),
                                   static_cast<std::size_t>(frames));
    for (int ch = 0; ch < channels; ++ch) {
        std::uint32_t seed = 0x5eed1234u + static_cast<std::uint32_t>(ch);
        auto c = buf.channel(static_cast<std::size_t>(ch));
        for (std::int64_t i = 0; i < frames; ++i) {
            seed = seed * 1664525u + 1013904223u;
            const float noise =
                static_cast<float>(static_cast<double>(seed >> 8) / 8388608.0 - 1.0);
            const float tone = static_cast<float>(
                std::sin(2.0 * 3.14159265358979323846 * 440.0 *
                         static_cast<double>(i) / sample_rate));
            c[static_cast<std::size_t>(i)] = 0.3f * noise + 0.4f * tone;
        }
    }
    return buf;
}

/// Parameter automation. Steps land on frames that are a multiple of the
/// largest block size in the sweep, so a step is at offset 0 of its block for
/// EVERY partition. That is what makes the two harnesses comparable at all:
/// `RenderScenario` applies a step block-quantized (store write before the
/// block), while the CLAP adapter applies a host event sample-accurately. At
/// offset 0 those coincide exactly — the adapter's sub-block split degenerates
/// to one sub-block spanning the block. An unaligned step would diverge for a
/// reason that is a documented harness difference, not an adapter fault, so the
/// null test deliberately does not put one there. (The mid-block case is
/// covered directly against the adapter by
/// `test_adapter_boundary_parity.cpp`'s sub-block split fixture.)
constexpr std::int64_t kParamStepAlign = 256;

std::vector<ParamStep> parity_param_script() {
    return {
        {kGainParam, 0 * kParamStepAlign, 0.80f},
        {kGainParam, 4 * kParamStepAlign, 1.30f},
        {kOffsetParam, 6 * kParamStepAlign, 0.10f},
        {kGainParam, 10 * kParamStepAlign, 0.55f},
        {kOffsetParam, 12 * kParamStepAlign, -0.20f},
    };
}

/// Note on/off pairs at frames that are deliberately NOT block-aligned, so the
/// gate lands mid-block for most partitions and an offset-decode fault moves a
/// sample. Both harnesses deliver MIDI sample-accurately, so this is a fair
/// comparison at any partition.
std::vector<MidiScriptEvent> parity_midi_script() {
    return {
        {101, midi::MidiEvent::note_on(0, 60, 100)},
        {733, midi::MidiEvent::note_off(0, 60, 0)},
        {1500, midi::MidiEvent::note_on(0, 64, 90)},
        {2049, midi::MidiEvent::note_off(0, 64, 0)},
        {3001, midi::MidiEvent::note_on(0, 67, 127)},
    };
}

// ---------------------------------------------------------------------------
// Path (a) — direct, through HeadlessHost via the sanctioned scenario layer.
// ---------------------------------------------------------------------------

RenderScenario parity_scenario(std::int64_t frames) {
    RenderScenario s(&make_parity_processor);
    s.name("adapter-audio-parity")
        .channels(2, 2)
        .duration_frames(frames)
        .input([](double sr, int ch, std::int64_t n) {
            return make_parity_stimulus(sr, ch, n);
        })
        .automate(parity_param_script())
        .midi(parity_midi_script());
    return s;
}

// ---------------------------------------------------------------------------
// Path (b) — through the real CLAP adapter's C surface.
// ---------------------------------------------------------------------------

/// One `clap_input_events_t` backed by a flat vector of already-built events.
/// The adapter reads events through this exactly as a host would.
struct InputEventList {
    std::vector<clap_event_midi_t> midi;
    std::vector<clap_event_param_value_t> params;
    /// Header pointers in the order the adapter will walk them.
    std::vector<const clap_event_header_t*> order;
    clap_input_events_t iface{};

    void rebuild_iface() {
        order.clear();
        // CLAP requires ascending sample-offset order across all event types.
        // Params first at their offset, then MIDI — both scripts are sorted by
        // frame and the param steps are all at offset 0.
        for (auto& p : params) order.push_back(&p.header);
        for (auto& m : midi) order.push_back(&m.header);
        std::stable_sort(order.begin(), order.end(),
                         [](const clap_event_header_t* a, const clap_event_header_t* b) {
                             return a->time < b->time;
                         });
        iface.ctx = this;
        iface.size = [](const clap_input_events_t* l) -> std::uint32_t {
            return static_cast<std::uint32_t>(
                static_cast<const InputEventList*>(l->ctx)->order.size());
        };
        iface.get = [](const clap_input_events_t* l,
                       std::uint32_t i) -> const clap_event_header_t* {
            return static_cast<const InputEventList*>(l->ctx)->order[i];
        };
    }
};

/// One parameter value the adapter published back to the host.
struct PublishedParam {
    std::uint32_t block = 0;
    std::uint32_t time = 0;
    clap_id param_id = 0;
    double value = 0.0;
};

/// Records everything the adapter pushes to `out_events`, so the publication
/// path can be asserted on content and ORDER (CLAP requires a globally
/// ascending sample-offset stream).
struct OutputEventRecorder {
    std::vector<PublishedParam> params;
    std::vector<std::uint32_t> all_times;   // every pushed event, in push order
    std::uint32_t block = 0;
    clap_output_events_t iface{};

    void rebuild_iface() {
        iface.ctx = this;
        iface.try_push = [](const clap_output_events_t* l,
                            const clap_event_header_t* h) -> bool {
            auto* self = static_cast<OutputEventRecorder*>(l->ctx);
            self->all_times.push_back(h->time);
            if (h->type == CLAP_EVENT_PARAM_VALUE &&
                h->space_id == CLAP_CORE_EVENT_SPACE_ID) {
                const auto* ev = reinterpret_cast<const clap_event_param_value_t*>(h);
                self->params.push_back({self->block, h->time, ev->param_id, ev->value});
            }
            return true;
        };
    }
};

struct ClapRender {
    pulp::audio::Buffer<float> output;
    OutputEventRecorder events;
};

/// Drive the real CLAP adapter block by block with the SAME scripts the
/// scenario uses, and return the concatenated output plus everything the
/// adapter published to the host.
ClapRender render_through_clap(double sample_rate, int block_size,
                               std::int64_t frames) {
    const auto input = make_parity_stimulus(sample_rate, 2, frames);
    const auto param_script = parity_param_script();
    const auto midi_script = parity_midi_script();

    clap_adapter::PulpClapPlugin plugin;
    plugin.factory = &make_parity_processor;
    plugin.plugin.plugin_data = &plugin;
    REQUIRE(clap_adapter::clap_init(&plugin.plugin));
    REQUIRE(clap_adapter::clap_activate(&plugin.plugin, sample_rate, 1,
                                        static_cast<std::uint32_t>(block_size)));

    ClapRender render;
    render.output.resize(2, static_cast<std::size_t>(frames));
    render.events.rebuild_iface();

    std::vector<float> in_l, in_r, out_l, out_r;
    std::size_t param_idx = 0, midi_idx = 0;
    std::uint32_t block_index = 0;

    for (std::int64_t pos = 0; pos < frames; pos += block_size) {
        const auto n = static_cast<std::size_t>(
            std::min<std::int64_t>(block_size, frames - pos));

        in_l.assign(input.channel(0).begin() + pos,
                    input.channel(0).begin() + pos + static_cast<std::int64_t>(n));
        in_r.assign(input.channel(1).begin() + pos,
                    input.channel(1).begin() + pos + static_cast<std::int64_t>(n));
        out_l.assign(n, 0.0f);
        out_r.assign(n, 0.0f);
        float* in_ptrs[2] = {in_l.data(), in_r.data()};
        float* out_ptrs[2] = {out_l.data(), out_r.data()};

        InputEventList events;
        for (; param_idx < param_script.size() &&
               param_script[param_idx].frame < pos + static_cast<std::int64_t>(n);
             ++param_idx) {
            const auto& step = param_script[param_idx];
            clap_event_param_value_t ev{};
            ev.header.size = sizeof(ev);
            ev.header.type = CLAP_EVENT_PARAM_VALUE;
            ev.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
            ev.header.time = static_cast<std::uint32_t>(
                std::max<std::int64_t>(step.frame - pos, 0));
            ev.param_id = step.id;
            ev.note_id = -1;
            ev.port_index = -1;
            ev.channel = -1;
            ev.key = -1;
            ev.value = static_cast<double>(step.value);
            events.params.push_back(ev);
        }
        for (; midi_idx < midi_script.size() &&
               midi_script[midi_idx].frame < pos + static_cast<std::int64_t>(n);
             ++midi_idx) {
            const auto& scripted = midi_script[midi_idx];
            // Raw MIDI, not CLAP_EVENT_NOTE_ON: the adapter reconstructs a
            // note event's velocity as `uint8_t(velocity * 127.0)`, which is a
            // lossy round-trip. Handing it the same three bytes the direct path
            // renders keeps the stimulus itself bit-identical, so the null test
            // measures the adapter and not the note encoding.
            clap_event_midi_t ev{};
            ev.header.size = sizeof(ev);
            ev.header.type = CLAP_EVENT_MIDI;
            ev.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
            ev.header.time = static_cast<std::uint32_t>(
                std::max<std::int64_t>(scripted.frame - pos, 0));
            ev.port_index = 0;
            const std::uint8_t* bytes = scripted.event.data();
            ev.data[0] = bytes[0];
            ev.data[1] = bytes[1];
            ev.data[2] = bytes[2];
            events.midi.push_back(ev);
        }
        events.rebuild_iface();

        clap_audio_buffer_t in_bus{};
        in_bus.channel_count = 2;
        in_bus.data32 = in_ptrs;
        clap_audio_buffer_t out_bus{};
        out_bus.channel_count = 2;
        out_bus.data32 = out_ptrs;

        render.events.block = block_index;
        clap_process_t proc{};
        proc.frames_count = static_cast<std::uint32_t>(n);
        proc.audio_inputs = &in_bus;
        proc.audio_inputs_count = 1;
        proc.audio_outputs = &out_bus;
        proc.audio_outputs_count = 1;
        proc.in_events = &events.iface;
        proc.out_events = &render.events.iface;

        REQUIRE(clap_adapter::clap_process(&plugin.plugin, &proc) ==
                CLAP_PROCESS_CONTINUE);

        std::copy_n(out_l.begin(), n, render.output.channel(0).begin() + pos);
        std::copy_n(out_r.begin(), n, render.output.channel(1).begin() + pos);
        ++block_index;
    }

    clap_adapter::clap_deactivate(&plugin.plugin);
    return render;
}

// ---------------------------------------------------------------------------
// Bit-exact comparison.
// ---------------------------------------------------------------------------

/// First differing sample between two renders, or -1. Compares BITS: an
/// adapter refactor that only moves code must not perturb a single ULP, so
/// there is no tolerance to spend and a near-miss is still a failure.
struct FirstDiff {
    std::int64_t index = -1;
    std::size_t channel = 0;
    float direct = 0.0f;
    float adapter = 0.0f;
};

FirstDiff first_bitwise_difference(const pulp::audio::Buffer<float>& direct,
                                   const pulp::audio::Buffer<float>& adapter) {
    const auto channels = std::min(direct.num_channels(), adapter.num_channels());
    const auto frames = std::min(direct.num_samples(), adapter.num_samples());
    for (std::size_t i = 0; i < frames; ++i) {
        for (std::size_t ch = 0; ch < channels; ++ch) {
            const float a = direct.channel(ch)[i];
            const float b = adapter.channel(ch)[i];
            if (std::memcmp(&a, &b, sizeof(float)) != 0)
                return {static_cast<std::int64_t>(i), ch, a, b};
        }
    }
    return {};
}

bool all_finite(const pulp::audio::Buffer<float>& buf) {
    for (std::size_t ch = 0; ch < buf.num_channels(); ++ch)
        for (std::size_t i = 0; i < buf.num_samples(); ++i)
            if (!std::isfinite(buf.channel(ch)[i])) return false;
    return true;
}

constexpr std::int64_t kSweepFrames = 4096;   // divisible by every swept block
constexpr double kSweepRates[] = {44100.0, 48000.0, 96000.0};
constexpr int kSweepBlocks[] = {1, 16, 64, 128, 256};

}  // namespace


// ===========================================================================
// The null test.
// ===========================================================================

TEST_CASE("adapter audio parity: the CLAP adapter emits the same bits as a direct render",
          "[format][clap][parity][audio]") {
    for (double sr : kSweepRates) {
        for (int block : kSweepBlocks) {
            INFO("sample_rate=" << sr << " block=" << block);
            auto direct = parity_scenario(kSweepFrames).sample_rate(sr)
                              .block_size(block).render();
            auto through = render_through_clap(sr, block, kSweepFrames);

            REQUIRE(direct.output.num_samples() == through.output.num_samples());
            const auto diff = first_bitwise_difference(direct.output, through.output);
            INFO("first differing sample index=" << diff.index
                 << " channel=" << diff.channel
                 << " direct=" << diff.direct << " adapter=" << diff.adapter);
            CHECK(diff.index == -1);
        }
    }
}

TEST_CASE("adapter audio parity: a ragged final block does not perturb the stream",
          "[format][clap][parity][audio]") {
    // A render length that is NOT a multiple of the block size makes the
    // adapter's last block short. A phase that assumes a full block (a scratch
    // resize, a zero-fill, an offset clamp) shows up here and nowhere else.
    constexpr std::int64_t kRagged = 4096 + 37;
    for (int block : {64, 128, 256}) {
        INFO("block=" << block << " frames=" << kRagged);
        auto direct = parity_scenario(kRagged).sample_rate(48000.0)
                          .block_size(block).render();
        auto through = render_through_clap(48000.0, block, kRagged);

        REQUIRE(direct.output.num_samples() == through.output.num_samples());
        const auto diff = first_bitwise_difference(direct.output, through.output);
        INFO("first differing sample index=" << diff.index
             << " direct=" << diff.direct << " adapter=" << diff.adapter);
        CHECK(diff.index == -1);
    }
}

TEST_CASE("adapter audio parity: silence and DC survive the adapter unchanged",
          "[format][clap][parity][audio]") {
    // Two stimuli an adapter is most likely to special-case: an all-zero block
    // (a host may flag it constant / silent) and a constant non-zero block (DC,
    // which a silence heuristic that tests "is it changing" would wrongly drop).
    // Both must still null against the direct render.
    struct Case { const char* name; float level; };
    for (const Case c : {Case{"silence", 0.0f}, Case{"dc", 0.5f}}) {
        INFO("stimulus=" << c.name);
        constexpr std::int64_t kFrames = 1024;
        constexpr int kBlock = 128;

        RenderScenario s(&make_parity_processor);
        s.name(std::string("adapter-audio-parity.") + c.name)
            .channels(2, 2)
            .sample_rate(48000.0)
            .block_size(kBlock)
            .duration_frames(kFrames)
            .input([level = c.level](double, int ch, std::int64_t n) {
                return pulp::test::audio::make_dc(ch, static_cast<int>(n), level);
            });
        auto direct = s.render();

        // Drive the adapter with the identical constant stimulus.
        clap_adapter::PulpClapPlugin plugin;
        plugin.factory = &make_parity_processor;
        plugin.plugin.plugin_data = &plugin;
        REQUIRE(clap_adapter::clap_init(&plugin.plugin));
        REQUIRE(clap_adapter::clap_activate(&plugin.plugin, 48000.0, 1, kBlock));

        pulp::audio::Buffer<float> adapter_out(2, kFrames);
        for (std::int64_t pos = 0; pos < kFrames; pos += kBlock) {
            std::vector<float> in_l(kBlock, c.level), in_r(kBlock, c.level);
            std::vector<float> out_l(kBlock, 0.0f), out_r(kBlock, 0.0f);
            float* in_ptrs[2] = {in_l.data(), in_r.data()};
            float* out_ptrs[2] = {out_l.data(), out_r.data()};
            clap_audio_buffer_t in_bus{};
            in_bus.channel_count = 2;
            in_bus.data32 = in_ptrs;
            clap_audio_buffer_t out_bus{};
            out_bus.channel_count = 2;
            out_bus.data32 = out_ptrs;
            clap_process_t proc{};
            proc.frames_count = kBlock;
            proc.audio_inputs = &in_bus;
            proc.audio_inputs_count = 1;
            proc.audio_outputs = &out_bus;
            proc.audio_outputs_count = 1;
            REQUIRE(clap_adapter::clap_process(&plugin.plugin, &proc) ==
                    CLAP_PROCESS_CONTINUE);
            std::copy_n(out_l.begin(), kBlock, adapter_out.channel(0).begin() + pos);
            std::copy_n(out_r.begin(), kBlock, adapter_out.channel(1).begin() + pos);
        }
        clap_adapter::clap_deactivate(&plugin.plugin);

        const auto diff = first_bitwise_difference(direct.output, adapter_out);
        INFO("first differing sample index=" << diff.index
             << " direct=" << diff.direct << " adapter=" << diff.adapter);
        CHECK(diff.index == -1);
    }
}

TEST_CASE("adapter audio parity: the adapter never introduces a non-finite sample",
          "[format][clap][parity][audio]") {
    auto through = render_through_clap(48000.0, 128, kSweepFrames);
    CHECK(all_finite(through.output));
    auto direct = parity_scenario(kSweepFrames).sample_rate(48000.0)
                      .block_size(128).render();
    CHECK(all_finite(direct.output));
}

// ===========================================================================
// Output-parameter publication through the real adapter.
//
// The audio null above cannot see this: a plugin-side parameter change never
// reaches the samples, so a publication fault is silent in the waveform. These
// assert the published stream directly.
// ===========================================================================

TEST_CASE("adapter audio parity: plugin-side param changes reach the host every block",
          "[format][clap][parity][params]") {
    constexpr int kBlock = 128;
    auto through = render_through_clap(48000.0, kBlock, kSweepFrames);
    const auto blocks = static_cast<std::uint32_t>(kSweepFrames / kBlock);

    // `Echo` is pushed explicitly at the block's last frame AND written to the
    // store. Exactly one event per block, at the explicit offset: the skip-set
    // must suppress the offset-0 snapshot fallback for it. Two events per block
    // (or any at offset 0) means the skip-set marked the wrong parameter — the
    // exact failure an off-by-one in the param-index lookup produces.
    std::vector<std::uint32_t> echo_blocks;
    for (const auto& p : through.events.params) {
        if (p.param_id != kEchoParam) continue;
        CHECK(p.time == static_cast<std::uint32_t>(kBlock - 1));
        echo_blocks.push_back(p.block);
    }
    CHECK(echo_blocks.size() == blocks);
    CHECK(std::adjacent_find(echo_blocks.begin(), echo_blocks.end()) ==
          echo_blocks.end());  // no block published Echo twice

    // `Meter` has no explicit event, so it travels through the snapshot-diff
    // fallback at offset 0. The processor writes a distinct value every block,
    // so the diff must fire on every one: a publication path that resolved the
    // parameter index wrongly, or dropped the fallback, loses events here.
    std::size_t meter_events = 0;
    for (const auto& p : through.events.params) {
        if (p.param_id != kMeterParam) continue;
        CHECK(p.time == 0u);
        ++meter_events;
    }
    CHECK(meter_events == blocks);

    // A parameter the host drove itself (Gain) is not republished back at it:
    // the snapshot is taken after the host's event is applied, so there is no
    // diff to report. Catches a publication path that echoes host automation.
    for (const auto& p : through.events.params)
        CHECK(p.param_id != static_cast<clap_id>(kGainParam));
}

TEST_CASE("adapter audio parity: published events stay in ascending sample order",
          "[format][clap][parity][params]") {
    // CLAP requires out_events to be pushed in globally ascending sample-offset
    // order across every event type. The publication path emits the snapshot
    // fallback (offset 0) and the explicit events (their own offsets) from two
    // different loops, so the ordering is a real invariant of the merge and not
    // a property of either loop alone.
    auto through = render_through_clap(48000.0, 128, 1024);
    // `all_times` is recorded across the whole render; ordering is per-block, so
    // re-derive block boundaries from the recorded params' block index.
    std::vector<std::vector<std::uint32_t>> per_block(8);
    for (const auto& p : through.events.params) {
        REQUIRE(p.block < per_block.size());
        per_block[p.block].push_back(p.time);
    }
    for (std::size_t b = 0; b < per_block.size(); ++b) {
        INFO("block " << b);
        CHECK(std::is_sorted(per_block[b].begin(), per_block[b].end()));
    }
}
