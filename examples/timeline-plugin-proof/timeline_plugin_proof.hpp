#pragma once

#include <pulp/format/processor.hpp>
#include <pulp/runtime/seqlock.hpp>
#include <pulp/timeline/document_session.hpp>
#include <pulp/timeline/serialize.hpp>
#include <pulp/timeline_editor/edit_intent.hpp>
#include <pulp/timeline_editor/viewport_projection.hpp>
#include <pulp/view/view.hpp>

#include <atomic>
#include <cmath>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace pulp::examples {

struct TimelinePluginProofIds {
    timeline::ItemId project{1};
    timeline::ItemId sequence{2};
    timeline::ItemId track{3};
    timeline::ItemId clip{4};
};

class TimelinePluginProofView final : public view::View {
  public:
    explicit TimelinePluginProofView(timeline_editor::SequencerUiHost& host) : host_(host) {
        set_continuous_repaint(true);
    }

    void paint(canvas::Canvas& canvas) override {
        const auto bounds = local_bounds();
        canvas.set_fill_color(canvas::Color::rgba8(20, 24, 31));
        canvas.fill_rect(0.0f, 0.0f, bounds.width, bounds.height);

        canvas.set_fill_color(canvas::Color::rgba8(235, 239, 245));
        canvas.set_font("Inter", 18.0f);
        canvas.fill_text("Timeline plugin proof", 24.0f, 34.0f);

        const float ruler_x = 24.0f;
        const float ruler_width = std::max(1.0f, bounds.width - 48.0f);
        canvas.set_fill_color(canvas::Color::rgba8(42, 49, 61));
        canvas.fill_rect(ruler_x, 64.0f, ruler_width, 64.0f);

        const auto projection = timeline_editor::TickProjection::create(
            timebase::TickPosition{0},
            timebase::TickDuration{8 * timebase::kTicksPerQuarter},
            timeline_editor::PixelSpan{ruler_x, ruler_width});
        if (!projection)
            return;

        canvas.set_fill_color(canvas::Color::rgba8(85, 173, 238));
        const auto x = projection->x_at(host_.playhead().position);
        canvas.fill_rect(x - 1.0f, 58.0f, 2.0f, 82.0f);
    }

  private:
    timeline_editor::SequencerUiHost& host_;
};

class TimelinePluginProofProcessor final : public format::Processor,
                                           public timeline_editor::EditIntentHost {
  public:
    TimelinePluginProofProcessor() { install_default_project(); }

    format::PluginDescriptor descriptor() const override {
        return {
            .name = "Timeline Plugin Proof",
            .manufacturer = "Pulp Examples",
            .bundle_id = "com.pulp.examples.timeline-plugin-proof",
            .version = "0.1.0",
            .category = format::PluginCategory::Instrument,
            .input_buses = {},
            .output_buses = {{"Main Out", 2}},
            .accepts_midi = false,
            .produces_midi = false,
            .tail_samples = 0,
        };
    }

    void define_parameters(state::StateStore&) override {}
    void prepare(const format::PrepareContext&) override {}

    void process(audio::BufferView<float>& output,
                 const audio::BufferView<const float>&,
                 midi::MidiBuffer&,
                 midi::MidiBuffer&,
                 const format::ProcessContext& context) override {
        for (std::size_t channel = 0; channel < output.num_channels(); ++channel)
            for (float& sample : output.channel(channel))
                sample = 0.0f;

        timeline_editor::UiPlayhead reading;
        reading.program_generation = 1;
        reading.sequence = ++playhead_sequence_;
        reading.position = timebase::TickPosition{static_cast<std::int64_t>(
            std::llround(context.position_beats * timebase::kTicksPerQuarter))};
        reading.state = context.is_playing ? timeline_editor::UiTransportState::Playing
                                           : timeline_editor::UiTransportState::Stopped;
        reading.tempo_bpm = context.tempo_bpm;
        playhead_.write(reading);
    }

    std::unique_ptr<view::View> create_view() override {
        return std::make_unique<TimelinePluginProofView>(*this);
    }

    timeline_editor::UiPlayhead playhead() const noexcept override {
        return playhead_.read();
    }

    timeline_editor::AuditionResult
    begin_audition(const timeline_editor::AuditionRequest&) noexcept override {
        return {timeline_editor::AuditionStatus::Unsupported, {}};
    }

    void end_audition(timeline_editor::AuditionHandle) noexcept override {}

    timeline_editor::IntentResult
    submit_intent(const timeline_editor::EditIntent& intent) noexcept override {
        if (!session_ || !writer_.id().valid() || intent.phase != timeline::GesturePhase::Single)
            return {timeline_editor::IntentStatus::Rejected, 0};

        const timeline_editor::EditIntentIdentity identity{
            writer_.allocate_transaction_id(), session_->revision(),
            writer_.allocate_command_id(), std::nullopt};
        auto transaction = timeline_editor::lower_edit_intent(intent, identity);
        if (!transaction || !session_->submit(writer_, std::move(transaction).value()))
            return {timeline_editor::IntentStatus::Rejected, 0};

        return {timeline_editor::IntentStatus::Accepted, ++intent_sequence_};
    }

    std::vector<std::uint8_t> serialize_plugin_state() const override {
        if (!session_)
            return {};
        auto registry = timeline::make_builtin_timeline_registry();
        if (!registry)
            return {};
        auto encoded = timeline::serialize_project(*session_->snapshot(), registry.value());
        if (!encoded)
            return {};
        return {encoded->json.begin(), encoded->json.end()};
    }

    bool deserialize_plugin_state(std::span<const std::uint8_t> bytes) override {
        if (bytes.empty())
            return install_default_project();

        auto registry = timeline::make_builtin_timeline_registry();
        if (!registry)
            return false;
        const std::string json(bytes.begin(), bytes.end());
        auto decoded = timeline::deserialize_project(json, registry.value());
        if (!decoded)
            return false;
        return install_project(std::move(decoded).value());
    }

    bool valid() const noexcept { return session_ && writer_.id().valid(); }
    timeline::DocumentView document() const noexcept {
        return session_ ? session_->current() : timeline::DocumentView{};
    }
    constexpr TimelinePluginProofIds ids() const noexcept { return {}; }

  private:
    static runtime::Result<timeline::Project, timeline::ModelError> make_default_project() {
        const TimelinePluginProofIds ids;
        auto clip = timeline::Clip::create(
            ids.clip, timebase::TickPosition{0},
            timebase::TickDuration{4 * timebase::kTicksPerQuarter}, timeline::EmptyContent{});
        if (!clip)
            return runtime::Err(clip.error());
        auto track = timeline::Track::create(ids.track, "Proof track", {std::move(clip).value()});
        if (!track)
            return runtime::Err(track.error());
        auto sequence = timeline::Sequence::create(
            ids.sequence, "Root", timebase::TickDuration{8 * timebase::kTicksPerQuarter},
            {std::move(track).value()});
        if (!sequence)
            return runtime::Err(sequence.error());
        timeline::ProjectInput input;
        input.id = ids.project;
        input.name = "Timeline plugin proof";
        input.next_item_id = 5;
        input.root_sequence_id = ids.sequence;
        input.sequences.push_back(std::move(sequence).value());
        return timeline::Project::create(std::move(input));
    }

    bool install_default_project() {
        auto project = make_default_project();
        return project && install_project(std::move(project).value());
    }

    bool install_project(timeline::Project project) {
        auto candidate = timeline::DocumentSession::create(std::move(project));
        if (!candidate)
            return false;
        auto candidate_writer = candidate.value()->register_writer();
        if (!candidate_writer)
            return false;
        session_ = std::move(candidate).value();
        writer_ = std::move(candidate_writer).value();
        return true;
    }

    std::unique_ptr<timeline::DocumentSession> session_;
    timeline::WriterToken writer_;
    runtime::SeqLock<timeline_editor::UiPlayhead> playhead_;
    std::uint64_t playhead_sequence_ = 0;
    std::atomic<std::uint64_t> intent_sequence_{0};
};

inline std::unique_ptr<format::Processor> create_timeline_plugin_proof() {
    return std::make_unique<TimelinePluginProofProcessor>();
}

} // namespace pulp::examples
