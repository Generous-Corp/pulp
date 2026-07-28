#include "timeline_daw_project.hpp"

#include <pulp/timeline/document_session.hpp>

#include <algorithm>
#include <array>
#include <optional>
#include <string>
#include <utility>

namespace pulp::examples::timeline_session {
namespace {

constexpr timeline::ItemId kArrangementId{3};
constexpr timeline::ItemId kChorusId{4};
constexpr timeline::ItemId kHybridTrackId{10};
constexpr timeline::ItemId kCompTrackId{11};
constexpr timeline::ItemId kChorusTrackId{12};
constexpr timeline::ItemId kTakeLaneId{40};
constexpr timeline::ItemId kAssetId{2};
constexpr std::uint32_t kClipFrames = 480;
constexpr timebase::RationalRate kRate{48'000, 1};
constexpr std::uint64_t kFirstFreeItemId = 300;

// The three arrangement clips that reference the chorus.
constexpr std::array<timeline::ItemId, 3> kChorusRefClipIds{
    timeline::ItemId{100}, timeline::ItemId{101}, timeline::ItemId{102}};

constexpr timebase::TickDuration bar_ticks() noexcept {
    return timebase::TickDuration{4 * timebase::kTicksPerQuarter};
}

template <class T, class E> std::optional<T> value_or_none(runtime::Result<T, E> result) {
    if (!result)
        return std::nullopt;
    return std::move(result).value();
}

/// Counting autosave sink. A real host would write to disk here; the point the
/// example proves is that the session only reports durability the sink actually
/// acknowledged, so the counters are the honest record of what was saved.
///
/// `validate_restore` is NOT optional in practice. Attaching a sink calls
/// `checkpoint()` and then asks the sink to prove its durable state matches the
/// document it is about to journal for; the base class refuses by default, so a
/// sink that does not answer cannot be attached at all. That is the contract
/// working as intended — a sink that cannot confirm what it holds must not be
/// trusted to report durability later.
class CountingAutosaveSink final : public timeline::JournalSink {
  public:
    explicit CountingAutosaveSink(AutosaveStats& stats) noexcept : stats_(stats) {}

    runtime::Result<bool, timeline::JournalSinkError>
    append_batch(const timeline::JournalEntry& entry) noexcept override {
        ++stats_.durable_batches;
        stats_.last_durable_revision = entry.after;
        return runtime::Ok(true);
    }

    runtime::Result<bool, timeline::JournalSinkError>
    checkpoint(const timeline::Project&, timeline::DocumentRevision revision) noexcept override {
        ++stats_.checkpoints;
        checkpoint_revision_ = revision;
        has_checkpoint_ = true;
        return runtime::Ok(true);
    }

    runtime::Result<bool, timeline::JournalSinkError>
    validate_restore(const timeline::Project&,
                     timeline::DocumentRevision revision) noexcept override {
        // Answer from what was actually durably written, never unconditionally
        // true: a sink holding a different revision must refuse rather than let
        // a session attach to state it does not have.
        if (!has_checkpoint_ || checkpoint_revision_ != revision)
            return runtime::Err(timeline::JournalSinkError::InvalidState);
        return runtime::Ok(true);
    }

  private:
    AutosaveStats& stats_;
    timeline::DocumentRevision checkpoint_revision_{};
    bool has_checkpoint_ = false;
};

} // namespace

struct TimelineDawProject::Impl {
    AutosaveStats stats;
    std::shared_ptr<CountingAutosaveSink> sink;
    std::unique_ptr<timeline::DocumentSession> session;
    std::optional<timeline::WriterToken> writer;
    std::shared_ptr<const timeline::Project> project;
    timeline::ItemId diverged_chorus_id{};
    // Hybrid by construction: one track plays its arrangement, one plays the
    // launcher, and the arbitration primitive is what distinguishes them.
    std::array<std::pair<timeline::ItemId, playback::ProviderKind>, 3> providers{
        std::pair{kHybridTrackId, playback::ProviderKind::Arrangement},
        std::pair{kCompTrackId, playback::ProviderKind::Launcher},
        std::pair{kChorusTrackId, playback::ProviderKind::Arrangement}};

    bool submit(std::vector<timeline::Command> commands) {
        if (!session || !writer || commands.empty())
            return false;
        timeline::Transaction transaction;
        transaction.id = writer->allocate_transaction_id();
        transaction.expected_revision = session->revision();
        for (auto& command : commands)
            transaction.commands.push_back({writer->allocate_command_id(), std::move(command)});
        auto committed = session->submit(*writer, std::move(transaction));
        if (!committed)
            return false;
        project = committed.value().snapshot;
        return true;
    }
};

namespace {

std::optional<timeline::Sequence> make_chorus_sequence() {
    // A short, reusable arrangement: the thing a session references rather than
    // copies. Its own tracks are ordinary tracks.
    auto clip = value_or_none(timeline::Clip::create(
        timeline::ItemId{200}, {0}, bar_ticks(),
        timeline::MediaRef{kAssetId, {0}, kClipFrames}));
    if (!clip)
        return std::nullopt;
    auto track = value_or_none(
        timeline::Track::create(timeline::ItemId{201}, "Chorus", {std::move(*clip)}));
    if (!track)
        return std::nullopt;
    return value_or_none(timeline::Sequence::create(timeline::SequenceInput{
        .id = kChorusId,
        .name = "Chorus",
        .musical_duration = bar_ticks(),
        .tracks = {std::move(*track)},
    }));
}

std::optional<timeline::Sequence> make_arrangement_sequence() {
    // Track 1 — the hybrid track. It carries a real linear arrangement clip AND
    // a launcher slot, so per-track arbitration is what decides which sounds.
    auto linear = value_or_none(timeline::Clip::create(
        timeline::ItemId{110}, {0}, bar_ticks(),
        timeline::MediaRef{kAssetId, {0}, kClipFrames}));
    auto slot_source = value_or_none(timeline::Clip::create(
        timeline::ItemId{111}, timebase::TickPosition{bar_ticks().value}, bar_ticks(),
        timeline::MediaRef{kAssetId, {0}, kClipFrames}));
    if (!linear || !slot_source)
        return std::nullopt;
    const auto slot_clip_id = slot_source->id();
    auto hybrid = value_or_none(timeline::Track::create(
        kHybridTrackId, "Hybrid", {std::move(*linear), std::move(*slot_source)}));
    if (!hybrid)
        return std::nullopt;

    // Track 2 — a take lane with a comp across two takes.
    auto take_a = value_or_none(timeline::Take::create(
        timeline::ItemId{41}, timeline::MediaRef{kAssetId, {0}, kClipFrames}, {0}, kRate));
    // Both takes are alternates of the SAME passage, so both are placed at the
    // same point; a comp segment must lie inside its take's placed span.
    auto take_b = value_or_none(timeline::Take::create(
        timeline::ItemId{42}, timeline::MediaRef{kAssetId, {0}, kClipFrames}, {0}, kRate));
    if (!take_a || !take_b)
        return std::nullopt;
    const std::vector<timeline::TakeCompSegment> comp{
        {timeline::ItemId{41}, {{0}, kClipFrames / 2, kRate}},
        {timeline::ItemId{42},
         {{static_cast<std::int64_t>(kClipFrames / 2)}, kClipFrames / 2, kRate}}};
    auto lane = value_or_none(timeline::TakeLane::create(
        kTakeLaneId, "Comp", {std::move(*take_a), std::move(*take_b)}, comp));
    if (!lane)
        return std::nullopt;
    timeline::TrackInput comp_input;
    comp_input.id = kCompTrackId;
    comp_input.name = "Comped vocal";
    comp_input.take_lanes = {std::move(*lane)};
    comp_input.active_take_lane_id = kTakeLaneId;
    auto comp_track = value_or_none(timeline::Track::create(std::move(comp_input)));
    if (!comp_track)
        return std::nullopt;

    // Track 3 — the reusable chorus, referenced three times. Each reference is
    // a clip whose content is a SequenceRef; none of them owns the chorus.
    std::vector<timeline::Clip> reference_clips;
    for (std::size_t index = 0; index < kChorusRefClipIds.size(); ++index) {
        auto reference = value_or_none(timeline::Clip::create(
            kChorusRefClipIds[index],
            timebase::TickPosition{static_cast<std::int64_t>(index) * bar_ticks().value},
            bar_ticks(), timeline::SequenceRef{kChorusId, {0}}));
        if (!reference)
            return std::nullopt;
        reference_clips.push_back(std::move(*reference));
    }
    auto chorus_track = value_or_none(
        timeline::Track::create(kChorusTrackId, "Chorus refs", std::move(reference_clips)));
    if (!chorus_track)
        return std::nullopt;

    return value_or_none(timeline::Sequence::create(timeline::SequenceInput{
        .id = kArrangementId,
        .name = "Arrangement",
        .musical_duration = timebase::TickDuration{64 * bar_ticks().value},
        .tracks = {std::move(*hybrid), std::move(*comp_track), std::move(*chorus_track)},
        .scenes = {timeline::Scene{timeline::ItemId{50},
                                   "Session row",
                                   timeline::SlotList{std::vector<timeline::Slot>{
                                       timeline::Slot{timeline::ItemId{51}, slot_clip_id,
                                                      timeline::launch_immediate(), {}}}}}},
    }));
}

} // namespace

TimelineDawProject::TimelineDawProject() : impl_(std::make_unique<Impl>()) {}
TimelineDawProject::~TimelineDawProject() = default;

timeline::ItemId TimelineDawProject::arrangement_sequence_id() noexcept {
    return kArrangementId;
}
timeline::ItemId TimelineDawProject::chorus_sequence_id() noexcept {
    return kChorusId;
}
timeline::ItemId TimelineDawProject::hybrid_track_id() noexcept {
    return kHybridTrackId;
}
timeline::ItemId TimelineDawProject::comp_track_id() noexcept {
    return kCompTrackId;
}
timeline::ItemId TimelineDawProject::take_lane_id() noexcept {
    return kTakeLaneId;
}

bool TimelineDawProject::build() {
    auto chorus = make_chorus_sequence();
    auto arrangement = make_arrangement_sequence();
    if (!chorus || !arrangement)
        return false;

    const auto content_hash = timeline::ContentHash::from_hex(std::string(64, 'c'));
    auto tempo_map =
        value_or_none(timebase::TempoMap::create(std::array{timebase::TempoPoint{{0}, 120.0}}));
    auto meter_map =
        value_or_none(timebase::MeterMap::create(std::array{timebase::MeterPoint{{0}, {4, 4}}}));
    if (!content_hash || !tempo_map || !meter_map)
        return false;

    timeline::ProjectInput input;
    input.id = {1};
    input.name = "Full DAW-style project";
    input.next_item_id = kFirstFreeItemId;
    input.root_sequence_id = kArrangementId;
    input.assets = {{.id = kAssetId,
                     .name = "Session material",
                     .frame_count = kClipFrames,
                     .sample_rate = kRate,
                     .content_hash = *content_hash}};
    input.sequences = {std::move(*arrangement), std::move(*chorus)};
    input.tempo_map = std::move(*tempo_map);
    input.meter_map = std::move(*meter_map);
    auto project = value_or_none(timeline::Project::create(std::move(input)));
    if (!project)
        return false;

    impl_->sink = std::make_shared<CountingAutosaveSink>(impl_->stats);
    auto session = timeline::DocumentSession::create(*project, timeline::SessionLimits{},
                                                     impl_->sink);
    if (!session)
        return false;
    impl_->session = std::move(session).value();
    auto writer = impl_->session->register_writer();
    if (!writer)
        return false;
    impl_->writer.emplace(std::move(writer).value());
    impl_->project = impl_->session->snapshot();
    return true;
}

bool TimelineDawProject::diverge_third_chorus_reference() {
    if (!impl_->project || !impl_->session || !impl_->writer)
        return false;
    const auto location = impl_->project->locate(kChorusRefClipIds[2]);
    if (!location)
        return false;

    // Divergence has a purpose-built builder: it mints every cloned identity
    // from the project's own allocator and emits the CloneSequence +
    // SetClipSequenceRef pair as ONE transaction. Hand-rolling the id mapping
    // would be a second, drifting source of truth for what a sequence owns.
    const auto clone_command = impl_->writer->allocate_command_id();
    const auto retarget_command = impl_->writer->allocate_command_id();
    auto transaction = timeline::build_diverge_transaction(
        *impl_->project, *location, impl_->writer->allocate_transaction_id(),
        impl_->session->revision(), clone_command, retarget_command);
    if (!transaction)
        return false;

    // Recover the id the builder minted for the copy: it is the clone command's
    // destination, not a constant this example gets to choose.
    for (const auto& envelope : transaction.value().commands)
        if (const auto* clone = std::get_if<timeline::CloneSequence>(&envelope.command))
            impl_->diverged_chorus_id = clone->cloned_sequence_id;

    auto committed = impl_->session->submit(*impl_->writer, std::move(transaction).value());
    if (!committed)
        return false;
    impl_->project = committed.value().snapshot;
    return true;
}

timeline::ItemId TimelineDawProject::diverged_chorus_sequence_id() const noexcept {
    return impl_->diverged_chorus_id;
}

bool TimelineDawProject::apply_agent_batch(std::vector<timeline::Command> commands) {
    return impl_->submit(std::move(commands));
}

bool TimelineDawProject::autosave() {
    if (!impl_->session)
        return false;
    return impl_->session->checkpoint(impl_->session->revision());
}

const timeline::Project& TimelineDawProject::project() const noexcept {
    return *impl_->project;
}

const AutosaveStats& TimelineDawProject::autosave_stats() const noexcept {
    return impl_->stats;
}

timeline::DocumentRevision TimelineDawProject::revision() const noexcept {
    return impl_->session ? impl_->session->revision() : timeline::DocumentRevision{};
}

playback::ProviderKind
TimelineDawProject::track_provider(timeline::ItemId track_id) const noexcept {
    for (const auto& entry : impl_->providers)
        if (entry.first == track_id)
            return entry.second;
    return playback::ProviderKind::Arrangement;
}

bool TimelineDawProject::set_track_provider(timeline::ItemId track_id,
                                            playback::ProviderKind provider) {
    for (auto& entry : impl_->providers) {
        if (entry.first == track_id) {
            entry.second = provider;
            return true;
        }
    }
    return false;
}

std::size_t TimelineDawProject::reference_count(timeline::ItemId sequence_id) const noexcept {
    if (!impl_->project)
        return 0;
    const auto* arrangement = impl_->project->find_sequence(kArrangementId);
    if (arrangement == nullptr)
        return 0;
    std::size_t count = 0;
    for (const auto& track : arrangement->tracks())
        for (const auto& clip : track.clips())
            if (const auto* reference = std::get_if<timeline::SequenceRef>(&clip.content()))
                if (reference->sequence_id == sequence_id)
                    ++count;
    return count;
}

} // namespace pulp::examples::timeline_session
