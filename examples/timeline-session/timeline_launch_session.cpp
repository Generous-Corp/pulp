#include "timeline_launch_session.hpp"

#include <pulp/runtime/scoped_no_alloc.hpp>
#include <pulp/timebase/compiled_tempo_map.hpp>
#include <pulp/timeline/document_session.hpp>

#include <algorithm>
#include <array>
#include <optional>
#include <utility>

namespace pulp::examples::timeline_session {
namespace {

constexpr timeline::ItemId kSequenceId{3};
constexpr std::array<timeline::ItemId, 3> kTrackIds{timeline::ItemId{10}, timeline::ItemId{11},
                                                    timeline::ItemId{12}};
constexpr timeline::ItemId kSceneAId{20};
constexpr timeline::ItemId kSceneBId{21};
constexpr timeline::ItemId kPoolTrackId{13};
constexpr timeline::ItemId kAssetId{2};
constexpr std::uint64_t kFirstFreeItemId = 200;
constexpr std::uint32_t kClipFrames = 512;
constexpr timebase::RationalRate kRate{48'000, 1};

template <class T, class E> std::optional<T> value_or_none(runtime::Result<T, E> result) {
    if (!result)
        return std::nullopt;
    return std::move(result).value();
}

// One bar of 4/4 at the canonical tick resolution.
constexpr timebase::TickDuration bar_ticks() noexcept {
    return timebase::TickDuration{4 * timebase::kTicksPerQuarter};
}

} // namespace

std::vector<timeline::Command> flatten_launch_history(const timeline::Sequence& sequence,
                                                      std::span<const LaunchRecord> history,
                                                      std::uint64_t& next_item_id) {
    std::vector<timeline::Command> commands;
    commands.reserve(history.size());
    for (const auto& record : history) {
        // The capture lands on the slot's own row. The clip it copies is looked
        // up sequence-wide, because a slot may name a clip held in the launch
        // pool rather than on the row it plays from.
        if (sequence.find_track(record.track_id) == nullptr)
            continue;
        const timeline::Clip* source = nullptr;
        for (const auto& candidate : sequence.tracks()) {
            if (const auto* found = candidate.find_clip(record.clip_id)) {
                source = found;
                break;
            }
        }
        if (source == nullptr)
            continue;
        // The captured clip is an ordinary arrangement clip: the launched
        // content, placed at the beat the launch actually resolved to, for as
        // long as it actually sounded. Nothing about it records that a launcher
        // produced it.
        auto placed = timeline::Clip::create(timeline::ItemId{next_item_id}, record.start,
                                             record.duration, source->content(),
                                             source->playback_properties());
        if (!placed)
            continue;
        ++next_item_id;
        commands.push_back(
            timeline::InsertClip{sequence.id(), record.track_id, std::move(placed).value()});
    }
    return commands;
}

struct TimelineLaunchSession::Impl {
    std::shared_ptr<const timeline::Project> project;
    std::unique_ptr<timeline::DocumentSession> session;
    std::optional<timeline::WriterToken> writer;
    std::shared_ptr<const timebase::CompiledTempoMap> tempo_map;
    playback::MasterTransport transport;

    // One launch state machine per authored slot, parallel to `slot_ids`.
    std::vector<timeline::ItemId> slot_ids;
    std::vector<timeline::ItemId> slot_track_ids;
    std::vector<timeline::ItemId> slot_clip_ids;
    std::vector<playback::LaunchHandle> handles;
    std::vector<timebase::TickPosition> slot_started_at;
    std::vector<bool> slot_sounding;

    std::vector<LaunchRecord> history;
    std::size_t dropped_captures = 0;
    std::array<playback::ProviderKind, kTrackIds.size()> providers{
        playback::ProviderKind::Launcher, playback::ProviderKind::Launcher,
        playback::ProviderKind::Arrangement};
    bool prepared = false;

    std::size_t slot_index(timeline::ItemId slot_id) const noexcept {
        for (std::size_t index = 0; index < slot_ids.size(); ++index)
            if (slot_ids[index] == slot_id)
                return index;
        return slot_ids.size();
    }
};

namespace {

std::shared_ptr<const timeline::Project> make_session_project() {
    const auto content_hash = timeline::ContentHash::from_hex(std::string(64, 'b'));
    if (!content_hash)
        return nullptr;

    // The launcher rows are the three session tracks, and their arrangement
    // lanes are EMPTY — that is what makes this a session rather than a linear
    // arrangement, and it is what leaves room for a captured performance to be
    // written back into them.
    //
    // The clips a slot names live on a dedicated pool track. `Slot::clip_id`
    // documents exactly this shape ("a clip stored in the linear model, or a
    // launch-owned clip pool"): a slot references a clip that already exists in
    // the sequence, and a sequence rejects removing a clip a slot still names.
    std::vector<timeline::Track> tracks;
    std::vector<timeline::Clip> pool_clips;
    std::vector<timeline::Slot> scene_a_slots;
    std::vector<timeline::Slot> scene_b_slots;
    std::uint64_t clip_id = 100;
    std::uint64_t slot_id = 30;
    std::int64_t pool_frame = 0;
    for (std::size_t index = 0; index < kTrackIds.size(); ++index) {
        // Pool clips share one lane, so they obey the same non-overlap
        // invariant every arrangement clip does: laid end to end, not stacked.
        auto first = value_or_none(timeline::Clip::create_absolute(
            timeline::ItemId{clip_id}, {pool_frame}, kClipFrames, kRate,
            timeline::MediaRef{kAssetId, {0}, kClipFrames}));
        auto second = value_or_none(timeline::Clip::create_absolute(
            timeline::ItemId{clip_id + 1}, {pool_frame + kClipFrames}, kClipFrames, kRate,
            timeline::MediaRef{kAssetId, {0}, kClipFrames}));
        if (!first || !second)
            return nullptr;
        const auto first_clip_id = first->id();
        const auto second_clip_id = second->id();
        pool_clips.push_back(std::move(*first));
        pool_clips.push_back(std::move(*second));

        auto row = value_or_none(timeline::Track::create(
            kTrackIds[index], "Session track " + std::to_string(index + 1), {}));
        if (!row)
            return nullptr;
        tracks.push_back(std::move(*row));

        // Scene A launches on the bar; scene B is authored to launch immediately
        // so a test can prove the two quantizations resolve differently.
        scene_a_slots.push_back(timeline::Slot{timeline::ItemId{slot_id},
                                               first_clip_id,
                                               {bar_ticks(), timebase::TickPosition{0}},
                                               {}});
        scene_b_slots.push_back(
            timeline::Slot{timeline::ItemId{slot_id + 1}, second_clip_id,
                           timeline::launch_immediate(), {}});
        clip_id += 2;
        slot_id += 2;
        pool_frame += 2 * kClipFrames;
    }
    auto pool = value_or_none(
        timeline::Track::create(kPoolTrackId, "Slot clip pool", std::move(pool_clips)));
    if (!pool)
        return nullptr;
    tracks.push_back(std::move(*pool));

    auto sequence = value_or_none(timeline::Sequence::create(timeline::SequenceInput{
        .id = kSequenceId,
        .name = "Launch session",
        .musical_duration = timebase::TickDuration{1024 * bar_ticks().value},
        .tracks = std::move(tracks),
        .scenes = {timeline::Scene{kSceneAId, "A", timeline::SlotList{std::move(scene_a_slots)}},
                   timeline::Scene{kSceneBId, "B", timeline::SlotList{std::move(scene_b_slots)}}},
    }));
    if (!sequence)
        return nullptr;

    auto tempo_map = value_or_none(timebase::TempoMap::create(
        std::array{timebase::TempoPoint{{0}, 120.0}}));
    auto meter_map = value_or_none(
        timebase::MeterMap::create(std::array{timebase::MeterPoint{{0}, {4, 4}}}));
    if (!tempo_map || !meter_map)
        return nullptr;

    timeline::ProjectInput input;
    input.id = {1};
    input.name = "Timeline launch session";
    input.next_item_id = kFirstFreeItemId;
    input.root_sequence_id = kSequenceId;
    input.assets = {{.id = kAssetId,
                     .name = "Session impulse",
                     .frame_count = kClipFrames,
                     .sample_rate = kRate,
                     .content_hash = *content_hash}};
    input.sequences = {std::move(*sequence)};
    input.tempo_map = std::move(*tempo_map);
    input.meter_map = std::move(*meter_map);
    auto project = value_or_none(timeline::Project::create(std::move(input)));
    return project ? std::make_shared<const timeline::Project>(std::move(*project)) : nullptr;
}

} // namespace

TimelineLaunchSession::TimelineLaunchSession() : impl_(std::make_unique<Impl>()) {}
TimelineLaunchSession::~TimelineLaunchSession() = default;

bool TimelineLaunchSession::prepare(double sample_rate, std::uint32_t maximum_block_size) {
    impl_->project = make_session_project();
    if (!impl_->project)
        return false;

    auto session = timeline::DocumentSession::create(*impl_->project);
    if (!session)
        return false;
    impl_->session = std::move(session).value();
    auto writer = impl_->session->register_writer();
    if (!writer)
        return false;
    impl_->writer.emplace(std::move(writer).value());

    auto compiled = timebase::CompiledTempoMap::compile(
        impl_->project->tempo_map(),
        timebase::RationalRate{static_cast<std::uint64_t>(sample_rate), 1});
    if (!compiled)
        return false;
    impl_->tempo_map =
        std::make_shared<const timebase::CompiledTempoMap>(std::move(compiled).value());

    const auto* sequence = impl_->project->find_sequence(kSequenceId);
    if (sequence == nullptr)
        return false;
    for (const auto& scene : sequence->scenes()) {
        // A slot's row position in the scene names its track. That is the
        // launcher's own geometry: scenes are columns, tracks are rows, and a
        // slot belongs to the row it sits in — not to whichever track happens
        // to hold the clip it references.
        std::size_t row = 0;
        for (const auto& slot : scene.slots) {
            if (row >= kTrackIds.size())
                break;
            impl_->slot_ids.push_back(slot.id);
            impl_->slot_clip_ids.push_back(slot.clip_id);
            impl_->slot_track_ids.push_back(kTrackIds[row]);
            ++row;
        }
    }
    impl_->handles.resize(impl_->slot_ids.size());
    impl_->slot_started_at.assign(impl_->slot_ids.size(), timebase::TickPosition{0});
    impl_->slot_sounding.assign(impl_->slot_ids.size(), false);
    impl_->history.reserve(impl_->slot_ids.size() * 4);

    playback::MasterTransportConfig config;
    config.max_buffer_size = maximum_block_size;
    config.meter = {4, 4};
    config.initially_playing = true;
    if (impl_->transport.prepare(*impl_->tempo_map, config) != playback::TransportError::None)
        return false;
    impl_->prepared = true;
    return true;
}

bool TimelineLaunchSession::launch_scene(timeline::ItemId scene_id) noexcept {
    if (!impl_->prepared)
        return false;
    const auto* sequence = impl_->project->find_sequence(kSequenceId);
    if (sequence == nullptr)
        return false;
    const auto* scene = sequence->find_scene(scene_id);
    if (scene == nullptr)
        return false;
    for (const auto& slot : scene->slots) {
        const auto index = impl_->slot_index(slot.id);
        if (index < impl_->handles.size() && !slot.empty())
            impl_->handles[index].arm(slot.launch_quantize);
    }
    return true;
}

void TimelineLaunchSession::stop_all(timeline::LaunchQuantize quantize) noexcept {
    for (auto& handle : impl_->handles)
        handle.stop(quantize);
}

void TimelineLaunchSession::process(std::uint32_t frames) noexcept {
    if (!impl_->prepared)
        return;
    runtime::ScopedNoAlloc no_alloc;
    playback::TransportSnapshot snapshot;
    if (impl_->transport.begin_block(frames, snapshot) != playback::TransportError::None)
        return;
    if (snapshot.range_count == 0)
        return;
    for (std::size_t index = 0; index < impl_->handles.size(); ++index) {
        const auto event = impl_->handles[index].process(snapshot, *impl_->tempo_map);
        if (event.kind == playback::LaunchEventKind::None)
            continue;
        if (event.kind == playback::LaunchEventKind::Start) {
            // The launch resolved to an exact monotonic beat; record that rather
            // than a tick recovered back from the sample offset.
            impl_->slot_started_at[index] = impl_->handles[index].last_start().position;
            impl_->slot_sounding[index] = true;
            continue;
        }
        if (!impl_->slot_sounding[index])
            continue;
        impl_->slot_sounding[index] = false;
        const auto start = impl_->slot_started_at[index];
        // A stop resolves to an exact boundary too, and the handle keeps that
        // beat after firing — so the captured span is bounded by two exact
        // musical positions, not by the block the events happened to land in.
        const auto end = impl_->handles[index].target().position;
        if (end <= start)
            continue;
        // push_back stays allocation-free only while the vector is inside the
        // capacity reserved in prepare(). Dropping the guard would allocate on
        // the audio thread; counting the overflow keeps the truncation visible
        // instead of silently losing a launch.
        if (impl_->history.size() == impl_->history.capacity()) {
            ++impl_->dropped_captures;
            continue;
        }
        impl_->history.push_back({impl_->slot_ids[index], impl_->slot_track_ids[index],
                                  impl_->slot_clip_ids[index], start, end - start});
    }
}

bool TimelineLaunchSession::set_track_provider(timeline::ItemId track_id,
                                               playback::ProviderKind provider) {
    for (std::size_t index = 0; index < kTrackIds.size(); ++index) {
        if (kTrackIds[index] == track_id) {
            impl_->providers[index] = provider;
            return true;
        }
    }
    return false;
}

playback::ProviderKind
TimelineLaunchSession::track_provider(timeline::ItemId track_id) const noexcept {
    for (std::size_t index = 0; index < kTrackIds.size(); ++index)
        if (kTrackIds[index] == track_id)
            return impl_->providers[index];
    return playback::ProviderKind::Arrangement;
}

const std::vector<LaunchRecord>& TimelineLaunchSession::history() const noexcept {
    return impl_->history;
}

const timeline::Project& TimelineLaunchSession::project() const noexcept {
    return *impl_->project;
}

const timebase::CompiledTempoMap& TimelineLaunchSession::tempo_map() const noexcept {
    return *impl_->tempo_map;
}

std::size_t TimelineLaunchSession::dropped_capture_count() const noexcept {
    return impl_->dropped_captures;
}

std::size_t TimelineLaunchSession::sounding_slot_count() const noexcept {
    return static_cast<std::size_t>(
        std::count(impl_->slot_sounding.begin(), impl_->slot_sounding.end(), true));
}

bool TimelineLaunchSession::apply(std::span<const timeline::Command> commands) {
    if (!impl_->session || !impl_->writer || commands.empty())
        return false;
    timeline::Transaction transaction;
    transaction.id = impl_->writer->allocate_transaction_id();
    transaction.expected_revision = impl_->session->revision();
    for (const auto& command : commands)
        transaction.commands.push_back({impl_->writer->allocate_command_id(), command});
    auto committed = impl_->session->submit(*impl_->writer, std::move(transaction));
    if (!committed)
        return false;
    impl_->project = committed.value().snapshot;
    return true;
}

} // namespace pulp::examples::timeline_session
