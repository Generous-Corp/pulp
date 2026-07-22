#include <pulp/timeline/dawproject_import.hpp>

#include <pulp/runtime/crypto.hpp>
#include <pulp/timebase/rational_time.hpp>
#include <pulp/timebase/tick.hpp>

#include <pugixml.hpp>

#include <cmath>
#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace pulp::timeline {
namespace {

using runtime::Err;
using runtime::Ok;

using ImportResult = runtime::Result<Project, DawProjectImportError>;

constexpr double kPpq = static_cast<double>(timebase::kTicksPerQuarter);

DawProjectImportError err(DawProjectImportErrorCode code, std::string message) {
    return DawProjectImportError{code, std::move(message), {}};
}

std::int64_t beats_to_ticks(double beats) {
    return static_cast<std::int64_t>(std::llround(beats * kPpq));
}

// A monotonic ItemId source. Sequential allocation guarantees the uniqueness and
// next-id monotonicity the model requires, without threading an allocator result
// through every call site.
struct IdSource {
    std::uint64_t next = 1;
    ItemId take() { return ItemId{next++}; }
};

// Assembles the timeline Project from a parsed DAWproject document. Every method
// returns std::optional<DawProjectImportError>: an engaged optional aborts the
// import (fail closed); std::nullopt means success.
class Importer {
  public:
    ImportResult run(const pugi::xml_node& project);

  private:
    std::optional<DawProjectImportError> read_transport(const pugi::xml_node& project);
    std::optional<DawProjectImportError> read_structure(const pugi::xml_node& project);
    std::optional<DawProjectImportError> read_arrangement(const pugi::xml_node& project);
    std::optional<DawProjectImportError> read_track_lanes(const pugi::xml_node& lanes);
    std::optional<DawProjectImportError>
    read_clip(const pugi::xml_node& clip, std::vector<Clip>& out);
    std::optional<DawProjectImportError>
    read_notes(const pugi::xml_node& notes, ClipContent& out);
    std::optional<DawProjectImportError>
    read_audio(const pugi::xml_node& audio, ClipContent& out);

    // Resolve (deduplicating by package path) a MediaAsset for an <Audio>,
    // returning its id and frame count.
    std::optional<DawProjectImportError>
    resolve_asset(const pugi::xml_node& audio, ItemId& asset_id_out,
                  std::uint64_t& frame_count_out);

    IdSource ids_;
    double tempo_bpm_ = 120.0;
    std::int32_t meter_num_ = 4;
    std::int32_t meter_den_ = 4;

    struct TrackEntry {
        std::string daw_id;
        std::string name;
        ItemId id;
    };
    // Structure order is preserved; the map indexes the same tracks by DAWproject
    // string id for arrangement cross-referencing.
    std::vector<TrackEntry> track_order_;
    std::unordered_map<std::string, ItemId> track_by_daw_id_;
    std::unordered_map<std::uint64_t, std::vector<Clip>> clips_by_track_;

    std::unordered_map<std::string, ItemId> asset_by_path_;
    std::vector<MediaAsset> assets_;
    std::int64_t max_end_tick_ = 0;
};

// --- attribute helpers ------------------------------------------------------

bool has_attr(const pugi::xml_node& node, const char* name) {
    return !node.attribute(name).empty();
}

std::optional<DawProjectImportError> require_double(const pugi::xml_node& node, const char* name,
                                                    const char* context, double& out) {
    auto attr = node.attribute(name);
    if (attr.empty())
        return err(DawProjectImportErrorCode::MissingAttribute,
                   std::string(context) + " is missing required attribute '" + name + "'");
    out = attr.as_double();
    return std::nullopt;
}

std::optional<DawProjectImportError> require_int(const pugi::xml_node& node, const char* name,
                                                 const char* context, long long& out) {
    auto attr = node.attribute(name);
    if (attr.empty())
        return err(DawProjectImportErrorCode::MissingAttribute,
                   std::string(context) + " is missing required attribute '" + name + "'");
    out = attr.as_llong();
    return std::nullopt;
}

// --- transport (tempo + meter) ---------------------------------------------

std::optional<DawProjectImportError> Importer::read_transport(const pugi::xml_node& project) {
    auto transport = project.child("Transport");
    if (!transport)
        return std::nullopt; // Defaults (120 bpm, 4/4) stand.

    if (auto tempo = transport.child("Tempo")) {
        auto unit = tempo.attribute("unit");
        if (!unit.empty() && std::string_view(unit.as_string()) != "bpm")
            return err(DawProjectImportErrorCode::UnsupportedFeature,
                       std::string("<Tempo unit='") + unit.as_string() +
                           "'> is unsupported; only 'bpm' is imported");
        double bpm = 0.0;
        if (auto e = require_double(tempo, "value", "<Tempo>", bpm))
            return e;
        if (!(bpm > 0.0))
            return err(DawProjectImportErrorCode::InvalidValue, "<Tempo value> must be positive");
        tempo_bpm_ = bpm;
    }

    if (auto sig = transport.child("TimeSignature")) {
        long long num = 0, den = 0;
        if (auto e = require_int(sig, "numerator", "<TimeSignature>", num))
            return e;
        if (auto e = require_int(sig, "denominator", "<TimeSignature>", den))
            return e;
        if (num <= 0 || den <= 0)
            return err(DawProjectImportErrorCode::InvalidValue,
                       "<TimeSignature> numerator and denominator must be positive");
        meter_num_ = static_cast<std::int32_t>(num);
        meter_den_ = static_cast<std::int32_t>(den);
    }
    return std::nullopt;
}

// --- structure (tracks) -----------------------------------------------------

std::optional<DawProjectImportError> Importer::read_structure(const pugi::xml_node& project) {
    auto structure = project.child("Structure");
    if (!structure)
        return std::nullopt;

    for (auto track : structure.children("Track")) {
        // Nested group tracks are outside the linear subset — refuse rather than
        // flatten and silently lose the grouping.
        if (track.child("Track"))
            return err(DawProjectImportErrorCode::UnsupportedFeature,
                       "nested group <Track> elements are not supported");

        auto id_attr = track.attribute("id");
        if (id_attr.empty())
            return err(DawProjectImportErrorCode::MissingAttribute,
                       "<Track> is missing required attribute 'id'");
        std::string daw_id = id_attr.as_string();
        if (track_by_daw_id_.count(daw_id))
            return err(DawProjectImportErrorCode::DuplicateTrackId,
                       "duplicate <Track id='" + daw_id + "'>");

        // The DAWproject display name is the human label; fall back to the id
        // when an exporter omits it.
        std::string name =
            has_attr(track, "name") ? track.attribute("name").as_string() : daw_id;

        ItemId id = ids_.take();
        track_by_daw_id_.emplace(daw_id, id);
        track_order_.push_back(TrackEntry{daw_id, std::move(name), id});
        clips_by_track_.emplace(id.value, std::vector<Clip>{});
    }
    return std::nullopt;
}

// --- arrangement (clips) ----------------------------------------------------

std::optional<DawProjectImportError> Importer::read_arrangement(const pugi::xml_node& project) {
    auto arrangement = project.child("Arrangement");
    if (!arrangement)
        return std::nullopt;

    auto root_lanes = arrangement.child("Lanes");
    if (!root_lanes)
        return std::nullopt;

    auto time_unit = root_lanes.attribute("timeUnit");
    if (!time_unit.empty() && std::string_view(time_unit.as_string()) != "beats")
        return err(DawProjectImportErrorCode::UnsupportedFeature,
                   std::string("<Arrangement> timeUnit='") + time_unit.as_string() +
                       "' is unsupported; only musical 'beats' timing is imported");

    for (auto child : root_lanes.children()) {
        if (child.type() != pugi::node_element)
            continue;
        std::string_view name = child.name();
        if (name == "Lanes") {
            if (auto e = read_track_lanes(child))
                return e;
        } else {
            // Markers, master automation, and similar arrangement-level timelines
            // are not yet imported; fail closed so their content is never silently
            // dropped.
            return err(DawProjectImportErrorCode::UnsupportedFeature,
                       std::string("<Arrangement> contains unsupported timeline <") +
                           std::string(name) + ">");
        }
    }
    return std::nullopt;
}

std::optional<DawProjectImportError> Importer::read_track_lanes(const pugi::xml_node& lanes) {
    auto track_attr = lanes.attribute("track");
    if (track_attr.empty())
        return err(DawProjectImportErrorCode::UnsupportedFeature,
                   "<Lanes> without a 'track' reference is not supported in the arrangement");
    std::string daw_id = track_attr.as_string();
    auto it = track_by_daw_id_.find(daw_id);
    if (it == track_by_daw_id_.end())
        return err(DawProjectImportErrorCode::DanglingTrackReference,
                   "<Lanes track='" + daw_id + "'> references an unknown track");
    ItemId track_id = it->second;

    for (auto child : lanes.children()) {
        if (child.type() != pugi::node_element)
            continue;
        std::string_view name = child.name();
        if (name == "Clips") {
            auto& clips = clips_by_track_.at(track_id.value);
            for (auto clip : child.children("Clip"))
                if (auto e = read_clip(clip, clips))
                    return e;
        } else {
            // Per-track automation lanes (<Points>) and other content are a
            // documented follow-up; refuse rather than drop.
            return err(DawProjectImportErrorCode::UnsupportedFeature,
                       std::string("<Lanes track='") + daw_id + "'> contains unsupported <" +
                           std::string(name) + ">");
        }
    }
    return std::nullopt;
}

std::optional<DawProjectImportError> Importer::read_clip(const pugi::xml_node& clip,
                                                         std::vector<Clip>& out) {
    double time_beats = 0.0, duration_beats = 0.0;
    if (auto e = require_double(clip, "time", "<Clip>", time_beats))
        return e;
    if (auto e = require_double(clip, "duration", "<Clip>", duration_beats))
        return e;
    if (time_beats < 0.0)
        return err(DawProjectImportErrorCode::InvalidValue, "<Clip time> must be non-negative");
    if (!(duration_beats > 0.0))
        return err(DawProjectImportErrorCode::InvalidValue, "<Clip duration> must be positive");

    // Exactly one content timeline is imported. Anything else is refused so a
    // clip's real content is never lost to an unrecognized child element.
    ClipContent content = EmptyContent{};
    int content_children = 0;
    for (auto child : clip.children()) {
        if (child.type() != pugi::node_element)
            continue;
        std::string_view name = child.name();
        if (name == "Notes") {
            ++content_children;
            if (auto e = read_notes(child, content))
                return e;
        } else if (name == "Audio") {
            ++content_children;
            if (auto e = read_audio(child, content))
                return e;
        } else {
            return err(DawProjectImportErrorCode::UnsupportedFeature,
                       std::string("<Clip> contains unsupported content <") + std::string(name) +
                           ">");
        }
    }
    if (content_children > 1)
        return err(DawProjectImportErrorCode::UnsupportedFeature,
                   "<Clip> with multiple content timelines is not supported");

    std::int64_t start = beats_to_ticks(time_beats);
    std::int64_t duration = beats_to_ticks(duration_beats);
    auto made = Clip::create(ids_.take(), timebase::TickPosition{start},
                             timebase::TickDuration{duration}, std::move(content));
    if (made.is_err())
        return DawProjectImportError{DawProjectImportErrorCode::ModelRejected,
                                     "model rejected clip", made.error()};

    if (start + duration > max_end_tick_)
        max_end_tick_ = start + duration;
    out.push_back(std::move(made.value()));
    return std::nullopt;
}

std::optional<DawProjectImportError> Importer::read_notes(const pugi::xml_node& notes,
                                                          ClipContent& out) {
    std::vector<NoteEvent> events;
    for (auto note : notes.children("Note")) {
        double time_beats = 0.0, duration_beats = 0.0;
        if (auto e = require_double(note, "time", "<Note>", time_beats))
            return e;
        if (auto e = require_double(note, "duration", "<Note>", duration_beats))
            return e;
        if (time_beats < 0.0)
            return err(DawProjectImportErrorCode::InvalidValue, "<Note time> must be non-negative");
        if (!(duration_beats > 0.0))
            return err(DawProjectImportErrorCode::InvalidValue, "<Note duration> must be positive");

        long long key = 0;
        if (auto e = require_int(note, "key", "<Note>", key))
            return e;
        if (key < 0 || key > 127)
            return err(DawProjectImportErrorCode::InvalidValue,
                       "<Note key> must be in the range 0..127");

        // DAWproject velocity/channel are optional; default to full velocity on
        // channel 0. vel is a normalized 0..1 gain.
        double vel = note.attribute("vel").empty() ? 1.0 : note.attribute("vel").as_double();
        if (vel < 0.0 || vel > 1.0)
            return err(DawProjectImportErrorCode::InvalidValue,
                       "<Note vel> must be in the range 0..1");
        long long channel = note.attribute("channel").as_llong(0);
        if (channel < 0 || channel > 15)
            return err(DawProjectImportErrorCode::InvalidValue,
                       "<Note channel> must be in the range 0..15");

        NoteEvent ev;
        ev.id = ids_.take();
        ev.start = timebase::TickPosition{beats_to_ticks(time_beats)};
        ev.duration = timebase::TickDuration{beats_to_ticks(duration_beats)};
        ev.velocity = static_cast<std::uint16_t>(std::llround(vel * 65535.0));
        ev.pitch = static_cast<std::uint8_t>(key);
        ev.channel = static_cast<std::uint8_t>(channel);
        events.push_back(ev);
    }

    auto made = NoteContent::create(std::move(events));
    if (made.is_err())
        return DawProjectImportError{DawProjectImportErrorCode::ModelRejected,
                                     "model rejected notes", made.error()};
    out = std::move(made.value());
    return std::nullopt;
}

std::optional<DawProjectImportError> Importer::resolve_asset(const pugi::xml_node& audio,
                                                             ItemId& asset_id_out,
                                                             std::uint64_t& frame_count_out) {
    auto file = audio.child("File");
    if (!file)
        return err(DawProjectImportErrorCode::UnsupportedFeature,
                   "<Audio> without a <File> reference is not supported");
    auto path_attr = file.attribute("path");
    if (path_attr.empty())
        return err(DawProjectImportErrorCode::MissingAttribute,
                   "<File> is missing required attribute 'path'");
    std::string path = path_attr.as_string();
    if (path.empty())
        return err(DawProjectImportErrorCode::InvalidValue, "<File path> must not be empty");

    if (auto it = asset_by_path_.find(path); it != asset_by_path_.end()) {
        asset_id_out = it->second;
        // Reuse the already-sealed asset's frame count.
        for (const auto& a : assets_)
            if (a.id == it->second)
                frame_count_out = a.frame_count;
        return std::nullopt;
    }

    double duration_sec = 0.0, sample_rate = 0.0;
    if (auto e = require_double(audio, "duration", "<Audio>", duration_sec))
        return e;
    if (auto e = require_double(audio, "sampleRate", "<Audio>", sample_rate))
        return e;
    if (!(duration_sec > 0.0))
        return err(DawProjectImportErrorCode::InvalidValue, "<Audio duration> must be positive");
    if (!(sample_rate > 0.0))
        return err(DawProjectImportErrorCode::InvalidValue, "<Audio sampleRate> must be positive");

    std::uint64_t frame_count =
        static_cast<std::uint64_t>(std::llround(duration_sec * sample_rate));
    if (frame_count == 0)
        return err(DawProjectImportErrorCode::InvalidValue,
                   "<Audio> resolves to a zero-length asset");

    // The durable identity is the SHA-256 of the media bytes; those bytes live in
    // the (not-yet-unzipped) container, so provisionally derive identity from the
    // package path and preserve the path itself only as a resolution hint.
    auto hash = ContentHash::from_hex(runtime::sha256_hex(path));
    if (!hash)
        return err(DawProjectImportErrorCode::InvalidValue,
                   "failed to derive a content hash for '" + path + "'");

    ItemId asset_id = ids_.take();
    MediaAsset asset;
    asset.id = asset_id;
    asset.name = path;
    asset.frame_count = frame_count;
    asset.sample_rate = timebase::RationalRate{static_cast<std::uint64_t>(sample_rate), 1};
    asset.content_hash = *hash;
    asset.storage_policy = AssetStoragePolicy::External;
    asset.locators.push_back(AssetLocator{AssetLocatorKind::PackageRelative, path});
    frame_count_out = frame_count;
    assets_.push_back(std::move(asset));
    asset_by_path_.emplace(path, asset_id);
    asset_id_out = asset_id;
    return std::nullopt;
}

std::optional<DawProjectImportError> Importer::read_audio(const pugi::xml_node& audio,
                                                          ClipContent& out) {
    ItemId asset_id;
    std::uint64_t frame_count = 0;
    if (auto e = resolve_asset(audio, asset_id, frame_count))
        return e;
    // Reference the whole asset from the clip. Sub-range playback (playStart /
    // warps) is a follow-up.
    out = MediaRef{asset_id, timebase::SamplePosition{0}, frame_count};
    return std::nullopt;
}

ImportResult Importer::run(const pugi::xml_node& project) {
    if (auto e = read_transport(project))
        return Err(std::move(*e));
    if (auto e = read_structure(project))
        return Err(std::move(*e));
    if (auto e = read_arrangement(project))
        return Err(std::move(*e));

    // Assemble the model. ItemId order below continues the same monotonic source,
    // so the whole project stays within one identity domain.
    std::vector<Track> tracks;
    tracks.reserve(track_order_.size());
    for (auto& entry : track_order_) {
        auto made = Track::create(entry.id, entry.name,
                                  std::move(clips_by_track_.at(entry.id.value)));
        if (made.is_err())
            return Err(DawProjectImportError{DawProjectImportErrorCode::ModelRejected,
                                             "model rejected track '" + entry.daw_id + "'",
                                             made.error()});
        tracks.push_back(std::move(made.value()));
    }

    ItemId sequence_id = ids_.take();
    std::optional<timebase::TickDuration> duration;
    if (max_end_tick_ > 0)
        duration = timebase::TickDuration{max_end_tick_};
    auto sequence = Sequence::create(sequence_id, "Arrangement", duration, std::move(tracks));
    if (sequence.is_err())
        return Err(DawProjectImportError{DawProjectImportErrorCode::ModelRejected,
                                         "model rejected sequence", sequence.error()});

    auto tempo = timebase::TempoMap::create(
        std::vector<timebase::TempoPoint>{{timebase::TickPosition{0}, tempo_bpm_}});
    if (tempo.is_err())
        return Err(err(DawProjectImportErrorCode::InvalidValue, "invalid tempo map"));
    auto meter = timebase::MeterMap::create(std::vector<timebase::MeterPoint>{
        {timebase::TickPosition{0}, timebase::MeterSignature{meter_num_, meter_den_}}});
    if (meter.is_err())
        return Err(err(DawProjectImportErrorCode::InvalidValue, "invalid meter map"));

    ProjectInput input;
    input.id = ids_.take();
    input.name = "DAWproject";
    input.root_sequence_id = sequence_id;
    input.assets = std::move(assets_);
    input.sequences.push_back(std::move(sequence.value()));
    input.tempo_map = std::move(tempo.value());
    input.meter_map = std::move(meter.value());
    input.next_item_id = ids_.next;

    auto project_result = Project::create(std::move(input));
    if (project_result.is_err())
        return Err(DawProjectImportError{DawProjectImportErrorCode::ModelRejected,
                                         "model rejected project", project_result.error()});
    return Ok(std::move(project_result.value()));
}

} // namespace

runtime::Result<Project, DawProjectImportError>
import_dawproject_xml(std::string_view project_xml) {
    pugi::xml_document doc;
    auto parsed = doc.load_buffer(project_xml.data(), project_xml.size());
    if (!parsed)
        return Err(err(DawProjectImportErrorCode::ParseError,
                       std::string("XML parse error: ") + parsed.description()));

    auto root = doc.document_element();
    if (!root || std::string_view(root.name()) != "Project")
        return Err(err(DawProjectImportErrorCode::MissingRoot,
                       "document root is not <Project>"));

    // Accept major version 1 only. A missing version attribute is tolerated (some
    // exporters omit it); a present one must be 1.x.
    if (has_attr(root, "version")) {
        std::string_view version = root.attribute("version").as_string();
        if (version.substr(0, 2) != "1.")
            return Err(err(DawProjectImportErrorCode::UnsupportedVersion,
                           std::string("unsupported DAWproject version '") + std::string(version) +
                               "'"));
    }

    Importer importer;
    return importer.run(root);
}

} // namespace pulp::timeline
