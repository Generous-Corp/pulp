// VST3-adapter-vs-direct audio null test.
//
// `test_adapter_audio_parity.cpp` pins the same claim for CLAP: a block a
// Processor renders through HeadlessHost and the same block rendered through
// the real adapter must be the same bits. The VST3 adapter had no equivalent.
// `test_adapter_boundary_parity.cpp` drives VST3 only as a neutral-struct
// matrix column — it never enters `PulpVst3Processor::process()` — so the VST3
// process() phase structure and its use of the shared output-parameter
// publication helpers (`find_param_index` / `snapshot_param_values` /
// `changed_since_snapshot`) rested on diff review rather than measurement.
// This file closes that gap by driving the real Steinberg `IAudioProcessor`
// surface: ProcessData, AudioBusBuffers, IEventList, IParameterChanges in and
// out — the same plumbing a host builds.
//
// The claim under test is that the adapter is TRANSPARENT: it does not filter,
// resample, or round anything on the normal (non-bypassed) path, so there is no
// tolerance to spend. Any reordering of the adapter's phases (params after the
// snapshot, MIDI decoded after the DSP call, a dropped or duplicated block)
// perturbs the output, and the direct render — which shares no adapter code —
// does not move with it. The null is therefore a real discriminator, not a
// self-consistency check.
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
//   * it writes one parameter and pushes several explicit output param events,
//     so the host-facing publication path runs on every block, on both of its
//     branches, and with a queue that carries more than one point.
//
// The sweep is (sample rate × block size): the adapter's per-block function is
// re-entered every block, so a phase-ordering fault shows up as a block-boundary
// artifact and is loudest at the extreme partitions (block=1 and a ragged tail).

#include <catch2/catch_test_macros.hpp>

#include "support/audio_signal_generators.hpp"
#include "support/render_scenario.hpp"

#include <pulp/audio/buffer.hpp>
#include <pulp/format/processor.hpp>
#include <pulp/format/quirk_apply.hpp>
#include <pulp/format/vst3_adapter.hpp>
#include <pulp/midi/message.hpp>
#include <pulp/state/store.hpp>

#include <pluginterfaces/vst/ivsteditcontroller.h>
#include <public.sdk/source/vst/hosting/eventlist.h>
#include <public.sdk/source/vst/hosting/parameterchanges.h>

#include <algorithm>
#include <array>
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

namespace SpeakerArr = Steinberg::Vst::SpeakerArr;

// ---------------------------------------------------------------------------
// The processor under both harnesses.
// ---------------------------------------------------------------------------

constexpr state::ParamID kGainParam = 1;
constexpr state::ParamID kOffsetParam = 2;
constexpr state::ParamID kMeterParam = 3;   // plugin-written (snapshot-diff path)
constexpr state::ParamID kEchoParam = 4;    // plugin-written (explicit-event path)

/// The ranges the fixture registers. Held here as well so the harness can
/// normalize a plain value the way the host would before handing it to the
/// adapter, and so the round-trip can be asserted exact (see the
/// `normalize/denormalize` REQUIREs in `vst3_param_round_trip_is_exact`).
const state::ParamRange kGainRange{0.0f, 2.0f, 1.0f, 0.0f};
const state::ParamRange kOffsetRange{-1.0f, 1.0f, 0.0f, 0.0f};

/// One explicit output parameter event the fixture pushes for `Echo`.
struct EchoEvent {
    std::uint32_t offset;
    float value;
};

/// The `Echo` events the fixture pushes for a block of `frames` samples while
/// `gain` is in force, IN PUSH ORDER — descending by offset, which is NOT the
/// order VST3 requires them to reach the host in. The adapter is what must sort
/// them. Held as a free function so the assertions below predict the published
/// stream from the same source the processor emits it from.
///
/// The multipliers are dyadic and `gain` is dyadic, so every value is exact in
/// float and survives normalization onto Echo's linear 0..2 range bit-for-bit —
/// which is what lets the published values be asserted as bits.
///
/// A block shorter than 3 frames collapses these onto repeated offsets. That is
/// deliberate and harmless: the ordering claim is `ascending`, not `strictly
/// ascending`, and the param cases below run at block sizes where the three
/// offsets are distinct.
std::array<EchoEvent, 3> echo_events(float gain, std::size_t frames) {
    const auto last = frames > 0 ? static_cast<std::uint32_t>(frames - 1) : 0u;
    const auto mid = static_cast<std::uint32_t>(frames / 2);
    return {EchoEvent{last, gain},
            EchoEvent{mid, gain * 0.5f},
            EchoEvent{0, gain * 0.25f}};
}

/// The same events in the ascending-offset order the adapter must publish them
/// in — the prediction the published stream is checked against.
std::array<EchoEvent, 3> expected_published_echo(float gain, std::size_t frames) {
    auto e = echo_events(gain, frames);
    std::stable_sort(e.begin(), e.end(),
                     [](const EchoEvent& a, const EchoEvent& b) {
                         return a.offset < b.offset;
                     });
    return e;
}

/// Deterministic, stateful, sample-rate-dependent, MIDI- and parameter-driven.
/// Every arithmetic step is plain float arithmetic evaluated in a fixed order,
/// so two renders of the same stimulus at the same block size are bit-identical
/// by construction — which is exactly what lets the null test below assert bits
/// rather than a tolerance.
class Vst3ParityProcessor : public Processor {
public:
    PluginDescriptor descriptor() const override {
        PluginDescriptor d;
        d.name = "PulpVst3AudioParity";
        d.manufacturer = "PulpTest";
        d.bundle_id = "com.pulp.test.vst3-audio-parity";
        d.version = "1.0.0";
        d.category = PluginCategory::Effect;
        d.input_buses = {{"In", 2}};
        d.output_buses = {{"Out", 2}};
        d.accepts_midi = true;
        return d;
    }

    void define_parameters(state::StateStore& store) override {
        store.add_parameter({.id = kGainParam, .name = "Gain", .range = kGainRange});
        store.add_parameter({.id = kOffsetParam, .name = "Offset", .range = kOffsetRange});
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
                // off-by-one in the adapter's offset decode moves a sample.
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
        //     counter rather than the block peak: a repeated value produces no
        //     diff and so no event, which would force the test to settle for
        //     "some blocks published" — a bar a broken publication path clears.
        //     A value that is distinct every block makes "exactly one per block"
        //     true, so a dropped publication fails;
        //   * `Echo` is BOTH pushed as explicit sample-accurate events AND
        //     written to the store. That combination is what exercises the
        //     skip-set: the explicit events must suppress the snapshot fallback
        //     for the same parameter, or the host records the change twice.
        //     Only a parameter on both branches can catch a skip-set that marks
        //     the wrong index.
        //
        //     It pushes THREE events per block, at descending offsets. One
        //     event per block would make a single-point queue, and a queue with
        //     one point is ascending no matter what the adapter does — the
        //     per-queue sample-offset ordering VST3 requires would be
        //     unfalsifiable. Three points make it a real property, and pushing
        //     them out of order puts the adapter's sort on the hook for
        //     establishing it: a processor is free to emit events in any order,
        //     so the ascending guarantee is the adapter's to keep, not the
        //     processor's to supply. The value carried is a function of the
        //     offset, so a sort that moved an offset without its value is
        //     caught too.
        meter_block_ = (meter_block_ + 1u) % 64u;
        state().set_value_rt(kMeterParam, static_cast<float>(meter_block_) / 64.0f);
        state().set_value_rt(kEchoParam, gain);
        for (const auto& e : echo_events(gain, frames))
            push_output_param_event(kEchoParam, e.value, e.offset);
    }

private:
    float coeff_ = 0.0f;
    float z_[2] = {0.0f, 0.0f};
    float gate_ = 0.0f;
    std::uint32_t meter_block_ = 0;
};

std::unique_ptr<Processor> make_parity_processor() {
    return std::make_unique<Vst3ParityProcessor>();
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
/// block), while the VST3 adapter applies a host point sample-accurately. At
/// offset 0 those coincide exactly. An unaligned step would diverge for a reason
/// that is a documented harness difference, not an adapter fault, so the null
/// test deliberately does not put one there. (The mid-block case is covered
/// directly against the adapter by `test_vst3_plugin_state.cpp`'s
/// sample-accurate parameter-point fixture.)
///
/// Every value is a dyadic rational chosen so `denormalize(normalize(v)) == v`
/// bitwise on its linear range — the host boundary is normalized 0..1, and a
/// value that did not survive that round-trip would make the two paths differ
/// for a reason that is the range math, not the adapter. The round-trip is
/// asserted rather than assumed; see `vst3_param_round_trip_is_exact`.
constexpr std::int64_t kParamStepAlign = 256;

std::vector<ParamStep> parity_param_script() {
    return {
        {kGainParam, 0 * kParamStepAlign, 0.75f},
        {kGainParam, 4 * kParamStepAlign, 1.25f},
        {kOffsetParam, 6 * kParamStepAlign, 0.125f},
        {kGainParam, 10 * kParamStepAlign, 0.5f},
        {kOffsetParam, 12 * kParamStepAlign, -0.25f},
    };
}

/// The range a scripted parameter id belongs to.
const state::ParamRange& range_for(state::ParamID id) {
    return id == kGainParam ? kGainRange : kOffsetRange;
}

// ---------------------------------------------------------------------------
// MIDI script.
//
// VST3 has no raw-short-message event: notes arrive as kNoteOnEvent /
// kNoteOffEvent with a FLOAT velocity, and the adapter reconstructs a MIDI byte
// as `uint8_t(velocity * 127.0f)`. That reconstruction is a documented VST3
// boundary, not the thing under test, so the script is authored in VST3 terms
// and the direct path is handed the byte that conversion produces. The
// velocities are dyadic so the decode is exact and checkable by hand
// (0.5*127 = 63.5 -> 63), which keeps the stimulus itself bit-identical and
// leaves the null measuring the adapter rather than the note encoding.
// ---------------------------------------------------------------------------

struct NoteScriptEvent {
    std::int64_t frame;
    bool on;
    std::uint8_t channel;
    std::uint8_t pitch;
    float velocity;   ///< VST3 normalized note velocity
};

/// The MIDI velocity byte the adapter reconstructs from a VST3 float velocity.
std::uint8_t decoded_velocity(float velocity) {
    return static_cast<std::uint8_t>(velocity * 127.0f);
}

/// Note on/off pairs at frames that are deliberately NOT block-aligned, so the
/// gate lands mid-block for most partitions and an offset-decode fault moves a
/// sample. Both harnesses deliver MIDI sample-accurately, so this is a fair
/// comparison at any partition.
std::vector<NoteScriptEvent> parity_note_script() {
    return {
        {101, true, 0, 60, 0.5f},     // -> velocity 63
        {733, false, 0, 60, 0.25f},   // -> velocity 31
        {1500, true, 0, 64, 0.75f},   // -> velocity 95
        {2049, false, 0, 64, 0.25f},  // -> velocity 31
        {3001, true, 0, 67, 0.5f},    // -> velocity 63
    };
}

/// The same script expressed as the plain MIDI bytes the direct path renders.
std::vector<MidiScriptEvent> parity_midi_script() {
    std::vector<MidiScriptEvent> out;
    for (const auto& n : parity_note_script()) {
        const auto vel = decoded_velocity(n.velocity);
        out.push_back({n.frame, n.on ? midi::MidiEvent::note_on(n.channel, n.pitch, vel)
                                     : midi::MidiEvent::note_off(n.channel, n.pitch, vel)});
    }
    return out;
}

// ---------------------------------------------------------------------------
// Path (a) — direct, through HeadlessHost via the sanctioned scenario layer.
// ---------------------------------------------------------------------------

RenderScenario parity_scenario(std::int64_t frames) {
    RenderScenario s(&make_parity_processor);
    s.name("vst3-audio-parity")
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
// Path (b) — through the real VST3 adapter's IAudioProcessor surface.
// ---------------------------------------------------------------------------

/// Minimal IHostApplication. `PulpVst3Processor::initialize()` requires a host
/// context; nothing in the audio path calls back into it.
class HostApp final : public Steinberg::Vst::IHostApplication {
public:
    Steinberg::tresult PLUGIN_API getName(Steinberg::Vst::String128 name) override {
        const char* kName = "PulpTest";
        for (int i = 0; i < 8; ++i) name[i] = static_cast<Steinberg::Vst::TChar>(kName[i]);
        name[8] = 0;
        return Steinberg::kResultTrue;
    }

    Steinberg::tresult PLUGIN_API createInstance(Steinberg::TUID, Steinberg::TUID,
                                                 void** obj) override {
        if (obj) *obj = nullptr;
        return Steinberg::kNotImplemented;
    }

    Steinberg::tresult PLUGIN_API queryInterface(const Steinberg::TUID iid,
                                                 void** obj) override {
        if (Steinberg::FUnknownPrivate::iidEqual(iid, Steinberg::Vst::IHostApplication::iid) ||
            Steinberg::FUnknownPrivate::iidEqual(iid, Steinberg::FUnknown::iid)) {
            *obj = static_cast<Steinberg::Vst::IHostApplication*>(this);
            return Steinberg::kResultTrue;
        }
        *obj = nullptr;
        return Steinberg::kNoInterface;
    }

    Steinberg::uint32 PLUGIN_API addRef() override { return 1; }
    Steinberg::uint32 PLUGIN_API release() override { return 1; }
};

/// One parameter point the adapter published back to the host, with the block
/// it was published in. VST3 carries NORMALIZED values over this boundary.
struct PublishedParam {
    std::uint32_t block = 0;
    Steinberg::int32 offset = 0;
    Steinberg::Vst::ParamID param_id = 0;
    double normalized = 0.0;
};

/// What the adapter's EditController half reported for a parameter at the end of
/// a block, read back through `getParamNormalized`.
struct ControllerRead {
    std::uint32_t block = 0;
    Steinberg::Vst::ParamID param_id = 0;
    double normalized = 0.0;
};

struct Vst3Render {
    pulp::audio::Buffer<float> output;
    std::vector<PublishedParam> params;
    std::vector<ControllerRead> controller;
};

/// A host-side IParamValueQueue that records `addPoint` calls VERBATIM: in call
/// order, with no sorting and no same-offset coalescing.
///
/// The SDK's own `Vst::ParameterChanges` CANNOT observe point order.
/// `ParameterValueQueue::addPoint` walks the queue and inserts each point at its
/// sorted position (overwriting a point that repeats an offset), so reading it
/// back always yields an ascending, de-duplicated queue no matter what order the
/// plug-in called it in. Against that container the ordering invariant is
/// enforced by the host helper, not by the adapter, and a test that reads it can
/// never fail — the adapter could publish its points in reverse and the SDK
/// would tidy them up before the test looked.
///
/// VST3 puts the ordering obligation on the PLUG-IN: the points it adds to one
/// IParamValueQueue must ascend by sample offset. A host is not required to use
/// the SDK helper, and one that appends what it is given keeps whatever order
/// the plug-in produced. This recorder is that strict host — it observes exactly
/// what the adapter did, which is the thing under test.
class RecordingParamValueQueue final : public Steinberg::Vst::IParamValueQueue {
public:
    explicit RecordingParamValueQueue(Steinberg::Vst::ParamID id) : id_(id) {}

    Steinberg::Vst::ParamID PLUGIN_API getParameterId() override { return id_; }
    Steinberg::int32 PLUGIN_API getPointCount() override {
        return static_cast<Steinberg::int32>(points_.size());
    }

    Steinberg::tresult PLUGIN_API getPoint(Steinberg::int32 index,
                                           Steinberg::int32& sampleOffset,
                                           Steinberg::Vst::ParamValue& value) override {
        if (index < 0 || static_cast<std::size_t>(index) >= points_.size())
            return Steinberg::kResultFalse;
        const auto& p = points_[static_cast<std::size_t>(index)];
        sampleOffset = p.offset;
        value = p.value;
        return Steinberg::kResultTrue;
    }

    Steinberg::tresult PLUGIN_API addPoint(Steinberg::int32 sampleOffset,
                                           Steinberg::Vst::ParamValue value,
                                           Steinberg::int32& index) override {
        index = static_cast<Steinberg::int32>(points_.size());
        points_.push_back({sampleOffset, value});
        return Steinberg::kResultTrue;
    }

    Steinberg::tresult PLUGIN_API queryInterface(const Steinberg::TUID iid,
                                                 void** obj) override {
        if (Steinberg::FUnknownPrivate::iidEqual(iid, Steinberg::Vst::IParamValueQueue::iid) ||
            Steinberg::FUnknownPrivate::iidEqual(iid, Steinberg::FUnknown::iid)) {
            *obj = static_cast<Steinberg::Vst::IParamValueQueue*>(this);
            return Steinberg::kResultTrue;
        }
        *obj = nullptr;
        return Steinberg::kNoInterface;
    }
    Steinberg::uint32 PLUGIN_API addRef() override { return 1; }
    Steinberg::uint32 PLUGIN_API release() override { return 1; }

private:
    struct Point {
        Steinberg::int32 offset;
        Steinberg::Vst::ParamValue value;
    };
    Steinberg::Vst::ParamID id_;
    std::vector<Point> points_;
};

/// The IParameterChanges the adapter publishes into. `addParameterData` returns
/// the EXISTING queue when a parameter id repeats, matching the SDK's contract —
/// that is what makes a double-publication (an explicit event plus a snapshot
/// fallback for the same parameter) land in one queue, where it is visible as
/// both an extra point and an out-of-order one.
class RecordingParameterChanges final : public Steinberg::Vst::IParameterChanges {
public:
    Steinberg::int32 PLUGIN_API getParameterCount() override {
        return static_cast<Steinberg::int32>(queues_.size());
    }

    Steinberg::Vst::IParamValueQueue* PLUGIN_API getParameterData(
        Steinberg::int32 index) override {
        if (index < 0 || static_cast<std::size_t>(index) >= queues_.size()) return nullptr;
        return queues_[static_cast<std::size_t>(index)].get();
    }

    Steinberg::Vst::IParamValueQueue* PLUGIN_API addParameterData(
        const Steinberg::Vst::ParamID& id, Steinberg::int32& index) override {
        for (std::size_t i = 0; i < queues_.size(); ++i) {
            if (queues_[i]->getParameterId() == id) {
                index = static_cast<Steinberg::int32>(i);
                return queues_[i].get();
            }
        }
        index = static_cast<Steinberg::int32>(queues_.size());
        queues_.push_back(std::make_unique<RecordingParamValueQueue>(id));
        return queues_.back().get();
    }

    Steinberg::tresult PLUGIN_API queryInterface(const Steinberg::TUID iid,
                                                 void** obj) override {
        if (Steinberg::FUnknownPrivate::iidEqual(iid, Steinberg::Vst::IParameterChanges::iid) ||
            Steinberg::FUnknownPrivate::iidEqual(iid, Steinberg::FUnknown::iid)) {
            *obj = static_cast<Steinberg::Vst::IParameterChanges*>(this);
            return Steinberg::kResultTrue;
        }
        *obj = nullptr;
        return Steinberg::kNoInterface;
    }
    Steinberg::uint32 PLUGIN_API addRef() override { return 1; }
    Steinberg::uint32 PLUGIN_API release() override { return 1; }

private:
    std::vector<std::unique_ptr<RecordingParamValueQueue>> queues_;
};

/// Drain everything the adapter wrote into this block's output
/// IParameterChanges, preserving the per-queue point order.
void collect_published(RecordingParameterChanges& changes,
                       std::uint32_t block, std::vector<PublishedParam>& out) {
    const Steinberg::int32 count = changes.getParameterCount();
    for (Steinberg::int32 i = 0; i < count; ++i) {
        auto* queue = changes.getParameterData(i);
        if (!queue) continue;
        const Steinberg::int32 points = queue->getPointCount();
        for (Steinberg::int32 p = 0; p < points; ++p) {
            Steinberg::int32 offset = 0;
            Steinberg::Vst::ParamValue value = 0.0;
            if (queue->getPoint(p, offset, value) != Steinberg::kResultTrue) continue;
            out.push_back({block, offset, queue->getParameterId(),
                           static_cast<double>(value)});
        }
    }
}

/// Drive the real VST3 adapter block by block with the SAME scripts the
/// scenario uses, and return the concatenated output plus everything the
/// adapter published to the host.
Vst3Render render_through_vst3(double sample_rate, int block_size,
                               std::int64_t frames) {
    const auto input = make_parity_stimulus(sample_rate, 2, frames);
    const auto param_script = parity_param_script();
    const auto note_script = parity_note_script();

    HostApp host_app;
    vst3::PulpVst3Processor processor(&make_parity_processor);
    REQUIRE(processor.initialize(&host_app) == Steinberg::kResultOk);

    // Pin the parameter layout the publication path's index reasoning rests on.
    // The fixture registers four, but the adapter synthesizes a host-facing
    // Bypass on top of them (the default `synthesize_bypass_parameter`
    // accommodation), so the store carries FIVE and the synthesized entry is
    // last. Every per-block scratch vector the publication path resolves an
    // index into is sized off that list, so a change here silently moves what
    // `find_param_index` returns — assert it rather than assume it.
    const std::array<Steinberg::Vst::ParamID, 5> kExpectedParams{
        kGainParam, kOffsetParam, kMeterParam, kEchoParam,
        kSynthesizedBypassParamId};
    for (Steinberg::int32 i = 0; i < 5; ++i) {
        Steinberg::Vst::ParameterInfo info{};
        REQUIRE(processor.getParameterInfo(i, info) == Steinberg::kResultOk);
        INFO("parameter index " << i);
        REQUIRE(info.id == kExpectedParams[static_cast<std::size_t>(i)]);
    }

    Steinberg::Vst::SpeakerArrangement in_arr[1] = {SpeakerArr::kStereo};
    Steinberg::Vst::SpeakerArrangement out_arr[1] = {SpeakerArr::kStereo};
    REQUIRE(processor.setBusArrangements(in_arr, 1, out_arr, 1) == Steinberg::kResultTrue);

    Steinberg::Vst::ProcessSetup setup{};
    setup.processMode = Steinberg::Vst::kRealtime;
    setup.symbolicSampleSize = Steinberg::Vst::kSample32;
    setup.maxSamplesPerBlock = block_size;
    setup.sampleRate = sample_rate;
    REQUIRE(processor.setupProcessing(setup) == Steinberg::kResultOk);
    REQUIRE(processor.setActive(true) == Steinberg::kResultOk);

    Vst3Render render;
    render.output.resize(2, static_cast<std::size_t>(frames));

    std::vector<float> in_l, in_r, out_l, out_r;
    std::size_t param_idx = 0, note_idx = 0;
    std::uint32_t block_index = 0;

    for (std::int64_t pos = 0; pos < frames; pos += block_size) {
        const auto n = static_cast<std::size_t>(
            std::min<std::int64_t>(block_size, frames - pos));

        in_l.assign(input.channel(0).begin() + pos,
                    input.channel(0).begin() + pos + static_cast<std::int64_t>(n));
        in_r.assign(input.channel(1).begin() + pos,
                    input.channel(1).begin() + pos + static_cast<std::int64_t>(n));
        // Fill the output with a sentinel, not silence: a phase that fails to
        // write a channel would otherwise be indistinguishable from one that
        // legitimately rendered zeros.
        out_l.assign(n, 9.0f);
        out_r.assign(n, 9.0f);
        float* in_ptrs[2] = {in_l.data(), in_r.data()};
        float* out_ptrs[2] = {out_l.data(), out_r.data()};

        Steinberg::Vst::AudioBusBuffers audio_in[1]{};
        audio_in[0].numChannels = 2;
        audio_in[0].channelBuffers32 = in_ptrs;
        Steinberg::Vst::AudioBusBuffers audio_out[1]{};
        audio_out[0].numChannels = 2;
        audio_out[0].channelBuffers32 = out_ptrs;

        // Host parameter automation for this block, as normalized points.
        Steinberg::Vst::ParameterChanges input_params(4);
        for (; param_idx < param_script.size() &&
               param_script[param_idx].frame < pos + static_cast<std::int64_t>(n);
             ++param_idx) {
            const auto& step = param_script[param_idx];
            Steinberg::int32 qi = 0;
            auto* queue = input_params.addParameterData(
                static_cast<Steinberg::Vst::ParamID>(step.id), qi);
            REQUIRE(queue != nullptr);
            Steinberg::int32 pt = 0;
            const auto offset = static_cast<Steinberg::int32>(
                std::max<std::int64_t>(step.frame - pos, 0));
            REQUIRE(queue->addPoint(
                        offset,
                        static_cast<Steinberg::Vst::ParamValue>(
                            range_for(step.id).normalize(step.value)),
                        pt) == Steinberg::kResultTrue);
        }

        Steinberg::Vst::EventList input_events(8);
        for (; note_idx < note_script.size() &&
               note_script[note_idx].frame < pos + static_cast<std::int64_t>(n);
             ++note_idx) {
            const auto& s = note_script[note_idx];
            Steinberg::Vst::Event evt{};
            evt.sampleOffset = static_cast<Steinberg::int32>(
                std::max<std::int64_t>(s.frame - pos, 0));
            if (s.on) {
                evt.type = Steinberg::Vst::Event::kNoteOnEvent;
                evt.noteOn.channel = s.channel;
                evt.noteOn.pitch = s.pitch;
                evt.noteOn.velocity = s.velocity;
                evt.noteOn.noteId = -1;
            } else {
                evt.type = Steinberg::Vst::Event::kNoteOffEvent;
                evt.noteOff.channel = s.channel;
                evt.noteOff.pitch = s.pitch;
                evt.noteOff.velocity = s.velocity;
                evt.noteOff.noteId = -1;
            }
            REQUIRE(input_events.addEvent(evt) == Steinberg::kResultOk);
        }

        RecordingParameterChanges output_params;
        Steinberg::Vst::EventList output_events(8);

        Steinberg::Vst::ProcessData data{};
        data.symbolicSampleSize = Steinberg::Vst::kSample32;
        data.numSamples = static_cast<Steinberg::int32>(n);
        data.numInputs = 1;
        data.numOutputs = 1;
        data.inputs = audio_in;
        data.outputs = audio_out;
        data.inputParameterChanges = &input_params;
        data.outputParameterChanges = &output_params;
        data.inputEvents = &input_events;
        data.outputEvents = &output_events;

        REQUIRE(processor.process(data) == Steinberg::kResultOk);

        collect_published(output_params, block_index, render.params);
        // Read the EditController half of the publication phase. VST3's
        // parameter system is not the output-change list: a host reads the
        // controller for the value it shows in its UI and for a host-side
        // automation read, and process() syncs it alongside the points it
        // publishes. Draining `outputParameterChanges` alone cannot see it, so
        // sample it here, per block, while the values still differ block to
        // block.
        for (auto id : {kGainParam, kOffsetParam, kMeterParam, kEchoParam}) {
            render.controller.push_back(
                {block_index, static_cast<Steinberg::Vst::ParamID>(id),
                 static_cast<double>(processor.getParamNormalized(
                     static_cast<Steinberg::Vst::ParamID>(id)))});
        }
        std::copy_n(out_l.begin(), n, render.output.channel(0).begin() + pos);
        std::copy_n(out_r.begin(), n, render.output.channel(1).begin() + pos);
        ++block_index;
    }

    REQUIRE(processor.setActive(false) == Steinberg::kResultOk);
    REQUIRE(processor.terminate() == Steinberg::kResultOk);
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

/// The gain in force for the block starting at `block_start` — the last scripted
/// Gain step at or before the block's first frame. Every step is block-aligned
/// (see `kParamStepAlign`), so the value holds for the whole block and the
/// prediction is exact.
float gain_in_force(std::int64_t block_start) {
    float gain = kGainRange.default_value;
    for (const auto& step : parity_param_script())
        if (step.id == kGainParam && step.frame <= block_start) gain = step.value;
    return gain;
}

/// Echo's own range — the same linear 0..2 mapping the fixture registers, held
/// here so a prediction normalizes against exactly what the adapter does.
const state::ParamRange kEchoRange{0.0f, 2.0f, 0.0f, 0.0f};

constexpr std::int64_t kSweepFrames = 4096;   // divisible by every swept block
constexpr double kSweepRates[] = {44100.0, 48000.0, 96000.0};
constexpr int kSweepBlocks[] = {1, 16, 64, 128, 256};

}  // namespace


// ===========================================================================
// Preconditions the null test's comparability rests on.
// ===========================================================================

TEST_CASE("vst3 audio parity: scripted param values survive the normalized host "
          "boundary exactly",
          "[format][vst3][parity][params]") {
    // The VST3 host boundary is normalized 0..1: the harness normalizes a plain
    // value, the adapter denormalizes it back. The direct path writes the plain
    // value straight to the store. So the two paths only see the same number if
    // that round-trip is lossless for every scripted value. Asserted rather than
    // assumed — a value that did not survive would make the null fail for a
    // reason that is the range math, not the adapter, and this test says which.
    for (const auto& step : parity_param_script()) {
        INFO("param=" << step.id << " value=" << step.value);
        const auto& range = range_for(step.id);
        const float round_tripped = range.denormalize(range.normalize(step.value));
        CHECK(std::memcmp(&round_tripped, &step.value, sizeof(float)) == 0);
    }
}

TEST_CASE("vst3 audio parity: scripted note velocities decode to the bytes the "
          "direct path renders",
          "[format][vst3][parity][midi]") {
    // VST3 carries note velocity as a float; the adapter reconstructs a MIDI
    // byte. The velocities are dyadic so the decode is exact and hand-checkable.
    // If this drifts, the two paths would be fed different stimuli and the null
    // would fail for a reason that is the note encoding, not the adapter.
    CHECK(decoded_velocity(0.5f) == 63);
    CHECK(decoded_velocity(0.25f) == 31);
    CHECK(decoded_velocity(0.75f) == 95);
    // Every scripted note must survive as a note, not decay to velocity 0 (which
    // MIDI reads as a note-off and would silently defeat the gate).
    for (const auto& n : parity_note_script()) {
        INFO("frame=" << n.frame << " velocity=" << n.velocity);
        CHECK(decoded_velocity(n.velocity) > 0);
    }
}

// ===========================================================================
// The null test.
// ===========================================================================

TEST_CASE("vst3 audio parity: the VST3 adapter emits the same bits as a direct render",
          "[format][vst3][parity][audio]") {
    for (double sr : kSweepRates) {
        for (int block : kSweepBlocks) {
            INFO("sample_rate=" << sr << " block=" << block);
            auto direct = parity_scenario(kSweepFrames).sample_rate(sr)
                              .block_size(block).render();
            auto through = render_through_vst3(sr, block, kSweepFrames);

            REQUIRE(direct.output.num_samples() == through.output.num_samples());
            const auto diff = first_bitwise_difference(direct.output, through.output);
            INFO("first differing sample index=" << diff.index
                 << " channel=" << diff.channel
                 << " direct=" << diff.direct << " adapter=" << diff.adapter);
            CHECK(diff.index == -1);
        }
    }
}

TEST_CASE("vst3 audio parity: a ragged final block does not perturb the stream",
          "[format][vst3][parity][audio]") {
    // A render length that is NOT a multiple of the block size makes the
    // adapter's last block short. A phase that assumes a full block (a scratch
    // resize, a zero-fill, an offset clamp) shows up here and nowhere else.
    constexpr std::int64_t kRagged = 4096 + 37;
    for (int block : {64, 128, 256}) {
        INFO("block=" << block << " frames=" << kRagged);
        auto direct = parity_scenario(kRagged).sample_rate(48000.0)
                          .block_size(block).render();
        auto through = render_through_vst3(48000.0, block, kRagged);

        REQUIRE(direct.output.num_samples() == through.output.num_samples());
        const auto diff = first_bitwise_difference(direct.output, through.output);
        INFO("first differing sample index=" << diff.index
             << " direct=" << diff.direct << " adapter=" << diff.adapter);
        CHECK(diff.index == -1);
    }
}

TEST_CASE("vst3 audio parity: the adapter never introduces a non-finite sample",
          "[format][vst3][parity][audio]") {
    auto through = render_through_vst3(48000.0, 128, kSweepFrames);
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

TEST_CASE("vst3 audio parity: plugin-side param changes reach the host every block",
          "[format][vst3][parity][params]") {
    constexpr int kBlock = 128;
    auto through = render_through_vst3(48000.0, kBlock, kSweepFrames);
    const auto blocks = static_cast<std::size_t>(kSweepFrames / kBlock);

    // `Echo` is pushed explicitly three times per block AND written to the
    // store. Exactly those three points per block, at exactly the pushed
    // offsets: the skip-set must suppress the offset-0 snapshot fallback for it.
    // A FOURTH point means the skip-set marked the wrong parameter — the exact
    // failure an off-by-one in the param-index lookup produces — and it would be
    // an offset-0 point appended after the explicit ones, so it breaks the
    // ordering case below too.
    // The offsets are a function of the block length alone, so the gain handed
    // to the prediction here is immaterial — only `.offset` is read.
    const auto expected_echo_offsets = [] {
        std::vector<Steinberg::int32> v;
        for (const auto& e : expected_published_echo(1.0f, kBlock))
            v.push_back(static_cast<Steinberg::int32>(e.offset));
        return v;
    }();
    for (std::uint32_t b = 0; b < blocks; ++b) {
        std::vector<Steinberg::int32> offsets;
        for (const auto& p : through.params) {
            if (p.block == b &&
                p.param_id == static_cast<Steinberg::Vst::ParamID>(kEchoParam))
                offsets.push_back(p.offset);
        }
        INFO("block=" << b);
        CHECK(offsets == expected_echo_offsets);
    }

    // `Meter` has no explicit event, so it travels through the snapshot-diff
    // fallback at offset 0. The processor writes a distinct value every block,
    // so the diff must fire on every one: a publication path that resolved the
    // parameter index wrongly, or dropped the fallback, loses points here.
    std::vector<std::uint32_t> meter_blocks;
    for (const auto& p : through.params) {
        if (p.param_id != static_cast<Steinberg::Vst::ParamID>(kMeterParam)) continue;
        CHECK(p.offset == 0);
        meter_blocks.push_back(p.block);
    }
    CHECK(meter_blocks.size() == blocks);
    CHECK(std::adjacent_find(meter_blocks.begin(), meter_blocks.end()) ==
          meter_blocks.end());

    // A parameter the host drove itself (Gain / Offset) is not republished back
    // at it: the snapshot is taken after the host's points are applied, so there
    // is no diff to report. Catches a publication path that echoes host
    // automation, and — because the snapshot phase moving after process() would
    // make every host-driven param diff — a mis-ordered snapshot.
    for (const auto& p : through.params) {
        CHECK(p.param_id != static_cast<Steinberg::Vst::ParamID>(kGainParam));
        CHECK(p.param_id != static_cast<Steinberg::Vst::ParamID>(kOffsetParam));
    }
}

TEST_CASE("vst3 audio parity: published Echo carries the normalized value the "
          "processor pushed at each offset",
          "[format][vst3][parity][params]") {
    // The VST3 boundary is normalized, so the publication path runs the plain
    // value the processor pushed back through the parameter's range. The scripted
    // gains are dyadic on a linear range, so the expected normalized value is
    // exact and this can assert bits rather than a tolerance. A publication path
    // that resolved the wrong index would normalize against the wrong range.
    //
    // The fixture pushes a DIFFERENT value at each of its three offsets, so this
    // also pins the pairing: the adapter's sort reorders (offset, value) pairs,
    // and a sort that moved an offset without carrying its value along would put
    // the right numbers on the wrong frames. The ordering case cannot see that —
    // the offsets would still ascend — so it is pinned here.
    constexpr int kBlock = 256;
    auto through = render_through_vst3(48000.0, kBlock, kSweepFrames);
    const auto blocks = static_cast<std::uint32_t>(kSweepFrames / kBlock);

    for (std::uint32_t b = 0; b < blocks; ++b) {
        std::vector<PublishedParam> echo;
        for (const auto& p : through.params) {
            if (p.block == b &&
                p.param_id == static_cast<Steinberg::Vst::ParamID>(kEchoParam))
                echo.push_back(p);
        }
        const auto gain = gain_in_force(static_cast<std::int64_t>(b) * kBlock);
        const auto expected = expected_published_echo(gain, kBlock);

        // Assert Echo was published at all before checking what it carried:
        // without this the per-point loop below would range over an empty vector
        // and report green for a path it never entered.
        REQUIRE(echo.size() == expected.size());
        for (std::size_t i = 0; i < echo.size(); ++i) {
            INFO("block=" << b << " gain=" << gain << " point=" << i);
            CHECK(echo[i].offset == static_cast<Steinberg::int32>(expected[i].offset));
            CHECK(echo[i].normalized ==
                  static_cast<double>(kEchoRange.normalize(expected[i].value)));
        }
    }
}

TEST_CASE("vst3 audio parity: each published queue stays in ascending sample order",
          "[format][vst3][parity][params]") {
    // VST3 requires the points a plug-in adds to one IParamValueQueue to ascend
    // by sample offset. The fixture pushes its three Echo events per block in
    // DESCENDING order, so the adapter's sort is the only thing that can make
    // this hold — a processor may emit events in any order it likes, and the
    // obligation to order them is the adapter's.
    //
    // Two things had to be true for this to be a real check rather than
    // decoration. The queue has to carry more than one point (one point is
    // ascending unconditionally), and it has to be read back from a host
    // container that preserves call order — see RecordingParamValueQueue for why
    // the SDK's own ParameterChanges cannot serve here.
    constexpr int kBlock = 128;
    constexpr std::int64_t kFrames = 1024;
    auto through = render_through_vst3(48000.0, kBlock, kFrames);
    const auto blocks = static_cast<std::uint32_t>(kFrames / kBlock);

    for (std::uint32_t block = 0; block < blocks; ++block) {
        for (state::ParamID id : {kMeterParam, kEchoParam}) {
            std::vector<Steinberg::int32> offsets;
            for (const auto& p : through.params) {
                if (p.block == block &&
                    p.param_id == static_cast<Steinberg::Vst::ParamID>(id))
                    offsets.push_back(p.offset);
            }
            INFO("block=" << block << " param=" << id);
            REQUIRE_FALSE(offsets.empty());
            CHECK(std::is_sorted(offsets.begin(), offsets.end()));
        }
    }

    // Echo's queue must actually be the multi-point one, or the ascending check
    // above is back to being vacuous.
    const auto echo_points_in_block_0 = std::count_if(
        through.params.begin(), through.params.end(), [](const auto& p) {
            return p.block == 0 &&
                   p.param_id == static_cast<Steinberg::Vst::ParamID>(kEchoParam);
        });
    CHECK(echo_points_in_block_0 == 3);
}

TEST_CASE("vst3 audio parity: an explicit-event param still syncs the "
          "EditController to its final value",
          "[format][vst3][parity][params]") {
    // A parameter that emitted explicit output events is skipped by the
    // snapshot-diff fallback — it has already been reported sample-accurately.
    // But the fallback loop is also where the adapter syncs VST3's parameter
    // system, so the skip branch has to carry that sync itself. Nothing in the
    // output-change list can see it: the host reads the EditController for the
    // value it shows in its UI and returns from a host-side automation read, and
    // that is a different surface from `outputParameterChanges`.
    //
    // Echo is the parameter on the skip branch, so it is what pins this. Meter
    // cannot: it travels the fallback branch, which has its own sync, and would
    // stay green with the skip branch's sync gone.
    constexpr int kBlock = 256;
    auto through = render_through_vst3(48000.0, kBlock, kSweepFrames);

    std::size_t echo_reads = 0;
    for (const auto& r : through.controller) {
        if (r.param_id != static_cast<Steinberg::Vst::ParamID>(kEchoParam)) continue;
        ++echo_reads;
        // The sync publishes the STORE's value, not the last event's: the
        // processor writes `gain` to the store and pushes scaled copies as
        // events, so this pins the sync to the parameter's final value.
        const auto gain = gain_in_force(static_cast<std::int64_t>(r.block) * kBlock);
        INFO("block=" << r.block << " gain=" << gain);
        CHECK(r.normalized == static_cast<double>(kEchoRange.normalize(gain)));
    }
    REQUIRE(echo_reads == static_cast<std::size_t>(kSweepFrames / kBlock));

    // The controller value must actually have MOVED off Echo's default, or a
    // sync that never ran would be indistinguishable from one that did.
    const auto default_normalized =
        static_cast<double>(kEchoRange.normalize(kEchoRange.default_value));
    CHECK(std::any_of(through.controller.begin(), through.controller.end(),
                      [&](const ControllerRead& r) {
                          return r.param_id ==
                                     static_cast<Steinberg::Vst::ParamID>(kEchoParam) &&
                                 r.normalized != default_normalized;
                      }));
}
