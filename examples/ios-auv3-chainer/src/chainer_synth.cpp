#include "chainer_synth.hpp"

#include <pulp/canvas/canvas.hpp>
#include <pulp/runtime/log.hpp>
#include <pulp/state/store.hpp>
#include <pulp/view/frame_clock.hpp>
#include <pulp/view/widgets.hpp>
#include <pulp/view/theme.hpp>

#include <algorithm>
#include <cmath>
#include <memory>
#include <string>

namespace pulp::examples::ios_chainer {

using namespace pulp::format;
using namespace pulp::state;
using namespace pulp::audio;
using namespace pulp::midi;

namespace {

constexpr double kTwoPi = 6.283185307179586;

// "Chainer-style" iOS editor: title Label + Knob (bound to the Drive
// parameter on the AU parameter tree) + Meter (audio-output peak). All
// three widgets paint through the same Skia/Dawn GPU path the iOS-D.1
// smoke validated; this view trades the cycling-quad smoke for a real
// plug-in editor shape so a HostApp screenshot looks like a plug-in.
class ChainerView : public pulp::view::View {
public:
    explicit ChainerView(ChainerSynth* owner) : owner_(owner) {
        set_requires_gpu_host(true);
        set_theme(pulp::view::Theme::dark());
    }

    void on_attached() override {
        // Owner ChainerSynth holds the AU parameter tree (via its
        // StateStore). The Knob's on_change calls back into the synth so
        // Drive updates from this editor land on the AU parameter tree
        // and propagate to the audio render block. We keep raw observer
        // pointers to the widgets so layout/tick can talk to them; the
        // View base owns the unique_ptr storage.
        if (!owner_) return;

        auto title = std::make_unique<pulp::view::Label>("Pulp Chainer");
        title->set_font_size(22.0f);
        title->set_font_weight(700);
        title_ = title.get();
        add_child(std::move(title));

        auto knob = std::make_unique<pulp::view::Knob>();
        knob->set_label("Drive");
        knob->set_format([](float v) {
            return std::to_string(static_cast<int>(v * 100.0f + 0.5f)) + "%";
        });
        ChainerSynth* owner_capture = owner_;
        knob->on_change = [owner_capture](float v) {
            if (owner_capture) {
                owner_capture->state().set_normalized(
                    owner_capture->drive_param_id(), v);
            }
        };
        knob_ = knob.get();
        add_child(std::move(knob));

        auto meter = std::make_unique<pulp::view::Meter>();
        meter->set_level(0.0f, 0.0f);
        meter_ = meter.get();
        add_child(std::move(meter));

        if (auto* clock = frame_clock()) {
            sub_id_ = clock->subscribe([this](float /*dt*/) {
                tick();
                return true;
            });
        }

        // Run an initial layout pass so the first paint after attach
        // already has sized children. on_resized() will fire later as
        // the AUv3 host sizes the editor, but the first vsync tick
        // happens before that on the iOS GPU path.
        const auto r = local_bounds();
        if (r.width > 0 && r.height > 0) {
            do_layout(r.width, r.height);
        }

        pulp::runtime::log_info(
            "[chainer-ios] editor attached, awaiting FrameClock ticks");
    }

    void on_detached() override {
        if (auto* clock = frame_clock()) {
            if (sub_id_ != 0) clock->unsubscribe(sub_id_);
        }
        sub_id_ = 0;
        // Child unique_ptrs are owned by View; just drop our observer
        // pointers so a stale handle can't leak across reattach.
        title_ = nullptr;
        knob_ = nullptr;
        meter_ = nullptr;
    }

    void on_resized() override {
        const auto r = local_bounds();
        do_layout(r.width, r.height);
    }

    void paint(pulp::canvas::Canvas& canvas) override {
        const auto r = local_bounds();
        // Background: panel dark blue so a CPU-fallback fill (pure
        // black, no widget paint) reads obviously as "GPU pipeline
        // never came up" rather than "view shape regressed".
        canvas.set_fill_color(pulp::canvas::Color::rgba(0.05f, 0.07f, 0.12f));
        canvas.fill_rect(r.x, r.y, r.width, r.height);

        // Direct-paint accents that don't depend on child layout firing.
        // Children (Label + Knob + Meter) still paint above this; the
        // accents make the editor visually identifiable as "Chainer"
        // even if a child widget's first paint races the first
        // on_resized() callback (PluginViewHost sets size after attach,
        // so the first render is the worst case). Cheap to draw.
        const float pad = 16.0f;
        // Title underline strip.
        canvas.set_fill_color(pulp::canvas::Color::rgba(0.42f, 0.66f, 1.0f, 0.9f));
        canvas.fill_rect(pad, pad + 32.0f + 6.0f, r.width - 2 * pad, 2.0f);
        // Meter-area dim outline so a flat black background can never
        // pass for "view rendered but had no content".
        canvas.set_fill_color(pulp::canvas::Color::rgba(0.16f, 0.22f, 0.32f, 0.6f));
        canvas.fill_rect(r.width * 0.6f, pad + 64.0f, 2.0f, std::max(0.0f, r.height - pad - 80.0f));
    }

private:
    void tick() {
        // Keep layout fresh — on_resized() fires when the AUv3 host
        // changes the editor size, but if the parent View's bounds
        // were set before our children were created we'd otherwise
        // paint at the stale layout from on_attached(). Cheap arithmetic.
        const auto r = local_bounds();
        if (r.width > 0 && r.height > 0) {
            do_layout(r.width, r.height);
        }

        // Knob → store sync handled by Binding::poll on the next layout
        // cycle; mirror the AU parameter value into the Knob here so a
        // HostApp slider drag (PulpAUv3Host fallback UI) re-paints the
        // Knob inside the editor without round-tripping through JS.
        if (knob_ && owner_) {
            knob_->set_value(
                owner_->state().get_normalized(owner_->drive_param_id()));
        }
        // Meter consumes the latest peak from the audio thread and
        // ballistic-smooths toward it so the bar doesn't flicker.
        if (meter_ && owner_) {
            const float peak = owner_->consume_peak();
            meter_value_ = std::max(peak, meter_value_ * 0.86f);
            const float v = std::clamp(meter_value_, 0.0f, 1.0f);
            // Single-shot peak feed: pass the same peak as the RMS
            // smoothing target so the Meter widget shows visible motion
            // without us replicating MeterBallistics here.
            meter_->set_level(v * 0.7f, v);
        }
        request_repaint();
    }

    void do_layout(float w, float h) {
        const float pad = 16.0f;
        const float title_h = 32.0f;
        if (title_) {
            title_->set_bounds({pad, pad, w - 2 * pad, title_h});
        }
        const float row_top = pad + title_h + pad;
        const float row_h = std::max(80.0f, h - row_top - pad);
        const float knob_side = std::min(row_h, (w - 3 * pad) * 0.5f);
        if (knob_) {
            knob_->set_bounds({pad,
                               row_top + (row_h - knob_side) * 0.5f,
                               knob_side, knob_side});
        }
        const float meter_x = pad + knob_side + pad;
        const float meter_w = std::max(40.0f, w - meter_x - pad);
        if (meter_) {
            meter_->set_bounds({meter_x, row_top, meter_w, row_h});
        }
    }

    ChainerSynth* owner_ = nullptr;
    // Non-owning observers; View base owns the unique_ptr storage.
    pulp::view::Label* title_ = nullptr;
    pulp::view::Knob*  knob_  = nullptr;
    pulp::view::Meter* meter_ = nullptr;
    int sub_id_ = 0;
    float meter_value_ = 0.0f;
};

} // namespace

PluginDescriptor ChainerSynth::descriptor() const {
    PluginDescriptor d;
    d.name = "Pulp Chainer (iOS)";
    d.manufacturer = "Pulp";
    d.bundle_id = "com.pulp.examples.chainer-ios";
    d.version = "0.1.0";
    d.category = PluginCategory::Instrument;
    d.accepts_midi = true;
    d.produces_midi = false;
    d.input_buses.clear();
    d.output_buses = {{"Main Out", 2, false}};
    return d;
}

void ChainerSynth::define_parameters(StateStore& store) {
    drive_param_id_ = 1;
    store.add_parameter({static_cast<ParamID>(drive_param_id_),
                         "Drive", "",
                         ParamRange{0.0f, 1.0f, 0.35f}});
}

void ChainerSynth::prepare(const PrepareContext& ctx) {
    sample_rate_ = ctx.sample_rate > 0 ? ctx.sample_rate : 48000.0;
    phase_ = 0.0;
    gate_ = 0.0f;
    gate_target_ = 0.0f;
    peak_.store(0.0f, std::memory_order_release);
}

void ChainerSynth::process(BufferView<float>& out,
                           const BufferView<const float>& /*in*/,
                           MidiBuffer& midi_in,
                           MidiBuffer& /*midi_out*/,
                           const ProcessContext& ctx) {
    // Same gate scheme as ios-auv3-synth — any note-on opens the gate,
    // note-off closes. Keeps this example demo-able from the HostApp
    // "Play" button without a MIDI keyboard.
    for (const auto& ev : midi_in) {
        const auto& msg = ev.message;
        if (msg.isNoteOn())  gate_target_ = 1.0f;
        if (msg.isNoteOff()) gate_target_ = 0.0f;
    }

    const float drive = state().get_value(drive_param_id_);
    // 110 Hz fixed pitch so the audible output is deterministic; the
    // visible deliverable is the editor + Drive knob, not a playable
    // instrument.
    constexpr double kFreqHz = 110.0;
    const double phase_inc = kTwoPi * kFreqHz / sample_rate_;
    const float gate_step  = 0.002f;
    const float amp = 0.4f * (0.5f + drive * 0.5f);

    const int n = ctx.num_samples;
    auto L = out.channel(0);
    const bool has_right = out.num_channels() > 1;

    float local_peak = 0.0f;
    for (int i = 0; i < n; ++i) {
        gate_ += (gate_target_ - gate_) * gate_step;
        const float s = static_cast<float>(std::sin(phase_)) * gate_ * amp;
        phase_ += phase_inc;
        if (phase_ >= kTwoPi) phase_ -= kTwoPi;
        L[i] = s;
        const float a = std::fabs(s);
        if (a > local_peak) local_peak = a;
    }
    if (has_right) {
        auto R = out.channel(1);
        for (int i = 0; i < n; ++i) R[i] = L[i];
    }
    // Publish the block peak for the editor's Meter widget. Audio
    // thread is the only writer; UI thread reads via consume_peak()
    // which atomically swaps to zero so the meter naturally decays
    // when the synth goes silent.
    float prev = peak_.load(std::memory_order_acquire);
    while (local_peak > prev &&
           !peak_.compare_exchange_weak(prev, local_peak,
                                        std::memory_order_release,
                                        std::memory_order_acquire)) {
        // retry
    }
}

std::unique_ptr<pulp::view::View> ChainerSynth::create_view() {
    return std::make_unique<ChainerView>(this);
}

} // namespace pulp::examples::ios_chainer
