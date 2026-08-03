#include "project_package_test_access.hpp"

#include <pulp/project_package/project_package.hpp>
#include <pulp/runtime/crypto.hpp>
#include <pulp/timeline/model.hpp>
#include <pulp/timeline/schema_registry.hpp>

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <thread>
#include <vector>

namespace {

using pulp::project_package::BlobStore;
using pulp::project_package::detail::PackageFaultPoint;
namespace fs = std::filesystem;

PackageFaultPoint g_target = PackageFaultPoint::GenerationPublished;
fs::path g_ready_path;

std::optional<PackageFaultPoint> parse_fault_point(const std::string& value) {
    if (value == "staged-file-written")
        return PackageFaultPoint::StagedFileWritten;
    if (value == "staged-file-fenced")
        return PackageFaultPoint::StagedFileFenced;
    if (value == "blob-published")
        return PackageFaultPoint::BlobPublished;
    if (value == "blob-directory-fenced")
        return PackageFaultPoint::BlobDirectoryFenced;
    if (value == "generation-written")
        return PackageFaultPoint::GenerationWritten;
    if (value == "generation-fenced")
        return PackageFaultPoint::GenerationFenced;
    if (value == "generation-published")
        return PackageFaultPoint::GenerationPublished;
    if (value == "generation-directory-fenced")
        return PackageFaultPoint::GenerationDirectoryFenced;
    return std::nullopt;
}

void stop_at_target(PackageFaultPoint point) noexcept {
    if (point != g_target)
        return;
    if (auto* ready = std::fopen(g_ready_path.string().c_str(), "wb")) {
        constexpr char marker[] = "ready\n";
        (void)std::fwrite(marker, 1, sizeof(marker) - 1, ready);
        (void)std::fflush(ready);
        (void)std::fclose(ready);
    }
    for (;;)
        std::this_thread::sleep_for(std::chrono::hours(1));
}

pulp::timeline::ContentHash hash_bytes(std::span<const std::uint8_t> bytes) {
    const auto encoded = pulp::runtime::sha256_hex(bytes.data(), bytes.size());
    return *pulp::timeline::ContentHash::from_hex(encoded);
}

std::optional<pulp::timeline::Project> make_project(std::string name,
                                                    const pulp::timeline::ContentHash& hash) {
    using namespace pulp::timeline;
    MediaAsset asset{
        {2}, std::move(name), 4, {48'000, 1}, hash, AssetStoragePolicy::Embedded, {}, {}, {}};
    auto sequence = Sequence::create({3}, "sequence", pulp::timebase::TickDuration{0}, {});
    if (!sequence)
        return std::nullopt;
    auto project = Project::create(ProjectInput{.id = {1},
                                                .name = "new",
                                                .next_item_id = 4,
                                                .root_sequence_id = {3},
                                                .assets = {std::move(asset)},
                                                .sequences = {std::move(sequence).value()},
                                                .tempo_map = {},
                                                .meter_map = {},
                                                .session_start = std::nullopt});
    if (!project)
        return std::nullopt;
    return std::move(project).value();
}

} // namespace

int main(int argc, char** argv) {
    if (argc != 4 && argc != 5)
        return 10;
    const auto target = parse_fault_point(argv[2]);
    if (!target)
        return 11;
    g_target = *target;
    g_ready_path = argv[3];

    auto registry = pulp::timeline::make_builtin_timeline_registry();
    if (!registry)
        return 12;
    auto writer = pulp::project_package::PackageWriter::create(fs::path(argv[1]),
                                                               std::move(registry).value());
    if (!writer)
        return 13;

    const std::vector<std::uint8_t> bytes{'n', 'e', 'w', '\n'};
    const auto hash = hash_bytes(bytes);
    pulp::project_package::detail::ProjectPackageTestAccess::set_fault_hook(&stop_at_target);
    const bool skip_reference_validation = argc == 5 && std::string(argv[4]) == "--mutate";
    if (skip_reference_validation) {
#if defined(PULP_PROJECT_PACKAGE_MUTANT_HELPER)
        pulp::project_package::detail::ProjectPackageTestAccess::
            set_skip_reference_validation_for_test(true);
#else
        return 18;
#endif
    } else {
        const auto staged = writer.value().stage_blob(BlobStore::Media, hash, bytes);
        if (!staged)
            return 14;
    }
    const auto project = make_project("new-media", hash);
    if (!project)
        return 15;
    const auto published = writer.value().publish(*project);
    if (!published)
        return 16;
#if defined(PULP_PROJECT_PACKAGE_MUTANT_HELPER)
    pulp::project_package::detail::ProjectPackageTestAccess::set_skip_reference_validation_for_test(
        false);
#endif
    pulp::project_package::detail::ProjectPackageTestAccess::clear_fault_hook();
    return 17; // The selected production point was not reached.
}
