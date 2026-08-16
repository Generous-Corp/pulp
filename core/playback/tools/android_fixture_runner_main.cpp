#include <pulp/playback/audio_renderer.hpp>
#include <pulp/playback/note_renderer.hpp>
#include <pulp/playback/program_compiler.hpp>
#include <pulp/timeline/document_session.hpp>
#include <pulp/timeline/file_journal.hpp>

#include <array>
#include <bit>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

using namespace pulp;
using namespace pulp::playback;
using namespace pulp::timebase;
using namespace pulp::timeline;

namespace {

[[noreturn]] void fail(std::string_view message) {
    std::fprintf(stderr, "FAIL android-playback-fixtures: %.*s\n", static_cast<int>(message.size()),
                 message.data());
    std::exit(1);
}

template <typename T, typename E> T take(runtime::Result<T, E> result, std::string_view context) {
    if (!result)
        fail(context);
    return std::move(result).value();
}

std::shared_ptr<const CompiledTempoMap> tempo_map() {
    const std::array points{TempoPoint{{0}, 120.0}};
    return std::make_shared<const CompiledTempoMap>(
        take(CompiledTempoMap::compile(points, {48'000, 1}), "tempo-map compile"));
}

TickPosition tick_at_sample(const CompiledTempoMap& map, std::int64_t sample) {
    return map.samples_to_ticks({sample});
}

Project make_checkpoint(const CompiledTempoMap& map) {
    auto audio_clip = take(Clip::create({11}, {0}, TickDuration{tick_at_sample(map, 24).value},
                                        MediaRef{{30}, {0}, 24}),
                           "audio clip creation");
    auto audio_track =
        take(Track::create({10}, "audio", {std::move(audio_clip)}), "audio track creation");

    const auto note_start = tick_at_sample(map, 5);
    const auto note_end = tick_at_sample(map, 19);
    auto notes =
        take(MidiContent::create({{{22}, note_start, note_end - note_start, 0x8000, 64, 2}}),
             "MIDI content creation");
    auto note_clip =
        take(Clip::create({21}, {0}, TickDuration{tick_at_sample(map, 32).value}, std::move(notes)),
             "note clip creation");
    auto note_track =
        take(Track::create({20}, "notes", {std::move(note_clip)}), "note track creation");

    auto sequence = take(Sequence::create({2}, "root", TickDuration{tick_at_sample(map, 32).value},
                                          {std::move(audio_track), std::move(note_track)}),
                         "sequence creation");
    const auto content_hash = ContentHash::from_hex(std::string(64, 'a'));
    if (!content_hash)
        fail("content hash creation");
    MediaAsset asset{
        {30}, "golden.wav", 24, {48'000, 1}, *content_hash, AssetStoragePolicy::External,
        {},   {},           {}};
    return take(Project::create(ProjectInput{.id = {1},
                                             .name = "android fixture",
                                             .next_item_id = 31,
                                             .root_sequence_id = {2},
                                             .assets = {std::move(asset)},
                                             .sequences = {std::move(sequence)},
                                             .tempo_map = {},
                                             .meter_map = {},
                                             .session_start = {},
                                             .tuning = {}}),
                "project creation");
}

std::shared_ptr<const DecodedAudioAssetPool> make_audio_pool() {
    auto data = std::make_shared<audio::AudioFileData>();
    data->sample_rate = 48'000;
    data->channels.emplace_back(24, 1.0f);
    return take(
        DecodedAudioAssetPool::create({DecodedAudioAsset{
            .id = {30}, .audio = std::move(data), .content_hash = {}, .decoded_content_hash = {}}}),
        "decoded audio pool creation");
}

void append_u32(std::vector<std::uint8_t>& output, std::uint32_t value) {
    for (unsigned shift = 0; shift != 32; shift += 8)
        output.push_back(static_cast<std::uint8_t>(value >> shift));
}

std::string hex(std::span<const std::uint8_t> bytes) {
    constexpr char digits[] = "0123456789abcdef";
    std::string result;
    result.reserve(bytes.size() * 2);
    for (const auto byte : bytes) {
        result.push_back(digits[byte >> 4]);
        result.push_back(digits[byte & 0x0f]);
    }
    return result;
}

std::string read_text(const std::string& path) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream.good())
        fail("cannot read golden fixture");
    std::string value((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
    while (!value.empty() && (value.back() == '\n' || value.back() == '\r'))
        value.pop_back();
    return value;
}

void remove_journal_files(const std::filesystem::path& journal_path) {
    std::error_code ignored;
    std::filesystem::remove(journal_path, ignored);
    std::filesystem::remove(journal_path.string() + ".lock", ignored);
}

std::vector<std::uint8_t> render_trace(std::shared_ptr<const Project> project,
                                       const std::shared_ptr<const CompiledTempoMap>& map,
                                       const std::shared_ptr<const DecodedAudioAssetPool>& assets) {
    PlaybackProgramStore store;
    DeferredCompileExecutor executor;
    PlaybackProgramCompiler compiler(store, executor, std::chrono::microseconds(0));
    ProgramCompileRequest request;
    request.project = std::move(project);
    request.sequence_id = {2};
    request.tempo_map = map;
    request.sample_rate = map->sample_rate();
    request.document_revision = 1;
    request.dirty.all = true;
    request.audio_assets = assets;
    if (!compiler.submit(std::move(request)))
        fail("program compile submission");
    while (compiler.status().busy)
        executor.run_for(std::chrono::seconds(1), 64);
    if (compiler.status().has_error || !store.has_value())
        fail("program compilation");

    MasterTransport transport;
    if (transport.prepare(*map, {.max_buffer_size = 9, .initially_playing = true}) !=
        TransportError::None)
        fail("transport preparation");
    ArrangementNoteRenderer notes({20});
    if (!notes.prepare(8))
        fail("note renderer preparation");
    PlaybackProgramBlockLatch latch;

    std::vector<std::uint8_t> bytes;
    std::uint32_t absolute_sample = 0;
    constexpr std::array schedule{3u, 7u, 5u, 9u, 8u};
    for (const auto frames : schedule) {
        TransportSnapshot snapshot;
        if (transport.begin_block(frames, snapshot) != TransportError::None)
            fail("transport block");
        auto block = latch.begin_block(store);
        if (!block)
            fail("program adoption");

        std::vector<float> samples(frames, -99.0f);
        float* channel = samples.data();
        audio::BufferView<float> output(&channel, 1, frames);
        if (ArrangementAudioRenderer::process(*block.program(), snapshot, output) !=
            AudioRenderStatus::Rendered)
            fail("audio rendering");
        const auto note_result = notes.process(block, snapshot);
        if (note_result.code != NoteRenderCode::Ok)
            fail("note rendering");

        append_u32(bytes, frames);
        for (const auto sample : samples)
            append_u32(bytes, std::bit_cast<std::uint32_t>(sample));
        append_u32(bytes, static_cast<std::uint32_t>(notes.events().size()));
        for (const auto& event : notes.events()) {
            append_u32(bytes, absolute_sample + static_cast<std::uint32_t>(event.sample_offset));
            if (event.size() != 3)
                fail("unexpected MIDI event size");
            bytes.insert(bytes.end(), event.data(), event.data() + event.size());
        }
        absolute_sample += frames;
    }
    return bytes;
}

} // namespace

int main(int argc, char** argv) {
    if (argc != 3 || std::string_view(argv[1]) != "--corpus") {
        std::fputs("usage: pulp-android-fixture-runner --corpus <dir>\n", stderr);
        return 2;
    }

    const auto map = tempo_map();
    const auto checkpoint = make_checkpoint(*map);
    const auto fade_in_ticks = tick_at_sample(*map, 4).value;
    const auto fade_out_ticks = tick_at_sample(*map, 6).value;
    const ClipPlaybackProperties audible{0.25f, static_cast<std::uint64_t>(fade_in_ticks),
                                         static_cast<std::uint64_t>(fade_out_ticks)};
    const auto journal_path = std::filesystem::path(argv[2]) / ".android-fixture-journal.ptlj";
    remove_journal_files(journal_path);

    std::vector<std::uint8_t> committed;
    const auto assets = make_audio_pool();
    {
        auto registry = take(make_builtin_timeline_registry(), "schema registry creation");
        auto opened = take(FileJournal::open(journal_path, checkpoint, std::move(registry)),
                           "file journal creation");
        if (opened.recovered_existing)
            fail("new file journal reported recovered state");
        auto session = take(DocumentSession::create(opened.checkpoint, {}, opened.sink),
                            "document session creation");
        auto writer = take(session->register_writer(), "writer registration");
        Transaction transaction;
        transaction.id = writer.allocate_transaction_id();
        transaction.expected_revision = {};
        transaction.commands = {
            {writer.allocate_command_id(), SetClipPlaybackProperties{{2}, {10}, {11}, {}, audible}},
            {writer.allocate_command_id(), SetNoteVelocity{{2}, {20}, {21}, {22}, 0x8000, 0x6000}},
        };
        if (!session->submit(writer, std::move(transaction)))
            fail("transaction submission");
        committed = render_trace(session->snapshot(), map, assets);
    }

    auto registry = take(make_builtin_timeline_registry(), "recovery schema registry creation");
    auto replayed = take(FileJournal::open(journal_path, checkpoint, std::move(registry)),
                         "file journal recovery");
    if (!replayed.recovered_existing || replayed.repaired_torn_tail ||
        replayed.revision != DocumentRevision{1})
        fail("file journal recovery metadata");
    const auto* replayed_audio =
        replayed.checkpoint.find_sequence({2})->find_track({10})->find_clip({11});
    const auto* replayed_notes =
        replayed.checkpoint.find_sequence({2})->find_track({20})->find_clip({21});
    if (replayed_audio->playback_properties() != audible ||
        std::get<MidiContent>(replayed_notes->content()).notes()[0].velocity != 0x6000)
        fail("journal state recovery");
    std::puts("PASS android-journal-recovery");

    const auto replayed_trace =
        render_trace(std::make_shared<const Project>(replayed.checkpoint), map, assets);
    if (committed != replayed_trace)
        fail("replayed render differs from committed render");

    const auto golden_path = std::string(argv[2]) + "/v1/replay-render.golden";
    if (hex(committed) != read_text(golden_path))
        fail("audio and MIDI stream differs from replay-render golden");
    std::puts("PASS android-playback-render");
    replayed.sink.reset();
    remove_journal_files(journal_path);
    return 0;
}
