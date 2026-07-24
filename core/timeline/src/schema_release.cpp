#include <pulp/timeline/schema_release.hpp>

#include <algorithm>
#include <array>

namespace pulp::timeline {
namespace {

using Domain = SchemaDomain;
using Target = SchemaVersionTarget;

constexpr std::array release_v0_736_0{
    Target{Domain::Document, "pulp.timeline.project", 1},
    Target{Domain::Document, "pulp.timeline.asset", 1},
    Target{Domain::AssetRepresentation, "pulp.timeline.asset_representation", 1},
    Target{Domain::Document, "pulp.timeline.sequence", 1},
    Target{Domain::Document, "pulp.timeline.track", 1},
    Target{Domain::Document, "pulp.timeline.clip", 1},
    Target{Domain::Content, "pulp.timeline.content.empty", 1},
    Target{Domain::Content, "pulp.timeline.content.media", 1},
    Target{Domain::Content, "pulp.timeline.content.notes", 1},
};

constexpr std::array release_v0_744_0{
    Target{Domain::Document, "pulp.timeline.project", 1},
    Target{Domain::Document, "pulp.timeline.asset", 1},
    Target{Domain::AssetRepresentation, "pulp.timeline.asset_representation", 1},
    Target{Domain::Document, "pulp.timeline.sequence", 1},
    Target{Domain::Document, "pulp.timeline.track", 2},
    Target{Domain::Document, "pulp.timeline.device_placement", 1},
    Target{Domain::Document, "pulp.timeline.clip", 1},
    Target{Domain::Content, "pulp.timeline.content.empty", 1},
    Target{Domain::Content, "pulp.timeline.content.media", 1},
    Target{Domain::Content, "pulp.timeline.content.notes", 1},
};

constexpr std::array release_v0_748_0{
    Target{Domain::Document, "pulp.timeline.project", 1},
    Target{Domain::Document, "pulp.timeline.asset", 1},
    Target{Domain::AssetRepresentation, "pulp.timeline.asset_representation", 1},
    Target{Domain::Document, "pulp.timeline.sequence", 1},
    Target{Domain::Document, "pulp.timeline.track", 3},
    Target{Domain::Document, "pulp.timeline.automation_lane", 1},
    Target{Domain::Document, "pulp.timeline.automation_target.device_parameter", 1},
    Target{Domain::Document, "pulp.timeline.device_placement", 1},
    Target{Domain::Document, "pulp.timeline.clip", 1},
    Target{Domain::Content, "pulp.timeline.content.empty", 1},
    Target{Domain::Content, "pulp.timeline.content.media", 1},
    Target{Domain::Content, "pulp.timeline.content.notes", 1},
};

constexpr std::array releases{
    SchemaReleaseMap{"v0.736.0", release_v0_736_0},
    SchemaReleaseMap{"v0.744.0", release_v0_744_0},
    SchemaReleaseMap{"v0.748.0", release_v0_748_0},
};

} // namespace

const SchemaVersionTarget* SchemaReleaseMap::find(SchemaDomain domain,
                                                  std::string_view type_name) const noexcept {
    const auto found = std::find_if(versions.begin(), versions.end(), [&](const auto& target) {
        return target.domain == domain && target.type_name == type_name;
    });
    return found == versions.end() ? nullptr : &*found;
}

std::span<const SchemaReleaseMap> builtin_timeline_schema_releases() noexcept {
    return releases;
}

const SchemaReleaseMap*
find_builtin_timeline_schema_release(std::string_view release_label) noexcept {
    const auto found = std::find_if(releases.begin(), releases.end(), [&](const auto& release) {
        return release.release_label == release_label;
    });
    return found == releases.end() ? nullptr : &*found;
}

} // namespace pulp::timeline
