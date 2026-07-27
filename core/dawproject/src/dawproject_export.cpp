#include <pulp/dawproject/dawproject_export.hpp>

#include <pulp/timeline/assets.hpp>

#include <pugixml.hpp>

#include <algorithm>
#include <array>
#include <cstdio>
#include <sstream>
#include <string>
#include <variant>
#include <vector>

namespace pulp::dawproject {
namespace {

using interchange::Concept;

/// Every concept this writer actually emits. The capability table declares what
/// a writer MUST do; this array declares what this one DOES. The static_assert
/// below ties them together, so widening a row in `dawproject.json` without
/// writing the code stops the build instead of shipping a table that promises
/// more than the writer delivers.
///
/// This is the mirror of the importer's `kImplementedImports` assertion: there,
/// a table may not admit a construct the reader cannot parse; here, a table may
/// not promise a construct the writer cannot emit.
constexpr std::array kImplementedExports{
    Concept::TrackFlat,   Concept::ClipMusical,        Concept::ClipNote,
    Concept::ClipMedia,   Concept::TempoSingle,        Concept::MeterSingle,
    Concept::AssetSealedHash, Concept::AssetReferencedMedia,
};

constexpr bool implemented(Concept concept_value) noexcept {
    return std::find(kImplementedExports.begin(), kImplementedExports.end(), concept_value) !=
           kImplementedExports.end();
}

// Every concept the table rates Full for DAWproject export must be one this
// writer emits. A row widened to Full without writer code fails here.
static_assert([] {
    for (std::size_t index = 0; index < interchange::kConceptCount; ++index) {
        const auto concept_value = static_cast<Concept>(index);
        if (interchange::export_capability(interchange::Format::DawProject, concept_value).level ==
                interchange::ExportLevel::Full &&
            !implemented(concept_value))
            return false;
    }
    return true;
}(), "a DAWproject export row is rated Full but this writer does not emit it");

/// DAWproject measures arrangement time in beats; Pulp measures it in canonical
/// ticks. One quarter note is one beat, so the conversion is exact division by
/// the tick constant rather than a rate-dependent computation.
double beats(timebase::TickPosition position) noexcept {
    return static_cast<double>(position.value) / static_cast<double>(timebase::kTicksPerQuarter);
}

double beats(timebase::TickDuration duration) noexcept {
    return static_cast<double>(duration.value) / static_cast<double>(timebase::kTicksPerQuarter);
}

/// Fixed-precision so the output is byte-stable across platforms: a golden
/// comparison of project.xml would otherwise drift on locale or default
/// formatting. Trailing zeros are kept rather than trimmed for the same reason.
std::string number(double value) {
    std::array<char, 64> buffer{};
    const int written = std::snprintf(buffer.data(), buffer.size(), "%.6f", value);
    if (written <= 0)
        return "0.000000";
    return std::string(buffer.data(), static_cast<std::size_t>(written));
}

std::string element_id(std::string_view prefix, timeline::ItemId id) {
    return std::string(prefix) + "-" + std::to_string(id.value);
}

/// Which DAWproject contentType a track declares. A track holding note clips is
/// "notes"; anything else is "audio". Mixed content cannot occur here because
/// the plan refuses any concept this writer does not emit.
bool track_holds_notes(const timeline::Track& track) noexcept {
    for (const timeline::Clip& clip : track.clips())
        if (std::holds_alternative<timeline::NoteContent>(clip.content()))
            return true;
    return false;
}

const timeline::MediaAsset* find_asset(const timeline::Project& project, timeline::ItemId id) {
    for (const timeline::MediaAsset& asset : project.assets())
        if (asset.id == id)
            return &asset;
    return nullptr;
}

void write_clip(pugi::xml_node clips, const timeline::Project& project,
                const timeline::Clip& clip) {
    auto node = clips.append_child("Clip");
    node.append_attribute("time") = number(beats(clip.start())).c_str();
    node.append_attribute("duration") = number(beats(clip.duration())).c_str();
    node.append_attribute("name") = element_id("clip", clip.id()).c_str();

    if (const auto* notes = std::get_if<timeline::NoteContent>(&clip.content())) {
        auto notes_node = node.append_child("Notes");
        for (const timeline::NoteEvent& note : notes->notes()) {
            auto note_node = notes_node.append_child("Note");
            note_node.append_attribute("time") = number(beats(note.start)).c_str();
            note_node.append_attribute("duration") = number(beats(note.duration)).c_str();
            note_node.append_attribute("channel") = static_cast<int>(note.channel);
            note_node.append_attribute("key") = static_cast<int>(note.pitch);
            // DAWproject velocity is normalised; Pulp stores 16-bit.
            note_node.append_attribute("vel") =
                number(static_cast<double>(note.velocity) / 65535.0).c_str();
        }
        return;
    }

    if (const auto* media = std::get_if<timeline::MediaRef>(&clip.content())) {
        const auto* asset = find_asset(project, media->asset_id);
        auto audio = node.append_child("Audio");
        audio.append_attribute("algorithm") = "stretch";
        audio.append_attribute("channels") = 2;
        audio.append_attribute("duration") = number(beats(clip.duration())).c_str();
        audio.append_attribute("sampleRate") =
            asset ? static_cast<int>(asset->sample_rate.numerator) : 48'000;
        auto file = audio.append_child("File");
        // The container places media under audio/; the asset's own name is the
        // package-relative leaf the importer expects to resolve.
        file.append_attribute("path") =
            ("audio/" + (asset ? asset->name : std::string("unknown.wav"))).c_str();
    }
}

std::vector<std::uint8_t> to_bytes(std::string_view text) {
    return std::vector<std::uint8_t>(text.begin(), text.end());
}

/// The loss record, carried into the package rather than left behind in the
/// caller's console. An export whose manifest does not travel with it is an
/// export whose losses are invisible to whoever opens the file next.
std::string manifest_json(const interchange::ExportPlan& plan) {
    std::ostringstream out;
    out << "{\"format\":\"dawproject\",\"lossless\":" << (plan.is_lossless() ? "true" : "false")
        << ",\"losses\":[";
    bool first = true;
    for (const interchange::LossEntry& entry : plan.losses().entries()) {
        if (!first)
            out << ',';
        first = false;
        out << "{\"concept\":\"" << interchange::concept_id(entry.concept_value) << "\",\"class\":\""
            << interchange::loss_class_id(entry.loss_class) << "\",\"count\":" << entry.count
            << ",\"detail\":\"" << entry.detail << "\"}";
    }
    out << "]}";
    return out.str();
}

} // namespace

interchange::ExportWriter writer(const timeline::Project& project, const ExportOptions& options) {
    return [&project, options](const interchange::ExportPlan& plan)
               -> runtime::Result<interchange::ExportArtifacts, interchange::ExportError> {
        const timeline::Sequence* sequence = project.find_sequence(project.root_sequence_id());
        if (sequence == nullptr)
            return runtime::Err(interchange::ExportError{
                interchange::ExportErrorCode::WriterFailed,
                "the project names a root sequence that does not exist",
                {}});

        pugi::xml_document doc;
        auto declaration = doc.append_child(pugi::node_declaration);
        declaration.append_attribute("version") = "1.0";
        declaration.append_attribute("encoding") = "UTF-8";

        auto root = doc.append_child("Project");
        root.append_attribute("version") = "1.0";

        auto application = root.append_child("Application");
        application.append_attribute("name") = options.application_name.c_str();
        application.append_attribute("version") = options.application_version.c_str();

        // A single tempo and a single time signature: the plan has already
        // refused (or the caller has already accepted losing) anything richer.
        auto transport = root.append_child("Transport");
        auto tempo = transport.append_child("Tempo");
        tempo.append_attribute("unit") = "bpm";
        const auto tempo_points = project.tempo_map().points();
        tempo.append_attribute("value") =
            number(tempo_points.empty() ? 120.0 : tempo_points.front().bpm).c_str();
        auto signature = transport.append_child("TimeSignature");
        const auto meter_points = project.meter_map().points();
        const auto meter = meter_points.empty() ? timebase::MeterSignature{4, 4}
                                                : meter_points.front().signature;
        signature.append_attribute("numerator") = static_cast<int>(meter.numerator);
        signature.append_attribute("denominator") = static_cast<int>(meter.denominator);

        auto structure = root.append_child("Structure");
        auto arrangement = root.append_child("Arrangement");
        arrangement.append_attribute("id") = element_id("arrangement", sequence->id()).c_str();
        auto lanes = arrangement.append_child("Lanes");
        lanes.append_attribute("timeUnit") = "beats";
        lanes.append_attribute("id") = "lanes-root";

        for (const timeline::Track& track : sequence->tracks()) {
            const std::string track_id = element_id("track", track.id());
            auto track_node = structure.append_child("Track");
            track_node.append_attribute("contentType") =
                track_holds_notes(track) ? "notes" : "audio";
            track_node.append_attribute("loaded") = "true";
            track_node.append_attribute("id") = track_id.c_str();
            track_node.append_attribute("name") = track.name().c_str();

            auto track_lanes = lanes.append_child("Lanes");
            track_lanes.append_attribute("track") = track_id.c_str();
            track_lanes.append_attribute("id") = element_id("lanes", track.id()).c_str();
            auto clips = track_lanes.append_child("Clips");
            clips.append_attribute("id") = element_id("clips", track.id()).c_str();
            for (const timeline::Clip& clip : track.clips())
                write_clip(clips, project, clip);
        }

        std::ostringstream xml;
        doc.save(xml, "  ");

        interchange::ExportArtifacts artifacts;
        artifacts.artifacts.push_back({"project.xml", to_bytes(xml.str())});
        artifacts.artifacts.push_back({"pulp-loss-manifest.json", to_bytes(manifest_json(plan))});
        return runtime::Ok(std::move(artifacts));
    };
}

} // namespace pulp::dawproject
