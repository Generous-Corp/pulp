// The shared processing layer every hosted-plugin backend consumes
// (core/host/src/plugin_slot_process_request.hpp): host-bus to plugin-bus
// binding, the MIDI view, the plugin-edit inbox, and transport validity.

#include <catch2/catch_test_macros.hpp>

#include "plugin_slot_process_request.hpp"

#include <array>
#include <vector>

using namespace pulp;
using pulp::host::PluginParameterEdit;
using pulp::host::detail::ParameterEditInbox;
using pulp::host::detail::SlotBusBinder;
using pulp::host::detail::SlotBusDecl;

namespace {

// Owns per-channel storage and hands out the views a host would pass.
struct Channels {
    std::vector<std::vector<float>> data;
    std::vector<float*> ptrs;
    Channels(std::size_t channels, std::size_t frames, float fill) {
        data.assign(channels, std::vector<float>(frames, fill));
        for (auto& c : data)
            ptrs.push_back(c.data());
    }
    audio::BufferView<float> view(std::size_t frames) {
        return audio::BufferView<float>(ptrs.data(), ptrs.size(), frames);
    }
    audio::BufferView<const float> const_view(std::size_t frames) {
        return audio::BufferView<const float>(const_cast<const float* const*>(ptrs.data()),
                                              ptrs.size(), frames);
    }
};

format::ProcessBusBufferView<const float> in_bus(audio::BufferView<const float> view,
                                                 format::BusRole role) {
    format::ProcessBusBufferView<const float> bus{};
    bus.info.role = role;
    bus.info.direction = format::BusDirection::Input;
    bus.info.declared_channels = static_cast<int>(view.num_channels());
    bus.buffer = view;
    return bus;
}

format::ProcessBusBufferView<float> out_bus(audio::BufferView<float> view, format::BusRole role) {
    format::ProcessBusBufferView<float> bus{};
    bus.info.role = role;
    bus.info.direction = format::BusDirection::Output;
    bus.info.declared_channels = static_cast<int>(view.num_channels());
    bus.buffer = view;
    return bus;
}

} // namespace

TEST_CASE("Bus binder routes a host sidechain to the plugin's second input bus",
          "[host][plugin-slot][buses]") {
    constexpr std::size_t kFrames = 8;
    Channels main_in(2, kFrames, 0.25f);
    Channels side_in(2, kFrames, 0.75f);
    Channels main_out(2, kFrames, 9.0f);
    std::array ins{in_bus(main_in.const_view(kFrames), format::BusRole::Main),
                   in_bus(side_in.const_view(kFrames), format::BusRole::Sidechain)};
    std::array outs{out_bus(main_out.view(kFrames), format::BusRole::Main)};
    format::ProcessBuffers host{format::ProcessBusBufferSet<const float>{std::span(ins)},
                                format::ProcessBusBufferSet<float>{std::span(outs)}};

    const std::array<SlotBusDecl, 2> plugin_in{{{2, true}, {2, false}}};
    const std::array<SlotBusDecl, 1> plugin_out{{{2, true}}};
    SlotBusBinder binder;
    binder.prepare(plugin_in, plugin_out, 64, /*use_double=*/false);
    REQUIRE(binder.bind(host, kFrames));

    REQUIRE(binder.input_count() == 2);
    CHECK(binder.input_bound(0));
    CHECK(binder.input_bound(1));
    CHECK(binder.input32(1)[0] == side_in.ptrs[0]);
    CHECK(binder.input32(1)[0][3] == 0.75f);
    // The main output writes straight into the host buffer, no copy.
    CHECK(binder.output32(0)[1] == main_out.ptrs[1]);
}

TEST_CASE("Bus binder gives an unsupplied plugin bus silence and a discard buffer",
          "[host][plugin-slot][buses]") {
    constexpr std::size_t kFrames = 16;
    Channels main_in(2, kFrames, 0.5f);
    Channels main_out(2, kFrames, 0.0f);
    std::array ins{in_bus(main_in.const_view(kFrames), format::BusRole::Main)};
    std::array outs{out_bus(main_out.view(kFrames), format::BusRole::Main)};
    format::ProcessBuffers host{format::ProcessBusBufferSet<const float>{std::span(ins)},
                                format::ProcessBusBufferSet<float>{std::span(outs)}};

    // Plugin: stereo main + mono sidechain in; stereo main + stereo aux out.
    const std::array<SlotBusDecl, 2> plugin_in{{{2, true}, {1, false}}};
    const std::array<SlotBusDecl, 2> plugin_out{{{2, true}, {2, false}}};
    SlotBusBinder binder;
    binder.prepare(plugin_in, plugin_out, 32, false);
    REQUIRE(binder.bind(host, kFrames));

    CHECK_FALSE(binder.input_bound(1));
    REQUIRE(binder.input_channels(1) == 1);
    for (std::size_t i = 0; i < kFrames; ++i)
        CHECK(binder.input32(1)[0][i] == 0.0f);

    // A plugin writing into its scratch sidechain input must not leak into
    // the next block.
    binder.input32(1)[0][0] = 123.0f;
    REQUIRE(binder.bind(host, kFrames));
    CHECK(binder.input32(1)[0][0] == 0.0f);

    CHECK_FALSE(binder.output_bound(1));
    REQUIRE(binder.output_channels(1) == 2);
    CHECK(binder.output32(1)[0] != nullptr);
}

TEST_CASE("Bus binder zeroes host output channels and buses the plugin does not write",
          "[host][plugin-slot][buses]") {
    constexpr std::size_t kFrames = 4;
    Channels main_out(3, kFrames, 7.0f);
    Channels aux_out(2, kFrames, 7.0f);
    std::array<format::ProcessBusBufferView<const float>, 0> ins{};
    std::array outs{out_bus(main_out.view(kFrames), format::BusRole::Main),
                    out_bus(aux_out.view(kFrames), format::BusRole::Aux)};
    format::ProcessBuffers host{format::ProcessBusBufferSet<const float>{std::span(ins)},
                                format::ProcessBusBufferSet<float>{std::span(outs)}};

    // A mono instrument with one output bus.
    const std::array<SlotBusDecl, 1> plugin_out{{{1, true}}};
    SlotBusBinder binder;
    binder.prepare({}, plugin_out, 8, false);
    REQUIRE(binder.bind(host, kFrames));
    binder.output32(0)[0][0] = 1.0f; // the plugin writes its one channel
    binder.finish(host, kFrames);

    CHECK(main_out.data[0][0] == 1.0f);
    for (std::size_t i = 0; i < kFrames; ++i) {
        CHECK(main_out.data[1][i] == 0.0f);
        CHECK(main_out.data[2][i] == 0.0f);
        CHECK(aux_out.data[0][i] == 0.0f);
        CHECK(aux_out.data[1][i] == 0.0f);
    }
}

TEST_CASE("Bus binder converts through 64-bit scratch in double mode",
          "[host][plugin-slot][buses]") {
    constexpr std::size_t kFrames = 4;
    Channels main_in(1, kFrames, 0.5f);
    Channels main_out(1, kFrames, 0.0f);
    std::array ins{in_bus(main_in.const_view(kFrames), format::BusRole::Main)};
    std::array outs{out_bus(main_out.view(kFrames), format::BusRole::Main)};
    format::ProcessBuffers host{format::ProcessBusBufferSet<const float>{std::span(ins)},
                                format::ProcessBusBufferSet<float>{std::span(outs)}};

    const std::array<SlotBusDecl, 1> mono{{{1, true}}};
    SlotBusBinder binder;
    binder.prepare(mono, mono, 8, /*use_double=*/true);
    REQUIRE(binder.use_double());
    REQUIRE(binder.bind(host, kFrames));
    CHECK(binder.input64(0)[0][2] == 0.5);
    for (std::size_t i = 0; i < kFrames; ++i)
        binder.output64(0)[0][i] = binder.input64(0)[0][i] * 2.0;
    binder.finish(host, kFrames);
    CHECK(main_out.data[0][3] == 1.0f);
}

TEST_CASE("Bus binder refuses a block larger than it was prepared for",
          "[host][plugin-slot][buses]") {
    Channels main_out(2, 64, 0.0f);
    std::array<format::ProcessBusBufferView<const float>, 0> ins{};
    std::array outs{out_bus(main_out.view(64), format::BusRole::Main)};
    format::ProcessBuffers host{format::ProcessBusBufferSet<const float>{std::span(ins)},
                                format::ProcessBusBufferSet<float>{std::span(outs)}};
    const std::array<SlotBusDecl, 1> stereo{{{2, true}}};
    SlotBusBinder binder;
    binder.prepare({}, stereo, 32, false);
    CHECK_FALSE(binder.bind(host, 64));
    CHECK(binder.bind(host, 32));
}

TEST_CASE("Pass-through copies main input to main output and silences the rest",
          "[host][plugin-slot][buses]") {
    constexpr std::size_t kFrames = 4;
    Channels main_in(1, kFrames, 0.3f);
    Channels main_out(2, kFrames, 5.0f);
    Channels aux_out(1, kFrames, 5.0f);
    std::array ins{in_bus(main_in.const_view(kFrames), format::BusRole::Main)};
    std::array outs{out_bus(main_out.view(kFrames), format::BusRole::Main),
                    out_bus(aux_out.view(kFrames), format::BusRole::Aux)};
    format::ProcessBuffers host{format::ProcessBusBufferSet<const float>{std::span(ins)},
                                format::ProcessBusBufferSet<float>{std::span(outs)}};
    SlotBusBinder::pass_through(host, kFrames);
    CHECK(main_out.data[0][1] == 0.3f);
    CHECK(main_out.data[1][1] == 0.0f);
    CHECK(aux_out.data[0][1] == 0.0f);
}

TEST_CASE("MIDI decode covers every channel-voice message", "[host][plugin-slot][midi]") {
    using K = host::detail::ShortMidi::Kind;
    const std::uint8_t on[] = {0x93, 60, 100};
    const std::uint8_t on_zero[] = {0x90, 60, 0};
    const std::uint8_t pp[] = {0xA1, 64, 33};
    const std::uint8_t cc[] = {0xB2, 7, 99};
    const std::uint8_t pc[] = {0xC4, 12};
    const std::uint8_t cp[] = {0xD5, 77};
    const std::uint8_t pb[] = {0xE6, 0x00, 0x40};
    CHECK(host::detail::decode_short_midi(on, 3).kind == K::note_on);
    CHECK(host::detail::decode_short_midi(on, 3).channel == 3);
    CHECK(host::detail::decode_short_midi(on_zero, 3).kind == K::note_off);
    CHECK(host::detail::decode_short_midi(pp, 3).kind == K::poly_pressure);
    CHECK(host::detail::decode_short_midi(cc, 3).kind == K::control_change);
    CHECK(host::detail::decode_short_midi(pc, 2).kind == K::program_change);
    CHECK(host::detail::decode_short_midi(cp, 2).kind == K::channel_pressure);
    const auto bend = host::detail::decode_short_midi(pb, 3);
    CHECK(bend.kind == K::pitch_bend);
    CHECK(bend.bend == 8192);
}

TEST_CASE("Short messages and sysex are walked as one stream in sample order",
          "[host][plugin-slot][midi]") {
    midi::MidiBuffer buffer;
    auto a = midi::MidiEvent::note_on(0, 60, 100);
    a.sample_offset = 0;
    auto b = midi::MidiEvent::note_off(0, 60);
    b.sample_offset = 10;
    buffer.add(a);
    buffer.add(b);
    buffer.add_sysex({0xF0, 0x7E, 0xF7}, 5);
    buffer.add_sysex({0xF0, 0x01, 0xF7}, 10);

    std::vector<int> order;
    host::detail::for_each_midi_in_time_order(
        buffer, [&](const midi::MidiEvent& m) { order.push_back(m.sample_offset * 10); },
        [&](const midi::MidiBuffer::SysexEvent& s) { order.push_back(s.sample_offset * 10 + 1); });
    // Short at 0, sysex at 5, short at 10 before the sysex at 10.
    CHECK(order == std::vector<int>{0, 51, 100, 101});
}

TEST_CASE("Parameter edit inbox keeps order per lane and counts drops",
          "[host][plugin-slot][params]") {
    ParameterEditInbox inbox;
    REQUIRE(inbox.push_from_control({PluginParameterEdit::Kind::begin_gesture, 4, 0.0f}));
    REQUIRE(inbox.push_from_audio({PluginParameterEdit::Kind::value, 9, 0.5f}));
    REQUIRE(inbox.push_from_control({PluginParameterEdit::Kind::value, 4, 0.25f}));

    std::array<PluginParameterEdit, 8> out{};
    REQUIRE(inbox.drain(out) == 3);
    CHECK(out[0].kind == PluginParameterEdit::Kind::begin_gesture);
    CHECK(out[1].id == 4);
    CHECK(out[1].plain_value == 0.25f);
    CHECK(out[2].id == 9);
    CHECK(inbox.drain(out) == 0);

    for (std::size_t i = 0; i < ParameterEditInbox::kAudioCapacity + 10; ++i)
        inbox.push_from_audio({PluginParameterEdit::Kind::value, 1, 0.0f});
    CHECK(inbox.dropped() >= 10);
}

TEST_CASE("Transport validity: an unlabelled context supplies only its core fields",
          "[host][plugin-slot][transport]") {
    using F = format::TransportField;
    format::ProcessContext legacy;
    CHECK(host::detail::transport_has(legacy, F::Tempo));
    CHECK(host::detail::transport_has(legacy, F::BeatPosition));
    CHECK_FALSE(host::detail::transport_has(legacy, F::HostTime));
    CHECK_FALSE(host::detail::transport_has(legacy, F::Bar));

    format::ProcessContext labelled;
    labelled.transport_validity.set(F::Playing);
    CHECK(host::detail::transport_has(labelled, F::Playing));
    CHECK_FALSE(host::detail::transport_has(labelled, F::Tempo));

    labelled.is_looping = true;
    labelled.loop_start_beats = 4.0;
    labelled.loop_end_beats = 8.0;
    CHECK_FALSE(host::detail::transport_has_loop_range(labelled));
    labelled.transport_validity.set(F::LoopRange);
    CHECK(host::detail::transport_has_loop_range(labelled));
}
