#include "project_session_shell.hpp"

#include <pulp/timeline/serialize.hpp>

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace {

using namespace pulp;

class TemporaryDirectory {
  public:
    TemporaryDirectory() {
        const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
        path = std::filesystem::temp_directory_path() /
               ("pulp-timeline-project-session-" + std::to_string(nonce));
        std::error_code error;
        if (!std::filesystem::create_directory(path, error))
            path.clear();
    }

    ~TemporaryDirectory() {
        if (path.empty())
            return;
        std::error_code error;
        std::filesystem::remove_all(path, error);
    }

    std::filesystem::path path;
};

std::optional<timeline::Project> make_empty_project(std::string name) {
    auto sequence = timeline::Sequence::create(
        {2}, "Arrangement", timebase::TickDuration{16 * timebase::kTicksPerQuarter}, {});
    if (!sequence)
        return std::nullopt;
    auto project = timeline::Project::create({.id = {1},
                                              .name = std::move(name),
                                              .next_item_id = 3,
                                              .root_sequence_id = {2},
                                              .sequences = {std::move(sequence).value()}});
    if (!project)
        return std::nullopt;
    return std::move(project).value();
}

std::optional<timeline::Track> make_midi_track() {
    auto notes = timeline::MidiContent::create(
        {{{5}, {0}, {timebase::kTicksPerQuarter}, 12'000, 60, 0}});
    if (!notes)
        return std::nullopt;
    auto clip = timeline::Clip::create(
        {4}, {0}, timebase::TickDuration{4 * timebase::kTicksPerQuarter},
        timeline::ClipContent{std::move(notes).value()});
    if (!clip)
        return std::nullopt;
    auto track = timeline::Track::create({3}, "Piano", {std::move(clip).value()});
    if (!track)
        return std::nullopt;
    return std::move(track).value();
}

std::optional<std::string> canonical_bytes(const timeline::Project& project,
                                           const timeline::SchemaRegistry& registry) {
    auto serialized = timeline::serialize_project(project, registry);
    if (!serialized)
        return std::nullopt;
    return std::move(serialized).value().json;
}

std::optional<std::string> published_bytes(const std::filesystem::path& package,
                                           const timeline::SchemaRegistry& registry) {
    auto opened = project_package::open_package(package, registry);
    if (!opened)
        return std::nullopt;
    return canonical_bytes(std::move(opened).value().project, registry);
}

bool check(bool condition, const char* explanation) {
    if (condition)
        return true;
    std::fprintf(stderr, "project session: %s\n", explanation);
    return false;
}

} // namespace

int main() {
    using namespace pulp;
    using namespace pulp::examples::timeline_session;

    TemporaryDirectory temporary;
    if (!check(!temporary.path.empty(), "temporary package root creation failed"))
        return 1;
    const auto package = temporary.path / "acceptance.pulpproject";
    auto registry_result = timeline::make_builtin_timeline_registry();
    if (!check(static_cast<bool>(registry_result), "built-in schema registry creation failed"))
        return 1;
    const auto registry = std::move(registry_result).value();

    auto initial = make_empty_project("Acceptance Session");
    if (!check(initial.has_value(), "initial project construction failed"))
        return 1;
    const auto initial_bytes = canonical_bytes(*initial, registry);
    if (!check(initial_bytes.has_value(), "initial canonical serialization failed"))
        return 1;
    auto created = ProjectSessionShell::create(package, std::move(*initial), registry);
    if (!check(static_cast<bool>(created), "session creation failed"))
        return 1;
    auto shell = std::move(created).value();
    if (!check(shell->is_open() && shell->revision() == timeline::DocumentRevision{},
               "new session did not open at revision zero"))
        return 1;

    std::vector<timeline::Command> packaged_asset_edit;
    packaged_asset_edit.push_back(timeline::CreateAsset{timeline::MediaAsset{}});
    auto packaged_asset_rejected = shell->submit(std::move(packaged_asset_edit));
    if (!check(!packaged_asset_rejected,
               "shell admitted an asset without a blob-staging path") ||
        !check(packaged_asset_rejected.error().code ==
                   ProjectSessionErrorCode::PackagedAssetUnsupported,
               "unstageable asset returned the wrong failure") ||
        !check(shell->revision() == timeline::DocumentRevision{},
               "rejected asset advanced the document revision"))
        return 1;

    auto track = make_midi_track();
    if (!check(track.has_value(), "MIDI track construction failed"))
        return 1;
    std::vector<timeline::Command> first_edit;
    first_edit.push_back(timeline::InsertTrack{{2}, std::move(*track)});
    if (!check(static_cast<bool>(shell->submit(std::move(first_edit))),
               "identity-bearing MIDI edit failed"))
        return 1;
    const auto first_bytes = canonical_bytes(*shell->project(), registry);
    if (!check(first_bytes.has_value(), "first canonical serialization failed") ||
        !check(shell->revision() == timeline::DocumentRevision{1},
               "first edit did not advance to revision one") ||
        !check(shell->project()->item_id_allocator().next_value() == 6,
               "first edit did not persist the allocator frontier"))
        return 1;

    shell->close();
    if (!check(!shell->is_open(), "close left the session open"))
        return 1;
    const auto before_refusal_bytes = published_bytes(package, registry);
    if (!check(before_refusal_bytes == initial_bytes,
               "unsaved journal edit changed the published generation"))
        return 1;

    auto replacement = make_empty_project("Destructive Replacement");
    if (!check(replacement.has_value(), "replacement fixture construction failed"))
        return 1;
    auto refused = ProjectSessionShell::create(package, std::move(*replacement), registry);
    if (!check(!refused, "create replaced an existing project package") ||
        !check(refused.error().code == ProjectSessionErrorCode::AlreadyExists,
               "create-on-existing returned the wrong failure"))
        return 1;
    const auto after_refusal_bytes = published_bytes(package, registry);
    if (!check(after_refusal_bytes == initial_bytes,
               "refused create changed the published generation"))
        return 1;

    if (!check(static_cast<bool>(shell->reopen()), "first reopen failed") ||
        !check(shell->revision() == timeline::DocumentRevision{1},
               "journal recovery lost the first revision") ||
        !check(shell->project()->item_id_allocator().next_value() == 6,
               "journal recovery changed the allocator frontier"))
        return 1;
    const auto first_reopened_bytes = canonical_bytes(*shell->project(), registry);
    if (!check(first_reopened_bytes == first_bytes,
               "first reopen changed the canonical project bytes") ||
        !check(static_cast<bool>(shell->save()), "first package publication failed"))
        return 1;
    shell->close();
    const auto first_published_bytes = published_bytes(package, registry);
    if (!check(first_published_bytes == first_bytes,
               "first save did not publish the revision-one bytes") ||
        !check(static_cast<bool>(shell->reopen()), "post-save reopen failed"))
        return 1;

    std::vector<timeline::Command> second_edit;
    second_edit.push_back(timeline::SetTrackName{{2}, {3}, "Piano", "Concert Grand"});
    if (!check(static_cast<bool>(shell->submit(std::move(second_edit))),
               "second identity-targeted edit failed"))
        return 1;
    const auto second_bytes = canonical_bytes(*shell->project(), registry);
    if (!check(second_bytes.has_value() && second_bytes != first_bytes,
               "second edit did not change canonical bytes") ||
        !check(shell->revision() == timeline::DocumentRevision{2},
               "second edit did not advance to revision two") ||
        !check(shell->project()->item_id_allocator().next_value() == 6,
               "non-allocating edit changed the allocator frontier"))
        return 1;

    shell->close();
    if (!check(static_cast<bool>(shell->reopen()), "second reopen failed") ||
        !check(shell->revision() == timeline::DocumentRevision{2},
               "second reopen lost the journaled revision") ||
        !check(shell->project()->item_id_allocator().next_value() == 6,
               "second reopen changed the allocator frontier"))
        return 1;
    const auto second_reopened_bytes = canonical_bytes(*shell->project(), registry);
    if (!check(second_reopened_bytes == second_bytes,
               "second reopen changed the canonical project bytes") ||
        !check(static_cast<bool>(shell->save()), "second package publication failed"))
        return 1;

    shell->close();
    const auto second_published_bytes = published_bytes(package, registry);
    if (!check(second_published_bytes == second_bytes,
               "second save did not publish the revision-two bytes"))
        return 1;
    auto final_open = ProjectSessionShell::open(package, registry);
    if (!check(static_cast<bool>(final_open), "factory open of final package failed"))
        return 1;
    shell = std::move(final_open).value();
    if (!check(shell->revision() == timeline::DocumentRevision{2},
               "factory open lost the final revision") ||
        !check(shell->project()->item_id_allocator().next_value() == 6,
               "factory open changed the final allocator frontier"))
        return 1;

    std::printf("project session: revision %llu, allocator frontier %llu\n",
                static_cast<unsigned long long>(shell->revision().value),
                static_cast<unsigned long long>(
                    shell->project()->item_id_allocator().next_value()));
    return 0;
}
