#include <pulp/platform/child_process.hpp>
#include <pulp/project_package/atomic_publisher.hpp>
#include <pulp/project_package/project_package.hpp>
#include <pulp/runtime/crypto.hpp>
#include <pulp/timeline/model.hpp>
#include <pulp/timeline/schema_registry.hpp>
#include <pulp/timeline/serialize.hpp>

#include "native_io.hpp"
#include "project_package_test_access.hpp"

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <span>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <process.h>
#include <windows.h>
#else
#include <csignal>
#if defined(__APPLE__)
#include <membership.h>
#include <sys/acl.h>
#endif
#include <sys/stat.h>
#include <unistd.h>
#endif

#ifndef PULP_PROJECT_PACKAGE_PUBLISH_HELPER
#error "PULP_PROJECT_PACKAGE_PUBLISH_HELPER must name the crash helper"
#endif

namespace {

using namespace pulp::project_package;
namespace fs = std::filesystem;

fs::path g_swap_after_blob_verification;
fs::path g_blob_swap_source;
fs::path g_append_during_hash;
fs::path g_switch_current_path;
fs::path g_remove_after_reference_set;
fs::path g_rebind_source;
fs::path g_rebind_displaced;
fs::path g_rebind_replacement_file;
pulp::project_package::detail::PackageFaultPoint g_rebind_point =
    pulp::project_package::detail::PackageFaultPoint::DirectoryPublished;
std::atomic<std::uint64_t> g_blob_verifications{0};

void swap_verified_blob(pulp::project_package::detail::PackageFaultPoint point) noexcept {
    if (point != pulp::project_package::detail::PackageFaultPoint::ExistingBlobVerified &&
        point != pulp::project_package::detail::PackageFaultPoint::BlobReferenceVerified)
        return;
    std::error_code ignored;
    fs::remove(g_swap_after_blob_verification, ignored);
    fs::rename(g_blob_swap_source, g_swap_after_blob_verification, ignored);
}

void count_blob_verification(pulp::project_package::detail::PackageFaultPoint point) noexcept {
    if (point == pulp::project_package::detail::PackageFaultPoint::BlobReferenceVerified)
        g_blob_verifications.fetch_add(1, std::memory_order_relaxed);
}

void append_during_blob_hash(pulp::project_package::detail::PackageFaultPoint point) noexcept {
    if (point != pulp::project_package::detail::PackageFaultPoint::BlobHashSnapshot)
        return;
    std::ofstream output(g_append_during_hash, std::ios::binary | std::ios::app);
    output << "suffix";
}

void switch_current_path_after_directory_publish(
    pulp::project_package::detail::PackageFaultPoint point) noexcept {
    if (point != pulp::project_package::detail::PackageFaultPoint::DirectoryPublished)
        return;
    std::error_code ignored;
    fs::current_path(g_switch_current_path, ignored);
}

void remove_after_reference_set(pulp::project_package::detail::PackageFaultPoint point) noexcept {
    if (point != pulp::project_package::detail::PackageFaultPoint::ReferenceSetVerified)
        return;
    std::error_code ignored;
    fs::remove(g_remove_after_reference_set, ignored);
}

void rebind_publication_source(pulp::project_package::detail::PackageFaultPoint point) noexcept {
    if (point != g_rebind_point)
        return;
    std::error_code ignored;
    fs::rename(g_rebind_source, g_rebind_displaced, ignored);
    if (g_rebind_replacement_file.empty()) {
        fs::create_directory(g_rebind_source, ignored);
        std::ofstream(g_rebind_source / "replacement.txt") << "replacement";
    } else {
        std::ofstream(g_rebind_replacement_file) << "replacement";
    }
}

class TemporaryPackage {
  public:
    explicit TemporaryPackage(std::string_view label) {
        static std::atomic<std::uint64_t> serial{0};
#if defined(_WIN32)
        const auto process = static_cast<std::uint64_t>(_getpid());
#else
        const auto process = static_cast<std::uint64_t>(::getpid());
#endif
        path = fs::temp_directory_path() /
               ("pulp-project-package-" + std::string(label) + "-" + std::to_string(process) + "-" +
                std::to_string(serial.fetch_add(1, std::memory_order_relaxed)));
        std::error_code error;
        fs::remove_all(path, error);
    }

    ~TemporaryPackage() {
        std::error_code error;
        fs::remove_all(path, error);
    }

    fs::path path;
};

pulp::timeline::SchemaRegistry registry() {
    auto value = pulp::timeline::make_builtin_timeline_registry();
    REQUIRE(value);
    return std::move(value).value();
}

pulp::timeline::ContentHash hash_bytes(std::span<const std::uint8_t> bytes) {
    const auto encoded = pulp::runtime::sha256_hex(bytes.data(), bytes.size());
    const auto hash = pulp::timeline::ContentHash::from_hex(encoded);
    REQUIRE(hash);
    return *hash;
}

pulp::timeline::Project make_project(std::string project_name, std::string asset_name,
                                     const pulp::timeline::ContentHash& hash) {
    using namespace pulp::timeline;
    MediaAsset asset{
        {2}, std::move(asset_name), 4, {48'000, 1}, hash, AssetStoragePolicy::Embedded, {}, {}, {}};
    auto sequence = Sequence::create({3}, "sequence", pulp::timebase::TickDuration{0}, {});
    REQUIRE(sequence);
    auto project = Project::create(ProjectInput{.id = {1},
                                                .name = std::move(project_name),
                                                .next_item_id = 4,
                                                .root_sequence_id = {3},
                                                .assets = {std::move(asset)},
                                                .sequences = {std::move(sequence).value()},
                                                .tempo_map = {},
                                                .meter_map = {},
                                                .session_start = std::nullopt});
    REQUIRE(project);
    return std::move(project).value();
}

pulp::timeline::Project
make_repeated_reference_project(const pulp::timeline::ContentHash& first_hash,
                                const pulp::timeline::ContentHash& second_hash) {
    using namespace pulp::timeline;
    const auto canonical_locator = [](const ContentHash& hash) {
        return AssetLocator{AssetLocatorKind::PackageRelative, "media/" + hash.to_hex()};
    };
    MediaAsset first{{2},
                     "first",
                     4,
                     {48'000, 1},
                     first_hash,
                     AssetStoragePolicy::Embedded,
                     {canonical_locator(first_hash), canonical_locator(first_hash)},
                     {},
                     {}};
    MediaAsset second{{4},
                      "second",
                      4,
                      {48'000, 1},
                      second_hash,
                      AssetStoragePolicy::Embedded,
                      {canonical_locator(second_hash)},
                      {},
                      {}};
    auto sequence = Sequence::create({3}, "sequence", pulp::timebase::TickDuration{0}, {});
    REQUIRE(sequence);
    auto project = Project::create(ProjectInput{.id = {1},
                                                .name = "repeated references",
                                                .next_item_id = 5,
                                                .root_sequence_id = {3},
                                                .assets = {std::move(first), std::move(second)},
                                                .sequences = {std::move(sequence).value()},
                                                .tempo_map = {},
                                                .meter_map = {},
                                                .session_start = std::nullopt});
    REQUIRE(project);
    return std::move(project).value();
}

std::string canonical_project(const pulp::timeline::Project& project,
                              const pulp::timeline::SchemaRegistry& schema) {
    const auto encoded = pulp::timeline::serialize_project(project, schema);
    REQUIRE(encoded);
    return encoded->json;
}

std::string read_text(const fs::path& path) {
    std::ifstream input(path, std::ios::binary);
    REQUIRE(input);
    return {std::istreambuf_iterator<char>(input), {}};
}

void publish_baseline(const fs::path& root, const pulp::timeline::ContentHash& hash,
                      std::span<const std::uint8_t> bytes, const pulp::timeline::Project& project) {
    auto writer = PackageWriter::create(root, registry());
    REQUIRE(writer);
    const auto staged = writer.value().stage_blob(BlobStore::Media, hash, bytes);
    REQUIRE(staged);
    REQUIRE(staged.value() == BlobReference{BlobStore::Media, hash});
    const auto outcome = writer.value().publish(project);
    REQUIRE(outcome);
    REQUIRE(outcome.value() == AtomicPublishOutcome::PublishedDurably);
}

bool hard_kill(int process_id) {
#if defined(_WIN32)
    const auto process =
        ::OpenProcess(PROCESS_TERMINATE | SYNCHRONIZE, FALSE, static_cast<DWORD>(process_id));
    if (process == nullptr)
        return false;
    const bool killed = ::TerminateProcess(process, 137) != 0;
    ::CloseHandle(process);
    return killed;
#else
    return ::kill(static_cast<pid_t>(process_id), SIGKILL) == 0;
#endif
}

bool wait_for_ready_or_exit(pulp::platform::ChildProcess& child, const fs::path& ready) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (std::chrono::steady_clock::now() < deadline) {
        std::error_code error;
        if (fs::is_regular_file(ready, error) && !error)
            return true;
        if (!child.is_running())
            return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return false;
}

void require_visible_media_is_valid(const fs::path& root, const OpenPackageResult& opened,
                                    std::span<const std::uint8_t> expected) {
    REQUIRE(opened.project.assets().size() == 1);
    const auto& asset = opened.project.assets()[0];
    const BlobReference reference{BlobStore::Media, asset.content_hash};
    const auto bytes = read_blob(root, reference, 1024);
    REQUIRE(bytes);
    REQUIRE(bytes.value() == std::vector<std::uint8_t>(expected.begin(), expected.end()));
    REQUIRE(hash_bytes(bytes.value()) == asset.content_hash);
}

} // namespace

TEST_CASE("Project package stages content by verified hash and round trips its generation",
          "[project-package]") {
    TemporaryPackage temporary("roundtrip");
    const std::vector<std::uint8_t> media{'o', 'l', 'd', '\n'};
    const auto hash = hash_bytes(media);
    const auto project = make_project("roundtrip", "media", hash);
    const auto schema = registry();

    publish_baseline(temporary.path, hash, media, project);

    const BlobStore extra_stores[] = {BlobStore::State, BlobStore::Artifact, BlobStore::Receipt};
    const std::vector<std::vector<std::uint8_t>> extra_bytes = {
        {'s', 't', 'a', 't', 'e'}, {'a', 'r', 't', 'i', 'f', 'a', 'c', 't'}, {'{', '}', '\n'}};
    std::vector<BlobReference> extra_references;
    {
        auto writer = PackageWriter::create(temporary.path, registry());
        REQUIRE(writer);
        for (std::size_t index = 0; index < extra_bytes.size(); ++index) {
            const auto extra_hash = hash_bytes(extra_bytes[index]);
            const auto staged =
                writer.value().stage_blob(extra_stores[index], extra_hash, extra_bytes[index]);
            REQUIRE(staged);
            extra_references.push_back(staged.value());
        }
    }

    const auto opened = open_package(temporary.path, schema);
    REQUIRE(opened);
    REQUIRE(canonical_project(opened.value().project, schema) ==
            canonical_project(project, schema));
    const auto canonical_root = fs::canonical(temporary.path);
    REQUIRE(opened.value().journal_directory == canonical_root / "journal");
    REQUIRE(opened.value().cache_directory == canonical_root / "cache");
    require_visible_media_is_valid(temporary.path, opened.value(), media);
    REQUIRE(read_text(temporary.path / "project.json") == canonical_project(project, schema));
    for (std::size_t index = 0; index < extra_references.size(); ++index) {
        const auto stored = read_blob(temporary.path, extra_references[index], 1024);
        REQUIRE(stored);
        REQUIRE(stored.value() == extra_bytes[index]);
    }
}

TEST_CASE("Project package rejects wrong hashes, unsafe reads, conflicts, and size limits",
          "[project-package][errors]") {
    TemporaryPackage temporary("errors");
    TemporaryPackage mismatch_package("hash-mismatch");
    const std::vector<std::uint8_t> bytes{'d', 'a', 't', 'a'};
    const auto correct = hash_bytes(bytes);
    const auto wrong = *pulp::timeline::ContentHash::from_hex(std::string(64, 'a'));
    auto writer = PackageWriter::create(
        temporary.path, registry(), PackageLimits{.max_blob_bytes = 3, .max_project_bytes = 1024});
    REQUIRE(writer);
    const auto lock_conflict = PackageWriter::create(temporary.path, registry());
    REQUIRE_FALSE(lock_conflict);
    REQUIRE(lock_conflict.error().code == PackageErrorCode::AlreadyOpen);

    const auto too_large = writer.value().stage_blob(BlobStore::Media, correct, bytes);
    REQUIRE_FALSE(too_large);
    REQUIRE(too_large.error().code == PackageErrorCode::LimitExceeded);

    auto normal = PackageWriter::create(mismatch_package.path, registry());
    REQUIRE(normal);
    const auto mismatch = normal.value().stage_blob(BlobStore::Media, wrong, bytes);
    REQUIRE_FALSE(mismatch);
    REQUIRE(mismatch.error().code == PackageErrorCode::HashMismatch);

    const auto missing = open_package(temporary.path / "absent", registry());
    REQUIRE_FALSE(missing);
    const auto invalid_hash = read_blob(temporary.path, {BlobStore::Media, {}}, 1024);
    REQUIRE_FALSE(invalid_hash);
}

TEST_CASE("Project package treats cache loss as recoverable derived-state loss",
          "[project-package][recovery]") {
    TemporaryPackage temporary("cache");
    const std::vector<std::uint8_t> media{'c', 'a', 'c', 'h', 'e'};
    const auto hash = hash_bytes(media);
    const auto project = make_project("cache", "cache-media", hash);
    const auto schema = registry();
    publish_baseline(temporary.path, hash, media, project);
    REQUIRE(fs::remove_all(temporary.path / "cache") > 0);

    const auto opened = open_package(temporary.path, schema);
    REQUIRE(opened);
    REQUIRE(opened.value().cache_recreated);
    REQUIRE(fs::is_directory(opened.value().cache_directory));
    REQUIRE(canonical_project(opened.value().project, schema) ==
            canonical_project(project, schema));
    require_visible_media_is_valid(temporary.path, opened.value(), media);
}

TEST_CASE("Project package recovery removes only package-owned staging prefixes",
          "[project-package][recovery][staging]") {
    TemporaryPackage temporary("staging");
    const std::vector<std::uint8_t> media{'s', 't', 'a', 'g', 'e'};
    const auto hash = hash_bytes(media);
    publish_baseline(temporary.path, hash, media, make_project("staging", "staging-media", hash));

    const fs::path owned_stages[] = {temporary.path / ".pulp-stage-generation",
                                     temporary.path / "media" / ".pulp-stage-media",
                                     temporary.path / "state" / ".pulp-stage-state"};
    const fs::path preserved[] = {temporary.path / ".pulp-staging-user",
                                  temporary.path / "media" / "user-stage-note"};
    for (const auto& path : owned_stages) {
        std::ofstream output(path, std::ios::binary);
        REQUIRE(output);
        output << "abandoned";
    }
    for (const auto& path : preserved) {
        std::ofstream output(path, std::ios::binary);
        REQUIRE(output);
        output << "preserve";
    }

    auto recovered = PackageWriter::create(temporary.path, registry());
    REQUIRE(recovered);
    REQUIRE(recovered.value().recovered_staging());
    for (const auto& path : owned_stages)
        REQUIRE_FALSE(fs::exists(path));
    for (const auto& path : preserved)
        REQUIRE(fs::is_regular_file(path));
}

TEST_CASE("Package writer rejects a root pathname rebound away from its lock",
          "[project-package][root][race]") {
    TemporaryPackage temporary("writer-root-rebind");
    auto writer = PackageWriter::create(temporary.path, registry());
    REQUIRE(writer);
    const auto displaced = temporary.path.parent_path() /
                           (temporary.path.filename().string() + "-displaced");
    fs::rename(temporary.path, displaced);
    fs::create_directories(temporary.path / "media");
    const std::vector<std::uint8_t> bytes{'p', 'i', 'n'};

    const auto staged = writer->stage_blob(BlobStore::Media, hash_bytes(bytes), bytes);

    REQUIRE_FALSE(staged);
    REQUIRE(staged.error().code == PackageErrorCode::InvalidLayout);
    REQUIRE(fs::is_empty(temporary.path / "media"));
    std::error_code ignored;
    fs::remove_all(displaced, ignored);
}

TEST_CASE("Generation publication retains verified blob identities until replacement",
          "[project-package][references][race]") {
    TemporaryPackage temporary("reference-pin");
    const std::vector<std::uint8_t> media{'p', 'i', 'n', 'n', 'e', 'd'};
    const auto hash = hash_bytes(media);
    auto writer = PackageWriter::create(temporary.path, registry());
    REQUIRE(writer);
    REQUIRE(writer->stage_blob(BlobStore::Media, hash, media));
    g_remove_after_reference_set = temporary.path / "media" / hash.to_hex();
    pulp::project_package::detail::ProjectPackageTestAccess::set_fault_hook(
        remove_after_reference_set);

    const auto published = writer->publish(make_project("pin", "media", hash));

    pulp::project_package::detail::ProjectPackageTestAccess::clear_fault_hook();
    g_remove_after_reference_set.clear();
    REQUIRE_FALSE(published);
    REQUIRE(published.error().code == PackageErrorCode::InvalidGeneration);
    REQUIRE_FALSE(fs::exists(temporary.path / "project.json"));
}

TEST_CASE("Project package readers observe one complete generation during publication",
          "[project-package][concurrency]") {
    TemporaryPackage temporary("concurrent-read");
    const std::vector<std::uint8_t> first_media{'f', 'i', 'r', 's', 't'};
    const std::vector<std::uint8_t> second_media{'s', 'e', 'c', 'o', 'n', 'd'};
    const auto first_hash = hash_bytes(first_media);
    const auto second_hash = hash_bytes(second_media);
    const auto first_project = make_project("a", "first", first_hash);
    const auto second_project =
        make_project(std::string(512, 'b'), "second-with-a-longer-name", second_hash);
    const auto schema = registry();
    const auto first_json = canonical_project(first_project, schema);
    const auto second_json = canonical_project(second_project, schema);
    REQUIRE(first_json.size() != second_json.size());

    auto writer = PackageWriter::create(temporary.path, registry());
    REQUIRE(writer);
    REQUIRE(writer.value().stage_blob(BlobStore::Media, first_hash, first_media));
    REQUIRE(writer.value().stage_blob(BlobStore::Media, second_hash, second_media));
    const auto initial = writer.value().publish(first_project);
    REQUIRE(initial);
    REQUIRE(initial.value() == AtomicPublishOutcome::PublishedDurably);

    std::atomic<bool> start{false};
    std::atomic<bool> publisher_done{false};
    std::atomic<int> reader_error{0};
    std::atomic<std::size_t> reads{0};
    std::optional<PackageError> reader_package_error;
    const auto reader_schema = registry();
    std::thread reader([&] {
        while (!start.load(std::memory_order_acquire))
            std::this_thread::yield();
        do {
            const auto opened = open_package(temporary.path, reader_schema);
            if (!opened) {
                reader_package_error = opened.error();
                reader_error.store(100 + static_cast<int>(opened.error().code),
                                   std::memory_order_release);
                break;
            }
            const auto serialized =
                pulp::timeline::serialize_project(opened.value().project, reader_schema);
            if (!serialized) {
                reader_error.store(2, std::memory_order_release);
                break;
            }
            const bool saw_first = serialized->json == first_json;
            const bool saw_second = serialized->json == second_json;
            if (!saw_first && !saw_second) {
                reader_error.store(3, std::memory_order_release);
                break;
            }
            if (opened.value().project.assets().size() != 1) {
                reader_error.store(4, std::memory_order_release);
                break;
            }
            const auto& asset = opened.value().project.assets()[0];
            const auto expected_hash = saw_first ? first_hash : second_hash;
            const auto& expected_bytes = saw_first ? first_media : second_media;
            const auto bytes =
                read_blob(temporary.path, {BlobStore::Media, asset.content_hash}, 1024);
            if (!bytes || asset.content_hash != expected_hash || bytes.value() != expected_bytes ||
                pulp::runtime::sha256_hex(bytes->data(), bytes->size()) !=
                    asset.content_hash.to_hex()) {
                reader_error.store(5, std::memory_order_release);
                break;
            }
            reads.fetch_add(1, std::memory_order_relaxed);
        } while (!publisher_done.load(std::memory_order_acquire) ||
                 reads.load(std::memory_order_relaxed) < 32);
    });

    start.store(true, std::memory_order_release);
    for (std::size_t generation = 0; generation < 32; ++generation) {
        const auto published =
            writer.value().publish((generation % 2) == 0 ? second_project : first_project);
        REQUIRE(published);
        REQUIRE(published.value() == AtomicPublishOutcome::PublishedDurably);
    }
    publisher_done.store(true, std::memory_order_release);
    reader.join();

    if (reader_package_error) {
        INFO("concurrent open PackageErrorCode=" << static_cast<int>(reader_package_error->code)
                                                 << " path="
                                                 << reader_package_error->path.string());
    }
    REQUIRE(reader_error.load(std::memory_order_acquire) == 0);
    REQUIRE(reads.load(std::memory_order_relaxed) >= 32);
}

TEST_CASE("Project package publication remains an old or complete new generation after hard kill",
          "[project-package][crash-recovery]") {
    struct Point {
        const char* name;
        bool new_generation_visible;
    };
    constexpr Point points[] = {
        {"staged-file-written", false}, {"staged-file-fenced", false},
        {"blob-published", false},      {"blob-directory-fenced", false},
        {"generation-written", false},  {"generation-fenced", false},
        {"generation-published", true}, {"generation-directory-fenced", true},
    };

    for (const auto& point : points) {
        DYNAMIC_SECTION(point.name) {
            TemporaryPackage temporary(point.name);
            const std::vector<std::uint8_t> old_media{'o', 'l', 'd', '\n'};
            const std::vector<std::uint8_t> new_media{'n', 'e', 'w', '\n'};
            const auto old_hash = hash_bytes(old_media);
            const auto new_hash = hash_bytes(new_media);
            const auto old_project = make_project("old", "old-media", old_hash);
            const auto new_project = make_project("new", "new-media", new_hash);
            const auto schema = registry();
            const auto old_json = canonical_project(old_project, schema);
            const auto new_json = canonical_project(new_project, schema);
            publish_baseline(temporary.path, old_hash, old_media, old_project);

            const auto ready =
                temporary.path.parent_path() / (temporary.path.filename().string() + ".ready");
            pulp::platform::ProcessOptions options;
            options.timeout_ms = 10'000;
            pulp::platform::ChildProcess child;
            REQUIRE(child.start(PULP_PROJECT_PACKAGE_PUBLISH_HELPER,
                                {temporary.path.string(), point.name, ready.string()}, options));
            const bool reached = wait_for_ready_or_exit(child, ready);
            if (!reached) {
                if (child.is_running())
                    (void)hard_kill(child.process_id());
                const auto early_exit = child.wait();
                INFO("helper exited before fault point: status="
                     << early_exit.exit_code << " stdout=" << early_exit.stdout_output
                     << " stderr=" << early_exit.stderr_output);
            }
            REQUIRE(reached);
            REQUIRE(hard_kill(child.process_id()));
            const auto child_result = child.wait();
            REQUIRE_FALSE(child_result.timed_out);
            REQUIRE(child_result.exit_code != 0);
            std::error_code ignored;
            fs::remove(ready, ignored);

            const auto opened = open_package(temporary.path, schema);
            REQUIRE(opened);
            const auto visible_json = canonical_project(opened.value().project, schema);
            REQUIRE((visible_json == old_json || visible_json == new_json));
            REQUIRE(visible_json == (point.new_generation_visible ? new_json : old_json));
            REQUIRE(read_text(temporary.path / "project.json") == visible_json);
            if (point.new_generation_visible)
                require_visible_media_is_valid(temporary.path, opened.value(), new_media);
            else
                require_visible_media_is_valid(temporary.path, opened.value(), old_media);
        }
    }
}

TEST_CASE("Project package crash proof catches a production reference-admission mutation",
          "[project-package][crash-recovery][mutation-control]") {
    const auto* enabled = std::getenv("PULP_PROJECT_PACKAGE_RUN_MUTATION_CONTROL");
    if (enabled == nullptr || std::string_view(enabled) != "1")
        SKIP("set PULP_PROJECT_PACKAGE_RUN_MUTATION_CONTROL=1 for the deliberate failure");

    TemporaryPackage temporary("mutation");
    const std::vector<std::uint8_t> old_media{'o', 'l', 'd', '\n'};
    const auto old_hash = hash_bytes(old_media);
    const auto old_project = make_project("old", "old-media", old_hash);
    const auto schema = registry();
    publish_baseline(temporary.path, old_hash, old_media, old_project);

    const auto ready =
        temporary.path.parent_path() / (temporary.path.filename().string() + ".ready");
    pulp::platform::ProcessOptions options;
    options.timeout_ms = 10'000;
    pulp::platform::ChildProcess child;
    REQUIRE(child.start(
        PULP_PROJECT_PACKAGE_MUTANT_HELPER,
        {temporary.path.string(), "generation-published", ready.string(), "--mutate"}, options));
    const bool reached = wait_for_ready_or_exit(child, ready);
    if (!reached) {
        if (child.is_running())
            (void)hard_kill(child.process_id());
        const auto early_exit = child.wait();
        INFO("mutant helper exited before fault point: status="
             << early_exit.exit_code << " stdout=" << early_exit.stdout_output
             << " stderr=" << early_exit.stderr_output);
    }
    REQUIRE(reached);
    REQUIRE(hard_kill(child.process_id()));
    REQUIRE(child.wait().exit_code != 0);

    // Deliberately retain the normal crash-proof assertion. The production
    // mutation bypasses reference admission and publishes project.json without
    // its embedded blob, so this assertion must fail and the outer negative-
    // control command converts that expected non-zero result to exit 42.
    const auto opened = open_package(temporary.path, schema);
    INFO("project-package mutation sentinel: missing-reference generation exposed");
    REQUIRE(opened);
    require_visible_media_is_valid(temporary.path, opened.value(),
                                   std::vector<std::uint8_t>{'n', 'e', 'w', '\n'});
}

TEST_CASE("Atomic project-package publisher refuses unsafe paths and publication conflicts",
          "[project-package][atomic-publisher]") {
    TemporaryPackage temporary("atomic");
    const auto destination = temporary.path / "published";
    fs::create_directories(temporary.path);
    auto publisher = AtomicPublisher::create(destination);
    REQUIRE(publisher);
    REQUIRE_FALSE(publisher.value().write("../escape", "bad"));
    for (const auto path :
         {"media/file:stream", "NUL", "con.txt", "aux ", "COM1.wav", "COM\xc2\xb9.wav",
          "lpt\xc2\xb2.txt", "folder/trailing.", "folder//value", "folder/value?"}) {
        INFO("unsafe portable package path: " << path);
        REQUIRE_FALSE(publisher.value().write(path, "bad"));
    }
    for (const auto& path : std::vector<std::string>{
             std::string("media/\x80.wav", 11), std::string("media/\xc2.wav", 11),
             std::string("media/\xc0\xaf.wav", 12), std::string("media/\xe0\x80\xaf.wav", 13),
             std::string("media/\xed\xa0\x80.wav", 13),
             std::string("media/\xf0\x80\x80\xaf.wav", 14),
             std::string("media/\xf4\x90\x80\x80.wav", 14),
             std::string("media/\xf5\x80\x80\x80.wav", 14)}) {
        INFO("malformed UTF-8 package path");
        const auto rejected = publisher.value().write(path, "bad");
        REQUIRE_FALSE(rejected);
        REQUIRE(rejected.error().code == PackageErrorCode::InvalidPath);
    }
    const auto written = publisher.value().write("nested/value.txt", "value");
    REQUIRE(written);
    REQUIRE(written.value());
    const auto unicode_written = publisher.value().write("nested/caf\xc3\xa9.txt", "unicode");
    REQUIRE(unicode_written);
    REQUIRE(unicode_written.value());
    const auto committed = publisher.value().commit_directory();
    REQUIRE(committed);
    REQUIRE(committed.value() == AtomicPublishOutcome::PublishedDurably);
    REQUIRE(read_text(destination / "nested" / "value.txt") == "value");
    REQUIRE(read_text(destination / fs::path(u8"nested/caf\u00e9.txt")) == "unicode");
    REQUIRE_FALSE(AtomicPublisher::create(destination));
}

TEST_CASE("Atomic project-package publisher anchors a relative destination at creation",
          "[project-package][atomic-publisher]") {
    TemporaryPackage temporary("atomic-relative");
    const auto first = temporary.path / "first";
    const auto second = temporary.path / "second";
    fs::create_directories(first);
    fs::create_directories(second);

    struct CurrentPathRestore {
        fs::path path;
        ~CurrentPathRestore() {
            std::error_code ignored;
            fs::current_path(path, ignored);
        }
    } restore{fs::current_path()};

    fs::current_path(first);
    auto publisher = AtomicPublisher::create("published");
    REQUIRE(publisher);
    const auto written = publisher.value().write("value.txt", "anchored");
    REQUIRE(written);
    REQUIRE(written.value());

    fs::current_path(second);
    const auto committed = publisher.value().commit_directory();
    REQUIRE(committed);
    REQUIRE(committed.value() == AtomicPublishOutcome::PublishedDurably);
    REQUIRE(read_text(first / "published" / "value.txt") == "anchored");
    REQUIRE_FALSE(fs::exists(second / "published"));
}

TEST_CASE("Atomic project-package publisher anchors a symlinked destination parent",
          "[project-package][atomic-publisher][symlink]") {
    TemporaryPackage temporary("atomic-parent-symlink");
    const auto first = temporary.path / "first";
    const auto second = temporary.path / "second";
    const auto parent_link = temporary.path / "parent";
    fs::create_directories(first);
    fs::create_directories(second);

    std::error_code error;
    fs::create_directory_symlink(first, parent_link, error);
    if (error)
        SKIP("directory symlink creation is unavailable: " << error.message());

    auto publisher = AtomicPublisher::create(parent_link / "published");
    REQUIRE(publisher);
    REQUIRE(publisher->staging_directory().parent_path() == fs::canonical(first));
    const auto written = publisher->write("value.txt", "anchored");
    REQUIRE(written);
    REQUIRE(written.value());

    fs::remove(parent_link);
    fs::create_directory_symlink(second, parent_link, error);
    REQUIRE_FALSE(error);

    const auto committed = publisher->commit_directory();
    REQUIRE(committed);
    REQUIRE(committed.value() == AtomicPublishOutcome::PublishedDurably);
    REQUIRE(read_text(first / "published" / "value.txt") == "anchored");
    REQUIRE_FALSE(fs::exists(second / "published"));
}

#if !defined(_WIN32)
TEST_CASE("Atomic project-package publisher creates owner-private staging",
          "[project-package][atomic-publisher][permissions]") {
    TemporaryPackage temporary("atomic-private-staging");
    fs::create_directories(temporary.path);

    // A zero umask is the negative control: an ordinary directory creation
    // would expose 0777, so this test fails if restrictive creation is removed.
    std::optional<AtomicPublisher> publisher;
    {
        struct UmaskRestore {
            mode_t previous = ::umask(0);
            ~UmaskRestore() {
                ::umask(previous);
            }
        } restore;
        auto created = AtomicPublisher::create(temporary.path / "published");
        REQUIRE(created);
        publisher.emplace(std::move(created).value());
    }
    REQUIRE(publisher);
    std::error_code error;
    const auto status = fs::status(publisher->staging_directory(), error);
    REQUIRE_FALSE(error);
    REQUIRE(status.type() == fs::file_type::directory);
    REQUIRE((status.permissions() & fs::perms::all) == fs::perms::owner_all);
}
#endif

#if defined(__APPLE__)
TEST_CASE("Atomic project-package publisher rejects namespace-writing parent ACLs",
          "[project-package][atomic-publisher][permissions]") {
    TemporaryPackage temporary("atomic-parent-acl");
    fs::create_directories(temporary.path);
    acl_t acl = ::acl_init(1);
    REQUIRE(acl != nullptr);
    acl_entry_t entry = nullptr;
    REQUIRE(::acl_create_entry(&acl, &entry) == 0);
    REQUIRE(::acl_set_tag_type(entry, ACL_EXTENDED_ALLOW) == 0);
    uuid_t user{};
    REQUIRE(::mbr_uid_to_uuid(::geteuid(), user) == 0);
    REQUIRE(::acl_set_qualifier(entry, user) == 0);
    acl_permset_t permissions = nullptr;
    REQUIRE(::acl_get_permset(entry, &permissions) == 0);
    REQUIRE(::acl_add_perm(permissions, ACL_ADD_FILE) == 0);
    REQUIRE(::acl_set_permset(entry, permissions) == 0);
    REQUIRE(::acl_set_file(temporary.path.c_str(), ACL_TYPE_EXTENDED, acl) == 0);
    ::acl_free(acl);

    const auto publisher = AtomicPublisher::create(temporary.path / "published");

    REQUIRE_FALSE(publisher);
    REQUIRE(publisher.error().code == PackageErrorCode::InvalidPath);
}
#endif

TEST_CASE("Atomic project-package publisher rejects staged symlinks",
          "[project-package][atomic-publisher]") {
    TemporaryPackage temporary("atomic-symlink");
    fs::create_directories(temporary.path);
    const auto destination = temporary.path / "published";
    auto publisher = AtomicPublisher::create(destination);
    REQUIRE(publisher);
    const auto external = temporary.path / "external.txt";
    std::ofstream(external) << "external";
    std::error_code error;
    fs::create_symlink(external, publisher->staging_directory() / "escape", error);
    if (error)
        SKIP("symlink creation is unavailable: " << error.message());

    const auto committed = publisher->commit_directory();
    REQUIRE_FALSE(committed);
    REQUIRE(committed.error().code == PackageErrorCode::InvalidLayout);
    REQUIRE_FALSE(fs::exists(destination));
}

TEST_CASE("Atomic project-package file publication rejects a staged symlink",
          "[project-package][atomic-publisher][symlink]") {
    TemporaryPackage temporary("atomic-file-symlink");
    fs::create_directories(temporary.path);
    auto publisher = AtomicPublisher::create(temporary.path / "published");
    REQUIRE(publisher);
    const auto external = temporary.path / "external.txt";
    std::ofstream(external) << "external";
    const auto staged = publisher->staging_directory() / "redirect";
    std::error_code error;
    fs::create_symlink(external, staged, error);
    if (error)
        SKIP("symlink creation is unavailable: " << error.message());

    const auto committed = publisher->commit_file(staged);
    REQUIRE_FALSE(committed);
    REQUIRE(committed.error().code == PackageErrorCode::InvalidPath);
    REQUIRE_FALSE(fs::exists(temporary.path / "published"));
}

TEST_CASE("Atomic project-package publisher rejects symlinked write ancestors",
          "[project-package][atomic-publisher][symlink]") {
    TemporaryPackage temporary("atomic-ancestor-symlink");
    fs::create_directories(temporary.path);
    const auto destination = temporary.path / "published";
    auto publisher = AtomicPublisher::create(destination);
    REQUIRE(publisher);

    const auto external = temporary.path / "external";
    fs::create_directory(external);
    std::error_code error;
    fs::create_directory_symlink(external, publisher->staging_directory() / "redirect", error);
    if (error)
        SKIP("directory symlink creation is unavailable: " << error.message());

    REQUIRE_FALSE(publisher->write("redirect/escaped.txt", "escape"));
    REQUIRE_FALSE(fs::exists(external / "escaped.txt"));
}

TEST_CASE("Atomic project-package directory publication rejects a rebound staging name",
          "[project-package][atomic-publisher][race]") {
    TemporaryPackage temporary("atomic-staging-rebind");
    fs::create_directories(temporary.path);
    const auto destination = temporary.path / "published";
    auto publisher = AtomicPublisher::create(destination);
    REQUIRE(publisher);
    REQUIRE(publisher->write("original.txt", "original"));
    const auto staging = publisher->staging_directory();
    const auto displaced = temporary.path / "displaced";
    fs::rename(staging, displaced);
    fs::create_directory(staging);
    std::ofstream(staging / "replacement.txt") << "replacement";

    const auto committed = publisher->commit_directory();

    REQUIRE_FALSE(committed);
    REQUIRE(committed.error().code == PackageErrorCode::InvalidLayout);
    REQUIRE_FALSE(fs::exists(destination));
    publisher->cancel();
    REQUIRE(read_text(staging / "replacement.txt") == "replacement");
}

TEST_CASE("Atomic file publication revalidates its pinned source after callbacks",
          "[project-package][atomic-publisher][race]") {
    TemporaryPackage temporary("atomic-file-rebind");
    fs::create_directories(temporary.path);
    const auto destination = temporary.path / "published";
    auto publisher = AtomicPublisher::create(destination);
    REQUIRE(publisher);
    REQUIRE(publisher->write("source", "original"));
    const auto source = publisher->staging_directory() / "source";
    g_rebind_source = source;
    g_rebind_displaced = publisher->staging_directory() / "displaced";
    g_rebind_replacement_file = source;
    g_rebind_point = pulp::project_package::detail::PackageFaultPoint::StagedFileFenced;
    pulp::project_package::detail::ProjectPackageTestAccess::set_fault_hook(
        rebind_publication_source);

    const auto committed = publisher->commit_file(source);

    pulp::project_package::detail::ProjectPackageTestAccess::clear_fault_hook();
    g_rebind_source.clear();
    g_rebind_displaced.clear();
    g_rebind_replacement_file.clear();
    REQUIRE_FALSE(committed);
    REQUIRE(committed.error().code == PackageErrorCode::InvalidLayout);
    REQUIRE_FALSE(fs::exists(destination));
}

TEST_CASE("Atomic directory publication revalidates its pinned tree after callbacks",
          "[project-package][atomic-publisher][race]") {
    TemporaryPackage temporary("atomic-directory-rebind-callback");
    fs::create_directories(temporary.path);
    const auto destination = temporary.path / "published";
    auto publisher = AtomicPublisher::create(destination);
    REQUIRE(publisher);
    REQUIRE(publisher->write("original.txt", "original"));
    const auto staging = publisher->staging_directory();
    g_rebind_source = staging;
    g_rebind_displaced = temporary.path / "displaced";
    g_rebind_replacement_file.clear();
    g_rebind_point = pulp::project_package::detail::PackageFaultPoint::DirectoryTreeFenced;
    pulp::project_package::detail::ProjectPackageTestAccess::set_fault_hook(
        rebind_publication_source);

    const auto committed = publisher->commit_directory();

    pulp::project_package::detail::ProjectPackageTestAccess::clear_fault_hook();
    g_rebind_source.clear();
    g_rebind_displaced.clear();
    REQUIRE_FALSE(committed);
    REQUIRE(committed.error().code == PackageErrorCode::InvalidLayout);
    REQUIRE_FALSE(fs::exists(destination));
    publisher->cancel();
    REQUIRE(read_text(staging / "replacement.txt") == "replacement");
}

TEST_CASE("Atomic publication remains bound to a renamed destination parent",
          "[project-package][atomic-publisher][race]") {
    TemporaryPackage temporary("atomic-parent-rebind");
    const auto parent = temporary.path / "parent";
    fs::create_directories(parent);
    auto publisher = AtomicPublisher::create(parent / "published");
    REQUIRE(publisher);
    REQUIRE(publisher->write("original.txt", "original"));
    const auto displaced = temporary.path / "displaced";
    g_rebind_source = parent;
    g_rebind_displaced = displaced;
    g_rebind_replacement_file.clear();
    g_rebind_point = pulp::project_package::detail::PackageFaultPoint::PublicationSourceVerified;
    pulp::project_package::detail::ProjectPackageTestAccess::set_fault_hook(
        rebind_publication_source);

    const auto committed = publisher->commit_directory();

    pulp::project_package::detail::ProjectPackageTestAccess::clear_fault_hook();
    g_rebind_source.clear();
    g_rebind_displaced.clear();
    REQUIRE(committed);
    REQUIRE(committed.value() == AtomicPublishOutcome::PublishedDurabilityUncertain);
    REQUIRE(read_text(displaced / "published" / "original.txt") == "original");
    REQUIRE(read_text(parent / "replacement.txt") == "replacement");
    REQUIRE_FALSE(fs::exists(parent / "published"));
}

TEST_CASE("Package writer anchors a relative root before publication callbacks",
          "[project-package][root][race]") {
    TemporaryPackage temporary("relative-root-anchor");
    const auto first = temporary.path / "first";
    const auto second = temporary.path / "second";
    fs::create_directories(first);
    fs::create_directories(second);
    const auto previous = fs::current_path();
    struct CurrentPathRestore {
        fs::path path;
        ~CurrentPathRestore() {
            std::error_code ignored;
            fs::current_path(path, ignored);
        }
    } restore{previous};
    fs::current_path(first);
    g_switch_current_path = second;
    pulp::project_package::detail::ProjectPackageTestAccess::set_fault_hook(
        switch_current_path_after_directory_publish);
    auto writer = PackageWriter::create("package", registry());
    pulp::project_package::detail::ProjectPackageTestAccess::clear_fault_hook();
    g_switch_current_path.clear();

    REQUIRE(writer);
    REQUIRE(writer->root() == fs::canonical(first / "package"));
    REQUIRE(fs::is_directory(first / "package" / "media"));
    REQUIRE_FALSE(fs::exists(second / "package"));
}

#if !defined(_WIN32)
TEST_CASE("No-replace fallback publishes regular files without replacement",
          "[project-package][atomic-publisher][linux-fallback]") {
    TemporaryPackage temporary("atomic-file-fallback");
    fs::create_directories(temporary.path);
    const auto source = temporary.path / "source";
    const auto destination = temporary.path / "destination";
    std::ofstream(source) << "payload";

    const auto result = pulp::project_package::detail::publish_no_replace_fallback(
        source, destination,
        pulp::project_package::detail::NoReplaceSourceKind::RegularFile);

    REQUIRE(result == pulp::project_package::detail::NoReplaceOutcome::Published);
    REQUIRE_FALSE(fs::exists(source));
    REQUIRE(read_text(destination) == "payload");
}

TEST_CASE("No-replace fallback never applies the regular-file link path to directories",
          "[project-package][atomic-publisher][linux-fallback]") {
    TemporaryPackage temporary("atomic-directory-fallback");
    fs::create_directories(temporary.path);
    const auto source = temporary.path / "source";
    const auto destination = temporary.path / "destination";
    fs::create_directory(source);

    const auto result = pulp::project_package::detail::publish_no_replace_fallback(
        source, destination,
        pulp::project_package::detail::NoReplaceSourceKind::Directory);

    REQUIRE(result == pulp::project_package::detail::NoReplaceOutcome::Unsupported);
    REQUIRE(fs::is_directory(source));
    REQUIRE_FALSE(fs::exists(destination));
}
#endif

TEST_CASE("Opening through a symlinked ancestor returns canonical working directories",
          "[project-package][open][symlink]") {
    TemporaryPackage temporary("open-canonical-root");
    const auto first_parent = temporary.path / "first";
    const auto second_parent = temporary.path / "second";
    fs::create_directories(first_parent);
    fs::create_directories(second_parent);
    const auto package = first_parent / "package";
    const std::vector<std::uint8_t> media{'c', 'a', 'n', 'o', 'n'};
    const auto hash = hash_bytes(media);
    publish_baseline(package, hash, media, make_project("canonical", "media", hash));

    const auto alias = temporary.path / "alias";
    std::error_code error;
    fs::create_directory_symlink(first_parent, alias, error);
    if (error)
        SKIP("directory symlink creation is unavailable: " << error.message());

    const auto opened = open_package(alias / "package", registry());
    REQUIRE(opened);
    const auto canonical_package = fs::canonical(package);
    REQUIRE(opened->journal_directory == canonical_package / "journal");
    REQUIRE(opened->cache_directory == canonical_package / "cache");

    fs::remove(alias, error);
    REQUIRE_FALSE(error);
    fs::create_directory_symlink(second_parent, alias, error);
    REQUIRE_FALSE(error);
    REQUIRE(opened->journal_directory == canonical_package / "journal");
    REQUIRE(opened->cache_directory == canonical_package / "cache");
}

TEST_CASE("Package writer fences the same pre-existing blob handle that it verified",
          "[project-package][durability][race]") {
    TemporaryPackage temporary("existing-blob-fence");
    const std::vector<std::uint8_t> bytes{'d', 'u', 'r', 'a', 'b', 'l', 'e'};
    const auto hash = hash_bytes(bytes);
    auto writer = PackageWriter::create(temporary.path, registry());
    REQUIRE(writer);
    REQUIRE(writer->stage_blob(BlobStore::Media, hash, bytes));

    g_swap_after_blob_verification = temporary.path / "media" / hash.to_hex();
    g_blob_swap_source = temporary.path / "media/blob-swap";
    {
        std::ofstream replacement(g_blob_swap_source, std::ios::binary);
        REQUIRE(replacement);
        replacement << "different bytes";
    }
    pulp::project_package::detail::ProjectPackageTestAccess::set_fault_hook(swap_verified_blob);
    const auto restaged = writer->stage_blob(BlobStore::Media, hash, bytes);
    pulp::project_package::detail::ProjectPackageTestAccess::clear_fault_hook();
    g_swap_after_blob_verification.clear();
    g_blob_swap_source.clear();

    REQUIRE_FALSE(restaged);
    REQUIRE(restaged.error().code == PackageErrorCode::DurabilityUncertain);
}

TEST_CASE("Project publication rejects a blob pathname swapped after handle verification",
          "[project-package][race]") {
    TemporaryPackage temporary("publish-blob-swap");
    const std::vector<std::uint8_t> bytes{'v', 'e', 'r', 'i', 'f', 'i', 'e', 'd'};
    const auto hash = hash_bytes(bytes);
    auto writer = PackageWriter::create(temporary.path, registry());
    REQUIRE(writer);
    REQUIRE(writer->stage_blob(BlobStore::Media, hash, bytes));

    g_swap_after_blob_verification = temporary.path / "media" / hash.to_hex();
    g_blob_swap_source = temporary.path / "media/blob-swap";
    {
        std::ofstream replacement(g_blob_swap_source, std::ios::binary);
        REQUIRE(replacement);
        replacement << "different bytes";
    }
    pulp::project_package::detail::ProjectPackageTestAccess::set_fault_hook(swap_verified_blob);
    const auto published = writer->publish(make_project("swap", "media", hash));
    pulp::project_package::detail::ProjectPackageTestAccess::clear_fault_hook();
    g_swap_after_blob_verification.clear();
    g_blob_swap_source.clear();

    REQUIRE_FALSE(published);
    REQUIRE(published.error().code == PackageErrorCode::InvalidGeneration);
    REQUIRE_FALSE(fs::exists(temporary.path / "project.json"));
}

#if !defined(_WIN32)
TEST_CASE("Project package hashing rejects a file appended after its size snapshot",
          "[project-package][hash-race]") {
    TemporaryPackage temporary("hash-append-race");
    fs::create_directories(temporary.path);
    const std::vector<std::uint8_t> bytes{'p', 'r', 'e', 'f', 'i', 'x'};
    const auto path = temporary.path / "blob";
    {
        std::ofstream output(path, std::ios::binary);
        output.write(reinterpret_cast<const char*>(bytes.data()),
                     static_cast<std::streamsize>(bytes.size()));
    }
    auto pinned = pulp::project_package::detail::PinnedFile::open(path, false);
    REQUIRE(pinned);

    g_append_during_hash = path;
    pulp::project_package::detail::ProjectPackageTestAccess::set_fault_hook(
        append_during_blob_hash);
    const bool matches = pinned->hash_matches(hash_bytes(bytes).to_hex(), 1024);
    pulp::project_package::detail::ProjectPackageTestAccess::clear_fault_hook();
    g_append_during_hash.clear();

    REQUIRE_FALSE(matches);
    REQUIRE(fs::file_size(path) > bytes.size());
}
#endif

TEST_CASE("Project reference validation hashes each canonical blob once per pass",
          "[project-package][hash-amplification]") {
    TemporaryPackage temporary("reference-deduplication");
    const std::vector<std::uint8_t> first_bytes{'f', 'i', 'r', 's', 't'};
    const std::vector<std::uint8_t> second_bytes{'s', 'e', 'c', 'o', 'n', 'd'};
    const auto first_hash = hash_bytes(first_bytes);
    const auto second_hash = hash_bytes(second_bytes);
    const auto project = make_repeated_reference_project(first_hash, second_hash);

    {
        auto writer = PackageWriter::create(temporary.path, registry());
        REQUIRE(writer);
        REQUIRE(writer->stage_blob(BlobStore::Media, first_hash, first_bytes));
        REQUIRE(writer->stage_blob(BlobStore::Media, second_hash, second_bytes));

        g_blob_verifications.store(0, std::memory_order_relaxed);
        pulp::project_package::detail::ProjectPackageTestAccess::set_fault_hook(
            count_blob_verification);
        const auto published = writer->publish(project);
        pulp::project_package::detail::ProjectPackageTestAccess::clear_fault_hook();

        REQUIRE(published);
        REQUIRE(published.value() == AtomicPublishOutcome::PublishedDurably);
        REQUIRE(g_blob_verifications.load(std::memory_order_relaxed) == 2);
    }

    g_blob_verifications.store(0, std::memory_order_relaxed);
    pulp::project_package::detail::ProjectPackageTestAccess::set_fault_hook(
        count_blob_verification);
    const auto opened = open_package(temporary.path, registry());
    pulp::project_package::detail::ProjectPackageTestAccess::clear_fault_hook();

    REQUIRE(opened);
    REQUIRE(g_blob_verifications.load(std::memory_order_relaxed) == 2);
}

TEST_CASE("Project package size limits narrow only when the target size can represent them",
          "[project-package][limits]") {
    constexpr auto largest_32_bit = std::numeric_limits<std::uint32_t>::max();
    REQUIRE(pulp::project_package::detail::checked_size_limit<std::uint32_t>(largest_32_bit) ==
            largest_32_bit);
    REQUIRE_FALSE(pulp::project_package::detail::checked_size_limit<std::uint32_t>(
        static_cast<std::uint64_t>(largest_32_bit) + 1));
}
