#pragma once

// Human-friendly presentation of a loaded impulse-response "Source".
//
// Naming reality: the audio reader (audio::AudioFileInfo) does NOT surface
// embedded titles — WAV LIST/INFO (INAM), broadcast bext, AIFF NAME, and FLAC
// Vorbis TITLE are not parsed. So the display NAME is derived from the file
// name, and the always-available FACTS (duration · channels · sample rate) come
// from AudioFileInfo. If embedded titles are wanted later, that is a reader
// extension; the file-name backbone is what ships and is what most IR files
// actually carry in practice.

#include <pulp/audio/audio_file.hpp>

#include <cstdio>
#include <optional>
#include <string>
#include <vector>

namespace pulp::superconvolver {

struct SourceDisplay {
    std::string name;   // e.g. "Chapel St Vitus"
    std::string facts;  // e.g. "3.2 s · stereo · 48 kHz"  (empty when no info)
};

namespace detail {

inline std::string lower(std::string s) {
    for (char& c : s)
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    return s;
}

// A trailing "engineering" token we drop from a file-name: a bare number
// (48000), a sample-rate marker (48k / 96khz), a bit-depth marker (24bit / 16b),
// or a channel/format word (stereo / mono / ir / wav / aiff / flac / …).
inline bool is_engineering_token(const std::string& tok) {
    if (tok.empty()) return false;
    const std::string t = lower(tok);
    static const char* kWords[] = {"ir",  "stereo", "mono", "wav", "aiff",
                                   "aif", "flac",   "ogg",  "mp3", "wave"};
    for (const char* w : kWords)
        if (t == w) return true;
    // <digits> possibly followed by a unit suffix.
    std::size_t i = 0;
    while (i < t.size() && t[i] >= '0' && t[i] <= '9') ++i;
    if (i == 0) return false;               // must start with digits
    const std::string suffix = t.substr(i);
    // A unit suffix is unambiguous engineering: 48k, 96khz, 24bit, 16b, 48000hz.
    if (suffix == "k" || suffix == "khz" || suffix == "bit" || suffix == "b" ||
        suffix == "hz")
        return true;
    if (!suffix.empty()) return false;
    // A BARE number is engineering only if it is a real sample rate or bit
    // depth — otherwise it is a model name ("Plate 140", "Lexicon 480") we keep.
    long v = 0;
    for (char c : t) v = v * 10 + (c - '0');
    switch (v) {
        case 8: case 16: case 24: case 32:                       // bit depth
        case 8000: case 11025: case 16000: case 22050: case 32000:
        case 44100: case 48000: case 88200: case 96000:
        case 176400: case 192000:                                // sample rate
            return true;
        default:
            return false;
    }
}

inline std::string capitalize_word(const std::string& w) {
    // Only touch all-lowercase words, so acronyms/mixed case ("St", "EMT140")
    // are preserved as authored.
    bool all_lower = !w.empty();
    for (char c : w)
        if (!(c >= 'a' && c <= 'z')) { all_lower = false; break; }
    if (!all_lower) return w;
    std::string out = w;
    out[0] = static_cast<char>(out[0] - 'a' + 'A');
    return out;
}

}  // namespace detail

// Strip directory + extension, turn separators into spaces, drop trailing
// engineering tokens, collapse whitespace, and gently title-case. Falls back to
// the raw stem if cleaning would leave nothing.
inline std::string clean_source_name(const std::string& path) {
    // Stem: after the last '/' or '\\'.
    std::size_t slash = path.find_last_of("/\\");
    std::string stem = (slash == std::string::npos) ? path : path.substr(slash + 1);
    // Drop a single trailing extension.
    std::size_t dot = stem.find_last_of('.');
    if (dot != std::string::npos && dot != 0) stem = stem.substr(0, dot);
    const std::string raw = stem;

    // Separators → spaces.
    for (char& c : stem)
        if (c == '_' || c == '-' || c == '.') c = ' ';

    // Tokenize.
    std::vector<std::string> toks;
    std::string cur;
    for (char c : stem) {
        if (c == ' ') { if (!cur.empty()) { toks.push_back(cur); cur.clear(); } }
        else cur.push_back(c);
    }
    if (!cur.empty()) toks.push_back(cur);

    // Drop trailing engineering tokens (stop at the first real word).
    while (!toks.empty() && detail::is_engineering_token(toks.back()))
        toks.pop_back();

    std::string out;
    for (const auto& t : toks) {
        if (!out.empty()) out += ' ';
        out += detail::capitalize_word(t);
    }
    return out.empty() ? raw : out;
}

// Always-available facts from AudioFileInfo → "3.2 s · stereo · 48 kHz".
inline std::string format_source_facts(const audio::AudioFileInfo& info) {
    std::string out;
    char buf[64];

    double secs = info.duration_seconds;
    if (secs <= 0.0 && info.sample_rate > 0 && info.num_frames > 0)
        secs = static_cast<double>(info.num_frames) / info.sample_rate;
    if (secs > 0.0) {
        if (secs < 10.0) std::snprintf(buf, sizeof buf, "%.1f s", secs);
        else             std::snprintf(buf, sizeof buf, "%.0f s", secs);
        out = buf;
    }

    if (info.num_channels > 0) {
        const char* ch = info.num_channels == 1 ? "mono"
                       : info.num_channels == 2 ? "stereo" : nullptr;
        if (!out.empty()) out += " · ";
        if (ch) out += ch;
        else { std::snprintf(buf, sizeof buf, "%u ch", info.num_channels); out += buf; }
    }

    if (info.sample_rate > 0) {
        const double khz = info.sample_rate / 1000.0;
        if (info.sample_rate % 1000 == 0) std::snprintf(buf, sizeof buf, "%.0f kHz", khz);
        else                              std::snprintf(buf, sizeof buf, "%.1f kHz", khz);
        if (!out.empty()) out += " · ";
        out += buf;
    }
    return out;
}

inline SourceDisplay derive_source_display(
    const std::string& path, const std::optional<audio::AudioFileInfo>& info) {
    SourceDisplay d;
    d.name = clean_source_name(path);
    if (info) d.facts = format_source_facts(*info);
    return d;
}

}  // namespace pulp::superconvolver
