#include <pulp/timeline/dawproject_import.hpp>

#include <pulp/audio/wav_decoder.hpp>
#include <pulp/runtime/crypto.hpp>
#include <pulp/timebase/rational_time.hpp>
#include <pulp/timebase/tick.hpp>

#include <pugixml.hpp>

#include <cctype>
#include <cerrno>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

#if defined(_WIN32)
#include <clocale>
#else
#include <locale.h>
#endif

namespace pulp::timeline {
namespace {

using runtime::Err;
using runtime::Ok;

using ImportResult = runtime::Result<Project, DawProjectImportError>;

constexpr double kPpq = static_cast<double>(timebase::kTicksPerQuarter);

DawProjectImportError err(DawProjectImportErrorCode code, std::string message) {
    return DawProjectImportError{code, std::move(message), {}};
}

bool package_path_is_lexically_safe(std::string_view path) {
    if (path.empty() || path.front() == '/' || path.front() == '\\')
        return false;
    if (path.size() >= 2 && std::isalpha(static_cast<unsigned char>(path.front())) &&
        path[1] == ':')
        return false;
    std::size_t component_begin = 0;
    for (std::size_t index = 0; index <= path.size(); ++index) {
        if (index != path.size() && path[index] != '/' && path[index] != '\\')
            continue;
        if (path.substr(component_begin, index - component_begin) == "..")
            return false;
        component_begin = index + 1;
    }
    return true;
}

bool declared_duration_matches_frames(long double declared_frames,
                                      std::uint64_t actual_frames) noexcept {
    if (!std::isfinite(declared_frames) || actual_frames == 0)
        return false;
    const auto actual = static_cast<long double>(actual_frames);
    return declared_frames >= actual - 0.5L && declared_frames < actual + 0.5L;
}

std::optional<std::int64_t> beats_to_ticks(double beats) noexcept {
    constexpr long double kExclusiveUpperTick = static_cast<long double>(std::uint64_t{1} << 63u);
    const auto rounded =
        std::round(static_cast<long double>(beats) * static_cast<long double>(kPpq));
    if (!std::isfinite(rounded) || rounded < 0.0L || rounded >= kExclusiveUpperTick)
        return std::nullopt;
    return static_cast<std::int64_t>(rounded);
}

struct CheckedBeatRange {
    std::int64_t start = 0;
    std::int64_t duration = 0;
    std::int64_t end = 0;
};

std::optional<CheckedBeatRange> checked_beat_range(double start_beats,
                                                   double duration_beats) noexcept {
    if (!std::isfinite(start_beats) || start_beats < 0.0 || !std::isfinite(duration_beats) ||
        !(duration_beats > 0.0))
        return std::nullopt;
    const auto start = beats_to_ticks(start_beats);
    const auto duration = beats_to_ticks(duration_beats);
    if (!start || !duration || *duration <= 0 ||
        *start > std::numeric_limits<std::int64_t>::max() - *duration)
        return std::nullopt;
    return CheckedBeatRange{*start, *duration, *start + *duration};
}

// A monotonic ItemId source. Sequential allocation guarantees the uniqueness and
// next-id monotonicity the model requires, without threading an allocator result
// through every call site.
struct IdSource {
    std::uint64_t next = 1;
    ItemId take() {
        return ItemId{next++};
    }
};

// Assembles the timeline Project from a parsed DAWproject document. Every method
// returns std::optional<DawProjectImportError>: an engaged optional aborts the
// import (fail closed); std::nullopt means success.
class Importer {
  public:
    Importer(DawProjectMediaResolver media_resolver, const DawProjectImportLimits& limits)
        : media_resolver_(std::move(media_resolver)), limits_(limits) {}
    ImportResult run(const pugi::xml_node& project);

  private:
    std::optional<DawProjectImportError> read_transport(const pugi::xml_node& project);
    std::optional<DawProjectImportError> read_structure(const pugi::xml_node& project);
    std::optional<DawProjectImportError> read_arrangement(const pugi::xml_node& project);
    std::optional<DawProjectImportError> read_track_lanes(const pugi::xml_node& lanes);
    std::optional<DawProjectImportError> read_clip(const pugi::xml_node& clip,
                                                   std::vector<Clip>& out);
    std::optional<DawProjectImportError> read_notes(const pugi::xml_node& notes, ClipContent& out);
    std::optional<DawProjectImportError> read_audio(const pugi::xml_node& audio, ClipContent& out);

    // Resolve a MediaAsset for an <Audio>, deduplicating by sealed content
    // identity while retaining every package path as a resolution hint.
    std::optional<DawProjectImportError> resolve_asset(const pugi::xml_node& audio,
                                                       ItemId& asset_id_out,
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

    std::unordered_map<std::string, std::string> hash_by_path_;
    std::unordered_map<std::string, ItemId> asset_by_hash_;
    std::vector<MediaAsset> assets_;
    std::size_t clip_count_ = 0;
    std::size_t note_count_ = 0;
    std::size_t media_resolver_calls_ = 0;
    std::uint64_t resolved_media_bytes_ = 0;
    std::int64_t max_end_tick_ = 0;
    DawProjectMediaResolver media_resolver_;
    DawProjectImportLimits limits_;
};

// --- attribute helpers ------------------------------------------------------

bool has_attr(const pugi::xml_node& node, const char* name) {
    return !node.attribute(name).empty();
}

template <typename Value> bool parse_number(const pugi::xml_attribute& attribute, Value& out) {
    const std::string_view text = attribute.as_string();
    if constexpr (std::is_floating_point_v<Value>) {
        const std::string buffer(text);
        errno = 0;
        char* end = nullptr;
        double value = 0.0;
#if defined(_WIN32)
        static _locale_t c_locale = ::_create_locale(LC_ALL, "C");
        if (c_locale == nullptr)
            return false;
        value = ::_strtod_l(buffer.c_str(), &end, c_locale);
#else
        static ::locale_t c_locale = ::newlocale(LC_ALL_MASK, "C", static_cast<::locale_t>(0));
        if (c_locale == static_cast<::locale_t>(0))
            return false;
        const ::locale_t previous = ::uselocale(c_locale);
        value = std::strtod(buffer.c_str(), &end);
        ::uselocale(previous);
#endif
        if (errno == ERANGE || end == buffer.c_str() || end != buffer.c_str() + buffer.size())
            return false;
        out = value;
        return true;
    } else {
        const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), out);
        return error == std::errc{} && end == text.data() + text.size();
    }
}

std::optional<DawProjectImportError> require_double(const pugi::xml_node& node, const char* name,
                                                    const char* context, double& out) {
    auto attr = node.attribute(name);
    if (attr.empty())
        return err(DawProjectImportErrorCode::MissingAttribute,
                   std::string(context) + " is missing required attribute '" + name + "'");
    if (!parse_number(attr, out))
        return err(DawProjectImportErrorCode::InvalidValue,
                   std::string(context) + " has invalid numeric attribute '" + name + "'");
    return std::nullopt;
}

std::optional<DawProjectImportError> require_int(const pugi::xml_node& node, const char* name,
                                                 const char* context, long long& out) {
    auto attr = node.attribute(name);
    if (attr.empty())
        return err(DawProjectImportErrorCode::MissingAttribute,
                   std::string(context) + " is missing required attribute '" + name + "'");
    if (!parse_number(attr, out))
        return err(DawProjectImportErrorCode::InvalidValue,
                   std::string(context) + " has invalid integer attribute '" + name + "'");
    return std::nullopt;
}

std::optional<DawProjectImportError> reject_unsupported_play_start(const pugi::xml_node& node,
                                                                   const char* context) {
    const auto attribute = node.attribute("playStart");
    if (attribute.empty())
        return std::nullopt;
    double play_start = 0.0;
    if (!parse_number(attribute, play_start) || !std::isfinite(play_start))
        return err(DawProjectImportErrorCode::InvalidValue,
                   std::string(context) + " has invalid numeric attribute 'playStart'");
    if (play_start != 0.0)
        return err(DawProjectImportErrorCode::UnsupportedFeature,
                   std::string(context) + " uses unsupported playStart sub-range playback");
    return std::nullopt;
}

std::optional<DawProjectImportError> reject_unsupported_clip_playback(const pugi::xml_node& clip) {
    if (!clip.attribute("reference").empty())
        return err(DawProjectImportErrorCode::UnsupportedFeature,
                   "<Clip> uses unsupported referenced-content semantics");
    if (auto e = reject_unsupported_play_start(clip, "<Clip>"))
        return e;
    for (const char* attribute_name : {"playStop", "loopStart", "loopEnd"}) {
        if (!clip.attribute(attribute_name).empty())
            return err(DawProjectImportErrorCode::UnsupportedFeature,
                       std::string("<Clip> uses unsupported ") + attribute_name +
                           " playback semantics");
    }
    const auto content_time_unit = clip.attribute("contentTimeUnit");
    if (!content_time_unit.empty() && std::string_view(content_time_unit.as_string()) != "beats")
        return err(DawProjectImportErrorCode::UnsupportedFeature,
                   std::string("<Clip contentTimeUnit='") + content_time_unit.as_string() +
                       "'> is unsupported; only beat-relative content is imported");
    return std::nullopt;
}

std::optional<DawProjectImportError>
reject_unsupported_audio_playback(const pugi::xml_node& audio) {
    if (auto e = reject_unsupported_play_start(audio, "<Audio>"))
        return e;
    for (const char* attribute_name : {"playStop", "loopStart", "loopEnd"}) {
        if (!audio.attribute(attribute_name).empty())
            return err(DawProjectImportErrorCode::UnsupportedFeature,
                       std::string("<Audio> uses unsupported ") + attribute_name +
                           " playback semantics");
    }
    return std::nullopt;
}

// --- transport (tempo + meter) ---------------------------------------------

std::optional<DawProjectImportError> Importer::read_transport(const pugi::xml_node& project) {
    auto transport = project.child("Transport");
    if (!transport)
        return std::nullopt; // Defaults (120 bpm, 4/4) stand.

    pugi::xml_node tempo;
    pugi::xml_node sig;
    for (auto child : transport.children()) {
        if (child.type() != pugi::node_element)
            continue;
        const std::string_view name = child.name();
        pugi::xml_node* selected = nullptr;
        if (name == "Tempo")
            selected = &tempo;
        else if (name == "TimeSignature")
            selected = &sig;
        else
            return err(DawProjectImportErrorCode::UnsupportedFeature,
                       std::string("<Transport> contains unsupported <") + std::string(name) + ">");
        if (*selected)
            return err(DawProjectImportErrorCode::UnsupportedFeature,
                       std::string("<Transport> contains multiple <") + std::string(name) + ">");
        *selected = child;
        for (auto parameter_child : child.children()) {
            if (parameter_child.type() == pugi::node_element)
                return err(DawProjectImportErrorCode::UnsupportedFeature,
                           std::string("<") + std::string(name) + "> contains unsupported <" +
                               parameter_child.name() + ">");
        }
    }

    if (tempo) {
        auto unit = tempo.attribute("unit");
        if (!unit.empty() && std::string_view(unit.as_string()) != "bpm")
            return err(DawProjectImportErrorCode::UnsupportedFeature,
                       std::string("<Tempo unit='") + unit.as_string() +
                           "'> is unsupported; only 'bpm' is imported");
        double bpm = 0.0;
        if (auto e = require_double(tempo, "value", "<Tempo>", bpm))
            return e;
        if (!std::isfinite(bpm) || !(bpm > 0.0))
            return err(DawProjectImportErrorCode::InvalidValue, "<Tempo value> must be positive");
        tempo_bpm_ = bpm;
    }

    if (sig) {
        long long num = 0, den = 0;
        if (auto e = require_int(sig, "numerator", "<TimeSignature>", num))
            return e;
        if (auto e = require_int(sig, "denominator", "<TimeSignature>", den))
            return e;
        if (num <= 0 || den <= 0 || num > std::numeric_limits<std::int32_t>::max() ||
            den > std::numeric_limits<std::int32_t>::max())
            return err(DawProjectImportErrorCode::InvalidValue,
                       "<TimeSignature> numerator and denominator must be positive 32-bit values");
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

    for (auto track : structure.children()) {
        if (track.type() != pugi::node_element)
            continue;
        if (std::string_view(track.name()) != "Track")
            return err(DawProjectImportErrorCode::UnsupportedFeature,
                       std::string("<Structure> contains unsupported <") + track.name() + ">");
        if (track_order_.size() >= limits_.max_tracks)
            return err(DawProjectImportErrorCode::LimitExceeded,
                       "DAWproject track count exceeds max_tracks");
        for (auto track_child : track.children()) {
            if (track_child.type() == pugi::node_element)
                return err(DawProjectImportErrorCode::UnsupportedFeature,
                           std::string("<Track> contains unsupported <") + track_child.name() +
                               ">");
        }

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
        std::string name = has_attr(track, "name") ? track.attribute("name").as_string() : daw_id;

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

    pugi::xml_node root_lanes;
    for (auto child : arrangement.children()) {
        if (child.type() != pugi::node_element)
            continue;
        const std::string_view name = child.name();
        if (name != "Lanes")
            return err(DawProjectImportErrorCode::UnsupportedFeature,
                       std::string("<Arrangement> contains unsupported <") + std::string(name) +
                           ">");
        if (root_lanes)
            return err(DawProjectImportErrorCode::UnsupportedFeature,
                       "<Arrangement> contains multiple <Lanes>");
        root_lanes = child;
    }
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
    const auto time_unit = lanes.attribute("timeUnit");
    if (!time_unit.empty() && std::string_view(time_unit.as_string()) != "beats")
        return err(DawProjectImportErrorCode::UnsupportedFeature,
                   std::string("<Lanes timeUnit='") + time_unit.as_string() +
                       "'> is unsupported; only musical 'beats' timing is imported");

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
            const auto time_unit = child.attribute("timeUnit");
            if (!time_unit.empty() && std::string_view(time_unit.as_string()) != "beats")
                return err(DawProjectImportErrorCode::UnsupportedFeature,
                           std::string("<Clips timeUnit='") + time_unit.as_string() +
                               "'> is unsupported; only musical 'beats' timing is imported");
            auto& clips = clips_by_track_.at(track_id.value);
            for (auto clip : child.children()) {
                if (clip.type() != pugi::node_element)
                    continue;
                if (std::string_view(clip.name()) != "Clip")
                    return err(DawProjectImportErrorCode::UnsupportedFeature,
                               std::string("<Clips> contains unsupported <") + clip.name() + ">");
                if (clip_count_ >= limits_.max_clips)
                    return err(DawProjectImportErrorCode::LimitExceeded,
                               "DAWproject clip count exceeds max_clips");
                ++clip_count_;
                if (auto e = read_clip(clip, clips))
                    return e;
            }
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
    const auto range = checked_beat_range(time_beats, duration_beats);
    if (!range)
        return err(DawProjectImportErrorCode::InvalidValue,
                   "<Clip> time and duration must form a finite positive tick range");
    if (auto e = reject_unsupported_clip_playback(clip))
        return e;

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

    auto made = Clip::create(ids_.take(), timebase::TickPosition{range->start},
                             timebase::TickDuration{range->duration}, std::move(content));
    if (made.is_err())
        return DawProjectImportError{DawProjectImportErrorCode::ModelRejected,
                                     "model rejected clip", made.error()};

    if (range->end > max_end_tick_)
        max_end_tick_ = range->end;
    out.push_back(std::move(made.value()));
    return std::nullopt;
}

std::optional<DawProjectImportError> Importer::read_notes(const pugi::xml_node& notes,
                                                          ClipContent& out) {
    const auto time_unit = notes.attribute("timeUnit");
    if (!time_unit.empty() && std::string_view(time_unit.as_string()) != "beats")
        return err(DawProjectImportErrorCode::UnsupportedFeature,
                   std::string("<Notes timeUnit='") + time_unit.as_string() +
                       "'> is unsupported; only musical 'beats' timing is imported");

    std::vector<NoteEvent> events;
    for (auto note : notes.children()) {
        if (note.type() != pugi::node_element)
            continue;
        if (std::string_view(note.name()) != "Note")
            return err(DawProjectImportErrorCode::UnsupportedFeature,
                       std::string("<Notes> contains unsupported <") + note.name() + ">");
        if (note_count_ >= limits_.max_notes)
            return err(DawProjectImportErrorCode::LimitExceeded,
                       "DAWproject note count exceeds max_notes");
        ++note_count_;
        for (auto child : note.children()) {
            if (child.type() == pugi::node_element)
                return err(DawProjectImportErrorCode::UnsupportedFeature,
                           std::string("<Note> contains unsupported <") + child.name() + ">");
        }
        double time_beats = 0.0, duration_beats = 0.0;
        if (auto e = require_double(note, "time", "<Note>", time_beats))
            return e;
        if (auto e = require_double(note, "duration", "<Note>", duration_beats))
            return e;
        const auto range = checked_beat_range(time_beats, duration_beats);
        if (!range)
            return err(DawProjectImportErrorCode::InvalidValue,
                       "<Note> time and duration must form a finite positive tick range");

        long long key = 0;
        if (auto e = require_int(note, "key", "<Note>", key))
            return e;
        if (key < 0 || key > 127)
            return err(DawProjectImportErrorCode::InvalidValue,
                       "<Note key> must be in the range 0..127");

        // DAWproject velocity/channel are optional; default to full velocity on
        // channel 0. vel is a normalized 0..1 gain.
        double vel = 1.0;
        const auto velocity = note.attribute("vel");
        if (!velocity.empty() && !parse_number(velocity, vel))
            return err(DawProjectImportErrorCode::InvalidValue,
                       "<Note vel> must be a number in the range 0..1");
        if (!std::isfinite(vel) || vel < 0.0 || vel > 1.0)
            return err(DawProjectImportErrorCode::InvalidValue,
                       "<Note vel> must be in the range 0..1");
        long long channel = 0;
        const auto channel_attribute = note.attribute("channel");
        if (!channel_attribute.empty() && !parse_number(channel_attribute, channel))
            return err(DawProjectImportErrorCode::InvalidValue,
                       "<Note channel> must be an integer in the range 0..15");
        if (channel < 0 || channel > 15)
            return err(DawProjectImportErrorCode::InvalidValue,
                       "<Note channel> must be in the range 0..15");

        NoteEvent ev;
        ev.id = ids_.take();
        ev.start = timebase::TickPosition{range->start};
        ev.duration = timebase::TickDuration{range->duration};
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
    const std::string_view path_view = path_attr.as_string();
    if (path_view.empty())
        return err(DawProjectImportErrorCode::InvalidValue, "<File path> must not be empty");
    if (path_view.size() > limits_.max_package_path_bytes)
        return err(DawProjectImportErrorCode::LimitExceeded,
                   "<File path> exceeds max_package_path_bytes");
    if (!package_path_is_lexically_safe(path_view))
        return err(DawProjectImportErrorCode::InvalidValue,
                   "<File path> must be a safe package-relative path");
    std::string path(path_view);

    double duration_sec = 0.0, sample_rate = 0.0;
    if (auto e = require_double(audio, "duration", "<Audio>", duration_sec))
        return e;
    if (auto e = require_double(audio, "sampleRate", "<Audio>", sample_rate))
        return e;
    if (!std::isfinite(duration_sec) || !(duration_sec > 0.0))
        return err(DawProjectImportErrorCode::InvalidValue, "<Audio duration> must be positive");
    if (!std::isfinite(sample_rate) || !(sample_rate > 0.0))
        return err(DawProjectImportErrorCode::InvalidValue, "<Audio sampleRate> must be positive");
    const auto declared_frames =
        static_cast<long double>(duration_sec) * static_cast<long double>(sample_rate);
    if (!std::isfinite(declared_frames) || declared_frames <= 0.0L)
        return err(DawProjectImportErrorCode::InvalidValue,
                   "<Audio> duration exceeds the supported frame domain");

    if (!media_resolver_)
        return err(DawProjectImportErrorCode::MissingMediaBytes,
                   "media bytes are required to seal '" + path + "'");
    if (media_resolver_calls_ >= limits_.max_media_resolver_calls)
        return err(DawProjectImportErrorCode::LimitExceeded,
                   "media resolver call count exceeds max_media_resolver_calls");
    ++media_resolver_calls_;
    const auto bytes = media_resolver_(path);
    if (!bytes)
        return err(DawProjectImportErrorCode::MissingMediaBytes,
                   "media resolver did not provide '" + path + "'");
    const auto byte_count = static_cast<std::uint64_t>(bytes->size());
    if (byte_count > limits_.max_media_bytes_per_resolver_call)
        return err(DawProjectImportErrorCode::LimitExceeded,
                   "resolved media '" + path + "' exceeds max_media_bytes_per_resolver_call");
    if (resolved_media_bytes_ > limits_.max_total_media_bytes ||
        byte_count > limits_.max_total_media_bytes - resolved_media_bytes_)
        return err(DawProjectImportErrorCode::LimitExceeded,
                   "resolved media exceeds max_total_media_bytes");
    resolved_media_bytes_ += byte_count;
    const auto info = audio::inspect_wav(*bytes);
    if (!info)
        return err(DawProjectImportErrorCode::InvalidValue,
                   "media resolver provided an invalid or unsupported WAV for '" + path + "'");
    if (sample_rate != static_cast<double>(info->sample_rate))
        return err(DawProjectImportErrorCode::InvalidValue,
                   "<Audio sampleRate> does not match resolved media '" + path + "'");
    if (!declared_duration_matches_frames(declared_frames, info->num_frames))
        return err(DawProjectImportErrorCode::InvalidValue,
                   "<Audio duration> does not match resolved media '" + path + "'");

    auto hash = ContentHash::from_hex(runtime::sha256_hex(bytes->data(), bytes->size()));
    if (!hash)
        return err(DawProjectImportErrorCode::InvalidValue,
                   "failed to derive a content hash for '" + path + "'");
    const auto hash_hex = hash->to_hex();
    if (auto it = hash_by_path_.find(path); it != hash_by_path_.end() && it->second != hash_hex)
        return err(DawProjectImportErrorCode::InvalidValue,
                   "package path resolved to different content for '" + path + "'");

    if (auto it = asset_by_hash_.find(hash_hex); it != asset_by_hash_.end()) {
        for (auto& asset : assets_) {
            if (asset.id != it->second)
                continue;
            if (asset.frame_count != info->num_frames ||
                asset.sample_rate != timebase::RationalRate{info->sample_rate, 1})
                return err(DawProjectImportErrorCode::InvalidValue,
                           "content-identical media has inconsistent audio metadata for '" + path +
                               "'");
            const AssetLocator locator{AssetLocatorKind::PackageRelative, path};
            if (std::find(asset.locators.begin(), asset.locators.end(), locator) ==
                asset.locators.end())
                asset.locators.push_back(locator);
            hash_by_path_.emplace(path, hash_hex);
            asset_id_out = asset.id;
            frame_count_out = asset.frame_count;
            return std::nullopt;
        }
        return err(DawProjectImportErrorCode::ModelRejected,
                   "internal content-hash index lost resolved media '" + path + "'");
    }
    if (assets_.size() >= limits_.max_media_assets)
        return err(DawProjectImportErrorCode::LimitExceeded,
                   "DAWproject media asset count exceeds max_media_assets");

    ItemId asset_id = ids_.take();
    MediaAsset asset;
    asset.id = asset_id;
    asset.name = path;
    asset.frame_count = info->num_frames;
    asset.sample_rate = timebase::RationalRate{info->sample_rate, 1};
    asset.content_hash = *hash;
    asset.storage_policy = AssetStoragePolicy::External;
    asset.locators.push_back(AssetLocator{AssetLocatorKind::PackageRelative, path});
    frame_count_out = info->num_frames;
    assets_.push_back(std::move(asset));
    hash_by_path_.emplace(path, hash_hex);
    asset_by_hash_.emplace(hash_hex, asset_id);
    asset_id_out = asset_id;
    return std::nullopt;
}

std::optional<DawProjectImportError> Importer::read_audio(const pugi::xml_node& audio,
                                                          ClipContent& out) {
    if (auto e = reject_unsupported_audio_playback(audio))
        return e;
    bool saw_file = false;
    for (auto child : audio.children()) {
        if (child.type() != pugi::node_element)
            continue;
        if (std::string_view(child.name()) != "File")
            return err(DawProjectImportErrorCode::UnsupportedFeature,
                       std::string("<Audio> contains unsupported <") + child.name() + ">");
        if (saw_file)
            return err(DawProjectImportErrorCode::UnsupportedFeature,
                       "<Audio> contains multiple <File> references");
        for (auto file_child : child.children()) {
            if (file_child.type() == pugi::node_element)
                return err(DawProjectImportErrorCode::UnsupportedFeature,
                           std::string("<File> contains unsupported <") + file_child.name() + ">");
        }
        saw_file = true;
    }
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
    bool saw_application = false;
    bool saw_transport = false;
    bool saw_structure = false;
    bool saw_arrangement = false;
    bool saw_scenes = false;
    for (auto child : project.children()) {
        if (child.type() != pugi::node_element)
            continue;
        const std::string_view name = child.name();
        bool* seen = nullptr;
        if (name == "Application") {
            seen = &saw_application;
            for (auto metadata_child : child.children()) {
                if (metadata_child.type() == pugi::node_element)
                    return Err(err(DawProjectImportErrorCode::UnsupportedFeature,
                                   std::string("<Application> contains unsupported <") +
                                       metadata_child.name() + ">"));
            }
        } else if (name == "Transport") {
            seen = &saw_transport;
        } else if (name == "Structure") {
            seen = &saw_structure;
        } else if (name == "Arrangement") {
            seen = &saw_arrangement;
        } else if (name == "Scenes") {
            seen = &saw_scenes;
            for (auto scene : child.children()) {
                if (scene.type() == pugi::node_element)
                    return Err(
                        err(DawProjectImportErrorCode::UnsupportedFeature,
                            std::string("<Scenes> contains unsupported <") + scene.name() + ">"));
            }
        } else {
            return Err(
                err(DawProjectImportErrorCode::UnsupportedFeature,
                    std::string("<Project> contains unsupported <") + std::string(name) + ">"));
        }
        if (*seen)
            return Err(err(DawProjectImportErrorCode::UnsupportedFeature,
                           std::string("<Project> contains multiple <") + std::string(name) + ">"));
        *seen = true;
    }

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
        auto made =
            Track::create(entry.id, entry.name, std::move(clips_by_track_.at(entry.id.value)));
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
    return import_dawproject_xml(project_xml, {}, DawProjectImportLimits{});
}

runtime::Result<Project, DawProjectImportError>
import_dawproject_xml(std::string_view project_xml, DawProjectMediaResolver media_resolver) {
    return import_dawproject_xml(project_xml, std::move(media_resolver), DawProjectImportLimits{});
}

runtime::Result<Project, DawProjectImportError>
import_dawproject_xml(std::string_view project_xml, DawProjectMediaResolver media_resolver,
                      const DawProjectImportLimits& limits) {
    if (project_xml.size() > limits.max_xml_bytes)
        return Err(
            err(DawProjectImportErrorCode::LimitExceeded, "DAWproject XML exceeds max_xml_bytes"));

    pugi::xml_document doc;
    auto parsed = doc.load_buffer(project_xml.data(), project_xml.size());
    if (!parsed)
        return Err(err(DawProjectImportErrorCode::ParseError,
                       std::string("XML parse error: ") + parsed.description()));

    auto root = doc.document_element();
    if (!root || std::string_view(root.name()) != "Project")
        return Err(err(DawProjectImportErrorCode::MissingRoot, "document root is not <Project>"));

    // Accept major version 1 only. A missing version attribute is tolerated (some
    // exporters omit it); a present one must be 1.x.
    if (has_attr(root, "version")) {
        std::string_view version = root.attribute("version").as_string();
        if (version.substr(0, 2) != "1.")
            return Err(
                err(DawProjectImportErrorCode::UnsupportedVersion,
                    std::string("unsupported DAWproject version '") + std::string(version) + "'"));
    }

    Importer importer{std::move(media_resolver), limits};
    return importer.run(root);
}

} // namespace pulp::timeline
