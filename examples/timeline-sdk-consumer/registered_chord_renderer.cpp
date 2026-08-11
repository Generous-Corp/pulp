#include <pulp/playback/chord_pattern_renderer.hpp>
#include <pulp/playback/production_class.hpp>
#include <pulp/playback/program_compiler.hpp>
#include <pulp/timebase/compiled_tempo_map.hpp>
#include <pulp/timeline/document_session.hpp>

#include <array>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <memory>
#include <optional>
#include <span>
#include <utility>
#include <variant>
#include <vector>

namespace {

using namespace pulp;
using namespace pulp::playback;
using namespace pulp::timeline;
using namespace pulp::timebase;

constexpr ItemId kSequence{2};
constexpr ItemId kGeneratedTrack{10};
constexpr ItemId kMidiTrack{20};
constexpr ItemId kGeneratedClip{110};
constexpr TickDuration kStep{kTicksPerQuarter / 4};
constexpr TickDuration kGate{kTicksPerQuarter / 8};
constexpr TickDuration kClipDuration{3 * kStep.value};

template <typename T, typename E> std::optional<T> value_of(runtime::Result<T, E> result) {
    if (!result)
        return std::nullopt;
    return std::move(result).value();
}

std::optional<SchemaRegistry> make_schemas() {
    SchemaRegistryBuilder builder;
    if (!register_builtin_timeline_schemas(builder) ||
        !register_chord_pattern_content_schema(builder))
        return std::nullopt;
    return value_of(std::move(builder).build());
}

std::optional<Project> make_project(const SchemaRegistry& schemas) {
    auto chord_content = create_chord_pattern_content(
        {.seed = 1, .step = kStep, .gate = kGate, .octave = 4, .velocity = 40000}, schemas);
    if (!chord_content)
        return std::nullopt;
    auto generated_clip =
        Clip::create(kGeneratedClip, {0}, kClipDuration, std::move(chord_content).value());
    if (!generated_clip)
        return std::nullopt;
    auto generated_track =
        Track::create(kGeneratedTrack, "generated", {std::move(generated_clip).value()});
    if (!generated_track)
        return std::nullopt;

    auto midi = MidiContent::create({NoteEvent{{211}, {0}, {kTicksPerQuarter}, 0xffff, 36, 0}});
    if (!midi)
        return std::nullopt;
    auto midi_clip = Clip::create({210}, {0}, {kTicksPerQuarter}, std::move(midi).value());
    if (!midi_clip)
        return std::nullopt;
    auto midi_track = Track::create(kMidiTrack, "ordinary MIDI", {std::move(midi_clip).value()});
    if (!midi_track)
        return std::nullopt;

    auto harmony = ChordScaleLane::create({{{0}, ChordQuality::Major, 0, ScaleMode::Major, 0}});
    if (!harmony)
        return std::nullopt;
    SequenceInput sequence;
    sequence.id = kSequence;
    sequence.name = "registered chord renderer";
    sequence.tracks = {std::move(generated_track).value(), std::move(midi_track).value()};
    sequence.chord_scale_lane = std::move(harmony).value();
    auto root = Sequence::create(std::move(sequence));
    if (!root)
        return std::nullopt;

    ProjectInput project;
    project.id = {1};
    project.name = "registered content";
    project.next_item_id = 1000;
    project.root_sequence_id = kSequence;
    project.sequences = {std::move(root).value()};
    return value_of(Project::create(std::move(project)));
}

std::shared_ptr<const CompiledTempoMap> make_tempo_map() {
    const std::array points{TempoPoint{{0}, 120.0}};
    auto compiled = CompiledTempoMap::compile(points, {48'000, 1});
    if (!compiled)
        return {};
    return std::make_shared<const CompiledTempoMap>(std::move(compiled).value());
}

bool wait_for(PlaybackProgramCompiler& compiler, DeferredCompileExecutor& executor,
              CompileTicket ticket) {
    while (executor.pending_count() != 0 || compiler.status().busy)
        executor.run_for(std::chrono::milliseconds{1});
    const auto status = compiler.status();
    const bool complete =
        !status.has_error && status.latest_published_epoch >= ticket.submission_epoch;
    if (!complete)
        std::cerr << "compile incomplete: error=" << status.has_error
                  << " code=" << static_cast<unsigned>(status.last_error.code)
                  << " item=" << status.last_error.item.value
                  << " actual=" << status.last_error.actual << " limit=" << status.last_error.limit
                  << " busy=" << status.busy << " pending=" << executor.pending_count()
                  << " published_epoch=" << status.latest_published_epoch
                  << " ticket_epoch=" << ticket.submission_epoch << '\n';
    return complete;
}

ProgramCompileRequest
baseline_request(const DocumentView& view, std::shared_ptr<const CompiledTempoMap> tempo,
                 const std::shared_ptr<const CompileContextRegistry>& registry) {
    ProgramCompileRequest request;
    request.project = view.snapshot;
    request.sequence_id = kSequence;
    request.tempo_map = std::move(tempo);
    request.document_revision = view.revision.value;
    request.dirty.all = true;
    request.invalidation =
        CompileInvalidationInput::baseline(registry, view.snapshot, view.revision.value);
    return request;
}

std::shared_ptr<const TrackProgram> track_owner(const PlaybackProgramStore& store,
                                                ItemId track_id) {
    auto program = store.read();
    if (!program)
        return {};
    for (const auto& track : program->tracks())
        if (track->id() == track_id)
            return track;
    return {};
}

std::uint64_t semantic_hash(std::span<const NoteProgramEvent> events) {
    std::uint64_t hash = 14'695'981'039'346'656'037ull;
    const auto mix = [&](std::uint64_t value) {
        for (unsigned byte = 0; byte < 8; ++byte) {
            hash ^= (value >> (byte * 8)) & 0xffu;
            hash *= 1'099'511'628'211ull;
        }
    };
    for (const auto& event : events) {
        mix(static_cast<std::uint64_t>(event.sample.value));
        mix(static_cast<std::uint64_t>(event.tick.value));
        mix(event.velocity);
        mix(event.pitch);
        mix(event.channel);
        mix(static_cast<std::uint8_t>(event.kind));
    }
    return hash;
}

bool exact_baseline(std::span<const NoteProgramEvent> events) {
    struct Expected {
        std::int64_t sample;
        std::int64_t tick;
        std::uint16_t velocity;
        std::uint8_t pitch;
        NoteProgramEventKind kind;
    };
    constexpr std::array expected{
        Expected{0, 0, 40000, 64, NoteProgramEventKind::On},
        Expected{3000, kGate.value, 40000, 64, NoteProgramEventKind::Off},
        Expected{6000, kStep.value, 40000, 67, NoteProgramEventKind::On},
        Expected{9000, kStep.value + kGate.value, 40000, 67, NoteProgramEventKind::Off},
        Expected{12000, 2 * kStep.value, 40000, 60, NoteProgramEventKind::On},
        Expected{15000, 2 * kStep.value + kGate.value, 40000, 60, NoteProgramEventKind::Off},
    };
    if (events.size() != expected.size())
        return false;
    for (std::size_t index = 0; index < expected.size(); ++index) {
        const auto& actual = events[index];
        const auto& wanted = expected[index];
        if (actual.sample.value != wanted.sample || actual.tick.value != wanted.tick ||
            actual.velocity != wanted.velocity || actual.pitch != wanted.pitch ||
            actual.channel != 0 || actual.kind != wanted.kind)
            return false;
    }
    return semantic_hash(events) == 0x3c07dc8dbb99c381ull;
}

runtime::Result<ContentProgramFragment, ContentFragmentError>
overproduce(const RegisteredContentCompileInput& input, const void*) noexcept {
    return ContentProgramFragment::create({{{0}, {1}, 0xffff, 60, 0}, {{1}, {1}, 0xffff, 64, 0}},
                                          input.clip_duration);
}

CompileError compile_failure(const DocumentView& view,
                             std::shared_ptr<const CompiledTempoMap> tempo,
                             const std::shared_ptr<const CompileContextRegistry>& registry,
                             std::size_t maximum_note_events_per_track =
                                 ProgramCompileRequest::default_maximum_note_events_per_track) {
    PlaybackProgramStore store;
    DeferredCompileExecutor executor;
    PlaybackProgramCompiler compiler(store, executor, std::chrono::microseconds{0});
    auto request = baseline_request(view, std::move(tempo), registry);
    request.maximum_note_events_per_track = maximum_note_events_per_track;
    const auto ticket = compiler.submit(std::move(request));
    if (!ticket)
        return ticket.error();
    while (executor.pending_count() != 0 || compiler.status().busy)
        executor.run_for(std::chrono::milliseconds{1});
    return compiler.status().last_error;
}

} // namespace

int main() {
    auto schemas = make_schemas();
    if (!schemas)
        return 1;
    auto project = make_project(*schemas);
    if (!project)
        return 2;
    auto session_result = DocumentSession::create(std::move(*project));
    if (!session_result)
        return 3;
    auto session = std::move(session_result).value();
    auto writer_result = session->register_writer();
    if (!writer_result)
        return 4;
    auto writer = std::move(writer_result).value();
    const auto revision_zero = session->current();
    Transaction seed_revision;
    seed_revision.id = writer.allocate_transaction_id();
    seed_revision.expected_revision = revision_zero.revision;
    seed_revision.commands.push_back(
        {writer.allocate_command_id(),
         SetTrackName{kSequence, kMidiTrack, "ordinary MIDI", "baseline MIDI"}});
    if (!session->submit(writer, std::move(seed_revision)))
        return 4;
    auto tempo = make_tempo_map();
    if (!tempo)
        return 5;

    auto registry = std::make_shared<CompileContextRegistry>();
    if (declare_chord_pattern_renderer(*registry, *schemas))
        return 6;
    const auto initial = session->current();
    PlaybackProgramStore store;
    DeferredCompileExecutor executor;
    PlaybackProgramCompiler compiler(store, executor, std::chrono::microseconds{0});
    auto initial_ticket = compiler.submit(baseline_request(initial, tempo, registry));
    if (!initial_ticket) {
        std::cerr << "baseline submit error code="
                  << static_cast<unsigned>(initial_ticket.error().code)
                  << " item=" << initial_ticket.error().item.value
                  << " actual=" << initial_ticket.error().actual
                  << " limit=" << initial_ticket.error().limit << '\n';
        return 7;
    }
    if (!wait_for(compiler, executor, initial_ticket.value()))
        return 7;
    auto generated_before = track_owner(store, kGeneratedTrack);
    auto midi_before = track_owner(store, kMidiTrack);
    if (!generated_before || !midi_before ||
        !exact_baseline(generated_before->arrangement_note_events()))
        return 8;

    const auto* sequence = initial.snapshot->find_sequence(kSequence);
    if (!sequence)
        return 9;
    auto minor =
        ChordScaleLane::create({{{0}, ChordQuality::Minor, 0, ScaleMode::NaturalMinor, 0}});
    if (!minor)
        return 10;
    Transaction edit;
    edit.id = writer.allocate_transaction_id();
    edit.expected_revision = initial.revision;
    edit.commands.push_back(
        {writer.allocate_command_id(),
         SetChordScaleLane{kSequence, sequence->chord_scale_lane(), std::move(minor).value()}});
    auto committed = session->submit(writer, std::move(edit));
    if (!committed || committed->dirty.contexts().size() != 1 ||
        committed->dirty.contexts()[0] != DirtyContext{kSequence, CompileContextKind::ChordScale})
        return 11;

    ProgramCompileRequest incremental;
    incremental.project = committed->snapshot;
    incremental.sequence_id = kSequence;
    incremental.tempo_map = tempo;
    incremental.document_revision = committed->revision.value;
    incremental.invalidation = CompileInvalidationInput{registry, *committed};
    auto incremental_ticket = compiler.submit(std::move(incremental));
    if (!incremental_ticket || !wait_for(compiler, executor, incremental_ticket.value()))
        return 12;
    const auto status = compiler.status();
    if (status.latest_published_epoch < incremental_ticket->submission_epoch)
        return 13;
    auto generated_after = track_owner(store, kGeneratedTrack);
    auto midi_after = track_owner(store, kMidiTrack);
    if (!generated_after || !midi_after || generated_after == generated_before ||
        midi_after != midi_before)
        return 14;

    auto empty_registry = std::make_shared<CompileContextRegistry>();
    const auto unresolved = compile_failure(initial, tempo, empty_registry);
    if (unresolved.code != CompileErrorCode::UnresolvedRegisteredContent ||
        unresolved.item != kGeneratedClip)
        return 15;

    const auto* registered =
        registry->find(*std::get_if<RegisteredContent>(&initial.snapshot->find_sequence(kSequence)
                                                            ->find_track(kGeneratedTrack)
                                                            ->find_clip(kGeneratedClip)
                                                            ->content()));
    if (!registered)
        return 16;
    auto quota_registration = *registered;
    quota_registration.maximum_fragment_notes = 2;
    quota_registration.compile = overproduce;
    quota_registration.schema_registry_identity.reset();
    auto quota_registry = std::make_shared<CompileContextRegistry>();
    if (quota_registry->declare(std::move(quota_registration), *schemas))
        return 17;
    const auto quota = compile_failure(initial, tempo, quota_registry, 2);
    if (quota.code != CompileErrorCode::RegisteredContentFragmentQuotaExceeded ||
        quota.item != kGeneratedClip || quota.actual != 2 || quota.limit != 1)
        return 18;

    auto tolerance_registration = *registered;
    tolerance_registration.production.reproducibility = ReproducibilityClass::Tolerance;
    tolerance_registration.schema_registry_identity.reset();
    auto tolerance_registry = std::make_shared<CompileContextRegistry>();
    if (tolerance_registry->declare(std::move(tolerance_registration), *schemas))
        return 19;
    PlaybackProgramStore tolerance_store;
    DeferredCompileExecutor tolerance_executor;
    PlaybackProgramCompiler tolerance_compiler(tolerance_store, tolerance_executor,
                                               std::chrono::microseconds{0});
    auto tolerance_ticket =
        tolerance_compiler.submit(baseline_request(initial, tempo, tolerance_registry));
    if (!tolerance_ticket ||
        !wait_for(tolerance_compiler, tolerance_executor, tolerance_ticket.value()))
        return 20;
    auto tolerance_program = tolerance_store.read();
    if (!tolerance_program ||
        program_reproducibility(*tolerance_program) != ReproducibilityClass::Tolerance)
        return 21;

    std::cout << "registered chord renderer: deterministic hash 0x" << std::hex
              << semantic_hash(generated_before->arrangement_note_events()) << std::dec
              << ", exact invalidation reused MIDI, diagnostics and weakest production verified\n";
    return 0;
}
