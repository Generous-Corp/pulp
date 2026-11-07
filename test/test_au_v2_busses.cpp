// AU v2 multi-bus contract tests.
//
// Covers the multi-bus lift for the AU v2 adapters: the instrument (aumu /
// MusicDeviceBase) now advertises one output ELEMENT per declared output bus
// (main + aux) instead of a hard-capped single output, and the effect exposes a
// Sidechain input bus when the descriptor declares one. The descriptor->bus
// helpers are pure and header-inline (like build_channel_info), so the
// count/name/role/arrangement contract is unit-testable without an
// AudioComponentInstance or a live AU host.

#include <pulp/format/au_v2_instrument.hpp>
#include <pulp/format/au_v2_adapter.hpp>
#include <pulp/format/detail/audio_buffer_list_validation.hpp>
#include <pulp/format/processor.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstring>

using pulp::format::PluginDescriptor;
using pulp::format::BusDirection;
using pulp::format::BusRole;
using pulp::format::ProcessBusBufferInfo;

namespace {
PluginDescriptor multi_out_instrument(std::size_t n_out) {
    PluginDescriptor d;
    d.category = pulp::format::PluginCategory::Instrument;
    d.input_buses = {};
    d.output_buses.clear();
    d.output_buses.push_back({"Voice 1 (Main)", 2});
    for (std::size_t i = 1; i < n_out; ++i)
        d.output_buses.push_back({"Aux", 2});
    return d;
}
}  // namespace

TEST_CASE("AU AudioBufferList validation distinguishes valid and malformed storage",
          "[au][bus-layout][malformed-layout]") {
    constexpr UInt32 kFrames = 8;
    std::array<float, kFrames> left{};
    std::array<float, kFrames> right{};
    struct StereoBufferList {
        AudioBufferList list;
        AudioBuffer extra[1];
    } buffers{};
    buffers.list.mNumberBuffers = 2;
    buffers.list.mBuffers[0] = {1, sizeof(left), left.data()};
    buffers.list.mBuffers[1] = {1, sizeof(right), right.data()};

    REQUIRE(pulp::format::detail::audio_buffer_list_shape_matches(
        &buffers.list, 2));
    REQUIRE(pulp::format::detail::audio_buffer_list_has_storage(
        &buffers.list, kFrames, sizeof(float)));

    SECTION("fewer buffers") {
        buffers.list.mNumberBuffers = 1;
        REQUIRE_FALSE(pulp::format::detail::audio_buffer_list_shape_matches(
            &buffers.list, 2));
    }
    SECTION("more buffers") {
        buffers.list.mNumberBuffers = 3;
        REQUIRE_FALSE(pulp::format::detail::audio_buffer_list_shape_matches(
            &buffers.list, 2));
    }
    SECTION("wrong channel count") {
        buffers.list.mBuffers[1].mNumberChannels = 2;
        REQUIRE_FALSE(pulp::format::detail::audio_buffer_list_shape_matches(
            &buffers.list, 2));
    }
    SECTION("null active storage") {
        buffers.list.mBuffers[1].mData = nullptr;
        REQUIRE_FALSE(pulp::format::detail::audio_buffer_list_has_storage(
            &buffers.list, kFrames, sizeof(float)));
    }
    SECTION("undersized active storage") {
        buffers.list.mBuffers[1].mDataByteSize = sizeof(float);
        REQUIRE_FALSE(pulp::format::detail::audio_buffer_list_has_storage(
            &buffers.list, kFrames, sizeof(float)));
    }
}

TEST_CASE("AU v2 instrument: output element count is one per declared output bus",
          "[au][au-v2][bus]") {
    REQUIRE(pulp::format::au::instrument_output_element_count(
                multi_out_instrument(8)) == 8);
    REQUIRE(pulp::format::au::instrument_output_element_count(
                multi_out_instrument(1)) == 1);

    // No declared output bus still floors at one element — an AU MusicDevice
    // must always have at least one output element or the host rejects it.
    PluginDescriptor none;
    none.output_buses.clear();
    REQUIRE(pulp::format::au::instrument_output_element_count(none) == 1);
}

TEST_CASE("AU v2 instrument: output element count is clamped to the bus cap",
          "[au][au-v2][bus]") {
    // Render() fills at most BusBufferSet::kMaxBuses output views, so the
    // advertised AU element count MUST be clamped there — otherwise a descriptor
    // with more output buses than the cap would materialise AU elements that the
    // render path never routes audio to (permanently silent buses in the host).
    constexpr std::size_t kCap = pulp::format::BusBufferSet::kMaxBuses;

    // Exactly at the cap: every declared bus fits, nothing is dropped.
    REQUIRE(pulp::format::au::instrument_output_element_count(
                multi_out_instrument(kCap)) == kCap);

    // Above the cap: clamp to the cap rather than advertising unroutable buses.
    REQUIRE(pulp::format::au::instrument_output_element_count(
                multi_out_instrument(kCap + 1)) == kCap);
    REQUIRE(pulp::format::au::instrument_output_element_count(
                multi_out_instrument(kCap + 32)) == kCap);
}

TEST_CASE("AU v2 instrument: output bus infos map index 0 -> Main, rest -> Aux",
          "[au][au-v2][bus]") {
    const auto desc = multi_out_instrument(8);
    std::array<ProcessBusBufferInfo, 16> infos{};
    const std::size_t n =
        pulp::format::au::build_output_bus_infos(desc, infos.data(), infos.size());

    REQUIRE(n == 8);
    REQUIRE(infos[0].role == BusRole::Main);
    REQUIRE(infos[0].index == 0);
    REQUIRE(infos[0].name == "Voice 1 (Main)");
    REQUIRE_FALSE(infos[0].optional);
    for (std::size_t i = 1; i < n; ++i) {
        INFO("bus " << i);
        REQUIRE(infos[i].role == BusRole::Aux);
        REQUIRE(infos[i].index == i);
        REQUIRE(infos[i].optional);           // aux buses are optional
        REQUIRE(infos[i].direction == BusDirection::Output);
        REQUIRE(infos[i].declared_channels == 2);
    }
}

TEST_CASE("AU v2 instrument: build_output_bus_infos honours the cap without reorder",
          "[au][au-v2][bus]") {
    const auto desc = multi_out_instrument(8);
    std::array<ProcessBusBufferInfo, 3> infos{};
    const std::size_t n =
        pulp::format::au::build_output_bus_infos(desc, infos.data(), infos.size());
    REQUIRE(n == 3);
    REQUIRE(infos[0].index == 0);
    REQUIRE(infos[2].index == 2);  // indices stay contiguous / in order
}

TEST_CASE("AU v2 instrument: mono aux bus reports its declared width",
          "[au][au-v2][bus]") {
    PluginDescriptor d;
    d.category = pulp::format::PluginCategory::Instrument;
    d.input_buses = {};
    d.output_buses = {{"Main", 2}, {"Mono Aux", 1}};
    std::array<ProcessBusBufferInfo, 4> infos{};
    const std::size_t n =
        pulp::format::au::build_output_bus_infos(d, infos.data(), infos.size());
    REQUIRE(n == 2);
    REQUIRE(infos[0].declared_channels == 2);
    REQUIRE(infos[1].declared_channels == 1);
    REQUIRE(infos[1].role == BusRole::Aux);
}

TEST_CASE("AU v2 effect: default single-input descriptor exposes only a Main bus",
          "[au][au-v2][bus]") {
    PluginDescriptor d;  // default effect: one stereo input bus
    std::array<ProcessBusBufferInfo, 2> infos{};
    const std::size_t n =
        pulp::format::au::build_input_bus_infos(d, infos.data(), infos.size());
    REQUIRE(n == 1);  // regression: no phantom sidechain for a plain effect
    REQUIRE(infos[0].role == BusRole::Main);
    REQUIRE(infos[0].index == 0);
}

TEST_CASE("AU v2 effect: a declared sidechain input surfaces an inactive Sidechain bus",
          "[au][au-v2][bus]") {
    PluginDescriptor d;
    d.input_buses = {{"Main In", 2, false}, {"Sidechain", 2, true}};
    std::array<ProcessBusBufferInfo, 2> infos{};
    const std::size_t n =
        pulp::format::au::build_input_bus_infos(d, infos.data(), infos.size());
    REQUIRE(n == 2);
    REQUIRE(infos[0].role == BusRole::Main);
    REQUIRE(infos[1].role == BusRole::Sidechain);
    REQUIRE(infos[1].index == 1);
    REQUIRE(infos[1].name == "Sidechain");
    // AUEffectBase pulls only input element 0, so the sidechain view is
    // delivered inactive — sidechain_input() returns nullptr rather than
    // exposing a bus the stock render path cannot feed. Disconnected-but-present
    // keeps the bus->buffer mapping stable.
    REQUIRE_FALSE(infos[1].active);
    REQUIRE(infos[1].optional);
}

TEST_CASE("AU v2 effect: instrument descriptor (no input bus) reports 0-channel Main",
          "[au][au-v2][bus]") {
    PluginDescriptor d;
    d.input_buses = {};  // no audio input
    std::array<ProcessBusBufferInfo, 2> infos{};
    const std::size_t n =
        pulp::format::au::build_input_bus_infos(d, infos.data(), infos.size());
    REQUIRE(n == 1);
    REQUIRE(infos[0].declared_channels == 0);
    REQUIRE(infos[0].optional);  // a 0-channel main input is optional
}

TEST_CASE("AU v2 channel capabilities expose only descriptor-declared layouts",
          "[au][au-v2][bus-layout][negotiation]") {
    PluginDescriptor d;
    d.supported_bus_layouts = {
        {.inputs = {2}, .outputs = {2}, .name = "Stereo"},
        {.inputs = {1}, .outputs = {1}, .name = "Mono"},
        // Duplicate must not leak an extra capability pair to the host.
        {.inputs = {2}, .outputs = {2}, .name = "Stereo duplicate"},
    };

    std::array<AUChannelInfo, pulp::format::au::kMaxChannelInfoPairs> capabilities{};
    const auto count = pulp::format::au::build_channel_info(d, capabilities.data());
    REQUIRE(count == 2);
    REQUIRE(capabilities[0].inChannels == 2);
    REQUIRE(capabilities[0].outChannels == 2);
    REQUIRE(capabilities[1].inChannels == 1);
    REQUIRE(capabilities[1].outChannels == 1);
}

namespace {
PluginDescriptor instrument_with_inputs(bool optional_main, int sidechain_channels) {
    PluginDescriptor d;
    d.category = pulp::format::PluginCategory::Instrument;
    d.input_buses = {{"Main In", 2, optional_main}};
    if (sidechain_channels >= 0)
        d.input_buses.push_back({"Side Chain", sidechain_channels, true});
    d.output_buses = {{"Main Out", 2}};
    d.accepts_midi = true;
    return d;
}
} // namespace

TEST_CASE("AU v2 instrument input element count follows the declared input buses",
          "[au][au-v2][instrument][bus]") {
    using pulp::format::au::instrument_input_element_count;
    REQUIRE(instrument_input_element_count(multi_out_instrument(1)) == 0);
    REQUIRE(instrument_input_element_count(instrument_with_inputs(true, -1)) == 1);
    REQUIRE(instrument_input_element_count(instrument_with_inputs(true, 2)) == 2);
    REQUIRE(instrument_input_element_count(instrument_with_inputs(false, 1)) == 2);

    // A 0-channel side chain adds no element.
    REQUIRE(instrument_input_element_count(instrument_with_inputs(true, 0)) == 1);

    // Elements are positional: a side chain behind a 0-channel main input has
    // no element to live in, so neither is exposed.
    PluginDescriptor no_main = instrument_with_inputs(true, 2);
    no_main.input_buses[0].default_channels = 0;
    REQUIRE(instrument_input_element_count(no_main) == 0);
}

TEST_CASE("AU v2 input bus infos keep a declared-optional main input optional",
          "[au][au-v2][instrument][bus]") {
    // The infos view the descriptor's names, so it must outlive them.
    const PluginDescriptor d = instrument_with_inputs(true, 2);
    std::array<ProcessBusBufferInfo, 2> infos{};
    const std::size_t n = pulp::format::au::build_input_bus_infos(d, infos.data(), infos.size());
    REQUIRE(n == 2);
    REQUIRE(infos[0].name == "Main In");
    REQUIRE(infos[0].index == 0);
    REQUIRE(infos[0].role == BusRole::Main);
    REQUIRE(infos[0].declared_channels == 2);
    REQUIRE(infos[0].optional);
    REQUIRE(infos[1].name == "Side Chain");
    REQUIRE(infos[1].index == 1);
    REQUIRE(infos[1].role == BusRole::Sidechain);
    REQUIRE(infos[1].optional);
    REQUIRE_FALSE(infos[1].active);
}

TEST_CASE("AU v2 input bus infos report an effect's required main input as required",
          "[au][au-v2][bus]") {
    PluginDescriptor d; // default {"Main In", 2, false}
    std::array<ProcessBusBufferInfo, 2> infos{};
    const std::size_t n = pulp::format::au::build_input_bus_infos(d, infos.data(), infos.size());
    REQUIRE(n == 1);
    REQUIRE(infos[0].declared_channels == 2);
    REQUIRE_FALSE(infos[0].optional);
}

TEST_CASE("AU v2 channel capabilities for an instrument with an optional input "
          "include the unconnected rows",
          "[au][au-v2][instrument][bus-layout]") {
    std::array<AUChannelInfo, pulp::format::au::kMaxChannelInfoPairs> capabilities{};
    const auto count =
        pulp::format::au::build_channel_info(instrument_with_inputs(true, 2), capabilities.data());
    // {0,1} {0,2} for the unconnected input, then the matched {1,1} {2,2}.
    REQUIRE(count == 4);
    REQUIRE(capabilities[0].inChannels == 0);
    REQUIRE(capabilities[0].outChannels == 1);
    REQUIRE(capabilities[1].inChannels == 0);
    REQUIRE(capabilities[1].outChannels == 2);
    REQUIRE(capabilities[2].inChannels == 1);
    REQUIRE(capabilities[2].outChannels == 1);
    REQUIRE(capabilities[3].inChannels == 2);
    REQUIRE(capabilities[3].outChannels == 2);
}

TEST_CASE("AU v2 channel capabilities for a required input stay the matched ladder",
          "[au][au-v2][bus-layout]") {
    std::array<AUChannelInfo, pulp::format::au::kMaxChannelInfoPairs> capabilities{};
    SECTION("effect") {
        PluginDescriptor d;
        const auto count = pulp::format::au::build_channel_info(d, capabilities.data());
        REQUIRE(count == 2);
        REQUIRE(capabilities[0].inChannels == 1);
        REQUIRE(capabilities[1].inChannels == 2);
    }
    SECTION("instrument with a required input") {
        const auto count = pulp::format::au::build_channel_info(instrument_with_inputs(false, -1),
                                                                capabilities.data());
        REQUIRE(count == 2);
        REQUIRE(capabilities[0].inChannels == 1);
        REQUIRE(capabilities[1].inChannels == 2);
    }
    SECTION("instrument without an input keeps the zero-input ladder") {
        const auto count =
            pulp::format::au::build_channel_info(multi_out_instrument(1), capabilities.data());
        REQUIRE(count == 2);
        REQUIRE(capabilities[0].inChannels == 0);
        REQUIRE(capabilities[1].inChannels == 0);
        REQUIRE(capabilities[1].outChannels == 2);
    }
}
