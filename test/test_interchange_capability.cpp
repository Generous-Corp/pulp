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
    STATIC_REQUIRE(static_cast<std::uint16_t>(Concept::ClipEmpty) == 44);
    STATIC_REQUIRE(static_cast<std::uint16_t>(Concept::TempoRamp) == 45);
    STATIC_REQUIRE(static_cast<std::uint16_t>(Concept::ClipNoteVelocityQuantized) == 46);
    STATIC_REQUIRE(static_cast<std::uint16_t>(Concept::TempoValueQuantized) == 47);

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

TEST_CASE("the SMF table declares its bounded reader and writer", "[interchange][smf]") {
    STATIC_REQUIRE(static_cast<std::uint8_t>(Format::DawProject) == 0);
    STATIC_REQUIRE(static_cast<std::uint8_t>(Format::Smf) == 1);
    STATIC_REQUIRE(kFormatCount == 2);

    Format resolved{};
    REQUIRE(format_from_id("smf", resolved));
    REQUIRE(resolved == Format::Smf);
    REQUIRE(format_display_name(Format::Smf) == "Standard MIDI File");
    REQUIRE(format_has_writer(Format::Smf));

    for (Concept concept_value : {Concept::TrackFlat, Concept::ClipNote})
        REQUIRE(import_capability(Format::Smf, concept_value).level == ImportLevel::Full);
    for (Concept concept_value : {Concept::ClipMusical, Concept::TempoSingle,
                                  Concept::TempoMap, Concept::MeterSingle,
                                  Concept::MeterMap})
        REQUIRE(import_capability(Format::Smf, concept_value).level ==
                ImportLevel::Partial);

    for (Concept concept_value :
         {Concept::TrackFlat, Concept::ClipMusical, Concept::ClipNote,
          Concept::TempoSingle, Concept::TempoMap, Concept::MeterSingle,
          Concept::MeterMap})
        REQUIRE(export_capability(Format::Smf, concept_value).level == ExportLevel::Full);
    REQUIRE(export_capability(Format::Smf, Concept::ClipMedia).level == ExportLevel::Drop);
    REQUIRE(export_capability(Format::Smf, Concept::ClipNoteModifier).level ==
            ExportLevel::Degrade);
    REQUIRE(export_capability(Format::Smf, Concept::ClipNoteModifier).degrade_to ==
            Concept::ClipNote);
    REQUIRE(export_capability(Format::Smf, Concept::TempoRamp).level ==
            ExportLevel::Degrade);
    REQUIRE(export_capability(Format::Smf, Concept::TempoRamp).degrade_to ==
            Concept::TempoMap);
    REQUIRE(export_capability(Format::Smf, Concept::ClipNoteVelocityQuantized).level ==
            ExportLevel::Degrade);
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
        REQUIRE(export_capability(Format::DawProject, Concept::AssetEmbeddedMedia).level ==
                ExportLevel::Drop);
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

    SECTION("format identity round-trips and a writer is registered") {
        Format resolved{};
        REQUIRE(format_from_id("dawproject", resolved));
        REQUIRE(resolved == Format::DawProject);
        REQUIRE_FALSE(format_from_id("aaf", resolved));
        REQUIRE(format_display_name(Format::DawProject) == "DAWproject");
        // core/dawproject ships a bounded writer, so the table declares one.
        // A format without writer code must still read false here — that flag
        // is what makes run_export refuse rather than emit an empty artifact.
        REQUIRE(format_has_writer(Format::DawProject));
    }
}
