#include <pulp/playback/audio_renderer.hpp>
#include <pulp/playback/transport.hpp>
#include <pulp/timeline/serialize.hpp>

#include <array>

int main() {
    using namespace pulp;

    playback::AudioClipRendererProgram renderer_program;
    if (renderer_program.uses_sample_rate_conversion())
        return 1;

    auto sequence = timeline::Sequence::create(
        timeline::ItemId{2}, "Root", timebase::TickDuration{4 * timebase::kTicksPerQuarter}, {});
    if (!sequence)
        return 2;
    auto project = timeline::Project::create(timeline::ProjectInput{timeline::ItemId{1},
                                                                    "SDK consumer",
                                                                    3,
                                                                    timeline::ItemId{2},
                                                                    {},
                                                                    {std::move(sequence).value()}});
    if (!project)
        return 3;
    auto registry = timeline::make_builtin_timeline_registry();
    if (!registry)
        return 4;
    auto snapshot = timeline::serialize_project(project.value(), registry.value());
    if (!snapshot)
        return 5;
    auto summary = timeline::peek_project_summary(snapshot->json, registry.value());
    if (!summary || summary->project_id != timeline::ItemId{1} ||
        summary->counts.sequences != 1)
        return 6;

    const std::array tempo_points{
        timebase::TempoPoint{timebase::TickPosition{0}, 120.0},
    };
    auto tempo = timebase::CompiledTempoMap::compile(tempo_points, {48'000, 1});
    if (!tempo)
        return 7;
    playback::MasterTransport transport;
    if (transport.prepare(tempo.value(), {.max_buffer_size = 128}) !=
        playback::TransportError::None)
        return 8;
    return 0;
}
