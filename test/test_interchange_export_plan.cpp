#include <catch2/catch_test_macros.hpp>

#include <pulp/interchange/export_plan.hpp>
#include <pulp/timeline/model.hpp>

#include <algorithm>
#include <string>
#include <type_traits>
#include <vector>

using namespace pulp::interchange;
using namespace pulp::timeline;
using namespace pulp::timebase;

static_assert(!std::is_invocable_v<FormatBoundExportWriter, const ExportPlan&>);

namespace {

template <typename T> T take_value(pulp::runtime::Result<T, ModelError> result) {
    REQUIRE(result.has_value());
    return std::move(result).value();
}

ContentHash content_hash(char digit = 'a') {
    return *ContentHash::from_hex(std::string(64, digit));
}

// A document DAWproject carries without loss: flat track, beat-anchored clips,
// one tempo, one meter, media referenced by locator.
Project lossless_project() {
    auto clip = take_value(Clip::create({4}, {0}, {100}, MediaRef{{2}, {0}, 100}));
    auto track = take_value(Track::create({6}, "track", {clip}));
    auto sequence = take_value(Sequence::create({3}, "sequence", TickDuration{400}, {track}));
    return take_value(Project::create(
        ProjectInput{{1},
                     "project",
                     20,
                     {3},
                     {MediaAsset{{2}, "audio.wav", 1'000, {48'000, 1}, content_hash(),
                                 AssetStoragePolicy::External, {}, {}, {}}},
                     {sequence}}));
}

// The same document plus a sample-anchored clip, which DAWproject can only
// re-express in beats.
Project lossy_project() {
    auto musical = take_value(Clip::create({4}, {0}, {100}, EmptyContent{}));
    auto absolute = take_value(Clip::create_absolute({9}, {480}, 480, {48'000, 1}, EmptyContent{}));
    // A track anchors all of its clips the same way.
    auto musical_track = take_value(Track::create({6}, "musical", {musical}));
    auto absolute_track = take_value(Track::create({10}, "absolute", {absolute}));
    auto sequence = take_value(
        Sequence::create({3}, "sequence", TickDuration{4'000}, {musical_track, absolute_track}));
    return take_value(
        Project::create(ProjectInput{{1}, "project", 20, {3}, {}, {sequence}}));
}

bool has(std::span<const Concept> concepts, Concept concept_value) {
    return std::find(concepts.begin(), concepts.end(), concept_value) != concepts.end();
}

} // namespace

TEST_CASE("a plan states what an export carries and what it costs", "[interchange]") {
    SECTION("a document within the format's reach plans losslessly") {
        const ExportPlan plan = plan_export(lossless_project(), Format::DawProject);
        REQUIRE(plan.format() == Format::DawProject);
        REQUIRE(plan.is_lossless());
        REQUIRE(plan.losses().empty());
        REQUIRE(plan.required_consent().empty());
        REQUIRE(has(plan.representable(), Concept::ClipMusical));
        REQUIRE(has(plan.representable(), Concept::ClipMedia));
        REQUIRE(has(plan.representable(), Concept::TrackFlat));
    }

    SECTION("a concept the format drops is reported with its owner") {
        const ExportPlan plan = plan_export(lossy_project(), Format::DawProject);
        REQUIRE_FALSE(plan.is_lossless());

        const LossEntry* entry = plan.losses().find(Concept::ClipAbsolute);
        REQUIRE(entry != nullptr);
        REQUIRE(entry->level == ExportLevel::Drop);
        REQUIRE(entry->loss_class == LossClass::Dropped);
        REQUIRE_FALSE(entry->degraded_to.has_value());
        REQUIRE(entry->count == 1);
        REQUIRE(entry->owners.size() == 1);
        REQUIRE(entry->owners[0] == ItemId{9});
        REQUIRE_FALSE(entry->detail.empty());
    }

    SECTION("a concept the document does not use is never reported as lost") {
        const ExportPlan plan = plan_export(lossless_project(), Format::DawProject);
        REQUIRE(plan.losses().find(Concept::TakeLane) == nullptr);
        REQUIRE(plan.losses().find(Concept::TrackFreeze) == nullptr);
    }
}

TEST_CASE("an export cannot run without consent for each loss", "[interchange]") {
    const ExportPlan plan = plan_export(lossy_project(), Format::DawProject);
    REQUIRE_FALSE(plan.is_lossless());

    const FormatBoundExportWriter never_called{Format::DawProject, [](const ExportPlan&) {
        FAIL("the writer ran despite an unaccepted loss");
        return pulp::runtime::Err(ExportError{});
    }};

    SECTION("no consent refuses, naming every concept that would be lost") {
        auto result = run_export(plan, ExportOptions{}, never_called);
        REQUIRE(result.is_err());
        REQUIRE(result.error().code == ExportErrorCode::UnacceptedLoss);
        REQUIRE(result.error().concepts == plan.required_consent());
        REQUIRE(result.error().message.find("clip.absolute") != std::string::npos);
    }

    SECTION("consent is per concept, so accepting a different one does not carry") {
        ExportOptions options;
        options.accepted_losses = {Concept::TakeLane};
        auto result = run_export(plan, options, never_called);
        REQUIRE(result.is_err());
        REQUIRE(result.error().code == ExportErrorCode::UnacceptedLoss);
        REQUIRE(result.error().concepts.size() == 1);
        REQUIRE(result.error().concepts[0] == Concept::ClipAbsolute);
    }

    SECTION("accepting every loss lets the writer run") {
        ExportOptions options;
        options.accepted_losses = plan.required_consent();
        bool ran = false;
        const FormatBoundExportWriter writer{Format::DawProject, [&ran](const ExportPlan& planned) {
            ran = true;
            // The writer sees the plan it is executing, not the document.
            REQUIRE_FALSE(planned.is_lossless());
            ExportArtifacts artifacts;
            artifacts.artifacts.push_back(ExportArtifact{"project.xml", {1, 2, 3}});
            return pulp::runtime::Ok(std::move(artifacts));
        }};
        auto result = run_export(plan, options, writer);
        REQUIRE(ran);
        REQUIRE(result.has_value());
        REQUIRE(result.value().artifacts.size() == 2);
        REQUIRE(result.value().artifacts[0].name == "project.xml");
        REQUIRE(result.value().artifacts[1].name == "pulp-loss-manifest.json");
    }

    SECTION("a writer that fails on its own terms surfaces its error") {
        ExportOptions options;
        options.accepted_losses = plan.required_consent();
        const FormatBoundExportWriter writer{Format::DawProject, [](const ExportPlan&) {
            return pulp::runtime::Err(
                ExportError{ExportErrorCode::WriterFailed, "package entry rejected", {}});
        }};
        auto result = run_export(plan, options, writer);
        REQUIRE(result.is_err());
        REQUIRE(result.error().code == ExportErrorCode::WriterFailed);
    }
}

TEST_CASE("the released callable ExportWriter remains source compatible",
          "[interchange][compatibility]") {
    const ExportPlan plan = plan_export(lossless_project(), Format::DawProject);
    ExportWriter writer = [](const ExportPlan&) {
        ExportArtifacts artifacts;
        artifacts.artifacts.push_back({"legacy.bin", {1, 2, 3}});
        return pulp::runtime::Ok(std::move(artifacts));
    };

    // Both forms were public in v0.759 and remain valid for SDK consumers.
    const auto direct = writer(plan);
    REQUIRE(direct);
    REQUIRE(direct.value().artifacts.size() == 1);

    const auto gated = run_export(plan, ExportOptions{}, writer);
    REQUIRE(gated);
    REQUIRE(gated.value().artifacts.size() == 2);
    REQUIRE(gated.value().artifacts.back().name == "pulp-loss-manifest.json");
}

TEST_CASE("an export plan cannot authorize a different format's writer", "[interchange]") {
    const ExportPlan plan = plan_export(lossless_project(), Format::DawProject);
    bool ran = false;
    const FormatBoundExportWriter wrong_writer{Format::Smf, [&ran](const ExportPlan&) {
        ran = true;
        return pulp::runtime::Ok(ExportArtifacts{});
    }};

    const auto result = run_export(plan, ExportOptions{}, wrong_writer);
    REQUIRE(result.is_err());
    REQUIRE(result.error().code == ExportErrorCode::WriterFormatMismatch);
    REQUIRE_FALSE(ran);
}

TEST_CASE("run_export owns the one canonical loss manifest artifact", "[interchange]") {
    const ExportPlan plan = plan_export(lossless_project(), Format::DawProject);

    SECTION("a successful writer receives one manifest even for a lossless plan") {
        const FormatBoundExportWriter writer{Format::DawProject, [](const ExportPlan&) {
            ExportArtifacts artifacts;
            artifacts.artifacts.push_back({"project.xml", {1}});
            return pulp::runtime::Ok(std::move(artifacts));
        }};
        auto result = run_export(plan, ExportOptions{}, writer);
        REQUIRE(result);
        REQUIRE(result.value().artifacts.size() == 2);
        REQUIRE(result.value().artifacts[1].name == "pulp-loss-manifest.json");
        const auto& bytes = result.value().artifacts[1].bytes;
        const std::string json(bytes.begin(), bytes.end());
        REQUIRE(json.find("\"schema_version\":1") != std::string::npos);
        REQUIRE(json.find("\"format\":\"dawproject\"") != std::string::npos);
        REQUIRE(json.find("\"lossless\":true") != std::string::npos);
    }

    SECTION("the reserved manifest name cannot be forged by a format writer") {
        const FormatBoundExportWriter writer{Format::DawProject, [](const ExportPlan&) {
            ExportArtifacts artifacts;
            artifacts.artifacts.push_back({"pulp-loss-manifest.json", {1}});
            return pulp::runtime::Ok(std::move(artifacts));
        }};
        auto result = run_export(plan, ExportOptions{}, writer);
        REQUIRE_FALSE(result);
        REQUIRE(result.error().code == ExportErrorCode::WriterFailed);
        REQUIRE(result.error().message.find("reserved artifact") != std::string::npos);
    }
}

TEST_CASE("manifest owner ids remain exact beyond the JSON integer range", "[interchange]") {
    constexpr std::uint64_t kLargeOwner = 9'007'199'254'740'993ULL;
    auto absolute = take_value(
        Clip::create_absolute({kLargeOwner}, {0}, 480, {48'000, 1}, EmptyContent{}));
    auto track = take_value(Track::create({2}, "absolute", {std::move(absolute)}));
    auto sequence = take_value(
        Sequence::create({3}, "root", TickDuration{1'000}, {std::move(track)}));
    const Project project = take_value(Project::create(ProjectInput{
        {1}, "large owner", kLargeOwner + 1, {3}, {}, {std::move(sequence)}}));
    const ExportPlan plan = plan_export(project, Format::DawProject);
    ExportOptions options;
    options.accepted_losses = plan.required_consent();
    const FormatBoundExportWriter writer{Format::DawProject, [](const ExportPlan&) {
        ExportArtifacts artifacts;
        artifacts.artifacts.push_back({"project.xml", {1}});
        return pulp::runtime::Ok(std::move(artifacts));
    }};

    const auto result = run_export(plan, options, writer);
    REQUIRE(result);
    const auto& bytes = result.value().artifacts.back().bytes;
    const std::string json(bytes.begin(), bytes.end());
    REQUIRE(json.find("\"count\":\"1\"") != std::string::npos);
    REQUIRE(json.find("\"owners\":[\"9007199254740993\"]") != std::string::npos);
}

TEST_CASE("an export plan owns the document snapshot it measured", "[interchange]") {
    Project source = lossless_project();
    const ExportPlan plan = plan_export(source, Format::DawProject);
    source = lossy_project();

    REQUIRE(plan.project().assets().size() == 1);
    REQUIRE(plan.census().contains(Concept::ClipMedia));
    REQUIRE_FALSE(plan.census().contains(Concept::ClipAbsolute));
}

TEST_CASE("a lossless plan still needs a writer to produce bytes", "[interchange]") {
    const ExportPlan plan = plan_export(lossless_project(), Format::DawProject);
    REQUIRE(plan.is_lossless());

    // No writer means no artifact, rather than an empty one reported as success.
    // DAWproject now declares a writer, but passing an empty bound writer must
    // still refuse — the callable is what produces bytes, and the capability
    // flag does not stand in for one.
    REQUIRE(format_has_writer(Format::DawProject));
    auto result = run_export(plan, ExportOptions{}, FormatBoundExportWriter{});
    REQUIRE(result.is_err());
    REQUIRE(result.error().code == ExportErrorCode::NoWriterRegistered);
    REQUIRE(result.error().message.find("DAWproject") != std::string::npos);
}

TEST_CASE("a loss manifest names its loss classes durably", "[interchange]") {
    // These ids are written into exported artifacts, so they are a compatibility
    // surface, not a debug string.
    REQUIRE(loss_class_id(LossClass::Dropped) == "dropped");
    REQUIRE(loss_class_id(LossClass::Degraded) == "degraded");
    REQUIRE(loss_class_id(LossClass::Flattened) == "flattened");
    REQUIRE(loss_class_id(LossClass::Approximated) == "approximated");

    const ExportPlan plan = plan_export(lossy_project(), Format::DawProject);
    const LossEntry* entry = plan.losses().find(Concept::ClipAbsolute);
    REQUIRE(entry != nullptr);
    REQUIRE(loss_class_id(entry->loss_class) == "dropped");
}
