#pragma once

// The editor-host slice's processor: one gain parameter and one peak meter.
//
// Its audio code is compiled once, into one static library, and linked
// unchanged into every slice executable (no editor, a Vellum editor, an RmlUi
// editor). The executables differ only in which `slice_editor_factory()`
// definition they link, which is the per-plug-in hook that
// `pulp_add_plugin(... UI <package>)` is to generate.

#include <pulp/format/editor_host.hpp>
#include <pulp/format/processor.hpp>

#include <atomic>
#include <cstdint>
#include <memory>

namespace pulp::slice {

inline constexpr state::ParamID kGainParam = 1;
inline constexpr float kGainDefault = 0.5f;

/// The per-binary editor hook. Exactly one definition is linked into each
/// executable; the headless one returns null.
format::EditorFactory* slice_editor_factory();

/// Latest-wins peak meter the audio thread publishes and the UI reads.
struct PeakMeter {
    std::atomic<float> peak{0.0f};
    std::atomic<std::uint32_t> publishes{0};
};

class GainMeterProcessor final : public format::Processor {
  public:
    static std::unique_ptr<format::Processor> create();

    format::PluginDescriptor descriptor() const override;
    void define_parameters(state::StateStore& store) override;
    void prepare(const format::PrepareContext& context) override;
    void process(audio::BufferView<float>& output, const audio::BufferView<const float>& input,
                 midi::MidiBuffer& midi_in, midi::MidiBuffer& midi_out,
                 const format::ProcessContext& context) override;
    format::EditorFactory* editor_factory() override {
        return slice_editor_factory();
    }

    PeakMeter& meter() noexcept {
        return meter_;
    }

  private:
    PeakMeter meter_;
};

/// A `VisualPort` with one meter channel over `PeakMeter`. The data-plane move
/// replaces this with the real channel set.
class PeakMeterVisualPort final : public format::VisualPort {
  public:
    explicit PeakMeterVisualPort(PeakMeter& meter) : meter_(meter) {}

    std::size_t channel_count() const noexcept override {
        return 1;
    }
    format::VisualChannelInfo channel(std::size_t) const noexcept override {
        return {"output_peak", "", format::VisualChannelShape::meter, 0.0f};
    }
    bool read_scalar(std::size_t, float&) noexcept override {
        return false;
    }
    std::size_t read_meter(std::size_t index, std::span<float> rms,
                           std::span<float> peak) noexcept override;
    std::size_t read_vector(std::size_t, std::span<float>) noexcept override {
        return 0;
    }
    std::size_t read_events(std::size_t, std::span<format::VisualEvent>) noexcept override {
        return 0;
    }
    std::uint32_t publish_count(std::size_t index) const noexcept override {
        return index == 0 ? meter_.publishes.load(std::memory_order_acquire) : 0;
    }

  private:
    PeakMeter& meter_;
};

} // namespace pulp::slice
