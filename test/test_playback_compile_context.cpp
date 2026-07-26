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

runtime::Result<std::shared_ptr<const void>, PersistenceError>
decode_marker(const JsonValue&, const void*) noexcept {
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
    return take(registry.create_registered_no_owned_ids(
        {std::string(type_name), 1}, std::make_shared<const int>(1), 1024));
}

// Ids: sequence 2; track 10 holds a chord-following generator, track 20 holds a
// generator that reads nothing, track 30 holds only ordinary note content, and
// track 40 holds a groove-following generator. Two kinds with disjoint readers
// is what makes the exactness claim testable in both directions: a chord edit
// must leave the groove reader alone and a groove edit must leave the chord
// reader alone. Every track also carries a plain clip so no program is
// trivially empty.
std::shared_ptr<const Project> make_project(const SchemaRegistry& registry, ChordScaleLane lane,
                                            GrooveTemplate groove) {
    auto notes = [](std::uint64_t clip_id, std::uint64_t note_id) {
        auto content = take(NoteContent::create({NoteEvent{{note_id}, {0}, {240}, 0xffff, 60, 0}}));
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

    SequenceInput sequence_input;
    sequence_input.id = {2};
    sequence_input.name = "root";
    sequence_input.tracks = {follower, independent, plain, swinger};
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

std::shared_ptr<const Project> make_project_with_inactive_arrangement(
    const SchemaRegistry& registry, bool frozen) {
    auto clip = take(Clip::create(
        {110}, {0}, {480}, generator_content(registry, kFollowsHarmony)));
    std::vector<MediaAsset> assets{
        {{500}, "selected.wav", 480, {48'000, 1}, hash_of('a'),
         AssetStoragePolicy::External, {}, {}, {}}};

    TrackInput track_input{
        .id = {10},
        .name = frozen ? "frozen" : "take-selected",
        .clips = {std::move(clip)},
    };
    if (frozen) {
        track_input.freeze =
            TrackFreeze{MediaRef{{500}, {0}, 480}, {0}, {48'000, 1}, hash_of('b')};
    } else {
        auto recorded_take =
            take(Take::create({501}, MediaRef{{500}, {0}, 480}, {0}, {48'000, 1}));
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
    return std::make_shared<const Project>(
        take(Project::create(std::move(project_input))));
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

DirtySet chord_lane_edit(const Project& before, const Project& after) {
    Transaction transaction;
    transaction.id = {{1}, 1};
    transaction.commands.push_back(
        {{{1}, 1}, SetChordScaleLane{{2}, before.find_sequence({2})->chord_scale_lane(),
                                     after.find_sequence({2})->chord_scale_lane()}});
    return take(reduce_transaction(before, transaction)).dirty;
}

DirtySet groove_edit(const Project& before, const Project& after) {
    Transaction transaction;
    transaction.id = {{1}, 1};
    transaction.commands.push_back({{{1}, 1},
                                    SetGroove{{2}, before.find_sequence({2})->groove(),
                                              after.find_sequence({2})->groove()}});
    return take(reduce_transaction(before, transaction)).dirty;
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
    const auto duplicate =
        registry.declare({"vendor.one", CompileContextSubscriptions::none()});
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
    const auto index = ContextSubscriberIndex::build(*project, {2}, context_registry());

    const auto subscribers = index.subscribers(CompileContextKind::ChordScale);
    REQUIRE(subscribers.size() == 1);
    REQUIRE(subscribers[0] == ItemId{10});

    // With nothing registered, the same document has no subscribers at all —
    // the index reflects declarations, not the presence of the lane.
    REQUIRE(ContextSubscriberIndex::build(*project, {2}, CompileContextRegistry{}).empty());
    REQUIRE(ContextSubscriberIndex::build(*project, {99}, context_registry()).empty());
}

TEST_CASE("context resolution excludes arrangement clips replaced in playback",
          "[playback][compile-context][subscription]") {
    const auto schemas = generator_schema_registry();
    const auto registry = context_registry();
    const DirtySet context_edit(
        {{{2}, {}, {2}, DirtyFlags::Context}},
        {{{2}, CompileContextKind::ChordScale}});

    const auto frozen = make_project_with_inactive_arrangement(schemas, true);
    const auto frozen_index = ContextSubscriberIndex::build(*frozen, {2}, registry);
    REQUIRE(frozen_index.subscribers(CompileContextKind::ChordScale).size() == 1);
    REQUIRE(resolve_dirty_tracks(*frozen, {2}, context_edit, frozen_index).tracks.empty());

    const auto take_selected = make_project_with_inactive_arrangement(schemas, false);
    const auto take_index = ContextSubscriberIndex::build(*take_selected, {2}, registry);
    REQUIRE(take_index.subscribers(CompileContextKind::ChordScale).size() == 1);
    REQUIRE(resolve_dirty_tracks(*take_selected, {2}, context_edit, take_index).tracks.empty());
}

TEST_CASE("the subscriber index remains valid across provider selection changes",
          "[playback][compile-context][subscription]") {
    const auto schemas = generator_schema_registry();
    const auto registry = context_registry();
    const DirtySet context_edit(
        {{{2}, {}, {2}, DirtyFlags::Context}},
        {{{2}, CompileContextKind::ChordScale}});
    const auto frozen = make_project_with_inactive_arrangement(schemas, true);
    const auto frozen_index = ContextSubscriberIndex::build(*frozen, {2}, registry);

    // The same index becomes effective again from the current snapshot when
    // playback returns to the arrangement; no selection-state rebuild is
    // required and no live reader can disappear from invalidation.
    const auto arrangement = make_project(schemas, one_chord());
    const auto after_unfreeze =
        resolve_dirty_tracks(*arrangement, {2}, context_edit, frozen_index);
    REQUIRE(after_unfreeze.tracks == std::vector<ItemId>{ItemId{10}});
}

TEST_CASE("a chord-lane edit resolves to exactly its declared readers",
          "[playback][compile-context][dirty-set]") {
    const auto schemas = generator_schema_registry();
    const auto before = make_project(schemas, lane_of({}), straight_groove());
    const auto after = make_project(schemas, one_chord(), straight_groove());
    const auto index = ContextSubscriberIndex::build(*before, {2}, context_registry());

    const auto resolved =
        resolve_dirty_tracks(*after, {2}, chord_lane_edit(*before, *after), index);
    REQUIRE_FALSE(resolved.all);
    REQUIRE(resolved.tracks.size() == 1);
    REQUIRE(resolved.tracks[0] == ItemId{10});

    // Another sequence's context edit is not this sequence's business.
    Transaction elsewhere;
    elsewhere.id = {{1}, 2};
    elsewhere.commands.push_back({{{1}, 2}, SetChordScaleLane{{2}, lane_of({}), one_chord()}});
    const auto other_sequence =
        resolve_dirty_tracks(*before, {77},
                             take(reduce_transaction(*before, elsewhere)).dirty, index);
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
        resolve_dirty_tracks(*before, {2},
                             take(reduce_transaction(*before, tempo)).dirty, index);
    REQUIRE(project_scoped.all);
}

TEST_CASE("editing the chord lane recompiles its subscribers and reuses everything else",
          "[playback][compile-context][dirty-set]") {
    const auto schemas = generator_schema_registry();
    const auto before = make_project(schemas, lane_of({}), straight_groove());
    const auto after = make_project(schemas, one_chord(), straight_groove());
    const auto index = ContextSubscriberIndex::build(*before, {2}, context_registry());

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

    const auto resolved =
        resolve_dirty_tracks(*after, {2}, chord_lane_edit(*before, *after), index);
    REQUIRE(compiler.submit(request(after, map, 2, resolved)));
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
    const auto index = ContextSubscriberIndex::build(*before, {2}, CompileContextRegistry{});

    const auto resolved =
        resolve_dirty_tracks(*after, {2}, chord_lane_edit(*before, *after), index);
    REQUIRE_FALSE(resolved.all);
    REQUIRE(resolved.tracks.empty());
}

TEST_CASE("a structural edit and a context edit in one transaction dirty both",
          "[playback][compile-context][dirty-set]") {
    const auto schemas = generator_schema_registry();
    const auto before = make_project(schemas, lane_of({}), straight_groove());
    const auto index = ContextSubscriberIndex::build(*before, {2}, context_registry());

    Transaction transaction;
    transaction.id = {{1}, 1};
    transaction.commands.push_back(
        {{{1}, 1}, RemoveClip{{2}, {30}, {300}}});
    transaction.commands.push_back(
        {{{1}, 2}, SetChordScaleLane{{2}, lane_of({}), one_chord()}});
    const auto dirty = take(reduce_transaction(*before, transaction)).dirty;

    const auto resolved = resolve_dirty_tracks(*before, {2}, dirty, index);
    REQUIRE_FALSE(resolved.all);
    REQUIRE(resolved.tracks.size() == 2);
    REQUIRE(resolved.tracks[0] == ItemId{10});
    REQUIRE(resolved.tracks[1] == ItemId{30});
}

TEST_CASE("marker metadata does not dirty compiled track programs",
          "[playback][compile-context][dirty-set]") {
    const auto schemas = generator_schema_registry();
    const auto project = make_project(schemas, lane_of({}), straight_groove());
    const DirtySet marker_edit(
        {{{400}, {}, {2},
          DirtyFlags::Structure | DirtyFlags::Marker | DirtyFlags::Added}});
    const auto resolved =
        resolve_dirty_tracks(*project, {2}, marker_edit, ContextSubscriberIndex{});
    REQUIRE_FALSE(resolved.all);
    REQUIRE(resolved.tracks.empty());
}

TEST_CASE("the subscriber index separates the two context kinds",
          "[playback][compile-context][subscription]") {
    const auto schemas = generator_schema_registry();
    const auto project = make_project(schemas, one_chord(), shuffle());
    const auto index = ContextSubscriberIndex::build(*project, {2}, context_registry());

    const auto harmony_readers = index.subscribers(CompileContextKind::ChordScale);
    REQUIRE(harmony_readers.size() == 1);
    REQUIRE(harmony_readers[0] == ItemId{10});

    // A reader of one kind is not a reader of the other. If the index widened
    // to "anything that declared any context", these two spans would be equal.
    const auto groove_readers = index.subscribers(CompileContextKind::Groove);
    REQUIRE(groove_readers.size() == 1);
    REQUIRE(groove_readers[0] == ItemId{40});
}

TEST_CASE("editing the groove recompiles its subscribers and reuses everything else",
          "[playback][compile-context][dirty-set]") {
    const auto schemas = generator_schema_registry();
    const auto before = make_project(schemas, one_chord(), straight_groove());
    const auto after = make_project(schemas, one_chord(), shuffle());
    const auto index = ContextSubscriberIndex::build(*before, {2}, context_registry());

    const auto resolved =
        resolve_dirty_tracks(*after, {2}, groove_edit(*before, *after), index);
    REQUIRE_FALSE(resolved.all);
    REQUIRE(resolved.tracks.size() == 1);
    REQUIRE(resolved.tracks[0] == ItemId{40});

    PlaybackProgramStore store;
    InlineExecutor executor;
    PlaybackProgramCompiler compiler(store, executor, std::chrono::microseconds(0));
    const auto map = tempo_map();

    REQUIRE(compiler.submit(request(before, map, 1, {.all = true})));
    REQUIRE_FALSE(compiler.status().busy);
    const auto* follower_before = track_program(store, {10});
    const auto* independent_before = track_program(store, {20});
    const auto* plain_before = track_program(store, {30});
    const auto* swinger_before = track_program(store, {40});
    const auto generation_before = published_generation(store);

    REQUIRE(compiler.submit(request(after, map, 2, resolved)));
    REQUIRE_FALSE(compiler.status().busy);
    REQUIRE(published_generation(store) != generation_before);
    REQUIRE(published_revision(store) == 2);

    // The declared groove reader recompiled...
    REQUIRE(track_program(store, {40}) != swinger_before);
    // ...and nothing else did — including the track that reads the *other*
    // context kind, which is the part a per-kind index buys over a per-sequence
    // one.
    REQUIRE(track_program(store, {10}) == follower_before);
    REQUIRE(track_program(store, {20}) == independent_before);
    REQUIRE(track_program(store, {30}) == plain_before);
}

TEST_CASE("a chord-lane edit leaves the groove reader alone and the reverse",
          "[playback][compile-context][dirty-set]") {
    const auto schemas = generator_schema_registry();
    const auto plain = make_project(schemas, lane_of({}), straight_groove());
    const auto harmonised = make_project(schemas, one_chord(), straight_groove());
    const auto swung = make_project(schemas, lane_of({}), shuffle());
    const auto index = ContextSubscriberIndex::build(*plain, {2}, context_registry());

    const auto after_chord =
        resolve_dirty_tracks(*harmonised, {2}, chord_lane_edit(*plain, *harmonised), index);
    REQUIRE(after_chord.tracks.size() == 1);
    REQUIRE(after_chord.tracks[0] == ItemId{10});

    const auto after_groove =
        resolve_dirty_tracks(*swung, {2}, groove_edit(*plain, *swung), index);
    REQUIRE(after_groove.tracks.size() == 1);
    REQUIRE(after_groove.tracks[0] == ItemId{40});
}

TEST_CASE("an undeclared reader is not recompiled by a groove edit",
          "[playback][compile-context][dirty-set]") {
    // The mirror of the exactness claim for the second kind: with nothing
    // registered, the same groove edit recompiles nothing at all, so a renderer
    // that read the groove without declaring it would render a stale feel.
    const auto schemas = generator_schema_registry();
    const auto before = make_project(schemas, lane_of({}), straight_groove());
    const auto after = make_project(schemas, lane_of({}), shuffle());
    const auto index = ContextSubscriberIndex::build(*before, {2}, CompileContextRegistry{});

    const auto resolved =
        resolve_dirty_tracks(*after, {2}, groove_edit(*before, *after), index);
    REQUIRE_FALSE(resolved.all);
    REQUIRE(resolved.tracks.empty());
}
