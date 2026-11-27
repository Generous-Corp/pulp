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
#include <utility>

namespace pulp::slice {

inline constexpr state::ParamID kGainParam = 1;
inline constexpr float kGainDefault = 0.5f;

/// The smallest size every slice editor takes; set_size() refuses anything
/// smaller.
inline constexpr std::uint32_t kEditorMinWidth = 120;
inline constexpr std::uint32_t kEditorMinHeight = 80;

/// The size hints every slice editor declares: resizable from the minimum up,
/// with no maximum, preferring its current size. An editor that declares no
/// minimum is fixed-size, and its hosts then keep it at one size.
inline format::ViewSize slice_editor_size_hints(const format::EditorSize& current) {
    format::ViewSize hints;
    hints.preferred_width = current.width;
    hints.preferred_height = current.height;
    hints.min_width = kEditorMinWidth;
    hints.min_height = kEditorMinHeight;
    return hints;
}

/// The per-binary editor hook. Exactly one definition is linked into each
/// executable; the headless one returns null.
format::EditorFactory* slice_editor_factory();

/// While set, every `GainMeterProcessor` created hands out editors that wrap the
/// slice's own and declare no size hints, so they are fixed-size. It is the
/// control for the hosts' fixed-size behavior: the real editors declare a
/// minimum, and a check that only runs them cannot show what a fixed-size
/// editor does.
void slice_set_fixed_size_editors(bool fixed);
bool slice_fixed_size_editors();

/// How many times a fixed-size control editor's `set_size()` has been called.
std::uint32_t slice_fixed_editor_set_size_calls();

/// An editor that forwards everything to the slice's own editor except
/// `size_hints()`, which keeps the default (the current size, no minimum), and
/// `set_size()`, which it counts.
class FixedSizeEditor final : public format::Editor {
  public:
    explicit FixedSizeEditor(std::unique_ptr<format::Editor> inner) : inner_(std::move(inner)) {}

    bool supports_parent(format::NativeParentKind kind) const override {
        return inner_->supports_parent(kind);
    }
    bool attach(const format::NativeParent& parent) override {
        return inner_->attach(parent);
    }
    void detach() override {
        inner_->detach();
    }
    void invalidate() override {
        inner_->invalidate();
    }
    format::EditorSize size() const override {
        return inner_->size();
    }
    bool set_size(const format::EditorSize& size) override;
    format::EditorSize adjust_size(const format::EditorSize& proposed) const override {
        return inner_->adjust_size(proposed);
    }
    void idle() override {
        inner_->idle();
    }
    void visibility_changed(bool visible) override {
        inner_->visibility_changed(visible);
    }
    void focus_changed(bool focused) override {
        inner_->focus_changed(focused);
    }
    format::KeyDisposition offer_key(const format::EditorKeyEvent& event) override {
        return inner_->offer_key(event);
    }
    void parameter_highlight(state::ParamID id, bool highlighted,
                             std::uint32_t color_rgb) override {
        inner_->parameter_highlight(id, highlighted, color_rgb);
    }
    format::EditorAutomation* automation() override {
        return inner_->automation();
    }

  private:
    std::unique_ptr<format::Editor> inner_;
};

class FixedSizeEditorFactory final : public format::EditorFactory {
  public:
    explicit FixedSizeEditorFactory(format::EditorFactory* inner) : inner_(inner) {}

    bool supports(format::EditorRole role) const override {
        return inner_ != nullptr && inner_->supports(role);
    }
    std::unique_ptr<format::Editor> create(format::Processor& processor,
                                           format::EditorContext& context,
                                           format::EditorRole role) override {
        auto editor = inner_ != nullptr ? inner_->create(processor, context, role) : nullptr;
        if (editor == nullptr)
            return nullptr;
        return std::make_unique<FixedSizeEditor>(std::move(editor));
    }

  private:
    format::EditorFactory* inner_;
};

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
        if (slice_fixed_size_editors())
            return &fixed_size_factory_;
        return slice_editor_factory();
    }

    PeakMeter& meter() noexcept {
        return meter_;
    }

  private:
    PeakMeter meter_;
    FixedSizeEditorFactory fixed_size_factory_{slice_editor_factory()};
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
