#pragma once

// Minimal ARA-aware CLAP pitch tracker. Owns a concrete
// AraDocumentController subclass so the CLAP-side get_extension
// hook has something to bind against.
//
// This example is intentionally tiny — a real pitch tracker would
// implement the ARA audio-source content reader, run analysis on
// worker threads, and publish note-content updates. Here we just
// log controller lifecycle callbacks so developers can verify the
// CLAP ↔ ARA handshake is working end-to-end.

#include <pulp/format/processor.hpp>
#include <pulp/format/ara.hpp>

namespace pulp::examples::ara_pitch_tracker {

class PitchTrackerController : public pulp::format::AraDocumentController {
public:
    int supported_roles() const override {
        return static_cast<int>(pulp::format::AraRole::EditorRenderer)
             | static_cast<int>(pulp::format::AraRole::EditorView);
    }
    bool is_ara_supported() const override { return true; }
    std::string ara_factory_name() const override { return "Pulp Pitch Tracker"; }

    void begin_editing() override;
    void end_editing() override;
    void notify_audio_source_content_changed(int64_t audio_source_id) override;
};

class PitchTracker : public pulp::format::Processor {
public:
    pulp::format::PluginDescriptor descriptor() const override;
    void define_parameters(pulp::state::StateStore& store) override;
    void prepare(const pulp::format::PrepareContext& ctx) override;
    void process(pulp::audio::BufferView<float>& out,
                 const pulp::audio::BufferView<const float>& in,
                 pulp::midi::MidiBuffer& midi_in,
                 pulp::midi::MidiBuffer& midi_out,
                 const pulp::format::ProcessContext& ctx) override;

    std::unique_ptr<pulp::format::AraDocumentController>
    create_ara_document_controller() override;
};

} // namespace pulp::examples::ara_pitch_tracker
