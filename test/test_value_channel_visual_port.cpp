// test_value_channel_visual_port.cpp — the default VisualPort over a
// processor's ValueChannelSet, linked against pulp::format-core alone.
//
// The CMake target links pulp::format-core and nothing else, and the manifest
// walks its link closure at configure time, so these cases also prove the
// editor data plane reaches an editor with no view, canvas or render layer.

#include <catch2/catch_test_macros.hpp>

#include <pulp/dataplane/value_channel_set.hpp>
#include <pulp/format/processor.hpp>
#include <pulp/format/value_channel_visual_port.hpp>

#include <array>
#include <optional>
#include <string_view>

using pulp::dataplane::MeterFrame;
using pulp::dataplane::ValueChannelSet;
using pulp::dataplane::ValueEvent;
using pulp::format::ValueChannelVisualPort;
using pulp::format::VisualChannelShape;
using pulp::format::VisualEvent;

namespace {

class ChannelProcessor : public pulp::format::Processor {
  public:
    ChannelProcessor() {
        gain_reduction = channels.declare_scalar("gain_reduction", "dB", -1.0f);
        output = channels.declare_meter("output");
        scope = channels.declare_vector("scope");
        onsets = channels.declare_events("onsets");
    }

    pulp::format::PluginDescriptor descriptor() const override { return {}; }
    void define_parameters(pulp::state::StateStore&) override {}
    void prepare(const pulp::format::PrepareContext&) override {}
    void process(pulp::audio::BufferView<float>&, const pulp::audio::BufferView<const float>&,
                 pulp::midi::MidiBuffer&, pulp::midi::MidiBuffer&,
                 const pulp::format::ProcessContext&) override {}

    ValueChannelSet* value_channels() override { return &channels; }

    ValueChannelSet channels;
    pulp::dataplane::ScalarSource* gain_reduction = nullptr;
    pulp::dataplane::MeterSource* output = nullptr;
    pulp::dataplane::VectorSource* scope = nullptr;
    pulp::dataplane::EventSource* onsets = nullptr;
};

class SilentProcessor : public pulp::format::Processor {
  public:
    pulp::format::PluginDescriptor descriptor() const override { return {}; }
    void define_parameters(pulp::state::StateStore&) override {}
    void prepare(const pulp::format::PrepareContext&) override {}
    void process(pulp::audio::BufferView<float>&, const pulp::audio::BufferView<const float>&,
                 pulp::midi::MidiBuffer&, pulp::midi::MidiBuffer&,
                 const pulp::format::ProcessContext&) override {}
};

} // namespace

TEST_CASE("ValueChannelVisualPort lists the processor's channels in declaration order",
          "[editor-host][dataplane]") {
    ChannelProcessor processor;
    ValueChannelVisualPort port(processor);

    REQUIRE(port.channel_count() == 4);
    const auto gr = port.channel(0);
    CHECK(gr.name == "gain_reduction");
    CHECK(gr.unit == "dB");
    CHECK(gr.shape == VisualChannelShape::scalar);
    CHECK(gr.neutral == -1.0f);
    CHECK(port.channel(1).shape == VisualChannelShape::meter);
    CHECK(port.channel(2).shape == VisualChannelShape::vector);
    CHECK(port.channel(3).shape == VisualChannelShape::events);

    CHECK(port.find("output", VisualChannelShape::meter) == std::optional<std::size_t>{1});
    CHECK_FALSE(port.find("output", VisualChannelShape::scalar).has_value());
    CHECK(port.channel(4).name.empty());
}

TEST_CASE("ValueChannelVisualPort reads the latest scalar and refuses before a publish",
          "[editor-host][dataplane]") {
    ChannelProcessor processor;
    ValueChannelVisualPort port(processor);

    float value = 42.0f;
    CHECK_FALSE(port.read_scalar(0, value));
    CHECK(value == 42.0f);
    CHECK(port.publish_count(0) == 0);

    processor.gain_reduction->publish(-3.0f);
    processor.gain_reduction->publish(-6.0f);
    REQUIRE(port.read_scalar(0, value));
    CHECK(value == -6.0f);
    CHECK(port.publish_count(0) == 2);

    // A read of another shape's index is refused, not reinterpreted.
    CHECK_FALSE(port.read_scalar(1, value));
    CHECK_FALSE(port.read_scalar(99, value));
}

TEST_CASE("ValueChannelVisualPort copies meter frames up to the caller's spans",
          "[editor-host][dataplane]") {
    ChannelProcessor processor;
    ValueChannelVisualPort port(processor);

    MeterFrame frame;
    frame.channels = 3;
    frame.rms = {0.1f, 0.2f, 0.3f};
    frame.peak = {0.4f, 0.5f, 0.6f};
    processor.output->publish(frame);

    std::array<float, 2> rms{};
    std::array<float, 2> peak{};
    // The frame carries three channels; the spans hold two.
    CHECK(port.read_meter(1, rms, peak) == 3);
    CHECK(rms == std::array<float, 2>{0.1f, 0.2f});
    CHECK(peak == std::array<float, 2>{0.4f, 0.5f});
    CHECK(port.publish_count(1) == 1);

    // A count past the frame's capacity is bounded, never trusted.
    frame.channels = 1000;
    processor.output->publish(frame);
    std::array<float, MeterFrame::kMaxChannels + 2> wide{};
    CHECK(port.read_meter(1, wide, wide) == MeterFrame::kMaxChannels);

    CHECK(port.read_meter(0, rms, peak) == 0);
}

TEST_CASE("ValueChannelVisualPort reads vector blocks and event occurrences",
          "[editor-host][dataplane]") {
    ChannelProcessor processor;
    ValueChannelVisualPort port(processor);

    const std::array<float, 4> block{1.0f, -1.0f, 0.5f, 0.25f};
    processor.scope->publish(block.data(), static_cast<int>(block.size()));
    std::array<float, 3> samples{};
    CHECK(port.read_vector(2, samples) == 3);
    CHECK(samples == std::array<float, 3>{1.0f, -1.0f, 0.5f});

    const std::array<ValueEvent, 2> hits{ValueEvent{16, 0.0f}, ValueEvent{200, 0.9f}};
    processor.onsets->publish(hits.data(), static_cast<int>(hits.size()));
    std::array<VisualEvent, 4> events{};
    REQUIRE(port.read_events(3, events) == 2);
    // A zero value is still an occurrence.
    CHECK(events[0].frame_index == 16);
    CHECK(events[0].value == 0.0f);
    CHECK(events[1].frame_index == 200);
    CHECK(events[1].value == 0.9f);

    CHECK(port.read_vector(3, samples) == 0);
    CHECK(port.read_events(2, events) == 0);
}

TEST_CASE("ValueChannelVisualPort over a processor with no channels is empty",
          "[editor-host][dataplane]") {
    SilentProcessor processor;
    ValueChannelVisualPort port(processor);
    CHECK(port.channel_count() == 0);
    float value = 0.0f;
    CHECK_FALSE(port.read_scalar(0, value));

    ValueChannelVisualPort null_port(nullptr);
    CHECK(null_port.channel_count() == 0);
}
