#include <catch2/catch_test_macros.hpp>

#include <pulp/interchange/capability.hpp>
#include <pulp/interchange/concept.hpp>

#include <string_view>

using namespace pulp::interchange;

TEST_CASE("concept vocabulary is a closed, self-describing set", "[interchange]") {
    // Concept is an installed public enum. New vocabulary atoms append after
    // every existing ordinal rather than renumbering downstream binaries.
    STATIC_REQUIRE(static_cast<std::uint16_t>(Concept::ClipCrossfade) == 10);
    STATIC_REQUIRE(static_cast<std::uint16_t>(Concept::ContextGroove) == 42);
    STATIC_REQUIRE(static_cast<std::uint16_t>(Concept::ClipNoteModifier) == 43);

    SECTION("Unknown is the zero value so an unclassified construct refuses by default") {
        REQUIRE(static_cast<std::size_t>(Concept::Unknown) == 0);
        REQUIRE(concept_id(Concept::Unknown) == "unknown");
    }

    SECTION("every concept has a distinct id and a summary") {
        for (std::size_t index = 0; index < kConceptCount; ++index) {
            const auto concept_value = static_cast<Concept>(index);
            REQUIRE_FALSE(concept_id(concept_value).empty());
            REQUIRE_FALSE(concept_summary(concept_value).empty());
            for (std::size_t other = index + 1; other < kConceptCount; ++other)
                REQUIRE(concept_id(concept_value) != concept_id(static_cast<Concept>(other)));
        }
    }

    SECTION("ids round-trip, and an unrecognized id resolves to Unknown") {
        for (std::size_t index = 0; index < kConceptCount; ++index) {
            const auto concept_value = static_cast<Concept>(index);
            REQUIRE(concept_from_id(concept_id(concept_value)) == concept_value);
        }
        REQUIRE(concept_from_id("clip.telepathy") == Concept::Unknown);
        REQUIRE(concept_from_id("") == Concept::Unknown);
    }
}

TEST_CASE("the capability world is closed: undeclared means refused", "[interchange]") {
    SECTION("a concept no format declares is never importable") {
        // Nothing in the committed tables declares a video track or a nested
        // sequence, so the generated rows must refuse them everywhere.
        for (std::size_t index = 0; index < kFormatCount; ++index) {
            const auto format = static_cast<Format>(index);
            REQUIRE_FALSE(import_supports(format, Concept::TrackVideo));
            REQUIRE_FALSE(import_supports(format, Concept::SequenceNested));
            REQUIRE_FALSE(import_supports(format, Concept::Unknown));
        }
    }

    SECTION("every concept has a row for every format") {
        // The generator materializes the whole matrix, so a lookup is total and
        // no runtime code has to decide what an absent row means.
        for (std::size_t format_index = 0; format_index < kFormatCount; ++format_index) {
            const auto format = static_cast<Format>(format_index);
            for (std::size_t index = 0; index < kConceptCount; ++index) {
                const auto concept_value = static_cast<Concept>(index);
                const ImportRow& imported = import_capability(format, concept_value);
                if (imported.level == ImportLevel::None)
                    REQUIRE(imported.notes.empty());
                else
                    REQUIRE(imported.refusal.empty());

                const ExportRow& exported = export_capability(format, concept_value);
                if (exported.level == ExportLevel::Degrade)
                    REQUIRE_FALSE(exported.loss.empty());
                if (exported.level == ExportLevel::Full)
                    REQUIRE(exported.loss.empty());
            }
        }
    }

    SECTION("an undeclared export concept is a loss, not a silent pass") {
        REQUIRE_FALSE(export_is_lossless(Format::DawProject, Concept::Marker));
        REQUIRE(export_capability(Format::DawProject, Concept::Marker).level == ExportLevel::Drop);
        REQUIRE_FALSE(export_capability(Format::DawProject, Concept::Marker).loss.empty());
    }
}

TEST_CASE("the DAWproject table declares the importer's documented subset", "[interchange]") {
    SECTION("the subset the reader implements is importable") {
        REQUIRE(import_supports(Format::DawProject, Concept::TrackFlat));
        REQUIRE(import_supports(Format::DawProject, Concept::ClipMusical));
        REQUIRE(import_supports(Format::DawProject, Concept::ClipNote));
        REQUIRE(import_supports(Format::DawProject, Concept::ClipMedia));
        REQUIRE(import_supports(Format::DawProject, Concept::TempoSingle));
        REQUIRE(import_supports(Format::DawProject, Concept::MeterSingle));
    }

    SECTION("everything outside it refuses, each naming what it refuses") {
        for (Concept concept_value :
             {Concept::TrackGroup, Concept::ClipWarp, Concept::ClipAbsolute, Concept::ClipLaunch,
              Concept::Marker, Concept::TempoMap, Concept::MeterMap}) {
            REQUIRE_FALSE(import_supports(Format::DawProject, concept_value));
            REQUIRE_FALSE(import_capability(Format::DawProject, concept_value).refusal.empty());
        }
    }

    SECTION("format identity round-trips and no writer is registered yet") {
        Format resolved{};
        REQUIRE(format_from_id("dawproject", resolved));
        REQUIRE(resolved == Format::DawProject);
        REQUIRE_FALSE(format_from_id("aaf", resolved));
        REQUIRE(format_display_name(Format::DawProject) == "DAWproject");
        REQUIRE_FALSE(format_has_writer(Format::DawProject));
    }
}
