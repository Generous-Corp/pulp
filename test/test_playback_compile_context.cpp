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
constexpr std::string_view kIgnoresHarmony = "vendor.plain_generator";

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
    for (const auto name : {kFollowsHarmony, kIgnoresHarmony}) {
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
// generator that reads nothing, track 30 holds only ordinary note content.
// Every track also carries a plain clip so no program is trivially empty.
std::shared_ptr<const Project> make_project(const SchemaRegistry& registry,
                                            ChordScaleLane lane) {
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

    auto sequence =
        take(Sequence::create({2}, "root", std::nullopt, std::nullopt,
                              std::vector<Track>{follower, independent, plain}, {}, {},
                              std::move(lane)));
    ProjectInput input;
    input.id = {1};
    input.name = "context";
    input.next_item_id = 400;
    input.root_sequence_id = {2};
    input.sequences.push_back(std::move(sequence));
    return std::make_shared<const Project>(take(Project::create(std::move(input))));
}

CompileContextRegistry context_registry() {
    CompileContextRegistry registry;
    auto reads_harmony = CompileContextSubscriptions::none();
    reads_harmony.subscribe(CompileContextKind::ChordScale);
    REQUIRE_FALSE(registry.declare({std::string(kFollowsHarmony), reads_harmony}));
    REQUIRE_FALSE(
        registry.declare({std::string(kIgnoresHarmony), CompileContextSubscriptions::none()}));
    return registry;
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
    const auto project = make_project(schemas, one_chord());
    const auto index = ContextSubscriberIndex::build(*project, {2}, context_registry());

    const auto subscribers = index.subscribers(CompileContextKind::ChordScale);
    REQUIRE(subscribers.size() == 1);
    REQUIRE(subscribers[0] == ItemId{10});

    // With nothing registered, the same document has no subscribers at all —
    // the index reflects declarations, not the presence of the lane.
    REQUIRE(ContextSubscriberIndex::build(*project, {2}, CompileContextRegistry{}).empty());
    REQUIRE(ContextSubscriberIndex::build(*project, {99}, context_registry()).empty());
}

TEST_CASE("a chord-lane edit resolves to exactly its declared readers",
          "[playback][compile-context][dirty-set]") {
    const auto schemas = generator_schema_registry();
    const auto before = make_project(schemas, lane_of({}));
    const auto after = make_project(schemas, one_chord());
    const auto index = ContextSubscriberIndex::build(*before, {2}, context_registry());

    const auto resolved = resolve_dirty_tracks({2}, chord_lane_edit(*before, *after), index);
    REQUIRE_FALSE(resolved.all);
    REQUIRE(resolved.tracks.size() == 1);
    REQUIRE(resolved.tracks[0] == ItemId{10});

    // Another sequence's context edit is not this sequence's business.
    Transaction elsewhere;
    elsewhere.id = {{1}, 2};
    elsewhere.commands.push_back({{{1}, 2}, SetChordScaleLane{{2}, lane_of({}), one_chord()}});
    const auto other_sequence =
        resolve_dirty_tracks({77}, take(reduce_transaction(*before, elsewhere)).dirty, index);
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
        resolve_dirty_tracks({2}, take(reduce_transaction(*before, tempo)).dirty, index);
    REQUIRE(project_scoped.all);
}

TEST_CASE("editing the chord lane recompiles its subscribers and reuses everything else",
          "[playback][compile-context][dirty-set]") {
    const auto schemas = generator_schema_registry();
    const auto before = make_project(schemas, lane_of({}));
    const auto after = make_project(schemas, one_chord());
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

    const auto resolved = resolve_dirty_tracks({2}, chord_lane_edit(*before, *after), index);
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
    const auto before = make_project(schemas, lane_of({}));
    const auto after = make_project(schemas, one_chord());
    const auto index = ContextSubscriberIndex::build(*before, {2}, CompileContextRegistry{});

    const auto resolved = resolve_dirty_tracks({2}, chord_lane_edit(*before, *after), index);
    REQUIRE_FALSE(resolved.all);
    REQUIRE(resolved.tracks.empty());
}

TEST_CASE("a structural edit and a context edit in one transaction dirty both",
          "[playback][compile-context][dirty-set]") {
    const auto schemas = generator_schema_registry();
    const auto before = make_project(schemas, lane_of({}));
    const auto index = ContextSubscriberIndex::build(*before, {2}, context_registry());

    Transaction transaction;
    transaction.id = {{1}, 1};
    transaction.commands.push_back(
        {{{1}, 1}, RemoveClip{{2}, {30}, {300}}});
    transaction.commands.push_back(
        {{{1}, 2}, SetChordScaleLane{{2}, lane_of({}), one_chord()}});
    const auto dirty = take(reduce_transaction(*before, transaction)).dirty;

    const auto resolved = resolve_dirty_tracks({2}, dirty, index);
    REQUIRE_FALSE(resolved.all);
    REQUIRE(resolved.tracks.size() == 2);
    REQUIRE(resolved.tracks[0] == ItemId{10});
    REQUIRE(resolved.tracks[1] == ItemId{30});
}
