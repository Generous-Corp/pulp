#pragma once

/// @file pulpset.hpp
/// @brief Timed command-script (`.pulpset`) replay over a HeadlessHost — a general,
///        Magenta-agnostic offline render/replay harness (delivery item "G4").
///
/// A `.pulpset` is a timestamped stream of plugin steers — parameter changes and MIDI
/// notes — keyed by absolute sample offset. `render()` drives a `HeadlessHost` block by
/// block, applying each event when its sample window arrives, and returns the rendered
/// stereo audio. Because the steers are data, the same script that an agent emits live
/// (e.g. over MCP) can be replayed deterministically for a regression — render to a WAV
/// and assert range/property invariants, or bit-exact for a non-generative plugin.
///
/// Script grammar (one event per line; `#` starts a comment):
///   `<sample> param <id> <value>`        — set parameter `id` to `value`
///   `<sample> note_on <pitch> [vel]`     — MIDI note-on  (vel default 100, ch 0)
///   `<sample> note_off <pitch>`          — MIDI note-off
///
/// This sits on top of `HeadlessHost`; no audio device, no UI — CI-friendly.

#include <pulp/format/headless.hpp>
#include <pulp/audio/buffer.hpp>
#include <pulp/midi/buffer.hpp>
#include <pulp/midi/message.hpp>

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <locale>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <vector>

namespace pulp::format {

/// One timed steer in a `.pulpset`.
struct PulpsetEvent {
    enum class Kind { Param, NoteOn, NoteOff };
    std::int64_t sample = 0;   ///< absolute sample offset
    Kind kind = Kind::Param;
    std::uint32_t id = 0;      ///< param id (Param) or pitch (NoteOn/NoteOff)
    float value = 0.0f;        ///< param value (Param) or velocity (NoteOn)
};

/// A parsed `.pulpset` — events sorted by sample offset.
struct Pulpset {
    std::vector<PulpsetEvent> events;

    static Pulpset parse(const std::string& text) {
        Pulpset ps;
        std::istringstream in(text);
        std::string line;
        while (std::getline(in, line)) {
            auto hash = line.find('#');
            if (hash != std::string::npos) line = line.substr(0, hash);

            // Whitespace-delimited tokenizer over the line. Tokens are parsed
            // with locale-independent rules so a comma-decimal global locale
            // can never make a `.pulpset` round-trip "0.5" as "0,5". (The
            // previous std::istringstream >> path honored the stream's global
            // locale.)
            std::string_view rest{line};
            auto next_token = [&rest]() -> std::string_view {
                std::size_t b = rest.find_first_not_of(" \t\r");
                if (b == std::string_view::npos) { rest = {}; return {}; }
                std::size_t e = rest.find_first_of(" \t\r", b);
                std::string_view tok = (e == std::string_view::npos)
                                           ? rest.substr(b)
                                           : rest.substr(b, e - b);
                rest = (e == std::string_view::npos) ? std::string_view{}
                                                     : rest.substr(e);
                return tok;
            };

            std::string_view sample_tok = next_token();
            std::string_view op = next_token();
            if (sample_tok.empty() || op.empty()) continue;

            // A required numeric field must parse AND fully consume its token.
            // Full-token consumption is what rejects "0,5" (parses just the "0"
            // prefix) and "123abc" — a comma-decimal legacy file is rejected
            // cleanly rather than silently replaying 0.5 as 0.0.
            auto parse_full = [](std::string_view tok, auto& out) {
                if (tok.empty()) return false;
                using Out = std::remove_reference_t<decltype(out)>;
                if constexpr (std::is_floating_point_v<Out>) {
                    std::istringstream stream{std::string(tok)};
                    stream.imbue(std::locale::classic());
                    stream >> out;
                    return static_cast<bool>(stream) && stream.eof();
                } else {
                    const char* first = tok.data();
                    const char* last = first + tok.size();
                    auto r = std::from_chars(first, last, out);
                    return r.ec == std::errc{} && r.ptr == last;
                }
            };

            PulpsetEvent e;
            if (!parse_full(sample_tok, e.sample)) continue;

            if (op == "param") {
                e.kind = PulpsetEvent::Kind::Param;
                // id and value are required for a param steer.
                if (!parse_full(next_token(), e.id)) continue;
                if (!parse_full(next_token(), e.value)) continue;
            } else if (op == "note_on") {
                e.kind = PulpsetEvent::Kind::NoteOn;
                e.value = 100.0f;  // default velocity
                if (!parse_full(next_token(), e.id)) continue;
                // Velocity is optional; if a token is present it must be valid.
                std::string_view vel = next_token();
                if (!vel.empty() && !parse_full(vel, e.value)) continue;
            } else if (op == "note_off") {
                e.kind = PulpsetEvent::Kind::NoteOff;
                if (!parse_full(next_token(), e.id)) continue;
            } else {
                continue;
            }
            ps.events.push_back(e);
        }
        std::stable_sort(ps.events.begin(), ps.events.end(),
                         [](const PulpsetEvent& a, const PulpsetEvent& b) { return a.sample < b.sample; });
        return ps;
    }

    /// Serialize events back to `.pulpset` text. Floats are written with
    /// std::to_chars so write+read round-trips under any global locale.
    std::string to_text() const {
        std::string out;
        char buf[64];
        auto append_num = [&](auto v) {
            auto r = std::to_chars(buf, buf + sizeof(buf), v);
            out.append(buf, r.ptr);
        };
        for (const auto& e : events) {
            append_num(e.sample);
            switch (e.kind) {
                case PulpsetEvent::Kind::Param:
                    out += " param ";
                    append_num(e.id);
                    out += ' ';
                    append_num(e.value);
                    break;
                case PulpsetEvent::Kind::NoteOn:
                    out += " note_on ";
                    append_num(e.id);
                    out += ' ';
                    append_num(e.value);
                    break;
                case PulpsetEvent::Kind::NoteOff:
                    out += " note_off ";
                    append_num(e.id);
                    break;
            }
            out += '\n';
        }
        return out;
    }

    static Pulpset from_file(const std::string& path) {
        std::ifstream f(path);
        std::stringstream ss; ss << f.rdbuf();
        return parse(ss.str());
    }

    std::int64_t last_sample() const { return events.empty() ? 0 : events.back().sample; }
};

/// Rendered stereo result of a replay.
struct PulpsetRender {
    std::vector<float> left, right;
    std::size_t frames() const { return left.size(); }
};

/// Replay @p ps over @p host for @p total_samples, in @p block_size blocks.
/// Parameter events are applied at the start of the block they fall in; MIDI notes are
/// placed sample-accurately within the block. The host must already be `prepare()`d.
inline PulpsetRender render(HeadlessHost& host, const Pulpset& ps,
                            std::size_t total_samples, std::size_t block_size = 512) {
    PulpsetRender out;
    out.left.resize(total_samples, 0.0f);
    out.right.resize(total_samples, 0.0f);

    std::vector<float> obuf((std::size_t)2 * block_size);
    const float* in_planes[2] = {nullptr, nullptr};
    audio::BufferView<const float> in_view(in_planes, 0, 0);

    std::size_t ev = 0;
    for (std::size_t s = 0; s < total_samples; s += block_size) {
        const std::size_t n = std::min(block_size, total_samples - s);
        float* planes[2] = {obuf.data(), obuf.data() + n};
        audio::BufferView<float> out_view(planes, 2, n);

        midi::MidiBuffer midi_in, midi_out;
        // Apply all events whose sample falls in [s, s+n).
        for (; ev < ps.events.size() && ps.events[ev].sample < (std::int64_t)(s + n); ++ev) {
            const auto& e = ps.events[ev];
            if (e.sample < (std::int64_t)s) { /* past-due: still apply now */ }
            switch (e.kind) {
                case PulpsetEvent::Kind::Param:
                    host.state().set_value(e.id, e.value);
                    break;
                case PulpsetEvent::Kind::NoteOn: {
                    auto me = midi::MidiEvent::note_on(0, (std::uint8_t)e.id, (std::uint8_t)e.value);
                    me.sample_offset = (std::int32_t)std::max<std::int64_t>(0, e.sample - (std::int64_t)s);
                    midi_in.add(me);
                    break;
                }
                case PulpsetEvent::Kind::NoteOff: {
                    auto me = midi::MidiEvent::note_off(0, (std::uint8_t)e.id);
                    me.sample_offset = (std::int32_t)std::max<std::int64_t>(0, e.sample - (std::int64_t)s);
                    midi_in.add(me);
                    break;
                }
            }
        }
        midi_in.sort();
        host.process(out_view, in_view, midi_in, midi_out);

        std::copy(planes[0], planes[0] + n, out.left.begin() + s);
        std::copy(planes[1], planes[1] + n, out.right.begin() + s);
    }
    return out;
}

/// RMS of [a,b) of one channel — for range/property assertions.
inline double segment_rms(const std::vector<float>& v, std::size_t a, std::size_t b) {
    double acc = 0; std::size_t n = 0;
    for (std::size_t i = a; i < b && i < v.size(); ++i) { acc += (double)v[i] * v[i]; ++n; }
    return n ? std::sqrt(acc / n) : 0.0;
}

/// Write 32-bit float stereo WAV (offline render artifact).
inline bool write_wav_f32(const std::string& path, const PulpsetRender& r, std::uint32_t sample_rate = 48000) {
    std::ofstream o(path, std::ios::binary);
    if (!o) return false;
    const std::uint32_t ch = 2, bits = 32, n = (std::uint32_t)r.frames();
    const std::uint32_t data_bytes = n * ch * (bits / 8);
    auto u32 = [&](std::uint32_t v) { o.write((const char*)&v, 4); };
    auto u16 = [&](std::uint16_t v) { o.write((const char*)&v, 2); };
    o.write("RIFF", 4); u32(36 + data_bytes); o.write("WAVE", 4);
    o.write("fmt ", 4); u32(16); u16(3); u16((std::uint16_t)ch); u32(sample_rate);
    u32(sample_rate * ch * (bits / 8)); u16((std::uint16_t)(ch * (bits / 8))); u16((std::uint16_t)bits);
    o.write("data", 4); u32(data_bytes);
    for (std::uint32_t i = 0; i < n; ++i) { o.write((const char*)&r.left[i], 4); o.write((const char*)&r.right[i], 4); }
    return (bool)o;
}

} // namespace pulp::format
