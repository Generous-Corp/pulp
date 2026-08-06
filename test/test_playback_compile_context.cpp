#include <pulp/playback/compile_context_registry.hpp>
#include <pulp/playback/program_compiler.hpp>
#include <pulp/timeline/schema_registry.hpp>
#include <pulp/timeline/transaction.hpp>

#include "timebase_test_helpers.hpp"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <chrono>
#include <cstdlib>
#include <memory>
#include <string>
#include <vector>

using namespace pulp;
using namespace pulp::playback;
using namespace pulp::timeline;
using namespace pulp::timebase;

namespace {

template <typename T, typename E> T take(runtime::Result<T, E> result) {
    if (!result)
        std::abort();
    return std::move(result).value();
}

constexpr std::string_view kFollowsHarmony = "vendor.chord_follower";
constexpr std::string_view kFollowsGroove = "vendor.groove_follower";
constexpr std::string_view kIgnoresHarmony = "vendor.plain_generator";

ContentHash hash_of(char digit) {
    return *ContentHash::from_hex(std::string(64, digit));
}

runtime::Result<std::shared_ptr<const void>, PersistenceError> decode_marker(const JsonValue&,
                                                                             const void*) noexcept {
    return runtime::Ok(std::shared_ptr<const void>(std::make_shared<const int>(1)));
}

runtime::Result<SchemaWriteSuccess, PersistenceError>
encode_marker(const std::shared_ptr<const void>&, BoundedJsonSink& output, const void*) noexcept {
    output.append("{}");
    return runtime::Ok(SchemaWriteSuccess{});
}

std::size_t retained_marker(const std::shared_ptr<const void>&, const void*) noexcept {
    return sizeof(int);
}

SchemaRegistry generator_schema_registry() {
    SchemaRegistryBuilder builder;
    for (const auto name : {kFollowsHarmony, kFollowsGroove, kIgnoresHarmony}) {
        TypeSchema schema;
        schema.type_name = std::string(name);
        schema.domain = SchemaDomain::Content;
        schema.current_version = 1;
        schema.codec = {{}, decode_marker, encode_marker, retained_marker};
        REQUIRE(builder.register_type(std::move(schema)));
    }
    return take(std::move(builder).build());
}

ClipContent generator_content(const SchemaRegistry& registry, std::string_view type_name) {
    return take(registry.create_registered_no_owned_ids({std::string(type_name), 1},
                                                        std::make_shared<const int>(1), 1024));
}

// Ids: sequence 2; track 10 holds a chord-following generator, track 20 holds a
// generator that reads nothing, track 30 holds only ordinary note content, and
// track 40 holds a groove-following generator. Two kinds with disjoint readers
// is what makes the exactness claim testable in both directions: a chord edit
// must leave the groove reader alone and a groove edit must leave the chord
// reader alone. Every track also carries a plain clip so no program is
// trivially empty. Track 50 is intentionally empty content: it is the negative
// control proving a groove edit does not widen to every track in the sequence.
std::shared_ptr<const Project> make_project(const SchemaRegistry& registry, ChordScaleLane lane,
                                            GrooveTemplate groove) {
    auto notes = [](std::uint64_t clip_id, std::uint64_t note_id) {
        auto content = take(MidiContent::create({NoteEvent{{note_id}, {0}, {240}, 0xffff, 60, 0}}));
        return take(Clip::create({clip_id}, {0}, {480}, std::move(content)));
    };

    auto follower = take(Track::create(
        {10}, "follower",
        {notes(100, 101),
         take(Clip::create({110}, {480}, {480}, generator_content(registry, kFollowsHarmony)))}));
    auto independent = take(Track::create(
        {20}, "independent",
        {notes(200, 201),
         take(Clip::create({210}, {480}, {480}, generator_content(registry, kIgnoresHarmony)))}));
    auto plain = take(Track::create({30}, "plain", {notes(300, 301)}));
    auto swinger = take(Track::create(
        {40}, "swinger",
        {notes(400, 401),
         take(Clip::create({410}, {480}, {480}, generator_content(registry, kFollowsGroove)))}));
    auto empty =
        take(Track::create({50}, "empty", {take(Clip::create({450}, {0}, {480}, EmptyContent{}))}));

    SequenceInput sequence_input;
    sequence_input.id = {2};
    sequence_input.name = "root";
    sequence_input.tracks = {follower, independent, plain, swinger, empty};
    sequence_input.chord_scale_lane = std::move(lane);
    sequence_input.groove = std::move(groove);
    auto sequence = take(Sequence::create(std::move(sequence_input)));
    ProjectInput input;
    input.id = {1};
    input.name = "context";
    input.next_item_id = 500;
    input.root_sequence_id = {2};
    input.sequences.push_back(std::move(sequence));
    return std::make_shared<const Project>(take(Project::create(std::move(input))));
}

enum class ProviderSelection { Arrangement, Frozen, Take };

std::shared_ptr<const Project> make_project_with_provider_selection(const SchemaRegistry& registry,
                                                                    ProviderSelection selection) {
    auto clip = take(Clip::create({110}, {0}, {480}, generator_content(registry, kFollowsHarmony)));
    auto notes = take(MidiContent::create({NoteEvent{{112}, {0}, {240}, 0xffff, 60, 0}}));
    auto note_clip = take(Clip::create({111}, {480}, {480}, std::move(notes)));
    std::vector<MediaAsset> assets{{{500},
                                    "selected.wav",
                                    480,
                                    {48'000, 1},
                                    hash_of('a'),
                                    AssetStoragePolicy::External,
                                    {},
                                    {},
                                    {}}};

    TrackInput track_input{
        .id = {10},
        .name = selection == ProviderSelection::Frozen ? "frozen"
                : selection == ProviderSelection::Take ? "take-selected"
                                                       : "arrangement",
        .clips = {std::move(clip), std::move(note_clip)},
    };
    if (selection == ProviderSelection::Frozen) {
        track_input.freeze = TrackFreeze{MediaRef{{500}, {0}, 480}, {0}, {48'000, 1}, hash_of('b')};
    } else if (selection == ProviderSelection::Take) {
        auto recorded_take = take(Take::create({501}, MediaRef{{500}, {0}, 480}, {0}, {48'000, 1}));
        track_input.take_lanes = {
            take(TakeLane::create({502}, "selected", {std::move(recorded_take)}))};
        track_input.active_take_lane_id = {502};
    }

    auto track = take(Track::create(std::move(track_input)));
    auto sequence =
        take(Sequence::create({2}, "root", std::nullopt, std::nullopt, {std::move(track)}));
    ProjectInput project_input;
    project_input.id = {1};
    project_input.name = "inactive arrangement";
    project_input.next_item_id = 600;
    project_input.root_sequence_id = {2};
    project_input.assets = std::move(assets);
    project_input.sequences = {std::move(sequence)};
    return std::make_shared<const Project>(take(Project::create(std::move(project_input))));
}

std::shared_ptr<const Project> make_project_with_nested_generator(const SchemaRegistry& registry,
                                                                  std::size_t reference_count = 1) {
    auto generator =
        take(Clip::create({30}, {0}, {480}, generator_content(registry, kFollowsHarmony)));
    auto child_track = take(Track::create({20}, "child", {std::move(generator)}));
    auto child =
        take(Sequence::create({3}, "child", std::nullopt, std::nullopt, {std::move(child_track)}));

    std::vector<Track> root_tracks;
    for (std::size_t index = 0; index < reference_count; ++index) {
        auto reference = take(Clip::create({100 + index * 2}, {0}, {480}, SequenceRef{{3}, {0}}));
        root_tracks.push_back(
            take(Track::create({101 + index * 2}, "root", {std::move(reference)})));
    }
    auto root =
        take(Sequence::create({2}, "root", std::nullopt, std::nullopt, std::move(root_tracks)));

    ProjectInput input;
    input.id = {1};
    input.name = "nested context";
    input.next_item_id = 100 + reference_count * 2;
    input.root_sequence_id = {2};
    input.sequences = {std::move(root), std::move(child)};
    return std::make_shared<const Project>(take(Project::create(std::move(input))));
}

CompileContextRegistry context_registry() {
    CompileContextRegistry registry;
    auto reads_harmony = CompileContextSubscriptions::none();
    reads_harmony.subscribe(CompileContextKind::ChordScale);
    auto reads_groove = CompileContextSubscriptions::none();
    reads_groove.subscribe(CompileContextKind::Groove);
    REQUIRE_FALSE(registry.declare({std::string(kFollowsHarmony), reads_harmony}));
    REQUIRE_FALSE(registry.declare({std::string(kFollowsGroove), reads_groove}));
    REQUIRE_FALSE(
        registry.declare({std::string(kIgnoresHarmony), CompileContextSubscriptions::none()}));
    return registry;
}

GrooveTemplate groove_of(GrooveTemplateInput input) {
    return take(GrooveTemplate::create(std::move(input)));
}

GrooveTemplate straight_groove() {
    return groove_of({});
}

GrooveTemplate shuffle() {
    GrooveTemplateInput input;
    input.name = "shuffle";
    input.swing_grid = TickDuration{kTicksPerQuarter / 2};
    input.swing = kTripletSwing;
    return groove_of(std::move(input));
}

GrooveTemplate pushed_groove() {
    GrooveTemplateInput input;
    input.step = TickDuration{100};
    input.steps = {GrooveStep{TickDuration{20}, kGrooveUnitScale}};
    return groove_of(std::move(input));
}

ChordScaleLane lane_of(std::vector<ChordScaleEvent> events) {
    return take(ChordScaleLane::create(std::move(events)));
}

ChordScaleLane one_chord() {
    return lane_of({ChordScaleEvent{{0}, ChordQuality::Minor7, 9, ScaleMode::Dorian, 9}});
}

std::shared_ptr<const CompiledTempoMap> tempo_map() {
    const std::array points{TempoPoint{{0}, 120.0}};
    return shared_compiled_tempo_map(points, RationalRate{48'000, 1});
}

class InlineExecutor final : public CompileExecutor {
  public:
    bool submit(std::unique_ptr<CompileTask> task, std::chrono::steady_clock::time_point) override {
        if (!task)
            return false;
        while (task->run_slice({std::chrono::steady_clock::now() + std::chrono::seconds(1),
                                10'000}) == CompileTaskStatus::Pending) {
        }
        return true;
    }
};

class ManualSliceExecutor final : public CompileExecutor {
  public:
    bool submit(std::unique_ptr<CompileTask> task, std::chrono::steady_clock::time_point) override {
        if (reject || !task || task_)
            return false;
        task_ = std::move(task);
        return true;
    }

    void run_one(std::size_t max_work_units) {
        REQUIRE(task_);
        auto running = std::move(task_);
        const auto status = running->run_slice(
            {std::chrono::steady_clock::now() + std::chrono::seconds(1), max_work_units});
        if (status == CompileTaskStatus::Pending) {
            REQUIRE_FALSE(task_);
            task_ = std::move(running);
        }
    }

    bool pending() const noexcept {
        return task_ != nullptr;
    }
    bool reject = false;

  private:
    std::unique_ptr<CompileTask> task_;
};

ProgramCompileRequest request(std::shared_ptr<const Project> project,
                              std::shared_ptr<const CompiledTempoMap> map, std::uint64_t revision,
                              DirtyTrackSet dirty) {
    ProgramCompileRequest result;
    result.project = std::move(project);
    result.sequence_id = {2};
    result.tempo_map = std::move(map);
    result.document_revision = revision;
    result.dirty = std::move(dirty);
    return result;
}

CompileInvalidationInput
committed_invalidation(std::shared_ptr<const CompileContextRegistry> registry,
                       std::shared_ptr<const Project> predecessor,
                       std::shared_ptr<const Project> snapshot, std::uint64_t revision,
                       DirtySet dirty) {
    const CommitResult committed{
        snapshot, DocumentRevision{revision}, std::move(dirty), {}, std::move(predecessor)};
    return CompileInvalidationInput{std::move(registry), committed};
}

// A track's compiled program is a shared object in the published program. If
// the compiler reused it, the address is literally the same object; if it
// recompiled, it is a new one. That is a direct observation of what did and did
// not recompile, not a proxy for it.
const TrackProgram* track_program(const PlaybackProgramStore& store, ItemId track) {
    auto live = store.read();
    REQUIRE(live);
    const auto* program = live->find_track(track);
    REQUIRE(program != nullptr);
    return program;
}

ProgramGeneration published_generation(const PlaybackProgramStore& store) {
    auto live = store.read();
    REQUIRE(live);
    return live->generation();
}

std::uint64_t published_revision(const PlaybackProgramStore& store) {
    auto live = store.read();
    REQUIRE(live);
    return live->document_revision();
}

ReducedTransaction chord_lane_edit(const Project& before, const Project& after) {
    Transaction transaction;
    transaction.id = {{1}, 1};
    transaction.commands.push_back(
        {{{1}, 1},
         SetChordScaleLane{{2},
                           before.find_sequence({2})->chord_scale_lane(),
                           after.find_sequence({2})->chord_scale_lane()}});
    return take(reduce_transaction(before, transaction));
}

ReducedTransaction groove_edit(const Project& before, const Project& after) {
    Transaction transaction;
    transaction.id = {{1}, 1};
    transaction.commands.push_back(
        {{{1}, 1},
         SetGroove{{2}, before.find_sequence({2})->groove(), after.find_sequence({2})->groove()}});
    return take(reduce_transaction(before, transaction));
}

ReducedTransaction remove_track(const Project& before, ItemId track_id) {
    Transaction transaction;
    transaction.id = {{1}, 1};
    transaction.commands.push_back({{{1}, 1}, RemoveTrack{{2}, track_id}});
    return take(reduce_transaction(before, transaction));
}

} // namespace

TEST_CASE("a context registry refuses an empty or duplicate content type",
          "[playback][compile-context][subscription]") {
    CompileContextRegistry registry;
    auto reads_harmony = CompileContextSubscriptions::none();
    reads_harmony.subscribe(CompileContextKind::ChordScale);

    const auto empty = registry.declare({"", reads_harmony});
    REQUIRE(empty);
    REQUIRE(empty->code == ContextRegistrationErrorCode::EmptyContentTypeName);

    REQUIRE_FALSE(registry.declare({"vendor.one", reads_harmony}));
    // Two renderers disagreeing about what a content kind reads would make the
    // invalidation depend on registration order, so the second is refused.
    const auto duplicate = registry.declare({"vendor.one", CompileContextSubscriptions::none()});
    REQUIRE(duplicate);
    REQUIRE(duplicate->code == ContextRegistrationErrorCode::DuplicateContentType);
    REQUIRE(registry.size() == 1);
    REQUIRE(registry.subscriptions_for("vendor.one").reads(CompileContextKind::ChordScale));

    // An unregistered type reads nothing: no renderer compiles it, so there is
    // no program that could go stale.
    REQUIRE_FALSE(registry.subscriptions_for("vendor.unknown").any());
}

TEST_CASE("the subscriber index names only the tracks that declared the context",
          "[playback][compile-context][subscription]") {
    const auto schemas = generator_schema_registry();
    const auto project = make_project(schemas, one_chord(), straight_groove());
    const auto index = CompileInvalidationIndex::build(*project, {2}, context_registry());

    const auto subscribers = index.subscribers(CompileContextKind::ChordScale);
    REQUIRE(subscribers.size() == 1);
    REQUIRE(subscribers[0] == ItemId{10});

    // Built-in MIDI compilation reads groove without a plugin registration.
    // The built-in contract is part of the same exact index rather than a
    // second invalidation path that could silently drift from this one.
    const auto valid_empty =
        CompileInvalidationIndex::build(*project, {2}, CompileContextRegistry{});
    REQUIRE(valid_empty.valid());
    REQUIRE(valid_empty.subscribers(CompileContextKind::ChordScale).empty());
    const auto builtin_groove = valid_empty.subscribers(CompileContextKind::Groove);
    REQUIRE(std::vector<ItemId>(builtin_groove.begin(), builtin_groove.end()) ==
            std::vector<ItemId>{ItemId{10}, ItemId{20}, ItemId{30}, ItemId{40}});
    const auto invalid = CompileInvalidationIndex::build(*project, {99}, context_registry());
    REQUIRE_FALSE(invalid.valid());
    REQUIRE(invalid.empty());
}

TEST_CASE("a root track subscribes to contexts read through a sequence reference",
          "[playback][compile-context][subscription][sequence-ref]") {
    const auto schemas = generator_schema_registry();
    const auto project = make_project_with_nested_generator(schemas);
    const auto index = CompileInvalidationIndex::build(*project, {2}, context_registry());

    const auto subscribers = index.subscribers(CompileContextKind::ChordScale);
    REQUIRE(subscribers.size() == 1);
    REQUIRE(subscribers[0] == ItemId{101});
    REQUIRE(index.subscribers({2}, CompileContextKind::ChordScale).empty());
    const auto child_subscribers = index.subscribers({3}, CompileContextKind::ChordScale);
    REQUIRE(child_subscribers.size() == 1);
    REQUIRE(child_subscribers[0] == ItemId{101});

    const DirtySet child_context_edit({{{3}, {}, {3}, DirtyFlags::Context}},
                                      {{{3}, CompileContextKind::ChordScale}});
    const auto resolved = resolve_dirty_tracks(*project, {2}, child_context_edit, index);
    REQUIRE_FALSE(resolved.all);
    REQUIRE(resolved.tracks == std::vector<ItemId>{ItemId{101}});
}

TEST_CASE("shared referenced sequences fan out subscriptions without repeated graph walks",
          "[playback][compile-context][subscription][sequence-ref]") {
    const auto schemas = generator_schema_registry();
    constexpr std::size_t reference_count = 1'000;
    const auto project = make_project_with_nested_generator(schemas, reference_count);
    const auto index = CompileInvalidationIndex::build(*project, {2}, context_registry());

    const auto subscribers = index.subscribers(CompileContextKind::ChordScale);
    REQUIRE(subscribers.size() == reference_count);
    REQUIRE(subscribers.front() == ItemId{101});
    REQUIRE(subscribers.back() == ItemId{101 + (reference_count - 1) * 2});
}

TEST_CASE("a stale structural index fails closed instead of under-invalidating",
          "[playback][compile-context][subscription][sequence-ref]") {
    const auto schemas = generator_schema_registry();
    const auto before = make_project_with_nested_generator(schemas);
    const auto index = CompileInvalidationIndex::build(*before, {2}, context_registry());
    const auto after = make_project_with_nested_generator(schemas, 2);
    const DirtySet child_context_edit({{{3}, {}, {3}, DirtyFlags::Context}},
                                      {{{3}, CompileContextKind::ChordScale}});

    const auto resolved = resolve_dirty_tracks(*after, {2}, child_context_edit, index);
    REQUIRE(resolved.all);
    REQUIRE(resolved.tracks.empty());
}

TEST_CASE("root and registry mismatches invalidate the bundled index",
          "[playback][compile-context][subscription][invalidation]") {
    const auto schemas = generator_schema_registry();
    const auto project = make_project_with_nested_generator(schemas);
    auto registry = context_registry();
    const auto index = CompileInvalidationIndex::build(*project, {2}, registry);
    const DirtySet child_context_edit({{{3}, {}, {3}, DirtyFlags::Context}},
                                      {{{3}, CompileContextKind::ChordScale}});

    const auto wrong_root = resolve_dirty_tracks(*project, {3}, child_context_edit, index);
    REQUIRE(wrong_root.all);
    REQUIRE(wrong_root.tracks.empty());

    auto reads_harmony = CompileContextSubscriptions::none();
    reads_harmony.subscribe(CompileContextKind::ChordScale);
    REQUIRE_FALSE(registry.declare({"vendor.late-reader", reads_harmony}));
    REQUIRE_FALSE(index.valid());
    const auto stale_registry = resolve_dirty_tracks(*project, {2}, child_context_edit, index);
    REQUIRE(stale_registry.all);
    REQUIRE(stale_registry.tracks.empty());
}

TEST_CASE("registry assignment invalidates prior indices and leaves moved-from registries usable",
          "[playback][compile-context][subscription][invalidation]") {
    const auto schemas = generator_schema_registry();
    const auto project = make_project_with_nested_generator(schemas);

    auto copy_destination = context_registry();
    const auto copy_index = CompileInvalidationIndex::build(*project, {2}, copy_destination);
    const CompileContextRegistry empty_source;
    copy_destination = empty_source;
    REQUIRE_FALSE(copy_index.valid());

    auto move_destination = context_registry();
    const auto move_index = CompileInvalidationIndex::build(*project, {2}, move_destination);
    auto move_source = CompileContextRegistry{};
    move_destination = std::move(move_source);
    REQUIRE_FALSE(move_index.valid());

    auto reads_harmony = CompileContextSubscriptions::none();
    reads_harmony.subscribe(CompileContextKind::ChordScale);
    REQUIRE_FALSE(move_source.declare({"vendor.after-move", reads_harmony}));
    REQUIRE(move_source.size() == 1);
}

TEST_CASE("context resolution excludes arrangement clips replaced in playback",
          "[playback][compile-context][subscription]") {
    const auto schemas = generator_schema_registry();
    const auto registry = context_registry();
    const DirtySet context_edit({{{2}, {}, {2}, DirtyFlags::Context}},
                                {{{2}, CompileContextKind::ChordScale}});
    const DirtySet groove_edit({{{2}, {}, {2}, DirtyFlags::Context}},
                               {{{2}, CompileContextKind::Groove}});

    const auto frozen = make_project_with_provider_selection(schemas, ProviderSelection::Frozen);
    const auto frozen_index = CompileInvalidationIndex::build(*frozen, {2}, registry);
    REQUIRE(frozen_index.subscribers(CompileContextKind::ChordScale).size() == 1);
    REQUIRE(frozen_index.subscribers(CompileContextKind::Groove).size() == 1);
    REQUIRE(resolve_dirty_tracks(*frozen, {2}, context_edit, frozen_index).tracks.empty());
    REQUIRE(resolve_dirty_tracks(*frozen, {2}, groove_edit, frozen_index).tracks.empty());

    const auto take_selected =
        make_project_with_provider_selection(schemas, ProviderSelection::Take);
    const auto take_index = CompileInvalidationIndex::build(*take_selected, {2}, registry);
    REQUIRE(take_index.subscribers(CompileContextKind::ChordScale).size() == 1);
    REQUIRE(take_index.subscribers(CompileContextKind::Groove).size() == 1);
    REQUIRE(resolve_dirty_tracks(*take_selected, {2}, context_edit, take_index).tracks.empty());
    REQUIRE(resolve_dirty_tracks(*take_selected, {2}, groove_edit, take_index).tracks.empty());
}

TEST_CASE("the subscriber index remains valid across provider selection changes",
          "[playback][compile-context][subscription]") {
    const auto schemas = generator_schema_registry();
    const auto registry = context_registry();
    const DirtySet context_edit({{{2}, {}, {2}, DirtyFlags::Context}},
                                {{{2}, CompileContextKind::ChordScale}});
    const auto frozen = make_project_with_provider_selection(schemas, ProviderSelection::Frozen);
    const auto frozen_index = CompileInvalidationIndex::build(*frozen, {2}, registry);

    // The same index becomes effective again from the current snapshot when
    // playback returns to the arrangement; no selection-state rebuild is
    // required and no live reader can disappear from invalidation.
    Transaction unfreeze;
    unfreeze.id = {{1}, 1};
    unfreeze.commands.push_back(
        {{{1}, 1},
         SetTrackFreeze{
             {2}, {10}, frozen->find_sequence({2})->find_track({10})->freeze(), std::nullopt}});
    const auto arrangement = take(reduce_transaction(*frozen, unfreeze));
    const auto after_unfreeze =
        resolve_dirty_tracks(arrangement.project, {2}, context_edit, frozen_index);
    REQUIRE(after_unfreeze.tracks == std::vector<ItemId>{ItemId{10}});
}

TEST_CASE("a chord-lane edit resolves to exactly its declared readers",
          "[playback][compile-context][dirty-set]") {
    const auto schemas = generator_schema_registry();
    const auto before = make_project(schemas, lane_of({}), straight_groove());
    const auto after = make_project(schemas, one_chord(), straight_groove());
    const auto index = CompileInvalidationIndex::build(*before, {2}, context_registry());

    const auto edit = chord_lane_edit(*before, *after);
    const auto resolved = resolve_dirty_tracks(edit.project, {2}, edit.dirty, index);
    REQUIRE_FALSE(resolved.all);
    REQUIRE(resolved.tracks.size() == 1);
    REQUIRE(resolved.tracks[0] == ItemId{10});

    // Another sequence's context edit is not this sequence's business.
    Transaction elsewhere;
    elsewhere.id = {{1}, 2};
    elsewhere.commands.push_back({{{1}, 2}, SetChordScaleLane{{2}, lane_of({}), one_chord()}});
    const auto other_sequence = resolve_dirty_tracks(
        *before, {77}, take(reduce_transaction(*before, elsewhere)).dirty, index);
    REQUIRE_FALSE(other_sequence.all);
    REQUIRE(other_sequence.tracks.empty());

    // A project-scoped edit still asks for everything: every track's program is
    // derived from the tempo map, so none of them can be reused.
    Transaction tempo;
    tempo.id = {{1}, 3};
    const std::array faster{TempoPoint{{0}, 140.0, TempoCurve::Constant}};
    const auto replacement = take(TempoMap::create(faster));
    tempo.commands.push_back({{{1}, 3}, SetTempoMap{before->tempo_map(), replacement}});
    const auto project_scoped =
        resolve_dirty_tracks(*before, {2}, take(reduce_transaction(*before, tempo)).dirty, index);
    REQUIRE(project_scoped.all);
}

TEST_CASE("editing the chord lane recompiles its subscribers and reuses everything else",
          "[playback][compile-context][dirty-set]") {
    const auto schemas = generator_schema_registry();
    const auto before = make_project(schemas, lane_of({}), straight_groove());
    const auto after = make_project(schemas, one_chord(), straight_groove());
    const auto index = CompileInvalidationIndex::build(*before, {2}, context_registry());

    PlaybackProgramStore store;
    InlineExecutor executor;
    PlaybackProgramCompiler compiler(store, executor, std::chrono::microseconds(0));
    const auto map = tempo_map();

    REQUIRE(compiler.submit(request(before, map, 1, {.all = true})));
    REQUIRE_FALSE(compiler.status().busy);
    const auto* follower_before = track_program(store, {10});
    const auto* independent_before = track_program(store, {20});
    const auto* plain_before = track_program(store, {30});
    const auto generation_before = published_generation(store);

    const auto edit = chord_lane_edit(*before, *after);
    const auto edited = std::make_shared<const Project>(edit.project);
    const auto resolved = resolve_dirty_tracks(edit.project, {2}, edit.dirty, index);
    REQUIRE(compiler.submit(request(edited, map, 2, resolved)));
    REQUIRE_FALSE(compiler.status().busy);

    // The program really was republished, so "unchanged pointer" below means
    // reuse across a compile rather than the absence of one.
    REQUIRE(published_generation(store) != generation_before);
    REQUIRE(published_revision(store) == 2);

    // The declared reader recompiled...
    REQUIRE(track_program(store, {10}) != follower_before);
    // ...and nothing that did not declare the read was touched. This is the
    // exactness claim: not "few recompiles", but these and no others.
    REQUIRE(track_program(store, {20}) == independent_before);
    REQUIRE(track_program(store, {30}) == plain_before);
}

TEST_CASE("an undeclared reader is not recompiled by a chord-lane edit",
          "[playback][compile-context][dirty-set]") {
    // The mirror image of the exactness claim, and the reason the declaration
    // is mandatory: with no registration, the same document and the same edit
    // recompile nothing at all. A renderer that read the lane without declaring
    // it would go stale here, which is what makes the contract load-bearing
    // rather than advisory.
    const auto schemas = generator_schema_registry();
    const auto before = make_project(schemas, lane_of({}), straight_groove());
    const auto after = make_project(schemas, one_chord(), straight_groove());
    const auto index = CompileInvalidationIndex::build(*before, {2}, CompileContextRegistry{});

    const auto edit = chord_lane_edit(*before, *after);
    const auto resolved = resolve_dirty_tracks(edit.project, {2}, edit.dirty, index);
    REQUIRE_FALSE(resolved.all);
    REQUIRE(resolved.tracks.empty());

    const DirtySet direct_edit({{{300}, {30}, {2}, DirtyFlags::Structure | DirtyFlags::Removed}});
    const auto invalid_index =
        resolve_dirty_tracks(*after, {2}, direct_edit, CompileInvalidationIndex{});
    REQUIRE(invalid_index.all);
    REQUIRE(invalid_index.tracks.empty());
}

TEST_CASE("a structural edit and a context edit in one transaction dirty both",
          "[playback][compile-context][dirty-set]") {
    const auto schemas = generator_schema_registry();
    const auto before = make_project(schemas, lane_of({}), straight_groove());
    const auto index = CompileInvalidationIndex::build(*before, {2}, context_registry());

    Transaction transaction;
    transaction.id = {{1}, 1};
    transaction.commands.push_back({{{1}, 1}, RemoveClip{{2}, {50}, {450}}});
    transaction.commands.push_back({{{1}, 2}, SetChordScaleLane{{2}, lane_of({}), one_chord()}});
    const auto reduced = take(reduce_transaction(*before, transaction));
    REQUIRE(reduced.project.sequence_compile_structure_token() ==
            before->sequence_compile_structure_token());

    const auto resolved = resolve_dirty_tracks(reduced.project, {2}, reduced.dirty, index);
    REQUIRE_FALSE(resolved.all);
    REQUIRE(resolved.tracks.size() == 2);
    REQUIRE(resolved.tracks[0] == ItemId{10});
    REQUIRE(resolved.tracks[1] == ItemId{50});
}

TEST_CASE("adding or removing built-in MIDI invalidates an old subscriber index",
          "[playback][compile-context][dirty-set][invalidation]") {
    const auto schemas = generator_schema_registry();
    const auto before = make_project(schemas, lane_of({}), straight_groove());

    const auto groove_after = [](const Project& project) {
        Transaction groove;
        groove.id = {{2}, 1};
        groove.commands.push_back(
            {{{2}, 1}, SetGroove{{2}, project.find_sequence({2})->groove(), pushed_groove()}});
        return take(reduce_transaction(project, groove));
    };

    SECTION("add MIDI") {
        const auto old_index = CompileInvalidationIndex::build(*before, {2}, context_registry());
        auto midi = take(MidiContent::create({NoteEvent{{501}, {0}, {120}, 1000, 60, 0}}));
        Transaction insert;
        insert.id = {{1}, 1};
        insert.commands.push_back(
            {{{1}, 1},
             InsertClip{{2}, {50}, take(Clip::create({500}, {480}, {240}, std::move(midi)))}});
        const auto reduced = take(reduce_transaction(*before, insert));
        const auto grooved = groove_after(reduced.project);
        REQUIRE(reduced.project.sequence_compile_structure_token() !=
                before->sequence_compile_structure_token());
        REQUIRE(resolve_dirty_tracks(grooved.project, {2}, grooved.dirty, old_index).all);

        const auto fresh =
            CompileInvalidationIndex::build(grooved.project, {2}, context_registry());
        const auto resolved = resolve_dirty_tracks(grooved.project, {2}, grooved.dirty, fresh);
        REQUIRE_FALSE(resolved.all);
        REQUIRE(std::find(resolved.tracks.begin(), resolved.tracks.end(), ItemId{50}) !=
                resolved.tracks.end());
    }

    SECTION("remove MIDI") {
        const auto old_index = CompileInvalidationIndex::build(*before, {2}, context_registry());
        Transaction remove;
        remove.id = {{1}, 1};
        remove.commands.push_back({{{1}, 1}, RemoveClip{{2}, {30}, {300}}});
        const auto reduced = take(reduce_transaction(*before, remove));
        const auto grooved = groove_after(reduced.project);
        REQUIRE(reduced.project.sequence_compile_structure_token() !=
                before->sequence_compile_structure_token());
        REQUIRE(resolve_dirty_tracks(grooved.project, {2}, grooved.dirty, old_index).all);

        const auto fresh =
            CompileInvalidationIndex::build(grooved.project, {2}, context_registry());
        const auto resolved = resolve_dirty_tracks(grooved.project, {2}, grooved.dirty, fresh);
        REQUIRE_FALSE(resolved.all);
        REQUIRE(std::find(resolved.tracks.begin(), resolved.tracks.end(), ItemId{30}) ==
                resolved.tracks.end());
    }
}

TEST_CASE("a new registered-content reader invalidates the structural token",
          "[playback][compile-context][dirty-set][invalidation]") {
    const auto schemas = generator_schema_registry();
    const auto before = make_project(schemas, lane_of({}), straight_groove());
    const auto index = CompileInvalidationIndex::build(*before, {2}, context_registry());
    Transaction insert;
    insert.id = {{1}, 1};
    insert.commands.push_back(
        {{{1}, 1},
         InsertClip{{2},
                    {30},
                    take(Clip::create({500}, {960}, {480},
                                      generator_content(schemas, kFollowsHarmony)))}});
    const auto reduced = take(reduce_transaction(*before, insert));
    REQUIRE(reduced.project.sequence_compile_structure_token() !=
            before->sequence_compile_structure_token());
    const DirtySet context_edit({{{2}, {}, {2}, DirtyFlags::Context}},
                                {{{2}, CompileContextKind::ChordScale}});

    const auto resolved = resolve_dirty_tracks(reduced.project, {2}, context_edit, index);
    REQUIRE(resolved.all);
    REQUIRE(resolved.tracks.empty());
}

TEST_CASE("marker metadata does not dirty compiled track programs",
          "[playback][compile-context][dirty-set]") {
    const auto schemas = generator_schema_registry();
    const auto project = make_project(schemas, lane_of({}), straight_groove());
    const DirtySet marker_edit(
        {{{400}, {}, {2}, DirtyFlags::Structure | DirtyFlags::Marker | DirtyFlags::Added}});
    const auto resolved =
        resolve_dirty_tracks(*project, {2}, marker_edit, CompileInvalidationIndex{});
    REQUIRE_FALSE(resolved.all);
    REQUIRE(resolved.tracks.empty());
}

TEST_CASE("the subscriber index separates the two context kinds",
          "[playback][compile-context][subscription]") {
    const auto schemas = generator_schema_registry();
    const auto project = make_project(schemas, one_chord(), shuffle());
    const auto index = CompileInvalidationIndex::build(*project, {2}, context_registry());

    const auto harmony_readers = index.subscribers(CompileContextKind::ChordScale);
    REQUIRE(harmony_readers.size() == 1);
    REQUIRE(harmony_readers[0] == ItemId{10});

    // Every note-bearing track has built-in MIDI content, so all four are groove readers.
    // The registered chord follower remains the only harmony reader.
    const auto groove_readers = index.subscribers(CompileContextKind::Groove);
    REQUIRE(std::vector<ItemId>(groove_readers.begin(), groove_readers.end()) ==
            std::vector<ItemId>{ItemId{10}, ItemId{20}, ItemId{30}, ItemId{40}});
}

TEST_CASE("editing the groove recompiles every built-in MIDI subscriber",
          "[playback][compile-context][dirty-set]") {
    const auto schemas = generator_schema_registry();
    const auto before = make_project(schemas, one_chord(), straight_groove());
    const auto after = make_project(schemas, one_chord(), shuffle());
    auto registry = std::make_shared<CompileContextRegistry>(context_registry());
    const auto index = CompileInvalidationIndex::build(*before, {2}, *registry);

    const auto edit = groove_edit(*before, *after);
    const auto edited = std::make_shared<const Project>(edit.project);
    const auto resolved = resolve_dirty_tracks(edit.project, {2}, edit.dirty, index);
    REQUIRE_FALSE(resolved.all);
    REQUIRE(resolved.tracks == std::vector<ItemId>{ItemId{10}, ItemId{20}, ItemId{30}, ItemId{40}});

    PlaybackProgramStore store;
    InlineExecutor executor;
    PlaybackProgramCompiler compiler(store, executor, std::chrono::microseconds(0));
    const auto map = tempo_map();

    auto initial_request = request(before, map, 1, {.all = true});
    initial_request.invalidation = CompileInvalidationInput::baseline(registry, before, 1);
    REQUIRE(compiler.submit(std::move(initial_request)));
    REQUIRE_FALSE(compiler.status().busy);
    const auto* follower_before = track_program(store, {10});
    const auto* independent_before = track_program(store, {20});
    const auto* plain_before = track_program(store, {30});
    const auto* swinger_before = track_program(store, {40});
    const auto* empty_before = track_program(store, {50});
    const auto generation_before = published_generation(store);

    auto production_request = request(edited, map, 2, {});
    production_request.invalidation =
        committed_invalidation(registry, before, edited, 2, edit.dirty);
    REQUIRE(compiler.submit(std::move(production_request)));
    REQUIRE_FALSE(compiler.status().busy);
    REQUIRE(published_generation(store) != generation_before);
    REQUIRE(published_revision(store) == 2);

    // Built-in MIDI compilation itself declares the groove dependency, so all
    // four note programs recompile through the production submit path.
    REQUIRE(track_program(store, {40}) != swinger_before);
    REQUIRE(track_program(store, {10}) != follower_before);
    REQUIRE(track_program(store, {20}) != independent_before);
    REQUIRE(track_program(store, {30}) != plain_before);
    REQUIRE(track_program(store, {50}) == empty_before);
}

TEST_CASE("a successor cannot lose dirtiness already taken by an active compile",
          "[playback][compile-context][invalidation][coalescing]") {
    const auto schemas = generator_schema_registry();
    const auto base = make_project(schemas, lane_of({}), straight_groove());
    const auto groove_target = make_project(schemas, lane_of({}), pushed_groove());
    const auto combined_target = make_project(schemas, one_chord(), pushed_groove());
    const auto groove_commit = groove_edit(*base, *groove_target);
    const auto after_groove = std::make_shared<const Project>(groove_commit.project);
    const auto chord_commit = chord_lane_edit(*after_groove, *combined_target);
    const auto after_both = std::make_shared<const Project>(chord_commit.project);
    auto registry = std::make_shared<CompileContextRegistry>(context_registry());
    PlaybackProgramStore store;
    ManualSliceExecutor executor;
    PlaybackProgramCompiler compiler(store, executor, std::chrono::microseconds(0));
    const auto map = tempo_map();

    auto initial = request(base, map, 1, {.all = true});
    initial.invalidation = CompileInvalidationInput::baseline(registry, base, 1);
    REQUIRE(compiler.submit(std::move(initial)));
    while (executor.pending())
        executor.run_one(10'000);
    const auto* plain_before = track_program(store, {30});
    const auto* empty_before = track_program(store, {50});

    auto groove_request = request(after_groove, map, 2, {});
    groove_request.invalidation =
        committed_invalidation(registry, base, after_groove, 2, groove_commit.dirty);
    groove_request.track_policies.push_back({{10}, {}, RendererStatePolicy::Stateless});
    const auto groove_ticket = compiler.submit(std::move(groove_request));
    REQUIRE(groove_ticket);
    executor.run_one(1);
    REQUIRE(executor.pending());

    // This disjoint chord edit carries a cumulative snapshot. The groove task
    // is already active and will be denied publication, so its dirty delta must
    // not disappear when this successor is accepted.
    auto chord_request = request(after_both, map, 3, {});
    chord_request.invalidation =
        committed_invalidation(registry, after_groove, after_both, 3, chord_commit.dirty);
    const auto chord_ticket = compiler.submit(std::move(chord_request));
    REQUIRE(chord_ticket);
    while (executor.pending())
        executor.run_one(10'000);

    REQUIRE_FALSE(compiler.status().has_error);
    REQUIRE(compiler.status().latest_published_revision == 3);
    REQUIRE(compiler.status().latest_published_epoch >= groove_ticket.value().submission_epoch);
    REQUIRE(compiler.status().latest_published_epoch >= chord_ticket.value().submission_epoch);
    REQUIRE(track_program(store, {30}) != plain_before);
    REQUIRE(track_program(store, {50}) != empty_before);
    REQUIRE(track_program(store, {10})->state_policy() == RendererStatePolicy::Stateless);
    const auto events = track_program(store, {30})->arrangement_note_events();
    REQUIRE(events.size() == 2);
    REQUIRE(events[0].tick == TickPosition{20});
}

TEST_CASE("a later coalesced publication completes every accepted earlier ticket",
          "[playback][compile-context][ticket][coalescing]") {
    const auto schemas = generator_schema_registry();
    const auto project = make_project(schemas, one_chord(), straight_groove());
    PlaybackProgramStore store;
    ManualSliceExecutor executor;
    PlaybackProgramCompiler compiler(store, executor, std::chrono::microseconds(0));

    auto first = request(project, tempo_map(), 1, {.all = true});
    const auto first_ticket = compiler.submit(std::move(first));
    REQUIRE(first_ticket);
    auto second = request(project, tempo_map(), 2, {.all = true});
    const auto second_ticket = compiler.submit(std::move(second));
    REQUIRE(second_ticket);
    REQUIRE(second_ticket.value().submission_epoch > first_ticket.value().submission_epoch);

    while (executor.pending())
        executor.run_one(10'000);
    const auto status = compiler.status();
    REQUIRE_FALSE(status.busy);
    REQUIRE_FALSE(status.has_error);
    REQUIRE(status.latest_published_revision == 2);
    REQUIRE(status.latest_published_epoch >= first_ticket.value().submission_epoch);
    REQUIRE(status.latest_published_epoch >= second_ticket.value().submission_epoch);
}

TEST_CASE("a skipped committed revision forces cumulative snapshot recompilation",
          "[playback][compile-context][invalidation][revision-gap]") {
    const auto schemas = generator_schema_registry();
    const auto base = make_project(schemas, lane_of({}), straight_groove());
    const auto groove_target = make_project(schemas, lane_of({}), pushed_groove());
    const auto combined_target = make_project(schemas, one_chord(), pushed_groove());
    const auto skipped = groove_edit(*base, *groove_target);
    const auto after_skipped = std::make_shared<const Project>(skipped.project);
    const auto submitted = chord_lane_edit(*after_skipped, *combined_target);
    const auto cumulative = std::make_shared<const Project>(submitted.project);
    auto registry = std::make_shared<CompileContextRegistry>(context_registry());
    PlaybackProgramStore store;
    InlineExecutor executor;
    PlaybackProgramCompiler compiler(store, executor, std::chrono::microseconds(0));
    const auto map = tempo_map();

    auto initial = request(base, map, 1, {.all = true});
    initial.invalidation = CompileInvalidationInput::baseline(registry, base, 1);
    REQUIRE(compiler.submit(std::move(initial)));
    const auto* plain_before = track_program(store, {30});
    const auto* empty_before = track_program(store, {50});

    // Revision 2's groove commit was never submitted. Revision 3 contains its
    // state cumulatively but its own DirtySet names only the chord edit.
    auto changed = request(cumulative, map, 3, {});
    changed.invalidation =
        committed_invalidation(registry, after_skipped, cumulative, 3, submitted.dirty);
    REQUIRE(compiler.submit(std::move(changed)));
    REQUIRE_FALSE(compiler.status().has_error);
    REQUIRE(track_program(store, {30}) != plain_before);
    REQUIRE(track_program(store, {50}) != empty_before);
    const auto events = track_program(store, {30})->arrangement_note_events();
    REQUIRE(events.size() == 2);
    REQUIRE(events[0].tick == TickPosition{20});
}

TEST_CASE("a commit from a forked predecessor cannot reuse the live revision",
          "[playback][compile-context][invalidation][lineage]") {
    const auto schemas = generator_schema_registry();
    const auto live_base = make_project(schemas, lane_of({}), straight_groove());
    const auto fork_base = make_project(schemas, lane_of({}), pushed_groove());
    const auto fork_target = make_project(schemas, one_chord(), pushed_groove());
    const auto fork_commit = chord_lane_edit(*fork_base, *fork_target);
    const auto target = std::make_shared<const Project>(fork_commit.project);
    auto registry = std::make_shared<CompileContextRegistry>(context_registry());
    PlaybackProgramStore store;
    InlineExecutor executor;
    PlaybackProgramCompiler compiler(store, executor, std::chrono::microseconds(0));
    const auto map = tempo_map();

    auto initial = request(live_base, map, 1, {.all = true});
    initial.invalidation = CompileInvalidationInput::baseline(registry, live_base, 1);
    REQUIRE(compiler.submit(std::move(initial)));
    const auto* plain_before = track_program(store, {30});

    // Both bases claim revision 1 and the same IDs, but only fork_base is the
    // predecessor of this sparse chord commit. The groove difference must not
    // be hidden by reusing live_base's plain MIDI track.
    auto changed = request(target, map, 2, {});
    changed.invalidation =
        committed_invalidation(registry, fork_base, target, 2, fork_commit.dirty);
    REQUIRE(compiler.submit(std::move(changed)));
    REQUIRE_FALSE(compiler.status().has_error);
    REQUIRE(track_program(store, {30}) != plain_before);
    const auto events = track_program(store, {30})->arrangement_note_events();
    REQUIRE(events.size() == 2);
    REQUIRE(events[0].tick == TickPosition{20});
}

TEST_CASE("production invalidation accepts a committed root-track removal",
          "[playback][compile-context][invalidation][remove-track]") {
    const auto schemas = generator_schema_registry();
    const auto before = make_project(schemas, one_chord(), straight_groove());
    const auto removed = remove_track(*before, {50});
    const auto after = std::make_shared<const Project>(removed.project);
    auto registry = std::make_shared<CompileContextRegistry>(context_registry());
    PlaybackProgramStore store;
    InlineExecutor executor;
    PlaybackProgramCompiler compiler(store, executor, std::chrono::microseconds(0));
    const auto map = tempo_map();

    auto initial = request(before, map, 1, {.all = true});
    initial.invalidation = CompileInvalidationInput::baseline(registry, before, 1);
    REQUIRE(compiler.submit(std::move(initial)));
    const auto* survivor_before = track_program(store, {30});

    auto changed = request(after, map, 2, {});
    changed.invalidation = committed_invalidation(registry, before, after, 2, removed.dirty);
    REQUIRE(compiler.submit(std::move(changed)));
    REQUIRE_FALSE(compiler.status().has_error);
    REQUIRE(track_program(store, {30}) == survivor_before);
    const auto live = store.read();
    REQUIRE(live);
    REQUIRE(live->find_track({50}) == nullptr);
}

TEST_CASE("production invalidation rejects mismatched snapshot provenance",
          "[playback][compile-context][invalidation][provenance]") {
    const auto schemas = generator_schema_registry();
    const auto project = make_project(schemas, one_chord(), straight_groove());
    const auto other_snapshot = make_project(schemas, one_chord(), straight_groove());
    auto registry = std::make_shared<CompileContextRegistry>(context_registry());
    const auto map = tempo_map();

    {
        PlaybackProgramStore store;
        InlineExecutor executor;
        PlaybackProgramCompiler compiler(store, executor, std::chrono::microseconds(0));
        auto mismatched = request(project, map, 1, {.all = true});
        mismatched.invalidation = CompileInvalidationInput::baseline(registry, other_snapshot, 1);
        const auto result = compiler.submit(std::move(mismatched));
        REQUIRE_FALSE(result);
        REQUIRE(result.error().code == CompileErrorCode::InvalidRequest);
    }
    {
        PlaybackProgramStore store;
        InlineExecutor executor;
        PlaybackProgramCompiler compiler(store, executor, std::chrono::microseconds(0));
        auto mismatched = request(project, map, 2, {.all = true});
        mismatched.invalidation = CompileInvalidationInput::baseline(registry, project, 1);
        const auto result = compiler.submit(std::move(mismatched));
        REQUIRE_FALSE(result);
        REQUIRE(result.error().code == CompileErrorCode::InvalidRequest);
    }
    {
        PlaybackProgramStore store;
        InlineExecutor executor;
        PlaybackProgramCompiler compiler(store, executor, std::chrono::microseconds(0));
        auto sparse_baseline = request(project, map, 1, {});
        sparse_baseline.invalidation = CompileInvalidationInput::baseline(registry, project, 1);
        const auto result = compiler.submit(std::move(sparse_baseline));
        REQUIRE_FALSE(result);
        REQUIRE(result.error().code == CompileErrorCode::InvalidRequest);
    }
}

TEST_CASE("a registry-generation change forces a full production recompile",
          "[playback][compile-context][invalidation]") {
    const auto schemas = generator_schema_registry();
    const auto before = make_project(schemas, lane_of({}), straight_groove());
    const auto desired = make_project(schemas, one_chord(), straight_groove());
    const auto edit = chord_lane_edit(*before, *desired);
    const auto after = std::make_shared<const Project>(edit.project);
    auto registry = std::make_shared<CompileContextRegistry>(context_registry());
    PlaybackProgramStore store;
    InlineExecutor executor;
    PlaybackProgramCompiler compiler(store, executor, std::chrono::microseconds(0));
    const auto map = tempo_map();

    auto initial = request(before, map, 1, {.all = true});
    initial.invalidation = CompileInvalidationInput::baseline(registry, before, 1);
    REQUIRE(compiler.submit(std::move(initial)));
    const auto* follower_before = track_program(store, {10});
    const auto* independent_before = track_program(store, {20});
    const auto* plain_before = track_program(store, {30});
    const auto* swinger_before = track_program(store, {40});
    const auto* empty_before = track_program(store, {50});

    auto reads_harmony = CompileContextSubscriptions::none();
    reads_harmony.subscribe(CompileContextKind::ChordScale);
    REQUIRE_FALSE(registry->declare({"vendor.late-reader", reads_harmony}));

    auto changed = request(after, map, 2, {});
    changed.invalidation = committed_invalidation(registry, before, after, 2, edit.dirty);
    REQUIRE(compiler.submit(std::move(changed)));
    REQUIRE_FALSE(compiler.status().has_error);
    REQUIRE(track_program(store, {10}) != follower_before);
    REQUIRE(track_program(store, {20}) != independent_before);
    REQUIRE(track_program(store, {30}) != plain_before);
    REQUIRE(track_program(store, {40}) != swinger_before);
    REQUIRE(track_program(store, {50}) != empty_before);
}

TEST_CASE("a commit invalidation owns an immutable registry snapshot",
          "[playback][compile-context][invalidation][registry-snapshot]") {
    const auto schemas = generator_schema_registry();
    const auto before = make_project(schemas, lane_of({}), straight_groove());
    const auto desired = make_project(schemas, one_chord(), straight_groove());
    const auto edit = chord_lane_edit(*before, *desired);
    const auto after = std::make_shared<const Project>(edit.project);
    auto registry = std::make_shared<CompileContextRegistry>(context_registry());
    PlaybackProgramStore store;
    InlineExecutor executor;
    PlaybackProgramCompiler compiler(store, executor, std::chrono::microseconds(0));
    const auto map = tempo_map();

    auto initial = request(before, map, 1, {.all = true});
    initial.invalidation = CompileInvalidationInput::baseline(registry, before, 1);
    REQUIRE(compiler.submit(std::move(initial)));
    const auto* follower_before = track_program(store, {10});
    const auto* independent_before = track_program(store, {20});

    auto changed = request(after, map, 2, {});
    changed.invalidation = committed_invalidation(registry, before, after, 2, edit.dirty);

    // Replace the caller's registry after invalidation capture but before
    // submit. Resolution must still use the captured chord-reader declaration.
    *registry = CompileContextRegistry{};
    REQUIRE(compiler.submit(std::move(changed)));
    REQUIRE_FALSE(compiler.status().has_error);
    REQUIRE(track_program(store, {10}) != follower_before);
    REQUIRE(track_program(store, {20}) == independent_before);
}

TEST_CASE("registry replacement refreshes the same document with a fresh generation",
          "[playback][compile-context][invalidation]") {
    const auto schemas = generator_schema_registry();
    const auto project = make_project(schemas, one_chord(), straight_groove());
    // Start at revision zero. Assignment below also installs a fresh revision
    // zero generation, so pointer-plus-counter comparison would false-pass.
    auto registry = std::make_shared<CompileContextRegistry>();
    PlaybackProgramStore store;
    ManualSliceExecutor executor;
    PlaybackProgramCompiler compiler(store, executor, std::chrono::microseconds(0));

    auto pending = request(project, tempo_map(), 1, {.all = true});
    pending.invalidation = CompileInvalidationInput::baseline(registry, project, 1);
    REQUIRE(compiler.submit(std::move(pending)));
    REQUIRE(executor.pending());
    REQUIRE_FALSE(store.read());

    // Let the first generation become active before replacing the caller's
    // registry object. The later refresh must not alias its revision-zero token.
    executor.run_one(1);
    REQUIRE(executor.pending());

    CompileContextRegistry replacement;
    auto reads_harmony = CompileContextSubscriptions::none();
    reads_harmony.subscribe(CompileContextKind::ChordScale);
    REQUIRE_FALSE(replacement.declare({"vendor.replacement", reads_harmony}));
    *registry = std::move(replacement);

    while (executor.pending())
        executor.run_one(10'000);

    REQUIRE_FALSE(compiler.status().has_error);
    const auto* follower_before = track_program(store, {10});
    const auto* independent_before = track_program(store, {20});
    const auto* plain_before = track_program(store, {30});
    const auto* swinger_before = track_program(store, {40});
    const auto* empty_before = track_program(store, {50});

    // Registry-only configuration changes are compile inputs, not document
    // edits, so the same document revision is accepted and rebuilt in full.
    executor.reject = true;
    auto refresh = request(project, tempo_map(), 1, {.all = true});
    refresh.invalidation = CompileInvalidationInput::baseline(registry, project, 1);
    const auto rejected = compiler.submit(std::move(refresh));
    REQUIRE_FALSE(rejected);
    REQUIRE(rejected.error().code == CompileErrorCode::ExecutorUnavailable);
    executor.reject = false;

    auto retry = request(project, tempo_map(), 1, {.all = true});
    retry.invalidation = CompileInvalidationInput::baseline(registry, project, 1);
    const auto ticket = compiler.submit(std::move(retry));
    REQUIRE(ticket);
    REQUIRE(ticket.value().revision == 1);
    REQUIRE(ticket.value().submission_epoch > compiler.status().latest_published_epoch);
    while (executor.pending())
        executor.run_one(10'000);

    const auto status = compiler.status();
    REQUIRE_FALSE(status.busy);
    REQUIRE_FALSE(status.has_error);
    REQUIRE(status.latest_published_revision == 1);
    REQUIRE(status.latest_published_epoch == ticket.value().submission_epoch);
    REQUIRE(track_program(store, {10}) != follower_before);
    REQUIRE(track_program(store, {20}) != independent_before);
    REQUIRE(track_program(store, {30}) != plain_before);
    REQUIRE(track_program(store, {40}) != swinger_before);
    REQUIRE(track_program(store, {50}) != empty_before);
}

TEST_CASE("a full baseline adopts a live revision after compiler replacement",
          "[playback][compile-context][invalidation][lifecycle]") {
    const auto schemas = generator_schema_registry();
    const auto project = make_project(schemas, one_chord(), straight_groove());
    const auto divergent = make_project(schemas, one_chord(), straight_groove());
    auto registry = std::make_shared<CompileContextRegistry>(context_registry());
    PlaybackProgramStore store;
    InlineExecutor executor;

    {
        PlaybackProgramCompiler compiler(store, executor, std::chrono::microseconds(0));
        auto initial = request(project, tempo_map(), 1, {.all = true});
        REQUIRE(compiler.submit(std::move(initial)));
    }
    const auto generation_before = published_generation(store);
    const auto* track_before = track_program(store, {30});

    PlaybackProgramCompiler replacement(store, executor, std::chrono::microseconds(0));
    auto invalid = request(divergent, tempo_map(), 1, {.all = true});
    invalid.invalidation = CompileInvalidationInput::baseline(registry, divergent, 1);
    const auto rejected = replacement.submit(std::move(invalid));
    REQUIRE_FALSE(rejected);
    REQUIRE(rejected.error().code == CompileErrorCode::StaleRevision);

    auto refresh = request(project, tempo_map(), 1, {.all = true});
    refresh.invalidation = CompileInvalidationInput::baseline(registry, project, 1);
    const auto ticket = replacement.submit(std::move(refresh));
    REQUIRE(ticket);
    REQUIRE_FALSE(replacement.status().has_error);
    REQUIRE(published_generation(store) != generation_before);
    REQUIRE(track_program(store, {30}) != track_before);
    REQUIRE(replacement.status().latest_published_epoch == ticket.value().submission_epoch);
}

TEST_CASE("executor rejection after a legacy publish leaves a baseline refresh retryable",
          "[playback][compile-context][invalidation][lifecycle]") {
    const auto schemas = generator_schema_registry();
    const auto project = make_project(schemas, one_chord(), straight_groove());
    auto registry = std::make_shared<CompileContextRegistry>(context_registry());
    PlaybackProgramStore store;
    ManualSliceExecutor executor;
    PlaybackProgramCompiler compiler(store, executor, std::chrono::microseconds(0));

    auto initial = request(project, tempo_map(), 1, {.all = true});
    REQUIRE(compiler.submit(std::move(initial)));
    while (executor.pending())
        executor.run_one(10'000);

    executor.reject = true;
    auto rejected_refresh = request(project, tempo_map(), 1, {.all = true});
    rejected_refresh.invalidation = CompileInvalidationInput::baseline(registry, project, 1);
    const auto rejected = compiler.submit(std::move(rejected_refresh));
    REQUIRE_FALSE(rejected);
    REQUIRE(rejected.error().code == CompileErrorCode::ExecutorUnavailable);

    executor.reject = false;
    auto retry = request(project, tempo_map(), 1, {.all = true});
    retry.invalidation = CompileInvalidationInput::baseline(registry, project, 1);
    const auto ticket = compiler.submit(std::move(retry));
    REQUIRE(ticket);
    while (executor.pending())
        executor.run_one(10'000);
    REQUIRE_FALSE(compiler.status().has_error);
    REQUIRE(compiler.status().latest_published_epoch == ticket.value().submission_epoch);
}

TEST_CASE("a chord-lane edit leaves the groove reader alone and the reverse",
          "[playback][compile-context][dirty-set]") {
    const auto schemas = generator_schema_registry();
    const auto plain = make_project(schemas, lane_of({}), straight_groove());
    const auto harmonised = make_project(schemas, one_chord(), straight_groove());
    const auto swung = make_project(schemas, lane_of({}), shuffle());
    const auto index = CompileInvalidationIndex::build(*plain, {2}, context_registry());

    const auto chord_edit = chord_lane_edit(*plain, *harmonised);
    const auto after_chord = resolve_dirty_tracks(chord_edit.project, {2}, chord_edit.dirty, index);
    REQUIRE(after_chord.tracks.size() == 1);
    REQUIRE(after_chord.tracks[0] == ItemId{10});

    const auto groove = groove_edit(*plain, *swung);
    const auto after_groove = resolve_dirty_tracks(groove.project, {2}, groove.dirty, index);
    REQUIRE(after_groove.tracks ==
            std::vector<ItemId>{ItemId{10}, ItemId{20}, ItemId{30}, ItemId{40}});
}

TEST_CASE("built-in MIDI remains a groove subscriber with an empty plugin registry",
          "[playback][compile-context][dirty-set]") {
    // Built-in dependencies cannot be disabled by omitting plugin declarations.
    const auto schemas = generator_schema_registry();
    const auto before = make_project(schemas, lane_of({}), straight_groove());
    const auto after = make_project(schemas, lane_of({}), shuffle());
    const auto index = CompileInvalidationIndex::build(*before, {2}, CompileContextRegistry{});

    const auto edit = groove_edit(*before, *after);
    const auto resolved = resolve_dirty_tracks(edit.project, {2}, edit.dirty, index);
    REQUIRE_FALSE(resolved.all);
    REQUIRE(resolved.tracks == std::vector<ItemId>{ItemId{10}, ItemId{20}, ItemId{30}, ItemId{40}});
}
