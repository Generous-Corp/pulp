#include <pulp/dawproject/dawproject_export.hpp>
#include <pulp/interchange/export_plan.hpp>
#include <pulp/timeline/model.hpp>

#include <algorithm>
#include <string>

namespace {

pulp::runtime::Result<pulp::timeline::Project, pulp::timeline::ModelError>
project_with_track(pulp::timeline::ItemId project_id,
                   pulp::timeline::ItemId sequence_id,
                   pulp::timeline::ItemId track_id,
                   const char* track_name) {
    auto track = pulp::timeline::Track::create(track_id, track_name, {});
    if (!track)
        return pulp::runtime::Err(track.error());
    auto sequence = pulp::timeline::Sequence::create(
        sequence_id, "arrangement", pulp::timebase::TickDuration{4},
        {std::move(track).value()});
    if (!sequence)
        return pulp::runtime::Err(sequence.error());
    return pulp::timeline::Project::create(pulp::timeline::ProjectInput{
        project_id, "project", track_id.value + 1, sequence_id, {},
        {std::move(sequence).value()}});
}

} // namespace

int main() {
    auto planned = project_with_track({1}, {3}, {5}, "planned-track");
    auto legacy = project_with_track({10}, {30}, {50}, "legacy-track");
    if (!planned || !legacy)
        return 1;

    const auto plan = pulp::interchange::plan_export(
        planned.value(), pulp::interchange::Format::DawProject);

    // Source-compatibility proof for the released v0.759 writer contract: a
    // consumer can still construct the std::function alias from a lambda and
    // invoke it directly. The format-bound adapter below is the preferred API.
    pulp::interchange::ExportWriter legacy_writer =
        [](const pulp::interchange::ExportPlan&) {
            pulp::interchange::ExportArtifacts artifacts;
            artifacts.artifacts.push_back({"legacy.bin", {1}});
            return pulp::runtime::Ok(std::move(artifacts));
        };
    const auto legacy_direct = legacy_writer(plan);
    if (!legacy_direct || legacy_direct.value().artifacts.size() != 1)
        return 2;
    const auto legacy_gated = pulp::interchange::run_export(
        plan, pulp::interchange::ExportOptions{}, legacy_writer);
    if (!legacy_gated || legacy_gated.value().artifacts.size() != 2)
        return 3;

    pulp::interchange::ExportWriter writer = pulp::dawproject::writer(legacy.value());
    auto exported = pulp::interchange::run_export(
        plan, pulp::interchange::ExportOptions{}, writer);
    if (!exported)
        return 4;

    const auto artifact = std::find_if(
        exported.value().artifacts.begin(), exported.value().artifacts.end(),
        [](const pulp::interchange::ExportArtifact& candidate) {
            return candidate.name == "project.xml";
        });
    if (artifact == exported.value().artifacts.end())
        return 5;
    const std::string xml(artifact->bytes.begin(), artifact->bytes.end());
    return xml.find("planned-track") != std::string::npos &&
                   xml.find("legacy-track") == std::string::npos
               ? 0
               : 6;
}
