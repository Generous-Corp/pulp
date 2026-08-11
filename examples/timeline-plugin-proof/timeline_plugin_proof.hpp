#pragma once

#include <pulp/format/processor.hpp>
#include <pulp/runtime/alive_token.hpp>
#include <pulp/runtime/seqlock.hpp>
#include <pulp/timeline/document_session.hpp>
#include <pulp/timeline/serialize.hpp>
#include <pulp/timeline_editor/edit_intent.hpp>
#include <pulp/timeline_view/piano_roll_view.hpp>
#include <pulp/view/view.hpp>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace pulp::examples {

struct TimelinePluginProofIds {
    timeline::ItemId project{1};
    timeline::ItemId sequence{2};
    timeline::ItemId track{3};
    timeline::ItemId clip{4};
    timeline::ItemId first_note{5};
    timeline::ItemId second_note{6};
};

inline constexpr std::int64_t kTimelinePluginProofClipTicks =
    2 * timebase::kTicksPerQuarter;
inline constexpr std::int64_t kTimelinePluginProofNoteTicks =
    timebase::kTicksPerQuarter / 4;
inline constexpr std::int64_t kTimelinePluginProofSecondNoteStart =
    timebase::kTicksPerQuarter / 2;
inline constexpr float kTimelinePluginProofViewWidth = 400.0f;
inline constexpr float kTimelinePluginProofViewHeight = 300.0f;

class TimelinePluginProofProcessor;

class TimelinePluginProofView final : public view::View {
  public:
    TimelinePluginProofView(TimelinePluginProofProcessor& processor,
                            runtime::AliveToken::Handle processor_alive);
    ~TimelinePluginProofView() override;

    timeline_view::PianoRollView& piano_roll() noexcept { return *piano_roll_; }
    const timeline::Project* bound_project() const noexcept { return snapshot_.get(); }

  private:
    friend class TimelinePluginProofProcessor;
    void detach_processor() noexcept;
    void rebind(std::shared_ptr<const timeline::Project> snapshot);

    TimelinePluginProofProcessor* processor_ = nullptr;
    runtime::AliveToken::Handle processor_alive_;
    std::shared_ptr<const timeline::Project> snapshot_;
    timeline_view::PianoRollView* piano_roll_ = nullptr;
};

class TimelinePluginProofProcessor final : public format::Processor,
                                           public timeline_editor::NoteEditIntentHost {
  public:
    TimelinePluginProofProcessor() { install_default_project(); }

    ~TimelinePluginProofProcessor() override {
        owner_alive_.retire();
        for (auto* view : views_)
            view->detach_processor();
        views_.clear();
    }

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
        return std::make_unique<TimelinePluginProofView>(*this, owner_alive_.capture());
    }

    timeline_editor::UiPlayhead playhead() const noexcept override { return playhead_.read(); }

    timeline_editor::AuditionResult
    begin_audition(const timeline_editor::AuditionRequest&) noexcept override {
        return {timeline_editor::AuditionStatus::Unsupported, {}};
    }

    void end_audition(timeline_editor::AuditionHandle) noexcept override {}

    timeline_editor::IntentResult
    submit_intent(const timeline_editor::ValidatedNoteEditIntent& intent) noexcept override {
        timeline_editor::IntentResult result{timeline_editor::IntentStatus::Rejected, 0};
        if (session_ && writer_.id().valid()) {
            const auto current = session_->current();
            const auto notes = current_notes(*current.snapshot);
            const timeline_editor::EditIntentIdentity identity{
                writer_.allocate_transaction_id(), current.revision,
                writer_.allocate_command_id(), std::nullopt};
            auto transaction = timeline_editor::lower_note_edit_intent(intent, notes, identity);
            if (transaction && session_->submit(writer_, std::move(transaction).value()))
                result = {timeline_editor::IntentStatus::Accepted, ++intent_sequence_};
        }
        rebind_views();
        return result;
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
        if (!decoded) {
            rebind_views();
            return false;
        }
        auto project = std::move(decoded).value();
        if (!compatible(project)) {
            auto migrated = migrate_legacy_project(project);
            if (!migrated) {
                rebind_views();
                return false;
            }
            project = std::move(*migrated);
        }
        return install_project(std::move(project));
    }

    bool undo() {
        const bool accepted = session_ && static_cast<bool>(session_->undo(writer_));
        rebind_views();
        return accepted;
    }

    bool redo() {
        const bool accepted = session_ && static_cast<bool>(session_->redo(writer_));
        rebind_views();
        return accepted;
    }

    bool valid() const noexcept { return session_ && writer_.id().valid(); }
    timeline::DocumentView document() const noexcept {
        return session_ ? session_->current() : timeline::DocumentView{};
    }
    constexpr TimelinePluginProofIds ids() const noexcept { return {}; }

  private:
    friend class TimelinePluginProofView;

    static runtime::Result<timeline::Project, timeline::ModelError> make_default_project() {
        const TimelinePluginProofIds ids;
        auto content = timeline::MidiContent::create({
            timeline::NoteEvent{ids.first_note, {0}, {kTimelinePluginProofNoteTicks},
                                1'000, 60, 0},
            timeline::NoteEvent{ids.second_note, {kTimelinePluginProofSecondNoteStart},
                                {kTimelinePluginProofNoteTicks}, 1'000, 62, 0},
        });
        if (!content)
            return runtime::Err(content.error());
        auto clip = timeline::Clip::create(ids.clip, timebase::TickPosition{0},
                                           timebase::TickDuration{kTimelinePluginProofClipTicks},
                                           std::move(content).value());
        if (!clip)
            return runtime::Err(clip.error());
        auto track = timeline::Track::create(ids.track, "Proof track", {std::move(clip).value()});
        if (!track)
            return runtime::Err(track.error());
        auto sequence = timeline::Sequence::create(
            ids.sequence, "Root", timebase::TickDuration{kTimelinePluginProofClipTicks},
            {std::move(track).value()});
        if (!sequence)
            return runtime::Err(sequence.error());
        timeline::ProjectInput input;
        input.id = ids.project;
        input.name = "Timeline plugin proof";
        input.next_item_id = 7;
        input.root_sequence_id = ids.sequence;
        input.sequences.push_back(std::move(sequence).value());
        return timeline::Project::create(std::move(input));
    }

    bool install_default_project() {
        auto project = make_default_project();
        return project && install_project(std::move(project).value());
    }

    bool install_project(timeline::Project project) {
        if (!compatible(project))
            return false;
        auto candidate = timeline::DocumentSession::create(std::move(project));
        if (!candidate)
            return false;
        auto candidate_writer = candidate.value()->register_writer();
        if (!candidate_writer)
            return false;
        session_ = std::move(candidate).value();
        writer_ = std::move(candidate_writer).value();
        rebind_views();
        return true;
    }

    static bool compatible(const timeline::Project& project) {
        const TimelinePluginProofIds ids;
        const auto* sequence = project.find_sequence(ids.sequence);
        const auto* track = sequence ? sequence->find_track(ids.track) : nullptr;
        const auto* clip = track ? track->find_clip(ids.clip) : nullptr;
        if (!clip || !std::holds_alternative<timeline::MidiContent>(clip->content()))
            return false;
        const auto* range = std::get_if<timeline::MusicalTimeRange>(&clip->time_range());
        return range &&
               range->duration == timebase::TickDuration{kTimelinePluginProofClipTicks};
    }

    static std::optional<timeline::Project>
    migrate_legacy_project(const timeline::Project& project) {
        const TimelinePluginProofIds ids;
        const auto* sequence = project.find_sequence(ids.sequence);
        const auto* track = sequence ? sequence->find_track(ids.track) : nullptr;
        const auto* clip = track ? track->find_clip(ids.clip) : nullptr;
        const auto* range = clip ? std::get_if<timeline::MusicalTimeRange>(&clip->time_range())
                                 : nullptr;
        if (project.root_sequence_id() != ids.sequence || !sequence || !track || !clip ||
            !std::holds_alternative<timeline::EmptyContent>(clip->content()) || !range ||
            range->duration != timebase::TickDuration{4 * timebase::kTicksPerQuarter} ||
            sequence->duration() !=
                std::optional<timebase::TickDuration>{
                    timebase::TickDuration{8 * timebase::kTicksPerQuarter}})
            return std::nullopt;

        auto allocator = project.item_id_allocator();
        auto first_note_id = allocator.allocate();
        auto second_note_id = allocator.allocate();
        if (!first_note_id || !second_note_id)
            return std::nullopt;
        auto content = timeline::MidiContent::create({
            timeline::NoteEvent{first_note_id.value(), {0}, {kTimelinePluginProofNoteTicks},
                                1'000, 60, 0},
            timeline::NoteEvent{second_note_id.value(), {kTimelinePluginProofSecondNoteStart},
                                {kTimelinePluginProofNoteTicks}, 1'000, 62, 0},
        });
        if (!content)
            return std::nullopt;
        auto migrated_clip = timeline::Clip::create(
            ids.clip, range->start, timebase::TickDuration{kTimelinePluginProofClipTicks},
            std::move(content).value(), clip->playback_properties(), clip->time_conform());
        if (!migrated_clip)
            return std::nullopt;
        timeline::TrackInput track_input;
        track_input.id = track->id();
        track_input.name = track->name();
        for (const auto& existing : track->clips())
            track_input.clips.push_back(existing.id() == ids.clip
                                            ? std::move(migrated_clip).value()
                                            : existing);
        track_input.device_chain = {track->device_chain().begin(), track->device_chain().end()};
        track_input.automation_lanes = {track->automation_lanes().begin(),
                                        track->automation_lanes().end()};
        track_input.modulators = {track->modulators().begin(), track->modulators().end()};
        track_input.macros = {track->macros().begin(), track->macros().end()};
        track_input.modulation_routes = {track->modulation_routes().begin(),
                                         track->modulation_routes().end()};
        track_input.take_lanes = {track->take_lanes().begin(), track->take_lanes().end()};
        track_input.record_armed = track->record_armed();
        track_input.active_take_lane_id = track->active_take_lane_id();
        track_input.freeze = track->freeze();
        track_input.mixer = track->mixer();
        track_input.tuning = track->tuning();
        auto migrated_track = timeline::Track::create(std::move(track_input));
        if (!migrated_track)
            return std::nullopt;

        timeline::SequenceInput sequence_input;
        sequence_input.id = sequence->id();
        sequence_input.name = sequence->name();
        sequence_input.musical_duration = sequence->duration();
        sequence_input.absolute_duration = sequence->absolute_duration();
        for (const auto& existing : sequence->tracks())
            sequence_input.tracks.push_back(existing.id() == ids.track
                                                ? std::move(migrated_track).value()
                                                : existing);
        sequence_input.markers = {sequence->markers().begin(), sequence->markers().end()};
        sequence_input.regions = {sequence->regions().begin(), sequence->regions().end()};
        sequence_input.chord_scale_lane = sequence->chord_scale_lane();
        sequence_input.groove = sequence->groove();
        for (const auto& scene : sequence->scenes())
            sequence_input.scenes.push_back(scene);
        sequence_input.track_order = {sequence->track_order().begin(),
                                      sequence->track_order().end()};
        auto migrated_sequence = timeline::Sequence::create(std::move(sequence_input));
        if (!migrated_sequence)
            return std::nullopt;

        timeline::ProjectInput input;
        input.id = project.id();
        input.name = project.name();
        input.next_item_id = allocator.next_value();
        input.root_sequence_id = project.root_sequence_id();
        input.assets = {project.assets().begin(), project.assets().end()};
        for (const auto& existing : project.sequences())
            input.sequences.push_back(existing.id() == ids.sequence
                                          ? std::move(migrated_sequence).value()
                                          : existing);
        input.tempo_map = project.tempo_map();
        input.meter_map = project.meter_map();
        input.session_start = project.session_start();
        input.tuning = project.tuning();
        auto migrated = timeline::Project::create(std::move(input));
        if (!migrated)
            return std::nullopt;
        return std::move(migrated).value();
    }

    std::span<const timeline::NoteEvent> current_notes(const timeline::Project& snapshot) const {
        const auto ids = this->ids();
        const auto* sequence = snapshot.find_sequence(ids.sequence);
        const auto* track = sequence ? sequence->find_track(ids.track) : nullptr;
        const auto* clip = track ? track->find_clip(ids.clip) : nullptr;
        if (!clip)
            return {};
        const auto* content = std::get_if<timeline::MidiContent>(&clip->content());
        return content ? content->notes() : std::span<const timeline::NoteEvent>{};
    }

    void register_view(TimelinePluginProofView* view) {
        views_.push_back(view);
        view->rebind(session_ ? session_->snapshot() : nullptr);
    }

    void unregister_view(TimelinePluginProofView* view) {
        std::erase(views_, view);
    }

    void rebind_views() {
        const auto snapshot = session_ ? session_->snapshot() : nullptr;
        for (auto* view : views_)
            view->rebind(snapshot);
    }

    std::unique_ptr<timeline::DocumentSession> session_;
    timeline::WriterToken writer_;
    runtime::SeqLock<timeline_editor::UiPlayhead> playhead_;
    std::uint64_t playhead_sequence_ = 0;
    std::atomic<std::uint64_t> intent_sequence_{0};
    std::vector<TimelinePluginProofView*> views_;
    runtime::AliveToken owner_alive_;
};

inline TimelinePluginProofView::TimelinePluginProofView(
    TimelinePluginProofProcessor& processor, runtime::AliveToken::Handle processor_alive)
    : processor_(&processor), processor_alive_(std::move(processor_alive)) {
    set_bounds({0.0f, 0.0f, kTimelinePluginProofViewWidth, kTimelinePluginProofViewHeight});
    auto roll = std::make_unique<timeline_view::PianoRollView>();
    piano_roll_ = roll.get();
    piano_roll_->set_bounds(
        {0.0f, 0.0f, kTimelinePluginProofViewWidth, kTimelinePluginProofViewHeight});
    auto time = timeline_editor::TickProjection::create(
        {0}, {kTimelinePluginProofClipTicks},
        timeline_editor::PixelSpan{0.0f, kTimelinePluginProofViewWidth});
    auto pitch = timeline_editor::PitchProjection::create(
        48, 71, timeline_editor::PixelSpan{0.0f, kTimelinePluginProofViewHeight});
    if (time && pitch)
        piano_roll_->set_layout({std::move(time).value(), std::move(pitch).value(), {1}});
    piano_roll_->set_host(processor_);
    piano_roll_->set_note_factory([this](timebase::TickPosition start,
                                         std::uint8_t pitch_value)
                                      -> std::optional<timeline::NoteEvent> {
        if (!runtime::AliveToken::is_alive(processor_alive_) || processor_ == nullptr)
            return std::nullopt;
        const auto current = processor_->document();
        if (!current.snapshot)
            return std::nullopt;
        return timeline::NoteEvent{timeline::ItemId{current.snapshot->next_item_id()}, start,
                                   {kTimelinePluginProofNoteTicks}, 900, pitch_value, 0};
    });
    add_child(std::move(roll));
    processor_->register_view(this);
}

inline TimelinePluginProofView::~TimelinePluginProofView() {
    if (runtime::AliveToken::is_alive(processor_alive_) && processor_ != nullptr)
        processor_->unregister_view(this);
}

inline void TimelinePluginProofView::detach_processor() noexcept {
    piano_roll_->set_host(nullptr);
    piano_roll_->set_note_factory({});
    processor_ = nullptr;
}

inline void TimelinePluginProofView::rebind(std::shared_ptr<const timeline::Project> snapshot) {
    snapshot_ = std::move(snapshot);
    const TimelinePluginProofIds ids;
    piano_roll_->set_clip(snapshot_.get(), ids.sequence, ids.track, ids.clip);
    request_repaint();
}

inline std::unique_ptr<format::Processor> create_timeline_plugin_proof() {
    return std::make_unique<TimelinePluginProofProcessor>();
}

} // namespace pulp::examples
