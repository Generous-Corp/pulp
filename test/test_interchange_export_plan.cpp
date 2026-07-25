#include <catch2/catch_test_macros.hpp>

#include <pulp/interchange/export_plan.hpp>
#include <pulp/timeline/model.hpp>

#include <algorithm>
#include <string>
#include <vector>

using namespace pulp::interchange;
using namespace pulp::timeline;
using namespace pulp::timebase;

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

    SECTION("a concept the format degrades is reported with what it becomes") {
        const ExportPlan plan = plan_export(lossy_project(), Format::DawProject);
        REQUIRE_FALSE(plan.is_lossless());

        const LossEntry* entry = plan.losses().find(Concept::ClipAbsolute);
        REQUIRE(entry != nullptr);
        REQUIRE(entry->level == ExportLevel::Degrade);
        REQUIRE(entry->loss_class == LossClass::Approximated);
        REQUIRE(entry->degraded_to.has_value());
        REQUIRE(*entry->degraded_to == Concept::ClipMusical);
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

    const ExportWriter never_called = [](const ExportPlan&) {
        FAIL("the writer ran despite an unaccepted loss");
        return pulp::runtime::Err(ExportError{});
    };

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

    SECTION("accepting every loss gets past consent") {
        ExportOptions options;
        options.accepted_losses = plan.required_consent();
        bool ran = false;
        const ExportWriter writer = [&ran](const ExportPlan&) {
            ran = true;
            return pulp::runtime::Ok(ExportArtifacts{});
        };
        // The format declares no writer, so this still refuses -- but for the
        // writer's absence, not for consent.
        auto result = run_export(plan, options, writer);
        REQUIRE(result.is_err());
        REQUIRE(result.error().code == ExportErrorCode::NoWriterRegistered);
        REQUIRE_FALSE(ran);
    }
}

TEST_CASE("a lossless plan still needs a writer to produce bytes", "[interchange]") {
    const ExportPlan plan = plan_export(lossless_project(), Format::DawProject);
    REQUIRE(plan.is_lossless());

    // Phase-0 state: the capability data declares no DAWproject writer, so every
    // export refuses at the last step rather than silently emitting nothing.
    REQUIRE_FALSE(format_has_writer(Format::DawProject));
    auto result = run_export(plan, ExportOptions{}, ExportWriter{});
    REQUIRE(result.is_err());
    REQUIRE(result.error().code == ExportErrorCode::NoWriterRegistered);
    REQUIRE(result.error().message.find("DAWproject") != std::string::npos);
}
