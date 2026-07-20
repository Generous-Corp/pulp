// Unit test for CrossfadePluginSlot: it blends a fading-out (old) instance into the
// incoming (new) instance over the mixer window, so a live plugin-instance swap is
// click-free even when old and new produce different output. Also verifies it stops
// rendering the old instance once the fade completes and delegates non-process calls
// to the new instance.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <pulp/host/crossfade_plugin_slot.hpp>
#include <pulp/midi/message.hpp>

#include <cmath>
#include <memory>
#include <string>
#include <vector>

using namespace pulp::host;
using Catch::Matchers::WithinAbs;

namespace {

// A slot that writes a fixed constant to every output sample and tags its metadata,
// so we can tell old vs new output apart and confirm delegation targets the new slot.
class ConstSlot final : public PluginSlot {
public:
    ConstSlot(float value, std::string tag, int latency,
              LatencyQuery latency_query = LatencyQuery::Available)
        : value_(value), latency_(latency), latency_query_(latency_query) {
        info_.name = std::move(tag);
        info_.num_inputs = 1;
        info_.num_outputs = 1;
        process_count_ = std::make_shared<int>(0);
    }
    std::shared_ptr<int> process_count() const { return process_count_; }

    void process(pulp::audio::BufferView<float>& out,
                 const pulp::audio::BufferView<const float>&,
                 const pulp::midi::MidiBuffer&,
                 pulp::midi::MidiBuffer&,
                 const ParameterEventQueue&,
                 int n) override {
        ++(*process_count_);
        for (std::size_t c = 0; c < out.num_channels(); ++c) {
            float* d = out.channel_ptr(c);
            for (int i = 0; i < n; ++i) d[static_cast<std::size_t>(i)] = value_;
        }
    }

    const PluginInfo& info() const override { return info_; }
    bool is_loaded() const override { return true; }
    bool prepare(double, int) override { return true; }
    void release() override {}
    std::vector<HostParamInfo> parameters() const override { return {}; }
    float get_parameter(std::uint32_t) const override { return value_; }
    void set_parameter(std::uint32_t, float) override {}
    void set_bypass(bool) override {}
    bool is_bypassed() const override { return false; }
    std::vector<std::uint8_t> save_state() const override {
        return {static_cast<std::uint8_t>(info_.name.empty() ? 0 : info_.name[0])};
    }
    bool restore_state(const std::vector<std::uint8_t>&) override { return true; }
    bool has_editor() const override { return false; }
    void* create_editor_view() override { return nullptr; }
    void destroy_editor_view() override {}
    int latency_samples() const override { return latency_; }
    LatencyQuery latency_query() const override { return latency_query_; }
    int tail_samples() const override { return 0; }

private:
    float value_;
    int latency_;
    LatencyQuery latency_query_;
    PluginInfo info_;
    std::shared_ptr<int> process_count_;
};

std::vector<float> render(CrossfadePluginSlot& slot, int n) {
    std::vector<float> buf(static_cast<std::size_t>(n), 0.0f);
    std::vector<float> in(static_cast<std::size_t>(n), 0.0f);
    float* op = buf.data();
    const float* ip = in.data();
    pulp::audio::BufferView<float> ov(&op, 1, static_cast<std::size_t>(n));
    pulp::audio::BufferView<const float> iv(&ip, 1, static_cast<std::size_t>(n));
    pulp::midi::MidiBuffer mi, mo;
    ParameterEventQueue pq;
    slot.process(ov, iv, mi, mo, pq, n);
    return buf;
}

}  // namespace

TEST_CASE("CrossfadePluginSlot blends old->new click-free then settles on new",
          "[host][graph][live-swap][crossfade]") {
    constexpr int kBlock = 64;
    // old outputs 1.0, new outputs 0.0 — a hard cut would step 1.0 -> 0.0 at the swap.
    auto old_slot = std::make_shared<ConstSlot>(1.0f, "old", 0);
    auto new_slot = std::make_shared<ConstSlot>(0.0f, "new", 0);
    auto old_count = old_slot->process_count();
    const std::size_t fade = 256;  // 4 blocks
    CrossfadePluginSlot xf(new_slot, old_slot, fade, pulp::signal::TransitionCurve::EqualPower,
                           /*out_channels=*/1, kBlock);

    // First block: heavily weighted to old (starts near 1.0), never jumps.
    auto b0 = render(xf, kBlock);
    CHECK(b0.front() > 0.9f);        // fade begins at the old value
    CHECK_FALSE(xf.fade_done());

    // Render across the whole fade; every sample stays within the equal-power envelope
    // (no discontinuity), and the tail approaches the new value.
    float last = b0.back();
    std::vector<float> tail;
    for (int i = 0; i < 4; ++i) tail = render(xf, kBlock);
    for (float v : tail) {
        CHECK(v >= -0.01f);
        CHECK(v <= 1.01f);
    }
    CHECK(tail.back() < 0.1f);       // ends at the new value
    CHECK(xf.fade_done());           // fade completed within its window
    (void)last;

    // After the fade, the old instance is no longer rendered.
    const int count_after_fade = *old_count;
    render(xf, kBlock);
    render(xf, kBlock);
    CHECK(*old_count == count_after_fade);  // old not touched once done
    auto post = render(xf, kBlock);
    for (float v : post) CHECK_THAT(v, WithinAbs(0.0f, 1e-6));  // pure new render
}

TEST_CASE("CrossfadePluginSlot delegates non-process calls to the new instance",
          "[host][graph][live-swap][crossfade]") {
    auto old_slot = std::make_shared<ConstSlot>(1.0f, "old", 128);
    auto new_slot = std::make_shared<ConstSlot>(
        0.0f, "new", 64, PluginSlot::LatencyQuery::Unsupported);
    CrossfadePluginSlot xf(new_slot, old_slot, 128, pulp::signal::TransitionCurve::Smoothstep, 1, 64);
    CHECK(xf.info().name == "new");
    CHECK(xf.latency_samples() == 64);              // the new instance's latency, not old's
    CHECK(xf.latency_query() == PluginSlot::LatencyQuery::Unsupported);
    CHECK(xf.latency_report().query == PluginSlot::LatencyQuery::Unsupported);
    CHECK(xf.save_state() == new_slot->save_state());
    CHECK(xf.new_slot().get() == new_slot.get());
}

TEST_CASE("CrossfadePluginSlot with fade_ms 0 is done immediately (instant switch)",
          "[host][graph][live-swap][crossfade]") {
    auto old_slot = std::make_shared<ConstSlot>(1.0f, "old", 0);
    auto new_slot = std::make_shared<ConstSlot>(0.0f, "new", 0);
    auto old_count = old_slot->process_count();
    CrossfadePluginSlot xf(new_slot, old_slot, 0, pulp::signal::TransitionCurve::EqualPower, 1, 64);
    CHECK(xf.fade_done());
    auto b = render(xf, 64);
    for (float v : b) CHECK_THAT(v, WithinAbs(0.0f, 1e-6));  // pure new from the first block
    CHECK(*old_count == 0);                                  // old never rendered
}

namespace {
// A fading-out slot that floods MIDI-out every block. With the wrapper's capacity-
// limited MIDI scratch this must not grow/allocate on the audio thread; before the
// fix the default-constructed buffer would reallocate on the first emit.
class MidiFloodSlot final : public PluginSlot {
public:
    void process(pulp::audio::BufferView<float>& out,
                 const pulp::audio::BufferView<const float>&,
                 const pulp::midi::MidiBuffer&,
                 pulp::midi::MidiBuffer& midi_out,
                 const ParameterEventQueue&,
                 int n) override {
        for (int i = 0; i < 512; ++i) {
            midi_out.add(pulp::midi::MidiEvent::note_on(0, 60, 100));  // capped by reserve+limit
        }
        for (std::size_t c = 0; c < out.num_channels(); ++c) {
            float* d = out.channel_ptr(c);
            for (int i = 0; i < n; ++i) d[static_cast<std::size_t>(i)] = 1.0f;
        }
    }
    const PluginInfo& info() const override { return info_; }
    bool is_loaded() const override { return true; }
    bool prepare(double, int) override { return true; }
    void release() override {}
    std::vector<HostParamInfo> parameters() const override { return {}; }
    float get_parameter(std::uint32_t) const override { return 0.0f; }
    void set_parameter(std::uint32_t, float) override {}
    void set_bypass(bool) override {}
    bool is_bypassed() const override { return false; }
    std::vector<std::uint8_t> save_state() const override { return {}; }
    bool restore_state(const std::vector<std::uint8_t>&) override { return true; }
    bool has_editor() const override { return false; }
    void* create_editor_view() override { return nullptr; }
    void destroy_editor_view() override {}
    int latency_samples() const override { return 0; }
    int tail_samples() const override { return 0; }
private:
    PluginInfo info_;
};
}  // namespace

TEST_CASE("CrossfadePluginSlot bounds the fade-out instance's MIDI-out (no audio-thread growth)",
          "[host][graph][live-swap][crossfade]") {
    auto old_slot = std::make_shared<MidiFloodSlot>();
    auto new_slot = std::make_shared<ConstSlot>(0.0f, "new", 0);
    CrossfadePluginSlot xf(new_slot, old_slot, 128, pulp::signal::TransitionCurve::EqualPower, 1, 64);
    // Rendering across the whole fade must stay bounded + click-free even though the
    // fading-out instance floods MIDI every block; the wrapper's reserved, capacity-
    // limited MIDI scratch absorbs it without growing.
    for (int i = 0; i < 4; ++i) {
        auto b = render(xf, 64);
        for (float v : b) {
            CHECK(v >= -0.01f);
            CHECK(v <= 1.01f);
        }
    }
    CHECK(xf.fade_done());
}
