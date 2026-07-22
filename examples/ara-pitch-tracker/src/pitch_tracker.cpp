#include "pitch_tracker.hpp"

#include <pulp/runtime/log.hpp>

namespace pulp::examples::ara_pitch_tracker {

using namespace pulp::format;
using namespace pulp::audio;
using namespace pulp::midi;

// ── PitchTrackerController ──────────────────────────────────────────────────

void PitchTrackerController::begin_editing() {
    pulp::runtime::log_info("ara-pitch-tracker: begin_editing");
}

void PitchTrackerController::end_editing() {
    pulp::runtime::log_info("ara-pitch-tracker: end_editing");
}

void PitchTrackerController::notify_audio_source_content_changed(int64_t id) {
    pulp::runtime::log_info("ara-pitch-tracker: audio source content changed "
                            "(id={}) — real impl would run YIN analysis here", id);
}

// ── PitchTracker (the Processor) ────────────────────────────────────────────

PluginDescriptor PitchTracker::descriptor() const {
    PluginDescriptor d;
    d.name = "Pulp Pitch Tracker";
    d.manufacturer = "Pulp";
    d.bundle_id = "com.pulp.examples.ara_pitch_tracker";
    d.version = "0.1.0";
    d.category = PluginCategory::Effect;
    d.accepts_midi = false;
    d.produces_midi = true;   // would emit notes from detected pitches
    d.vendor_url = "https://github.com/Generous-Corp/pulp";
    return d;
}

void PitchTracker::define_parameters(state::StateStore&) {
    // No params in this tiny example. A real pitch tracker would add
    // threshold, hysteresis, pitch-round-to-semitone, etc.
}

void PitchTracker::prepare(const PrepareContext&) {}

void PitchTracker::process(BufferView<float>& out,
                           const BufferView<const float>& in,
                           MidiBuffer&, MidiBuffer&,
                           const ProcessContext& ctx) {
    // Pass-through audio so the plug-in is still useful without an ARA host.
    const int nch = std::min(in.num_channels(), out.num_channels());
    for (int ch = 0; ch < nch; ++ch) {
        const float* src = in.channel_ptr(ch);
        float* dst = out.channel_ptr(ch);
        for (int i = 0; i < ctx.num_samples; ++i) dst[i] = src[i];
    }
}

std::unique_ptr<AraDocumentController>
PitchTracker::create_ara_document_controller() {
    return std::make_unique<PitchTrackerController>();
}

} // namespace pulp::examples::ara_pitch_tracker

// Factory hook for the CLAP entry point.
std::unique_ptr<pulp::format::Processor> pulp_create_processor() {
    return std::make_unique<pulp::examples::ara_pitch_tracker::PitchTracker>();
}
