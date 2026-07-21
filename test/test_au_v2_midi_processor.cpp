// AU v2 MIDI-processor (`aumi`) adapter tests.
//
// These drive the REAL adapter: a live `PulpAUMidiProcessor`, MIDI pushed in
// through the SDK's `MusicDeviceBase::MIDIEvent` / `SysEx` entry points, a
// `kAudioUnitProperty_MIDIOutputCallback` installed the way a host installs it,
// and `Render()` called to advance the block. What the callback receives is the
// assertion. A compile-only check would miss every failure mode that matters
// here — an inactive output bus that makes `process()` never run, a MIDI queue
// that never drains, a sample offset dropped on the floor.

#include <catch2/catch_test_macros.hpp>

#include <pulp/format/au_v2_midi_processor.hpp>
#include <pulp/format/processor.hpp>
#include <pulp/format/registry.hpp>
#include <pulp/midi/buffer.hpp>
#include <pulp/midi/message.hpp>

#include <AudioUnitSDK/AUPlugInDispatch.h>
#include <AudioToolbox/AudioToolbox.h>

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

using namespace pulp;

namespace {

// ── Test processor: transposes every note by `Semitones`, echoes SysEx ──────
//
// Deliberately overrides only the CLASSIC main-in/main-out `process()`
// signature — the one nearly every real MIDI effect will implement. Reaching it
// depends on the adapter marking its (zero-channel) main output bus ACTIVE, so
// this doubles as the regression guard for that contract.
class TransposeProcessor : public format::Processor {
public:
    static constexpr state::ParamID kSemitonesId = 1;
    static constexpr state::ParamID kBypassId = 2;

    format::PluginDescriptor descriptor() const override {
        return {
            .name = "AuMidiProcTest",
            .manufacturer = "PulpTest",
            .bundle_id = "com.pulp.test.au-midi-processor",
            .version = "1.0.0",
            .category = format::PluginCategory::MidiEffect,
            .input_buses = {},
            .output_buses = {},
            .accepts_midi = true,
            .produces_midi = true,
        };
    }

    void define_parameters(state::StateStore& store) override {
        store.add_parameter({
            .id = kSemitonesId,
            .name = "Semitones",
            .unit = "",
            .range = {-24.0f, 24.0f, 0.0f, 1.0f},
        });
        store.add_parameter({
            .id = kBypassId,
            .name = "Bypass",
            .unit = "",
            .range = {0.0f, 1.0f, 0.0f, 1.0f},
        });
    }

    void prepare(const format::PrepareContext& ctx) override {
        prepared_input_channels = ctx.input_channels;
        prepared_output_channels = ctx.output_channels;
    }

    void process(audio::BufferView<float>& output,
                 const audio::BufferView<const float>& input,
                 midi::MidiBuffer& midi_in,
                 midi::MidiBuffer& midi_out,
                 const format::ProcessContext&) override {
        ++process_calls;
        seen_output_channels = output.num_channels();
        seen_input_channels = input.num_channels();

        const auto shift =
            static_cast<int>(state().get_value(kSemitonesId));
        for (const auto& ev : midi_in) {
            auto out_ev = ev;
            const auto status = ev.message.data()[0];
            const bool is_note = (status & 0xF0) == 0x90 || (status & 0xF0) == 0x80;
            if (is_note) {
                int note = static_cast<int>(ev.message.data()[1]) + shift;
                note = note < 0 ? 0 : (note > 127 ? 127 : note);
                out_ev.message = choc::midi::ShortMessage(
                    status, static_cast<uint8_t>(note), ev.message.data()[2]);
            }
            midi_out.add(out_ev);
        }
        for (const auto& sx : midi_in.sysex()) {
            midi_out.add_sysex_copy(sx.data.data(), sx.data.size(),
                                    sx.sample_offset, sx.timestamp);
        }
    }

    int process_calls = 0;
    std::size_t seen_output_channels = 99;
    std::size_t seen_input_channels = 99;
    int prepared_input_channels = -1;
    int prepared_output_channels = -1;
};

TransposeProcessor* g_live_processor = nullptr;

std::unique_ptr<format::Processor> create_transpose_processor() {
    auto p = std::make_unique<TransposeProcessor>();
    g_live_processor = p.get();
    return p;
}

struct ScopedFactoryRegistration {
    ScopedFactoryRegistration()
        : previous(format::registered_factory()) {
        format::register_plugin(create_transpose_processor);
    }
    ~ScopedFactoryRegistration() {
        format::register_plugin(previous);
        g_live_processor = nullptr;
    }
    format::ProcessorFactory previous;
};

AudioStreamBasicDescription make_float_format(double sample_rate,
                                              UInt32 channels) {
    AudioStreamBasicDescription fmt{};
    fmt.mSampleRate = sample_rate;
    fmt.mFormatID = kAudioFormatLinearPCM;
    fmt.mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked |
                       kAudioFormatFlagIsNonInterleaved;
    fmt.mBytesPerPacket = sizeof(float);
    fmt.mFramesPerPacket = 1;
    fmt.mBytesPerFrame = sizeof(float);
    fmt.mChannelsPerFrame = channels;
    fmt.mBitsPerChannel = 32;
    return fmt;
}

// One decoded packet the host callback received.
struct CapturedPacket {
    MIDITimeStamp timestamp = 0;
    std::vector<std::uint8_t> bytes;
};

struct MidiOutputCapture {
    std::vector<CapturedPacket> packets;
    int callback_calls = 0;
    UInt32 last_midi_out_num = 0xFFFF;
};

MidiOutputCapture g_capture;

OSStatus capture_midi_output(void* user_data,
                             const AudioTimeStamp* /*timestamp*/,
                             UInt32 midi_out_num,
                             const MIDIPacketList* list) noexcept {
    auto* capture = static_cast<MidiOutputCapture*>(user_data);
    ++capture->callback_calls;
    capture->last_midi_out_num = midi_out_num;
    if (!list) return noErr;
    const MIDIPacket* packet = &list->packet[0];
    for (UInt32 i = 0; i < list->numPackets; ++i) {
        CapturedPacket captured;
        captured.timestamp = packet->timeStamp;
        captured.bytes.assign(packet->data, packet->data + packet->length);
        capture->packets.push_back(std::move(captured));
        packet = MIDIPacketNext(packet);
    }
    return noErr;
}

// Bring a directly-constructed adapter to the state the SDK dispatch would:
// create elements, configure the output element + block ceiling, initialize,
// and install the host's MIDI-output callback.
struct LiveMidiProcessor {
    static constexpr double kSampleRate = 48000.0;
    static constexpr UInt32 kFrames = 64;

    LiveMidiProcessor() : unit(nullptr) {
        g_capture = MidiOutputCapture{};
        unit.CreateElements();
        REQUIRE(unit.GetOutput(0)->SetStreamFormat(
                    make_float_format(kSampleRate, 2)) == noErr);
        UInt32 max_frames = kFrames;
        REQUIRE(unit.DispatchSetProperty(kAudioUnitProperty_MaximumFramesPerSlice,
                                         kAudioUnitScope_Global, 0, &max_frames,
                                         sizeof(max_frames)) == noErr);
        REQUIRE(unit.DoInitialize() == noErr);

        AUMIDIOutputCallbackStruct cb{};
        cb.midiOutputCallback = capture_midi_output;
        cb.userData = &g_capture;
        REQUIRE(unit.DispatchSetProperty(kAudioUnitProperty_MIDIOutputCallback,
                                         kAudioUnitScope_Global, 0, &cb,
                                         sizeof(cb)) == noErr);
    }

    ~LiveMidiProcessor() { unit.DoCleanup(); }

    OSStatus render(UInt32 frames = kFrames) {
        AudioUnitRenderActionFlags flags = 0;
        return render_with_flags(flags, frames);
    }

    OSStatus render_with_flags(AudioUnitRenderActionFlags& flags,
                               UInt32 frames = kFrames) {
        AudioTimeStamp timestamp{};
        timestamp.mFlags = kAudioTimeStampSampleTimeValid;
        timestamp.mSampleTime = 0;
        return unit.Render(flags, timestamp, frames);
    }

    format::au::PulpAUMidiProcessor unit;
};

}  // namespace

// The MIDI processor must be registered through a factory whose lookup carries
// the MusicDevice MIDI selectors — the same trap that made `aumf` and `aumu`
// silently drop every host note. `PULP_AU_MIDI_EFFECT` uses AUMIDIEffectFactory
// (AUMIDILookup); the plain base lookup carries no MIDI selector, so a
// regression to it returns -4 (unimpErr) for every delivery.
TEST_CASE("AU v2 MIDI processor: MIDI lookup carries the MusicDevice selectors",
          "[au][au-v2][midi-processor][dispatch]")
{
    REQUIRE(ausdk::AUMIDILookup::Lookup(kMusicDeviceMIDIEventSelect) != nullptr);
    REQUIRE(ausdk::AUMIDILookup::Lookup(kMusicDeviceSysExSelect) != nullptr);
    REQUIRE(ausdk::AUBaseLookup::Lookup(kMusicDeviceMIDIEventSelect) == nullptr);
}

TEST_CASE("AU v2 MIDI processor transforms host MIDI and returns it to the host",
          "[au][au-v2][midi-processor][midi-out]")
{
    ScopedFactoryRegistration registration;
    LiveMidiProcessor live;

    REQUIRE(live.unit.SetParameter(TransposeProcessor::kSemitonesId,
                                   kAudioUnitScope_Global, 0, 7.0f, 0) == noErr);

    // Host delivers a note-on at frame 0 and a note-off at frame 32.
    REQUIRE(live.unit.MIDIEvent(0x91, 60, 100, 0) == noErr);
    REQUIRE(live.unit.MIDIEvent(0x81, 60, 0, 32) == noErr);

    REQUIRE(live.render() == noErr);

    REQUIRE(g_capture.callback_calls == 1);
    REQUIRE(g_capture.last_midi_out_num == 0);
    REQUIRE(g_capture.packets.size() == 2);

    // Note-on: transposed 60 -> 67, channel preserved, offset preserved.
    REQUIRE(g_capture.packets[0].timestamp == 0);
    REQUIRE(g_capture.packets[0].bytes ==
            std::vector<std::uint8_t>{0x91, 67, 100});
    // Note-off keeps its in-block sample offset.
    REQUIRE(g_capture.packets[1].timestamp == 32);
    REQUIRE(g_capture.packets[1].bytes ==
            std::vector<std::uint8_t>{0x81, 67, 0});
}

// The adapter hands the Processor a zero-channel main output bus that is
// nonetheless marked ACTIVE. `Processor::process(ProcessBuffers&)`'s default
// projection returns early on a null `main_output()`, so an inactive bus would
// mean the classic `process()` override never runs at all and the MIDI effect
// would silently pass nothing through.
TEST_CASE("AU v2 MIDI processor reaches a classic process() with no audio buses",
          "[au][au-v2][midi-processor][process]")
{
    ScopedFactoryRegistration registration;
    LiveMidiProcessor live;

    REQUIRE(g_live_processor != nullptr);
    REQUIRE(g_live_processor->prepared_input_channels == 0);
    REQUIRE(g_live_processor->prepared_output_channels == 0);

    REQUIRE(live.render() == noErr);

    REQUIRE(g_live_processor->process_calls == 1);
    REQUIRE(g_live_processor->seen_input_channels == 0);
    REQUIRE(g_live_processor->seen_output_channels == 0);
}

TEST_CASE("AU v2 MIDI processor round-trips SysEx",
          "[au][au-v2][midi-processor][sysex]")
{
    ScopedFactoryRegistration registration;
    LiveMidiProcessor live;

    const std::array<UInt8, 6> payload{0xF0, 0x7E, 0x00, 0x06, 0x01, 0xF7};
    REQUIRE(live.unit.SysEx(payload.data(),
                            static_cast<UInt32>(payload.size())) == noErr);
    REQUIRE(live.render() == noErr);

    REQUIRE(g_capture.packets.size() == 1);
    REQUIRE(g_capture.packets[0].bytes ==
            std::vector<std::uint8_t>(payload.begin(), payload.end()));
}

// CoreMIDI packet lists are expected time-ordered, but short messages and SysEx
// arrive through separate sidecars. A SysEx at the block edge must still be
// delivered before a note later in the block.
TEST_CASE("AU v2 MIDI processor emits SysEx and notes in one ascending order",
          "[au][au-v2][midi-processor][sysex][midi-out]")
{
    ScopedFactoryRegistration registration;
    LiveMidiProcessor live;

    REQUIRE(live.unit.MIDIEvent(0x90, 64, 90, 40) == noErr);
    const std::array<UInt8, 4> payload{0xF0, 0x7D, 0x01, 0xF7};
    REQUIRE(live.unit.SysEx(payload.data(),
                            static_cast<UInt32>(payload.size())) == noErr);
    REQUIRE(live.render() == noErr);

    REQUIRE(g_capture.packets.size() == 2);
    // SysEx carries no per-event offset at this SDK layer, so it lands at 0 and
    // must precede the note at 40.
    REQUIRE(g_capture.packets[0].timestamp == 0);
    REQUIRE(g_capture.packets[0].bytes.front() == 0xF0);
    REQUIRE(g_capture.packets[1].timestamp == 40);
    REQUIRE(g_capture.packets[1].bytes.front() == 0x90);
}

// A bypassed MIDI processor is a WIRE. Dropping the stream instead would
// silence every instrument downstream of the bypassed slot.
TEST_CASE("AU v2 MIDI processor passes MIDI through untouched when bypassed",
          "[au][au-v2][midi-processor][bypass]")
{
    ScopedFactoryRegistration registration;
    LiveMidiProcessor live;

    REQUIRE(live.unit.SetParameter(TransposeProcessor::kSemitonesId,
                                   kAudioUnitScope_Global, 0, 7.0f, 0) == noErr);
    REQUIRE(live.unit.SetParameter(TransposeProcessor::kBypassId,
                                   kAudioUnitScope_Global, 0, 1.0f, 0) == noErr);

    REQUIRE(live.unit.MIDIEvent(0x90, 60, 100, 16) == noErr);
    const std::array<UInt8, 3> payload{0xF0, 0x11, 0xF7};
    REQUIRE(live.unit.SysEx(payload.data(),
                            static_cast<UInt32>(payload.size())) == noErr);
    REQUIRE(live.render() == noErr);

    // The Processor is skipped entirely...
    REQUIRE(g_live_processor->process_calls == 0);
    // ...and the inbound stream reaches the host unmodified (NOT transposed,
    // NOT dropped), offsets intact.
    REQUIRE(g_capture.packets.size() == 2);
    REQUIRE(g_capture.packets[0].timestamp == 0);
    REQUIRE(g_capture.packets[0].bytes ==
            std::vector<std::uint8_t>(payload.begin(), payload.end()));
    REQUIRE(g_capture.packets[1].timestamp == 16);
    REQUIRE(g_capture.packets[1].bytes ==
            std::vector<std::uint8_t>{0x90, 60, 100});
}

TEST_CASE("AU v2 MIDI processor renders an empty block without calling the host",
          "[au][au-v2][midi-processor][edge]")
{
    ScopedFactoryRegistration registration;
    LiveMidiProcessor live;

    REQUIRE(live.render() == noErr);

    // Nothing to deliver: the host callback must not be invoked with an empty
    // packet list.
    REQUIRE(g_capture.callback_calls == 0);
    REQUIRE(g_capture.packets.empty());
}

// The `aumi` output element exists only so the host has something to render.
// It must read as silence, and the block must be flagged silent.
TEST_CASE("AU v2 MIDI processor zeroes its audio output element and flags silence",
          "[au][au-v2][midi-processor][silence]")
{
    ScopedFactoryRegistration registration;
    LiveMidiProcessor live;

    // Dirty the element's buffer, then render over it.
    AudioBufferList& before =
        live.unit.GetOutput(0)->PrepareBuffer(LiveMidiProcessor::kFrames);
    for (UInt32 c = 0; c < before.mNumberBuffers; ++c) {
        auto* data = static_cast<float*>(before.mBuffers[c].mData);
        for (UInt32 n = 0; n < LiveMidiProcessor::kFrames; ++n) data[n] = 0.5f;
    }

    AudioUnitRenderActionFlags flags = 0;
    REQUIRE(live.render_with_flags(flags) == noErr);
    REQUIRE((flags & kAudioUnitRenderAction_OutputIsSilence) != 0);

    AudioBufferList& after =
        live.unit.GetOutput(0)->PrepareBuffer(LiveMidiProcessor::kFrames);
    REQUIRE(after.mNumberBuffers == 2);
    for (UInt32 c = 0; c < after.mNumberBuffers; ++c) {
        const auto* data = static_cast<const float*>(after.mBuffers[c].mData);
        for (UInt32 n = 0; n < LiveMidiProcessor::kFrames; ++n) {
            INFO("channel " << c << " frame " << n);
            REQUIRE(data[n] == 0.0f);
        }
    }
}

// The Processor's scratch was sized in prepare() to GetMaxFramesPerSlice();
// rejecting an oversized render is the canonical AU response and what auval's
// "Bad Max Frames" contract test expects.
TEST_CASE("AU v2 MIDI processor rejects a render past the advertised max frames",
          "[au][au-v2][midi-processor][edge]")
{
    ScopedFactoryRegistration registration;
    LiveMidiProcessor live;

    REQUIRE(live.render(LiveMidiProcessor::kFrames * 4) ==
            kAudioUnitErr_TooManyFramesToProcess);
}

// The input queue is a bounded lock-free backstop: a flood drops events rather
// than allocating or blocking the render thread. What must NOT happen is a
// crash, an overflowed packet buffer, or events leaking into the next block.
TEST_CASE("AU v2 MIDI processor survives a dense MIDI flood",
          "[au][au-v2][midi-processor][edge]")
{
    ScopedFactoryRegistration registration;
    LiveMidiProcessor live;

    constexpr int kEvents = 4096;  // 4x the input queue depth
    for (int i = 0; i < kEvents; ++i) {
        live.unit.MIDIEvent(0x90, static_cast<UInt32>(i % 128), 64,
                            static_cast<UInt32>(i % LiveMidiProcessor::kFrames));
    }
    REQUIRE(live.render() == noErr);

    REQUIRE(g_capture.packets.size() <= static_cast<std::size_t>(kEvents));
    REQUIRE(!g_capture.packets.empty());
    // Every delivered packet stays inside the rendered block.
    for (const auto& packet : g_capture.packets) {
        REQUIRE(packet.timestamp < LiveMidiProcessor::kFrames);
    }

    // The next block starts clean — nothing left over from the flood.
    g_capture = MidiOutputCapture{};
    REQUIRE(live.render() == noErr);
    REQUIRE(g_capture.callback_calls == 0);
}

// An out-of-block offset from a misbehaving Processor must be clamped into the
// block rather than handed to CoreMIDI as-is.
TEST_CASE("AU v2 MIDI processor clamps a stray offset into the rendered block",
          "[au][au-v2][midi-processor][midi-out]")
{
    ScopedFactoryRegistration registration;
    LiveMidiProcessor live;

    // The host itself can deliver an offset past the block the plugin ends up
    // rendering (a shorter block than the one the event was scheduled against).
    REQUIRE(live.unit.MIDIEvent(0x90, 60, 100,
                                LiveMidiProcessor::kFrames * 2) == noErr);
    REQUIRE(live.render() == noErr);

    REQUIRE(g_capture.packets.size() == 1);
    REQUIRE(g_capture.packets[0].timestamp == LiveMidiProcessor::kFrames - 1);
}

TEST_CASE("AU v2 MIDI processor publishes its parameters to the host",
          "[au][au-v2][midi-processor][params]")
{
    ScopedFactoryRegistration registration;
    LiveMidiProcessor live;

    UInt32 count = 0;
    REQUIRE(live.unit.GetParameterList(kAudioUnitScope_Global, nullptr, count) ==
            noErr);
    REQUIRE(count == 2);

    std::vector<AudioUnitParameterID> ids(count);
    REQUIRE(live.unit.GetParameterList(kAudioUnitScope_Global, ids.data(),
                                       count) == noErr);
    REQUIRE(ids[0] == TransposeProcessor::kSemitonesId);

    AudioUnitParameterInfo info{};
    REQUIRE(live.unit.GetParameterInfo(kAudioUnitScope_Global,
                                       TransposeProcessor::kSemitonesId,
                                       info) == noErr);
    REQUIRE(info.minValue == -24.0f);
    REQUIRE(info.maxValue == 24.0f);
    REQUIRE(info.defaultValue == 0.0f);
    if (info.cfNameString) CFRelease(info.cfNameString);

    // The StateStore is the single source of truth: a host write is readable
    // back immediately through GetParameter.
    REQUIRE(live.unit.SetParameter(TransposeProcessor::kSemitonesId,
                                   kAudioUnitScope_Global, 0, -5.0f, 0) == noErr);
    Float32 value = 0.0f;
    REQUIRE(live.unit.GetParameter(TransposeProcessor::kSemitonesId,
                                   kAudioUnitScope_Global, 0, value) == noErr);
    REQUIRE(value == -5.0f);
}

TEST_CASE("AU v2 MIDI processor advertises a MIDI output stream",
          "[au][au-v2][midi-processor][midi-out]")
{
    ScopedFactoryRegistration registration;
    LiveMidiProcessor live;

    // Query through the adapter's own property surface (no live component
    // instance to dispatch through).
    UInt32 size = 0;
    bool writable = false;
    REQUIRE(live.unit.GetPropertyInfo(kAudioUnitProperty_MIDIOutputCallbackInfo,
                                      kAudioUnitScope_Global, 0, size,
                                      writable) == noErr);
    REQUIRE(size == sizeof(CFArrayRef));
    REQUIRE(writable == false);

    CFArrayRef names = nullptr;
    REQUIRE(live.unit.GetProperty(kAudioUnitProperty_MIDIOutputCallbackInfo,
                                  kAudioUnitScope_Global, 0, &names) == noErr);
    REQUIRE(names != nullptr);
    REQUIRE(CFArrayGetCount(names) == 1);
    CFRelease(names);

    // The installed callback pair reads back exactly as the host wrote it.
    AUMIDIOutputCallbackStruct read_back{};
    REQUIRE(live.unit.GetProperty(kAudioUnitProperty_MIDIOutputCallback,
                                  kAudioUnitScope_Global, 0, &read_back) == noErr);
    REQUIRE(reinterpret_cast<const void*>(read_back.midiOutputCallback) ==
            reinterpret_cast<const void*>(&capture_midi_output));
    REQUIRE(read_back.userData == &g_capture);
}

TEST_CASE("AU v2 MIDI processor round-trips its state through an AU preset",
          "[au][au-v2][midi-processor][state]")
{
    ScopedFactoryRegistration registration;
    LiveMidiProcessor live;

    REQUIRE(live.unit.SetParameter(TransposeProcessor::kSemitonesId,
                                   kAudioUnitScope_Global, 0, 12.0f, 0) == noErr);

    CFPropertyListRef saved = nullptr;
    REQUIRE(live.unit.SaveState(&saved) == noErr);
    REQUIRE(saved != nullptr);

    REQUIRE(live.unit.SetParameter(TransposeProcessor::kSemitonesId,
                                   kAudioUnitScope_Global, 0, 0.0f, 0) == noErr);
    REQUIRE(live.unit.RestoreState(saved) == noErr);

    Float32 value = 0.0f;
    REQUIRE(live.unit.GetParameter(TransposeProcessor::kSemitonesId,
                                   kAudioUnitScope_Global, 0, value) == noErr);
    REQUIRE(value == 12.0f);
    CFRelease(saved);
}

// A MIDI processor that delays events (a quantizer, a humanizer with lookahead)
// reports that latency so the host can compensate.
TEST_CASE("AU v2 MIDI processor reports its latency in seconds",
          "[au][au-v2][midi-processor][latency]")
{
    ScopedFactoryRegistration registration;
    LiveMidiProcessor live;

    // The test Processor declares no latency, so the reported value is 0 and
    // the sample-rate divide is still exercised.
    REQUIRE(live.unit.GetLatency() == 0.0);
    // No audio tail on a MIDI processor.
    REQUIRE(live.unit.SupportsTail() == false);
}

// The host negotiates the (silent) output element's format and schedules
// parameters; the input scope has no element at all.
TEST_CASE("AU v2 MIDI processor exposes only a writable output scope",
          "[au][au-v2][midi-processor][element]")
{
    ScopedFactoryRegistration registration;
    LiveMidiProcessor live;

    REQUIRE(live.unit.StreamFormatWritable(kAudioUnitScope_Output, 0) == true);
    REQUIRE(live.unit.StreamFormatWritable(kAudioUnitScope_Input, 0) == false);
    REQUIRE(live.unit.CanScheduleParameters() == true);
    REQUIRE(live.unit.Inputs().GetNumberOfElements() == 0);
    REQUIRE(live.unit.Outputs().GetNumberOfElements() == 1);
}

// Every Pulp-owned property is Global scope. A scoped query for the wrong scope
// must be refused rather than served from element 0.
TEST_CASE("AU v2 MIDI processor rejects Pulp properties outside global scope",
          "[au][au-v2][midi-processor][properties]")
{
    ScopedFactoryRegistration registration;
    LiveMidiProcessor live;

    UInt32 size = 0;
    bool writable = false;
    REQUIRE(live.unit.GetPropertyInfo(kAudioUnitProperty_MIDIOutputCallback,
                                      kAudioUnitScope_Input, 0, size,
                                      writable) == kAudioUnitErr_InvalidScope);
    REQUIRE(live.unit.GetPropertyInfo(kAudioUnitProperty_ParameterStringFromValue,
                                      kAudioUnitScope_Input, 0, size,
                                      writable) == kAudioUnitErr_InvalidScope);

    AUMIDIOutputCallbackStruct cb{};
    REQUIRE(live.unit.SetProperty(kAudioUnitProperty_MIDIOutputCallback,
                                  kAudioUnitScope_Input, 0, &cb,
                                  sizeof(cb)) == kAudioUnitErr_InvalidScope);
    // An undersized payload is refused rather than read past its end.
    REQUIRE(live.unit.SetProperty(kAudioUnitProperty_MIDIOutputCallback,
                                  kAudioUnitScope_Global, 0, &cb, 1) ==
            kAudioUnitErr_InvalidPropertyValue);

    // Non-global parameter scopes report no parameters rather than erroring.
    UInt32 count = 99;
    REQUIRE(live.unit.GetParameterList(kAudioUnitScope_Input, nullptr, count) ==
            noErr);
    REQUIRE(count == 0);
    AudioUnitParameterInfo info{};
    REQUIRE(live.unit.GetParameterInfo(kAudioUnitScope_Input, 1, info) ==
            kAudioUnitErr_InvalidParameter);
}

// A parameter with no declared `to_string` keeps the host's stock numeric
// display, but the adapter still answers the conversion properties when asked.
TEST_CASE("AU v2 MIDI processor converts parameter values to and from text",
          "[au][au-v2][midi-processor][params]")
{
    ScopedFactoryRegistration registration;
    LiveMidiProcessor live;

    AudioUnitParameterStringFromValue sfv{};
    sfv.inParamID = TransposeProcessor::kSemitonesId;
    const Float32 value = 5.0f;
    sfv.inValue = &value;
    REQUIRE(live.unit.GetProperty(kAudioUnitProperty_ParameterStringFromValue,
                                  kAudioUnitScope_Global, 0, &sfv) == noErr);
    REQUIRE(sfv.outString != nullptr);
    CFRelease(sfv.outString);

    // An unknown ParamID is refused rather than formatted from garbage.
    AudioUnitParameterStringFromValue unknown{};
    unknown.inParamID = 9999;
    REQUIRE(live.unit.GetProperty(kAudioUnitProperty_ParameterStringFromValue,
                                  kAudioUnitScope_Global, 0, &unknown) ==
            kAudioUnitErr_InvalidPropertyValue);

    AudioUnitParameterValueFromString vfs{};
    vfs.inParamID = TransposeProcessor::kSemitonesId;
    vfs.inString = CFSTR("-7");
    REQUIRE(live.unit.GetProperty(kAudioUnitProperty_ParameterValueFromString,
                                  kAudioUnitScope_Global, 0, &vfs) == noErr);
    REQUIRE(vfs.outValue == -7.0f);
}
