#include <catch2/catch_test_macros.hpp>

#include <choc/text/choc_JSON.h>

#include <pulp/interchange/export_plan.hpp>
#include <pulp/smf/interchange.hpp>
#include <pulp/timebase/compiled_tempo_map.hpp>
#include <pulp/timebase/tick.hpp>
#include <pulp/timeline/assets.hpp>
#include <pulp/timeline/note_modifier.hpp>
#include <pulp/timeline/schema_registry.hpp>
#include <pulp/timeline/smf.hpp>

#include <algorithm>
#include <array>
#include <memory>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

using namespace pulp;
using namespace pulp::interchange;
using namespace pulp::timeline;
using namespace pulp::timebase;

namespace {

constexpr std::int64_t kQuarter = kTicksPerQuarter;

template <typename T> T take(runtime::Result<T, ModelError> result) {
    REQUIRE(result);
    return std::move(result).value();
}

ContentHash hash() { return *ContentHash::from_hex(std::string(64, 'a')); }

runtime::Result<std::shared_ptr<const void>, PersistenceError>
decode_registered_test_value(const JsonValue&, const void*) noexcept {
    std::shared_ptr<const void> value = std::make_shared<const int>(42);
    return runtime::Ok(std::move(value));
}

runtime::Result<SchemaWriteSuccess, PersistenceError>
encode_registered_test_value(const std::shared_ptr<const void>& value,
                             BoundedJsonSink& output, const void*) noexcept {
    output.append("{\"value\":");
    output.append(std::to_string(*static_cast<const int*>(value.get())));
    output.append("}");
    return runtime::Ok(SchemaWriteSuccess{});
}

std::size_t retained_registered_test_value(const std::shared_ptr<const void>&,
                                           const void*) noexcept {
    return sizeof(int);
}

SchemaRegistry registered_test_registry() {
    SchemaRegistryBuilder builder;
    TypeSchema schema;
    schema.type_name = "vendor.timeline.smf_test";
    schema.domain = SchemaDomain::Content;
    schema.current_version = 1;
    schema.fields = {{"value", SchemaValueKind::U32}};
    schema.codec = {{}, decode_registered_test_value, encode_registered_test_value,
                    retained_registered_test_value};
    REQUIRE(builder.register_type(std::move(schema)));
    auto registry = std::move(builder).build();
    REQUIRE(registry);
    return std::move(registry).value();
}

MidiContent notes(ItemId note_id, bool modified, std::uint16_t velocity = 0xffffu) {
    NoteEvent note{note_id, TickPosition{0}, TickDuration{kQuarter}, velocity, 60, 0};
    std::vector<NoteModifier> modifiers;
    if (modified) {
        NoteModifier modifier;
        modifier.note_id = note_id;
        modifier.probability = 0;
        modifiers.push_back(modifier);
    }
    return take(MidiContent::create({note}, std::move(modifiers), 0));
}

Project lossless_project(std::uint16_t velocity = 0xffffu) {
    auto clip = take(Clip::create({2}, {0}, {kQuarter}, notes({1}, false, velocity)));
    auto track = take(Track::create({3}, "notes", {std::move(clip)}));
    auto sequence = take(Sequence::create({4}, "root", TickDuration{2 * kQuarter},
                                          {std::move(track)}));
    return take(Project::create(ProjectInput{{5}, "lossless", 10, {4}, {},
                                             {std::move(sequence)}}));
}

Project lossy_project() {
    auto note = take(Clip::create({2}, {0}, {kQuarter}, notes({1}, true, 40'000)));
    auto empty = take(Clip::create({3}, {2 * kQuarter}, {kQuarter}, EmptyContent{}));
    auto media = take(Clip::create({4}, {4 * kQuarter}, {kQuarter},
                                   MediaRef{{30}, {0}, 100}));
    auto musical = take(Track::create({5}, "musical",
                                      {std::move(note), std::move(empty), std::move(media)}));

    auto absolute_note = take(Clip::create_absolute(
        {7}, {0}, 48'000, {48'000, 1}, notes({6}, false)));
    auto absolute = take(Track::create({8}, "absolute", {std::move(absolute_note)}));

    auto opaque_content = take(OpaqueContent::create(
        {"vendor.future", 7},
        R"({"data":{"answer":42},"type_name":"vendor.future","version":7})"));
    auto opaque = take(Clip::create({13}, {2 * kQuarter}, {kQuarter},
                                    std::move(opaque_content)));
    auto nested = take(Clip::create({14}, {3 * kQuarter}, {kQuarter},
                                    SequenceRef{{10}, TickPosition{0}}));
    auto extensions = take(Track::create(
        {15}, "extensions", {std::move(opaque), std::move(nested)}));

    auto root = take(Sequence::create(SequenceInput{
        .id = {9},
        .name = "root",
        .musical_duration = TickDuration{8 * kQuarter},
        .tracks = {std::move(musical), std::move(absolute), std::move(extensions)},
        .markers = {SequenceMarker{{12}, "marker", TickPosition{kQuarter}, {}}},
    }));
    auto other = take(Sequence::create({10}, "other", TickDuration{kQuarter}, {}));
    const std::array tempo_points{
        TempoPoint{TickPosition{0}, 100.0, TempoCurve::LinearInTicks},
        TempoPoint{TickPosition{4 * kQuarter}, 140.0, TempoCurve::Constant}};
    auto tempo = TempoMap::create(tempo_points);
    REQUIRE(tempo);

    ProjectInput input{
        {11},
        "lossy",
        40,
        {9},
        {MediaAsset{{30}, "audio.wav", 1'000, {48'000, 1}, hash(),
                    AssetStoragePolicy::External, {}, {}, {}}},
        {std::move(root), std::move(other)},
    };
    input.tempo_map = std::move(tempo.value());
    return take(Project::create(std::move(input)));
}

const ExportArtifact& artifact(const ExportArtifacts& artifacts, std::string_view name) {
    const auto found = std::find_if(artifacts.artifacts.begin(), artifacts.artifacts.end(),
                                    [&](const ExportArtifact& value) {
                                        return value.name == name;
                                    });
    REQUIRE(found != artifacts.artifacts.end());
    return *found;
}

std::size_t note_count(const Project& project) {
    std::size_t count = 0;
    const Sequence* root = project.find_sequence(project.root_sequence_id());
    REQUIRE(root != nullptr);
    for (const Track& track : root->tracks())
        for (const Clip& clip : track.clips())
            if (const auto* content = std::get_if<MidiContent>(&clip.content()))
                count += content->notes().size();
    return count;
}

} // namespace

TEST_CASE("SMF interchange keeps every declared loss behind exact consent",
          "[interchange][smf]") {
    const Project project = lossy_project();
    const ExportPlan plan = plan_export(project, Format::Smf);
    REQUIRE_FALSE(plan.is_lossless());
    for (Concept concept_value :
         {Concept::ClipEmpty, Concept::ClipMedia, Concept::ClipAbsolute,
          Concept::ClipNoteModifier, Concept::Marker, Concept::AssetSealedHash,
          Concept::AssetReferencedMedia, Concept::SequenceMultiple,
          Concept::TempoRamp, Concept::ClipNoteVelocityQuantized})
        REQUIRE(plan.losses().find(concept_value) != nullptr);

    REQUIRE_FALSE(export_smf(project));

    SECTION("zero or partial consent refuses before the adapter executes") {
        auto refused = run_export(plan, ExportOptions{}, smf::writer());
        REQUIRE_FALSE(refused);
        REQUIRE(refused.error().code == ExportErrorCode::UnacceptedLoss);

        ExportOptions partial;
        partial.accepted_losses = plan.required_consent();
        partial.accepted_losses.erase(
            std::remove(partial.accepted_losses.begin(), partial.accepted_losses.end(),
                        Concept::TempoRamp),
            partial.accepted_losses.end());
        refused = run_export(plan, partial, smf::writer());
        REQUIRE_FALSE(refused);
        REQUIRE(refused.error().concepts == std::vector<Concept>{Concept::TempoRamp});
    }

    SECTION("exact consent exports the surviving notes and a complete manifest") {
        ExportOptions accepted;
        accepted.accepted_losses = plan.required_consent();
        auto exported = run_export(plan, accepted, smf::writer());
        REQUIRE(exported);
        REQUIRE(exported.value().artifacts.size() == 2);

        const auto& midi = artifact(exported.value(), "project.mid");
        auto imported = import_smf(midi.bytes);
        REQUIRE(imported);
        REQUIRE(note_count(imported.value().project) == 1);
        REQUIRE(imported.value().project.tempo_map().points().size() == 2);
        REQUIRE(imported.value().project.tempo_map().points()[0].curve_to_next ==
                TempoCurve::Constant);

        const auto& manifest = artifact(exported.value(), "pulp-loss-manifest.json");
        const std::string json(manifest.bytes.begin(), manifest.bytes.end());
        const auto parsed = choc::json::parse(json);
        REQUIRE(parsed.isObject());
        REQUIRE(parsed["schema_version"].getInt64() == 1);
        REQUIRE(parsed["format"].getString() == "smf");
        REQUIRE_FALSE(parsed["lossless"].getBool());
        REQUIRE(parsed["losses"].isArray());
        REQUIRE(parsed["losses"].size() == plan.losses().entries().size());
        for (std::uint32_t index = 0; index < parsed["losses"].size(); ++index) {
            const auto entry = parsed["losses"][index];
            REQUIRE(entry["count"].isString());
            REQUIRE(entry["owners"].isArray());
            for (std::uint32_t owner = 0; owner < entry["owners"].size(); ++owner)
                REQUIRE(entry["owners"][owner].isString());
        }
        REQUIRE(json.find("\"schema_version\":1") != std::string::npos);
        REQUIRE(json.find("\"format\":\"smf\"") != std::string::npos);
        REQUIRE(json.find("\"concept\":\"tempo.ramp\"") != std::string::npos);
        REQUIRE(json.find("\"level\":\"degrade\"") != std::string::npos);
        REQUIRE(json.find("\"class\":\"approximated\"") != std::string::npos);
        REQUIRE(json.find("\"degraded_to\":\"tempo.map\"") != std::string::npos);
        REQUIRE(json.find("\"count\":\"1\"") != std::string::npos);
        REQUIRE(json.find("\"owners\":[\"11\"]") != std::string::npos);
        for (Concept concept_value : plan.required_consent())
            REQUIRE(json.find(std::string(concept_id(concept_value))) != std::string::npos);
    }
}

TEST_CASE("SMF interchange makes low velocity quantization explicit",
          "[interchange][smf]") {
    const Project project = lossless_project(1);
    const ExportPlan plan = plan_export(project, Format::Smf);
    REQUIRE(plan.required_consent() ==
            std::vector<Concept>{Concept::ClipNoteVelocityQuantized});
    REQUIRE_FALSE(export_smf(project));

    ExportOptions options;
    options.accepted_losses = plan.required_consent();
    auto exported = run_export(plan, options, smf::writer());
    REQUIRE(exported);
    auto imported = import_smf(artifact(exported.value(), "project.mid").bytes);
    REQUIRE(imported);
    const auto& clip = imported.value().project.sequences()[0].tracks()[0].clips()[0];
    REQUIRE(std::get<MidiContent>(clip.content()).notes()[0].velocity == 516);
}

TEST_CASE("SMF interchange drops registered content only after exact consent",
          "[interchange][smf]") {
    const auto registry = registered_test_registry();
    auto created = registry.create_registered_no_owned_ids(
        {"vendor.timeline.smf_test", 1}, std::make_shared<const int>(42), 1024);
    REQUIRE(created);
    auto clip = take(Clip::create({2}, {0}, {kQuarter}, std::move(created).value()));
    auto track = take(Track::create({3}, "registered", {std::move(clip)}));
    auto sequence = take(Sequence::create({4}, "root", TickDuration{kQuarter},
                                          {std::move(track)}));
    const Project project = take(Project::create(
        ProjectInput{{5}, "registered", 10, {4}, {}, {std::move(sequence)}}));
    const ExportPlan plan = plan_export(project, Format::Smf);
    REQUIRE(plan.required_consent() == std::vector<Concept>{Concept::ContentRegistered});
    REQUIRE_FALSE(export_smf(project));

    auto refused = run_export(plan, ExportOptions{}, smf::writer());
    REQUIRE_FALSE(refused);
    REQUIRE(refused.error().code == ExportErrorCode::UnacceptedLoss);

    ExportOptions accepted;
    accepted.accepted_losses = plan.required_consent();
    auto exported = run_export(plan, accepted, smf::writer());
    REQUIRE(exported);
    auto imported = import_smf(artifact(exported.value(), "project.mid").bytes);
    REQUIRE(imported);
    REQUIRE(note_count(imported.value().project) == 0);
}

TEST_CASE("SMF lossless success still carries the canonical manifest",
          "[interchange][smf]") {
    const ExportPlan plan = plan_export(lossless_project(), Format::Smf);
    REQUIRE(plan.is_lossless());
    auto exported = run_export(plan, ExportOptions{}, smf::writer());
    REQUIRE(exported);
    const auto& manifest = artifact(exported.value(), "pulp-loss-manifest.json");
    const std::string json(manifest.bytes.begin(), manifest.bytes.end());
    REQUIRE_NOTHROW(choc::json::parse(json));
    REQUIRE(json.find("\"lossless\":true") != std::string::npos);
    REQUIRE(json.find("\"losses\":[]") != std::string::npos);
}
