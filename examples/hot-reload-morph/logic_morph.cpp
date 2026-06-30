// Hot-reload MORPH demo — the DSP+UI "logic" you swap live.
//
// Build it two ways (the build flips -DMORPH_HARSH); each is a *different plugin*
// behind the SAME parameter contract (Depth id1, Rate id2) so the shell accepts
// the hot-swap:
//
//   Version A "WARM"   — a gentle SINE tremolo + a calm blue editor.
//   Version B "HARSH"  — a hard SQUARE chop  + an aggressive red editor.
//
// Reloading swaps BOTH the sound (sine wobble → square chop) and the look (blue
// "WARM" → red "HARSH"): visually and sonically different from one swap. The
// view is self-contained (static text + color, no reference to this processor),
// so it stays valid even after this processor is retired by a later swap.

#include <pulp/format/processor.hpp>
#include <pulp/format/reload/reload_abi.hpp>
#include <pulp/state/store.hpp>
#include <pulp/view/view.hpp>
#include <pulp/view/widgets.hpp>
#include <pulp/canvas/canvas.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <memory>

using namespace pulp;

namespace {

// IDENTICAL parameter contract for both versions — same ids, ranges, AND
// defaults — so the hot-swap is accepted (the contract gate compares defaults
// too). The audible difference is the LFO SHAPE in the code, not the params.
constexpr float kDefaultDepth = 0.70f;
constexpr float kDefaultRate  = 6.0f;

#if defined(MORPH_HARSH)
constexpr bool kSquare      = true;
constexpr const char* kName = "HARSH";
constexpr const char* kSub  = "square chop";
inline canvas::Color bg()    { return canvas::Color::rgba8(120, 20, 24, 255); }   // deep red
inline canvas::Color accent(){ return canvas::Color::rgba8(255, 90, 80, 255); }
#else
constexpr bool kSquare      = false;
constexpr const char* kName = "WARM";
constexpr const char* kSub  = "sine tremolo";
inline canvas::Color bg()    { return canvas::Color::rgba8(18, 34, 64, 255); }    // deep blue
inline canvas::Color accent(){ return canvas::Color::rgba8(120, 190, 255, 255); }
#endif

class MorphDsp final : public format::Processor {
public:
    format::PluginDescriptor descriptor() const override {
        return {.name = "Pulp Hot-Reload Morph", .manufacturer = "Pulp",
                .bundle_id = "com.pulp.hot-reload-morph", .version = "1.0.0",
                .category = format::PluginCategory::Effect,
                .input_buses = {{"In", 2}}, .output_buses = {{"Out", 2}}};
    }

    void define_parameters(state::StateStore& s) override {
        s.add_parameter({.id = 1, .name = "Depth", .unit = "",
                         .range = {0.0f, 1.0f, kDefaultDepth, 0.0f}});
        s.add_parameter({.id = 2, .name = "Rate", .unit = "Hz",
                         .range = {0.1f, 20.0f, kDefaultRate, 0.0f}});
    }

    void prepare(const format::PrepareContext& ctx) override {
        sr_ = ctx.sample_rate > 0 ? ctx.sample_rate : 48000.0;
        phase_ = 0.0;
    }

    void process(audio::BufferView<float>& out, const audio::BufferView<const float>& in,
                 midi::MidiBuffer&, midi::MidiBuffer&, const format::ProcessContext&) override {
        const float depth = std::clamp(state().get_value(1), 0.0f, 1.0f);
        const float rate  = std::clamp(state().get_value(2), 0.1f, 20.0f);
        const double inc  = static_cast<double>(rate) / sr_;
        const std::size_t ch = std::min(out.num_channels(), in.num_channels());
        const std::size_t frames = out.num_samples();
        for (std::size_t n = 0; n < frames; ++n) {
            float lfo;  // unipolar 0..1
            if constexpr (kSquare)
                lfo = phase_ < 0.5 ? 0.0f : 1.0f;                                   // hard chop
            else
                lfo = 0.5f * (1.0f - std::cos(2.0f * 3.14159265358979f * static_cast<float>(phase_)));
            const float g = 1.0f - depth * lfo;
            for (std::size_t c = 0; c < ch; ++c) out.channel(c)[n] = in.channel(c)[n] * g;
            phase_ += inc; if (phase_ >= 1.0) phase_ -= 1.0;
        }
        for (std::size_t c = ch; c < out.num_channels(); ++c) {
            auto o = out.channel(c);
            for (std::size_t n = 0; n < frames; ++n) o[n] = 0.0f;
        }
    }

    // A self-contained editor: just color + text, no reference to `this`, so it
    // survives this processor being retired by a later swap.
    std::unique_ptr<view::View> create_view() override {
        using namespace pulp::view;
        auto root = std::make_unique<View>();
        root->set_bounds({0, 0, 360, 200});
        root->set_background_color(bg());
        root->flex().direction = FlexDirection::column;
        root->flex().justify_content = FlexJustify::center;
        root->flex().align_items = FlexAlign::center;
        root->flex().gap = 8.0f;
        root->flex().padding = 24.0f;

        auto title = std::make_unique<Label>(kName);
        title->set_font_size(48.0f);
        title->set_text_color(accent());
        title->set_text_align(LabelAlign::center);
        title->flex().preferred_width = 312.0f; title->flex().preferred_height = 60.0f;
        root->add_child(std::move(title));

        auto sub = std::make_unique<Label>(kSub);
        sub->set_font_size(16.0f);
        sub->set_text_color(canvas::Color::rgba8(220, 224, 230, 255));
        sub->set_text_align(LabelAlign::center);
        sub->flex().preferred_width = 312.0f; sub->flex().preferred_height = 22.0f;
        root->add_child(std::move(sub));

        auto hint = std::make_unique<Label>("DSP + UI hot-reloaded together");
        hint->set_font_size(11.0f);
        hint->set_text_color(canvas::Color::rgba8(150, 160, 175, 255));
        hint->set_text_align(LabelAlign::center);
        hint->flex().preferred_width = 312.0f; hint->flex().preferred_height = 16.0f;
        root->add_child(std::move(hint));
        return root;
    }

private:
    double sr_ = 48000.0, phase_ = 0.0;
};

}  // namespace

PULP_RELOAD_LOGIC(new MorphDsp())
