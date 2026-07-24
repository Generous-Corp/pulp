#include "../core/timeline/native/file_journal_internal.hpp"
#include "timeline_command_test_helpers.hpp"

#include <pulp/timeline/file_journal.hpp>

#include <catch2/catch_test_macros.hpp>

#if defined(__linux__)
#include "linux_posix_acl_test_helpers.hpp"
#endif

#include <atomic>
#include <chrono>
#include <csignal>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <thread>
#include <vector>

#if defined(_WIN32)
#include <process.h>
#else
#include <sys/wait.h>
#include <unistd.h>
#endif

using namespace timeline_test;

namespace {

class TemporaryJournal {
  public:
    TemporaryJournal() {
        static std::atomic<std::uint64_t> serial{0};
#if defined(_WIN32)
        const auto process = static_cast<std::uint64_t>(_getpid());
#else
        const auto process = static_cast<std::uint64_t>(::getpid());
#endif
        directory = std::filesystem::temp_directory_path() /
                    ("pulp-timeline-journal-" + std::to_string(process) + "-" +
                     std::to_string(serial.fetch_add(1, std::memory_order_relaxed)));
        std::filesystem::create_directories(directory);
        path = directory / "session.ptlj";
    }

    ~TemporaryJournal() {
        std::error_code ignored;
        std::filesystem::remove_all(directory, ignored);
    }

    std::filesystem::path directory;
    std::filesystem::path path;
};

SchemaRegistry builtins() {
    auto registry = make_builtin_timeline_registry();
    REQUIRE(registry);
    return std::move(registry).value();
}

FileJournalOpenResult open_journal(const std::filesystem::path& path, const Project& fallback) {
    auto opened = FileJournal::open(path, fallback, builtins());
    REQUIRE(opened);
    return std::move(opened).value();
}

Project take_project(bool record_armed, std::int64_t placement_start) {
    auto recorded = Take::create({7}, MediaRef{{6}, {0}, 100}, {placement_start}, {48'000, 1});
    REQUIRE(recorded);
    auto lane = TakeLane::create({5}, "takes", {std::move(recorded).value()});
    REQUIRE(lane);
    auto track = Track::create(TrackInput{.id = {4},
                                          .name = "track",
                                          .take_lanes = {std::move(lane).value()},
                                          .record_armed = record_armed});
    REQUIRE(track);
    auto sequence =
        Sequence::create({2}, "sequence", TickDuration{100}, {std::move(track).value()});
    REQUIRE(sequence);
    MediaAsset asset{
        {6}, "audio.wav", 1'000, {48'000, 1}, content_hash(), AssetStoragePolicy::External, {}, {}};
    auto project =
        Project::create({{1}, "recording", 8, {2}, {asset}, {std::move(sequence).value()}});
    REQUIRE(project);
    return std::move(project).value();
}

Project playback_project(float gain_linear) {
    ClipPlaybackProperties playback;
    playback.gain_linear = gain_linear;
    auto track = Track::create({4}, "track", {make_note_clip({5}, {6}, 0, 1000, playback)});
    REQUIRE(track);
    auto sequence = Sequence::create({3}, "sequence", TickDuration{8 * kTicksPerQuarter},
                                     {std::move(track).value()});
    REQUIRE(sequence);
    auto project = Project::create({{1}, "project", 7, {3}, {}, {std::move(sequence).value()}});
    REQUIRE(project);
    return std::move(project).value();
}

#if !defined(_WIN32)
std::atomic<std::size_t> g_stop_after_bytes{0};

void stop_after_partial_frame(std::size_t bytes_written) noexcept {
    if (bytes_written >= g_stop_after_bytes.load(std::memory_order_relaxed))
        ::raise(SIGSTOP);
}
#endif

} // namespace

TEST_CASE("Timeline file journal recovers the last durable revision and continues") {
    TemporaryJournal temporary;
    const auto fallback = make_project();
    {
        auto opened = open_journal(temporary.path, fallback);
        REQUIRE_FALSE(opened.recovered_existing);
        auto session =
            std::move(DocumentSession::create(opened.checkpoint, {}, opened.sink)).value();
        auto writer = std::move(session->register_writer()).value();
        auto edit =
            session_transaction(writer, {}, {SetNoteVelocity{{3}, {4}, {5}, {6}, 1000, 2000}});
        REQUIRE(session->submit(writer, std::move(edit)));
        REQUIRE(session->revision() == DocumentRevision{1});
    }

    {
        auto recovered = open_journal(temporary.path, fallback);
        REQUIRE(recovered.recovered_existing);
        REQUIRE_FALSE(recovered.repaired_torn_tail);
        REQUIRE(recovered.revision == DocumentRevision{1});
        REQUIRE(velocity(recovered.checkpoint) == 2000);
        auto session = std::move(DocumentSession::restore(recovered.checkpoint, recovered.revision,
                                                          {}, recovered.sink))
                           .value();
        REQUIRE(session->revision() == DocumentRevision{1});
        auto writer = std::move(session->register_writer()).value();
        auto edit =
            session_transaction(writer, {1}, {SetNoteVelocity{{3}, {4}, {5}, {6}, 2000, 3000}});
        REQUIRE(session->submit(writer, std::move(edit)));
    }

    const auto recovered = open_journal(temporary.path, fallback);
    REQUIRE(recovered.revision == DocumentRevision{2});
    REQUIRE(velocity(recovered.checkpoint) == 3000);
}

TEST_CASE("Timeline file journal creates a missing parent directory chain") {
    TemporaryJournal temporary;
    const auto nested_path = temporary.directory / "nested" / "session" / "journal.ptlj";

    const auto opened = open_journal(nested_path, make_project());

    REQUIRE(opened.sink);
    REQUIRE(std::filesystem::is_regular_file(nested_path));
}

TEST_CASE("Timeline file journal rejects an existing empty file without resetting it") {
    TemporaryJournal temporary;
    {
        std::ofstream empty(temporary.path, std::ios::binary);
        REQUIRE(empty);
    }

    auto rejected = FileJournal::open(temporary.path, make_project(), builtins());

    REQUIRE_FALSE(rejected);
    REQUIRE(rejected.error().code == FileJournalErrorCode::InvalidFormat);
    REQUIRE(std::filesystem::file_size(temporary.path) == 0);
}

TEST_CASE("Timeline file journal validates restored state without mutating it") {
    TemporaryJournal temporary;
    const auto fallback = make_project();
    {
        auto opened = open_journal(temporary.path, fallback);
        auto session =
            std::move(DocumentSession::create(opened.checkpoint, {}, opened.sink)).value();
        auto writer = std::move(session->register_writer()).value();
        auto edit =
            session_transaction(writer, {}, {SetNoteVelocity{{3}, {4}, {5}, {6}, 1000, 2000}});
        REQUIRE(session->submit(writer, std::move(edit)));
    }

    auto recovered = open_journal(temporary.path, fallback);
    const auto durable_size = std::filesystem::file_size(temporary.path);

    auto stale = DocumentSession::restore(fallback, {}, {}, recovered.sink);
    REQUIRE_FALSE(stale);
    REQUIRE(stale.error().code == ConflictCode::JournalDurability);

    auto mismatched = DocumentSession::restore(fallback, recovered.revision, {}, recovered.sink);
    REQUIRE_FALSE(mismatched);
    REQUIRE(mismatched.error().code == ConflictCode::JournalDurability);

    auto restored =
        DocumentSession::restore(recovered.checkpoint, recovered.revision, {}, recovered.sink);
    REQUIRE(restored);
    REQUIRE(std::filesystem::file_size(temporary.path) == durable_size);
}

TEST_CASE("Timeline file journal restore validation includes recording state") {
    TemporaryJournal temporary;
    const auto durable = take_project(true, 0);
    auto opened = open_journal(temporary.path, durable);

    auto arm_mismatch = DocumentSession::restore(take_project(false, 0), {}, {}, opened.sink);
    REQUIRE_FALSE(arm_mismatch);
    REQUIRE(arm_mismatch.error().code == ConflictCode::JournalDurability);

    auto take_mismatch = DocumentSession::restore(take_project(true, 1), {}, {}, opened.sink);
    REQUIRE_FALSE(take_mismatch);
    REQUIRE(take_mismatch.error().code == ConflictCode::JournalDurability);

    REQUIRE(DocumentSession::restore(durable, {}, {}, opened.sink));
}

TEST_CASE("Timeline file journal restore validation compares canonical float bits") {
    TemporaryJournal temporary;
    const auto durable = playback_project(-0.0f);
    auto opened = open_journal(temporary.path, durable);

    auto mismatch = DocumentSession::restore(playback_project(0.0f), {}, {}, opened.sink);

    REQUIRE_FALSE(mismatch);
    REQUIRE(mismatch.error().code == ConflictCode::JournalDurability);
    REQUIRE(DocumentSession::restore(durable, {}, {}, opened.sink));
}

TEST_CASE("Timeline file journal discards a trailing partial frame") {
    TemporaryJournal temporary;
    const auto fallback = make_project();
    {
        auto opened = open_journal(temporary.path, fallback);
        auto session =
            std::move(DocumentSession::create(opened.checkpoint, {}, opened.sink)).value();
        auto writer = std::move(session->register_writer()).value();
        auto edit =
            session_transaction(writer, {}, {SetNoteVelocity{{3}, {4}, {5}, {6}, 1000, 2000}});
        REQUIRE(session->submit(writer, std::move(edit)));
    }
    const auto valid_size = std::filesystem::file_size(temporary.path);
    {
        std::ofstream output(temporary.path, std::ios::binary | std::ios::app);
        REQUIRE(output);
        output.write("PTLFRAGMENT", 11);
        REQUIRE(output);
    }
    REQUIRE(std::filesystem::file_size(temporary.path) == valid_size + 11);

    const auto recovered = open_journal(temporary.path, fallback);
    REQUIRE(recovered.repaired_torn_tail);
    REQUIRE(recovered.revision == DocumentRevision{1});
    REQUIRE(velocity(recovered.checkpoint) == 2000);
    REQUIRE(std::filesystem::file_size(temporary.path) == valid_size);
}

TEST_CASE("Timeline file journal discards a full-length torn final frame header") {
    TemporaryJournal temporary;
    const auto fallback = make_project();
    {
        auto opened = open_journal(temporary.path, fallback);
        auto session =
            std::move(DocumentSession::create(opened.checkpoint, {}, opened.sink)).value();
        auto writer = std::move(session->register_writer()).value();
        auto edit =
            session_transaction(writer, {}, {SetNoteVelocity{{3}, {4}, {5}, {6}, 1000, 2000}});
        REQUIRE(session->submit(writer, std::move(edit)));
    }
    const auto valid_size = std::filesystem::file_size(temporary.path);
    {
        std::ofstream output(temporary.path, std::ios::binary | std::ios::app);
        REQUIRE(output);
        const std::array<char, 32> torn_header{
            'P', 'T', 'L', 'F', 2, 0, 0, 0, 2, 0, 0, 0, 0, 0, 0, 0,
            1,   0,   0,   0,   0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
        output.write(torn_header.data(), static_cast<std::streamsize>(torn_header.size()));
        REQUIRE(output);
    }

    const auto recovered = open_journal(temporary.path, fallback);
    REQUIRE(recovered.repaired_torn_tail);
    REQUIRE(recovered.revision == DocumentRevision{1});
    REQUIRE(velocity(recovered.checkpoint) == 2000);
    REQUIRE(std::filesystem::file_size(temporary.path) == valid_size);
}

TEST_CASE("Timeline file journal fails closed on corruption before the tail") {
    TemporaryJournal temporary;
    const auto fallback = make_project();
    {
        auto opened = open_journal(temporary.path, fallback);
        auto session =
            std::move(DocumentSession::create(opened.checkpoint, {}, opened.sink)).value();
        auto writer = std::move(session->register_writer()).value();
        auto edit =
            session_transaction(writer, {}, {SetNoteVelocity{{3}, {4}, {5}, {6}, 1000, 2000}});
        REQUIRE(session->submit(writer, std::move(edit)));
    }
    {
        std::fstream file(temporary.path, std::ios::binary | std::ios::in | std::ios::out);
        REQUIRE(file);
        constexpr std::streamoff first_payload = 16 + 32;
        file.seekg(first_payload);
        char byte = 0;
        file.read(&byte, 1);
        REQUIRE(file);
        byte ^= 0x01;
        file.seekp(first_payload);
        file.write(&byte, 1);
        REQUIRE(file);
    }

    auto rejected = FileJournal::open(temporary.path, fallback, builtins());
    REQUIRE_FALSE(rejected);
    REQUIRE(rejected.error().code == FileJournalErrorCode::CorruptRecord);
}

TEST_CASE("Timeline file journal discards a full-length invalid final commit trailer") {
    TemporaryJournal temporary;
    const auto fallback = make_project();
    std::uintmax_t checkpoint_size = 0;
    {
        auto opened = open_journal(temporary.path, fallback);
        checkpoint_size = std::filesystem::file_size(temporary.path);
        auto session =
            std::move(DocumentSession::create(opened.checkpoint, {}, opened.sink)).value();
        auto writer = std::move(session->register_writer()).value();
        auto edit =
            session_transaction(writer, {}, {SetNoteVelocity{{3}, {4}, {5}, {6}, 1000, 2000}});
        REQUIRE(session->submit(writer, std::move(edit)));
    }
    {
        std::fstream file(temporary.path, std::ios::binary | std::ios::in | std::ios::out);
        REQUIRE(file);
        file.seekg(-1, std::ios::end);
        char byte = 0;
        file.read(&byte, 1);
        REQUIRE(file);
        byte ^= 0x01;
        file.seekp(-1, std::ios::end);
        file.write(&byte, 1);
        REQUIRE(file);
    }

    const auto recovered = open_journal(temporary.path, fallback);
    REQUIRE(recovered.repaired_torn_tail);
    REQUIRE(recovered.revision == DocumentRevision{});
    REQUIRE(velocity(recovered.checkpoint) == 1000);
    REQUIRE(std::filesystem::file_size(temporary.path) == checkpoint_size);
}

TEST_CASE("Timeline file journal rejects committed final-frame payload corruption") {
    TemporaryJournal temporary;
    const auto fallback = make_project();
    {
        auto opened = open_journal(temporary.path, fallback);
        auto session =
            std::move(DocumentSession::create(opened.checkpoint, {}, opened.sink)).value();
        auto writer = std::move(session->register_writer()).value();
        auto edit =
            session_transaction(writer, {}, {SetNoteVelocity{{3}, {4}, {5}, {6}, 1000, 2000}});
        REQUIRE(session->submit(writer, std::move(edit)));
    }
    const auto committed_size = std::filesystem::file_size(temporary.path);
    constexpr std::streamoff commit_trailer_bytes = 24;
    {
        std::fstream file(temporary.path, std::ios::binary | std::ios::in | std::ios::out);
        REQUIRE(file);
        file.seekg(-commit_trailer_bytes - 1, std::ios::end);
        char byte = 0;
        file.read(&byte, 1);
        REQUIRE(file);
        byte ^= 0x01;
        file.seekp(-commit_trailer_bytes - 1, std::ios::end);
        file.write(&byte, 1);
        REQUIRE(file);
    }

    auto rejected = FileJournal::open(temporary.path, fallback, builtins());
    REQUIRE_FALSE(rejected);
    REQUIRE(rejected.error().code == FileJournalErrorCode::CorruptRecord);
    REQUIRE(std::filesystem::file_size(temporary.path) == committed_size);
}

TEST_CASE("Timeline file journal rejects a corrupted committed final-frame header") {
    TemporaryJournal temporary;
    const auto fallback = make_project();
    std::uintmax_t revision_one_size = 0;
    {
        auto opened = open_journal(temporary.path, fallback);
        auto session =
            std::move(DocumentSession::create(opened.checkpoint, {}, opened.sink)).value();
        auto writer = std::move(session->register_writer()).value();
        auto first =
            session_transaction(writer, {}, {SetNoteVelocity{{3}, {4}, {5}, {6}, 1000, 2000}});
        REQUIRE(session->submit(writer, std::move(first)));
        revision_one_size = std::filesystem::file_size(temporary.path);
        auto second =
            session_transaction(writer, {1}, {SetNoteVelocity{{3}, {4}, {5}, {6}, 2000, 3000}});
        REQUIRE(session->submit(writer, std::move(second)));
    }
    const auto committed_size = std::filesystem::file_size(temporary.path);
    {
        std::fstream file(temporary.path, std::ios::binary | std::ios::in | std::ios::out);
        REQUIRE(file);
        file.seekg(static_cast<std::streamoff>(revision_one_size + 28));
        char byte = 0;
        file.read(&byte, 1);
        REQUIRE(file);
        byte ^= 0x01;
        file.seekp(static_cast<std::streamoff>(revision_one_size + 28));
        file.write(&byte, 1);
        REQUIRE(file);
    }

    auto rejected = FileJournal::open(temporary.path, fallback, builtins());
    REQUIRE_FALSE(rejected);
    REQUIRE(rejected.error().code == FileJournalErrorCode::CorruptRecord);
    REQUIRE(std::filesystem::file_size(temporary.path) == committed_size);
}

TEST_CASE("Timeline file journal discards a checksum-invalid final frame without a commit marker") {
    TemporaryJournal temporary;
    const auto fallback = make_project();
    std::uintmax_t revision_one_size = 0;
    {
        auto opened = open_journal(temporary.path, fallback);
        auto session =
            std::move(DocumentSession::create(opened.checkpoint, {}, opened.sink)).value();
        auto writer = std::move(session->register_writer()).value();
        auto first =
            session_transaction(writer, {}, {SetNoteVelocity{{3}, {4}, {5}, {6}, 1000, 2000}});
        REQUIRE(session->submit(writer, std::move(first)));
        revision_one_size = std::filesystem::file_size(temporary.path);
        auto second =
            session_transaction(writer, {1}, {SetNoteVelocity{{3}, {4}, {5}, {6}, 2000, 3000}});
        REQUIRE(session->submit(writer, std::move(second)));
    }

    constexpr std::streamoff commit_trailer_bytes = 24;
    {
        std::fstream file(temporary.path, std::ios::binary | std::ios::in | std::ios::out);
        REQUIRE(file);
        file.seekg(-commit_trailer_bytes - 1, std::ios::end);
        char byte = 0;
        file.read(&byte, 1);
        REQUIRE(file);
        byte ^= 0x01;
        file.seekp(-commit_trailer_bytes - 1, std::ios::end);
        file.write(&byte, 1);
        REQUIRE(file);
    }
    const auto uncommitted_size = std::filesystem::file_size(temporary.path) - commit_trailer_bytes;
    std::filesystem::resize_file(temporary.path, uncommitted_size);

    const auto recovered = open_journal(temporary.path, fallback);
    REQUIRE(recovered.repaired_torn_tail);
    REQUIRE(recovered.revision == DocumentRevision{1});
    REQUIRE(velocity(recovered.checkpoint) == 2000);
    REQUIRE(std::filesystem::file_size(temporary.path) == revision_one_size);
}

TEST_CASE("Timeline file journal rejects an invalid commit trailer before a later record") {
    TemporaryJournal temporary;
    const auto fallback = make_project();
    std::uintmax_t revision_one_size = 0;
    std::uintmax_t committed_size = 0;
    {
        auto opened = open_journal(temporary.path, fallback);
        auto session =
            std::move(DocumentSession::create(opened.checkpoint, {}, opened.sink)).value();
        auto writer = std::move(session->register_writer()).value();
        auto first =
            session_transaction(writer, {}, {SetNoteVelocity{{3}, {4}, {5}, {6}, 1000, 2000}});
        REQUIRE(session->submit(writer, std::move(first)));
        revision_one_size = std::filesystem::file_size(temporary.path);
        auto second =
            session_transaction(writer, {1}, {SetNoteVelocity{{3}, {4}, {5}, {6}, 2000, 3000}});
        REQUIRE(session->submit(writer, std::move(second)));
        committed_size = std::filesystem::file_size(temporary.path);
    }
    REQUIRE(committed_size > revision_one_size);
    {
        std::fstream file(temporary.path, std::ios::binary | std::ios::in | std::ios::out);
        REQUIRE(file);
        file.seekg(static_cast<std::streamoff>(revision_one_size - 1));
        char byte = 0;
        file.read(&byte, 1);
        REQUIRE(file);
        byte ^= 0x01;
        file.seekp(static_cast<std::streamoff>(revision_one_size - 1));
        file.write(&byte, 1);
        REQUIRE(file);
    }

    auto rejected = FileJournal::open(temporary.path, fallback, builtins());
    REQUIRE_FALSE(rejected);
    REQUIRE(rejected.error().code == FileJournalErrorCode::CorruptRecord);
    REQUIRE(std::filesystem::file_size(temporary.path) == committed_size);
}

TEST_CASE("Timeline file journal does not misclassify middle-header corruption as a torn tail") {
    TemporaryJournal temporary;
    const auto fallback = make_project();
    {
        auto opened = open_journal(temporary.path, fallback);
        auto session =
            std::move(DocumentSession::create(opened.checkpoint, {}, opened.sink)).value();
        auto writer = std::move(session->register_writer()).value();
        auto first =
            session_transaction(writer, {}, {SetNoteVelocity{{3}, {4}, {5}, {6}, 1000, 2000}});
        REQUIRE(session->submit(writer, std::move(first)));
        auto second =
            session_transaction(writer, {1}, {SetNoteVelocity{{3}, {4}, {5}, {6}, 2000, 3000}});
        REQUIRE(session->submit(writer, std::move(second)));
    }

    std::ifstream input(temporary.path, std::ios::binary | std::ios::ate);
    REQUIRE(input);
    const auto size = input.tellg();
    REQUIRE(size > 0);
    std::vector<char> bytes(static_cast<std::size_t>(size));
    input.seekg(0);
    input.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    REQUIRE(input);
    std::vector<std::size_t> frames;
    for (std::size_t offset = 0; offset + 4 <= bytes.size(); ++offset) {
        if (std::string_view(bytes.data() + offset, 4) == "PTLF")
            frames.push_back(offset);
    }
    REQUIRE(frames.size() >= 3);
    {
        std::fstream file(temporary.path, std::ios::binary | std::ios::in | std::ios::out);
        REQUIRE(file);
        file.seekp(static_cast<std::streamoff>(frames[1]));
        file.put('X');
        REQUIRE(file);
    }

    auto rejected = FileJournal::open(temporary.path, fallback, builtins());
    REQUIRE_FALSE(rejected);
    REQUIRE(rejected.error().code == FileJournalErrorCode::CorruptRecord);
}

TEST_CASE("Timeline file journal checkpoint atomically compacts prior revisions") {
    TemporaryJournal temporary;
    const auto fallback = make_project();
    auto opened = open_journal(temporary.path, fallback);
    auto session = std::move(DocumentSession::create(opened.checkpoint, {}, opened.sink)).value();
    auto writer = std::move(session->register_writer()).value();
    auto edit =
        session_transaction(writer, {}, {SetNoteVelocity{{3}, {4}, {5}, {6}, 1000, 2000}});
    REQUIRE(session->submit(writer, std::move(edit)));
#if !defined(_WIN32)
    constexpr auto preserved_permissions = std::filesystem::perms::owner_read |
                                           std::filesystem::perms::owner_write |
                                           std::filesystem::perms::group_read;
    std::filesystem::permissions(temporary.path, preserved_permissions,
                                 std::filesystem::perm_options::replace);
#if defined(__linux__)
    const auto acl_result = linux_acl_test::install(temporary.path);
    REQUIRE(acl_result != linux_acl_test::InstallResult::Failed);
    const auto expected_acl = acl_result == linux_acl_test::InstallResult::Installed
                                  ? linux_acl_test::read(temporary.path)
                                  : std::nullopt;
#endif
#endif
    const auto before = std::filesystem::file_size(temporary.path);
    REQUIRE(session->checkpoint({1}));
    const auto after = std::filesystem::file_size(temporary.path);
    REQUIRE(after < before);
#if !defined(_WIN32)
    REQUIRE(std::filesystem::status(temporary.path).permissions() ==
            preserved_permissions);
#if defined(__linux__)
    if (expected_acl)
        REQUIRE(linux_acl_test::read(temporary.path) == expected_acl);
#endif
#endif

    session.reset();
    opened.sink.reset();
    const auto recovered = open_journal(temporary.path, fallback);
    REQUIRE(recovered.revision == DocumentRevision{1});
    REQUIRE(velocity(recovered.checkpoint) == 2000);
}

TEST_CASE("Timeline file journal rejects a checkpoint snapshot from another state") {
    TemporaryJournal temporary;
    const auto fallback = make_project();
    auto opened = open_journal(temporary.path, fallback);
    auto session = std::move(DocumentSession::create(opened.checkpoint, {}, opened.sink)).value();
    auto writer = std::move(session->register_writer()).value();
    auto edit = session_transaction(writer, {}, {SetNoteVelocity{{3}, {4}, {5}, {6}, 1000, 2000}});
    REQUIRE(session->submit(writer, std::move(edit)));

    auto rejected = opened.sink->checkpoint(fallback, {1});
    REQUIRE_FALSE(rejected);
    REQUIRE(rejected.error() == JournalSinkError::InvalidState);

    session.reset();
    opened.sink.reset();
    const auto recovered = open_journal(temporary.path, fallback);
    REQUIRE(recovered.revision == DocumentRevision{1});
    REQUIRE(velocity(recovered.checkpoint) == 2000);
}

TEST_CASE("Timeline session creation rejects a sink at a newer durable revision") {
    TemporaryJournal temporary;
    const auto fallback = make_project();
    auto opened = open_journal(temporary.path, fallback);
    {
        auto session =
            std::move(DocumentSession::create(opened.checkpoint, {}, opened.sink)).value();
        auto writer = std::move(session->register_writer()).value();
        auto edit =
            session_transaction(writer, {}, {SetNoteVelocity{{3}, {4}, {5}, {6}, 1000, 2000}});
        REQUIRE(session->submit(writer, std::move(edit)));
    }

    auto rejected = DocumentSession::create(fallback, {}, opened.sink);
    REQUIRE_FALSE(rejected);
    REQUIRE(rejected.error().code == ConflictCode::JournalDurability);
}

TEST_CASE("Timeline file journal preserves newer revisions when checkpointing a prefix") {
    TemporaryJournal temporary;
    const auto fallback = make_project();
    auto opened = open_journal(temporary.path, fallback);
    auto session = std::move(DocumentSession::create(opened.checkpoint, {}, opened.sink)).value();
    auto writer = std::move(session->register_writer()).value();
    auto first = session_transaction(writer, {}, {SetNoteVelocity{{3}, {4}, {5}, {6}, 1000, 2000}});
    REQUIRE(session->submit(writer, std::move(first)));
    auto second =
        session_transaction(writer, {1}, {SetNoteVelocity{{3}, {4}, {5}, {6}, 2000, 3000}});
    REQUIRE(session->submit(writer, std::move(second)));

    REQUIRE(session->checkpoint({1}));
    REQUIRE(session->journal().base_revision() == DocumentRevision{1});
    session.reset();
    opened.sink.reset();

    const auto recovered = open_journal(temporary.path, fallback);
    REQUIRE(recovered.revision == DocumentRevision{2});
    REQUIRE(velocity(recovered.checkpoint) == 3000);
}

TEST_CASE("Timeline file journal replays history entries with tombstone restoration") {
    TemporaryJournal temporary;
    const auto fallback = make_project();
    {
        auto opened = open_journal(temporary.path, fallback);
        auto session =
            std::move(DocumentSession::create(opened.checkpoint, {}, opened.sink)).value();
        auto writer = std::move(session->register_writer()).value();
        auto remove = session_transaction(writer, {}, {RemoveClip{{3}, {4}, {5}}});
        REQUIRE(session->submit(writer, std::move(remove)));
        REQUIRE(session->undo(writer));
        REQUIRE(session->revision() == DocumentRevision{2});
        REQUIRE(velocity(*session->snapshot()) == 1000);
    }

    const auto recovered = open_journal(temporary.path, fallback);
    REQUIRE(recovered.revision == DocumentRevision{2});
    REQUIRE(velocity(recovered.checkpoint) == 1000);
}

TEST_CASE("Timeline file journal holds an exclusive path lock for its lifetime") {
    TemporaryJournal temporary;
    const auto fallback = make_project();
    auto first = open_journal(temporary.path, fallback);

    auto rejected = FileJournal::open(temporary.path, fallback, builtins());
    REQUIRE_FALSE(rejected);
    REQUIRE(rejected.error().code == FileJournalErrorCode::AlreadyOpen);

    first.sink.reset();
    auto reopened = FileJournal::open(temporary.path, fallback, builtins());
    REQUIRE(reopened);
}

TEST_CASE("Timeline file journal lock rejects a hard-link alias") {
    TemporaryJournal temporary;
    const auto fallback = make_project();
    auto first = open_journal(temporary.path, fallback);
    const auto alias = temporary.directory / "alias.ptlj";
    std::filesystem::create_hard_link(temporary.path, alias);

    auto checkpoint_rejected = first.sink->checkpoint(first.checkpoint, {});
    REQUIRE_FALSE(checkpoint_rejected);

    auto rejected = FileJournal::open(alias, fallback, builtins());
    REQUIRE_FALSE(rejected);
    REQUIRE(rejected.error().code == FileJournalErrorCode::AliasedPath);

    first.sink.reset();
    auto original_rejected = FileJournal::open(temporary.path, fallback, builtins());
    REQUIRE_FALSE(original_rejected);
    REQUIRE(original_rejected.error().code == FileJournalErrorCode::AliasedPath);
}

TEST_CASE("Timeline file journal rejects a replaced pathname before mutation") {
    const auto replacement = std::string("replacement");

    {
        TemporaryJournal temporary;
        const auto fallback = make_project();
        auto opened = open_journal(temporary.path, fallback);
        auto session =
            std::move(DocumentSession::create(opened.checkpoint, {}, opened.sink)).value();
        auto writer = std::move(session->register_writer()).value();
        std::filesystem::rename(temporary.path, temporary.directory / "detached.ptlj");
        {
            std::ofstream output(temporary.path, std::ios::binary);
            output << replacement;
            REQUIRE(output);
        }
        auto edit =
            session_transaction(writer, {}, {SetNoteVelocity{{3}, {4}, {5}, {6}, 1000, 2000}});

        auto rejected = session->submit(writer, std::move(edit));

        REQUIRE_FALSE(rejected);
        REQUIRE(rejected.error().code == ConflictCode::JournalDurability);
        std::ifstream input(temporary.path, std::ios::binary);
        REQUIRE(std::string(std::istreambuf_iterator<char>(input), {}) == replacement);
    }

    {
        TemporaryJournal temporary;
        const auto fallback = make_project();
        auto opened = open_journal(temporary.path, fallback);
        std::filesystem::rename(temporary.path, temporary.directory / "detached.ptlj");
        {
            std::ofstream output(temporary.path, std::ios::binary);
            output << replacement;
            REQUIRE(output);
        }

        auto rejected = opened.sink->checkpoint(opened.checkpoint, {});

        REQUIRE_FALSE(rejected);
        std::ifstream input(temporary.path, std::ios::binary);
        REQUIRE(std::string(std::istreambuf_iterator<char>(input), {}) == replacement);
    }

    {
        TemporaryJournal temporary;
        const auto fallback = make_project();
        auto opened = open_journal(temporary.path, fallback);
        auto session =
            std::move(DocumentSession::create(opened.checkpoint, {}, opened.sink)).value();
        auto writer = std::move(session->register_writer()).value();
        auto first =
            session_transaction(writer, {}, {SetNoteVelocity{{3}, {4}, {5}, {6}, 1000, 2000}});
        REQUIRE(session->submit(writer, std::move(first)));
        auto second =
            session_transaction(writer, {1}, {SetNoteVelocity{{3}, {4}, {5}, {6}, 2000, 3000}});
        REQUIRE(session->submit(writer, std::move(second)));
        std::filesystem::rename(temporary.path, temporary.directory / "detached.ptlj");
        {
            std::ofstream output(temporary.path, std::ios::binary);
            output << replacement;
            REQUIRE(output);
        }

        REQUIRE_FALSE(session->checkpoint({1}));
        std::ifstream input(temporary.path, std::ios::binary);
        REQUIRE(std::string(std::istreambuf_iterator<char>(input), {}) == replacement);
    }
}

#if !defined(_WIN32)
TEST_CASE("Timeline file journal lock canonicalizes a symbolic-link alias") {
    TemporaryJournal temporary;
    const auto fallback = make_project();
    auto first = open_journal(temporary.path, fallback);
    const auto symbolic_alias = temporary.directory / "symbolic-alias.ptlj";
    std::filesystem::create_symlink(temporary.path.filename(), symbolic_alias);

    auto symbolic_rejected = FileJournal::open(symbolic_alias, fallback, builtins());

    REQUIRE_FALSE(symbolic_rejected);
    REQUIRE(symbolic_rejected.error().code == FileJournalErrorCode::AlreadyOpen);
}

TEST_CASE("Timeline file journal initializes the target of a dangling symbolic link") {
    TemporaryJournal temporary;
    const auto fallback = make_project();
    const auto target = temporary.directory / "target.ptlj";
    const auto symbolic_alias = temporary.directory / "dangling-alias.ptlj";
    std::filesystem::create_symlink(target.filename(), symbolic_alias);
    REQUIRE(std::filesystem::is_symlink(
        std::filesystem::symlink_status(symbolic_alias)));
    REQUIRE_FALSE(std::filesystem::exists(target));

    auto opened = FileJournal::open(symbolic_alias, fallback, builtins());

    REQUIRE(opened);
    REQUIRE_FALSE(opened.value().recovered_existing);
    REQUIRE(std::filesystem::is_symlink(
        std::filesystem::symlink_status(symbolic_alias)));
    REQUIRE(std::filesystem::exists(target));
    auto target_rejected = FileJournal::open(target, fallback, builtins());
    REQUIRE_FALSE(target_rejected);
    REQUIRE(target_rejected.error().code == FileJournalErrorCode::AlreadyOpen);
}
#endif

#if !defined(_WIN32)
TEST_CASE("Timeline file journal recovers after SIGKILL in the middle of a frame") {
    for (const auto stop_after : {std::size_t{8}, std::size_t{96}}) {
        TemporaryJournal temporary;
        const auto fallback = make_project();
        {
            auto opened = open_journal(temporary.path, fallback);
            auto session =
                std::move(DocumentSession::create(opened.checkpoint, {}, opened.sink)).value();
            auto writer = std::move(session->register_writer()).value();
            auto edit =
                session_transaction(writer, {}, {SetNoteVelocity{{3}, {4}, {5}, {6}, 1000, 2000}});
            REQUIRE(session->submit(writer, std::move(edit)));
        }

        g_stop_after_bytes.store(stop_after, std::memory_order_relaxed);
        const auto child = ::fork();
        REQUIRE(child >= 0);
        if (child == 0) {
            auto registry = make_builtin_timeline_registry();
            if (!registry)
                _exit(10);
            auto opened = FileJournal::open(temporary.path, fallback, std::move(registry).value());
            if (!opened)
                _exit(11);
            auto recovered = std::move(opened).value();
            auto restored = DocumentSession::restore(recovered.checkpoint, recovered.revision, {},
                                                     recovered.sink);
            if (!restored)
                _exit(12);
            auto session = std::move(restored).value();
            auto registered = session->register_writer();
            if (!registered)
                _exit(13);
            auto writer = std::move(registered).value();
            auto edit =
                session_transaction(writer, {1}, {SetNoteVelocity{{3}, {4}, {5}, {6}, 2000, 3000}});
            pulp::timeline::detail::FileJournalTestAccess::set_write_hook(
                8, &stop_after_partial_frame);
            (void)session->submit(writer, std::move(edit));
            _exit(14);
        }

        int status = 0;
        bool stopped = false;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (std::chrono::steady_clock::now() < deadline) {
            const auto waited = ::waitpid(child, &status, WNOHANG | WUNTRACED);
            if (waited == child) {
                stopped = WIFSTOPPED(status);
                break;
            }
            REQUIRE(waited >= 0);
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        if (!stopped) {
            (void)::kill(child, SIGKILL);
            (void)::waitpid(child, &status, 0);
        }
        REQUIRE(stopped);
        REQUIRE(::kill(child, SIGKILL) == 0);
        REQUIRE(::waitpid(child, &status, 0) == child);
        REQUIRE(WIFSIGNALED(status));
        REQUIRE(WTERMSIG(status) == SIGKILL);

        const auto recovered = open_journal(temporary.path, fallback);
        REQUIRE(recovered.repaired_torn_tail);
        REQUIRE(recovered.revision == DocumentRevision{1});
        REQUIRE(velocity(recovered.checkpoint) == 2000);
    }
}
#endif
