#include <pulp/midi/midi_file.hpp>

#include <algorithm>
#include <cmath>
#include <fstream>
#include <optional>
#include <string>
#include <utility>
#include <vector>

// Standard MIDI Files are read and written here directly rather than through a
// flat-sequence helper, because a sequence of short messages cannot express the
// three things an SMF actually needs:
//
//   * a tempo, which lives in a meta event and is what gives a tick its
//     duration. Without it a writer cannot place events at the file's stated
//     division, and a reader cannot recover their times.
//   * more than one track.
//   * an end-of-track meta, which the specification requires.
//
// Routing the WRITE path through a lossy intermediate silently produced files
// whose header division disagreed with their own tick values: the ticks were
// baked at the intermediate's fixed timebase and the header was then stamped
// with the caller's. A file written at 480 PPQN played at roughly half speed,
// and nothing in the write path could notice.
//
// The READ path was correct about tempo (it honoured a tempo map), but it merged
// every track into one and never reported the file's tempo, so a multi-track file
// could not survive a round trip and a caller could not learn what tempo it had
// just read. Both halves live here now so the two sides cannot drift apart.

namespace pulp::midi {

namespace {

// ── Byte-level primitives ───────────────────────────────────────────────────
// SMF is big-endian throughout, and its delta times are base-128 variable-length
// quantities: seven bits per byte, high bit set on every byte but the last.

void put_u8(std::vector<uint8_t>& out, uint8_t v) { out.push_back(v); }

void put_u16(std::vector<uint8_t>& out, uint16_t v) {
    out.push_back(static_cast<uint8_t>(v >> 8));
    out.push_back(static_cast<uint8_t>(v));
}

void put_u32(std::vector<uint8_t>& out, uint32_t v) {
    out.push_back(static_cast<uint8_t>(v >> 24));
    out.push_back(static_cast<uint8_t>(v >> 16));
    out.push_back(static_cast<uint8_t>(v >> 8));
    out.push_back(static_cast<uint8_t>(v));
}

void put_varlen(std::vector<uint8_t>& out, uint32_t v) {
    uint8_t buf[5];
    int n = 0;
    buf[n++] = static_cast<uint8_t>(v & 0x7F);
    while ((v >>= 7) != 0)
        buf[n++] = static_cast<uint8_t>((v & 0x7F) | 0x80);
    while (n > 0)
        out.push_back(buf[--n]);
}

void put_bytes(std::vector<uint8_t>& out, const char* s, size_t n) {
    out.insert(out.end(), s, s + n);
}

/// A bounds-checked cursor. Every read is checked, so a truncated or hostile
/// file fails the parse instead of walking off the end of the buffer.
struct Cursor {
    const uint8_t* p = nullptr;
    const uint8_t* end = nullptr;

    bool has(size_t n) const { return static_cast<size_t>(end - p) >= n; }

    bool u8(uint8_t& out) {
        if (!has(1)) return false;
        out = *p++;
        return true;
    }
    bool u16(uint16_t& out) {
        if (!has(2)) return false;
        out = static_cast<uint16_t>((p[0] << 8) | p[1]);
        p += 2;
        return true;
    }
    bool u32(uint32_t& out) {
        if (!has(4)) return false;
        out = (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16)
            | (static_cast<uint32_t>(p[2]) << 8) | static_cast<uint32_t>(p[3]);
        p += 4;
        return true;
    }
    bool varlen(uint32_t& out) {
        out = 0;
        for (int i = 0; i < 4; ++i) {          // a VLQ is at most four bytes
            uint8_t b;
            if (!u8(b)) return false;
            out = (out << 7) | static_cast<uint32_t>(b & 0x7F);
            if ((b & 0x80) == 0) return true;
        }
        return false;                          // a fifth continuation byte: malformed
    }
    bool skip(size_t n) {
        if (!has(n)) return false;
        p += n;
        return true;
    }
};

constexpr uint32_t kChunkMThd = 0x4D546864;
constexpr uint32_t kChunkMTrk = 0x4D54726B;

constexpr uint8_t kMetaPrefix     = 0xFF;
constexpr uint8_t kMetaTrackName  = 0x03;
constexpr uint8_t kMetaEndOfTrack = 0x2F;
constexpr uint8_t kMetaTempo      = 0x51;

constexpr double kMicrosPerMinute = 60'000'000.0;
constexpr double kDefaultBpm      = 120.0;

/// Microseconds per quarter note — how a tempo is actually stored. The field is
/// three bytes, so it saturates rather than wrapping on an absurd BPM.
uint32_t micros_per_quarter(double bpm) {
    if (!(bpm > 0.0)) bpm = kDefaultBpm;
    const double us = kMicrosPerMinute / bpm;
    return static_cast<uint32_t>(std::lround(std::clamp(us, 1.0, 16777215.0)));
}

double bpm_from_micros(uint32_t us) {
    if (us == 0) return kDefaultBpm;
    return kMicrosPerMinute / static_cast<double>(us);
}

/// How many data bytes follow a channel-voice status byte.
int data_bytes_for(uint8_t status) {
    switch (status & 0xF0) {
        case 0xC0:            // program change
        case 0xD0: return 1;  // channel pressure
        default:   return 2;  // note off/on, poly pressure, CC, pitch bend
    }
}

} // namespace

double MidiFileData::duration_seconds() const {
    double max_time = 0;
    for (auto& track : tracks)
        for (auto& event : track.events)
            if (event.time_seconds > max_time) max_time = event.time_seconds;
    return max_time;
}

size_t MidiFileData::total_events() const {
    size_t total = 0;
    for (auto& track : tracks) total += track.events.size();
    return total;
}

// ── Read ────────────────────────────────────────────────────────────────────

std::optional<MidiFileData> read_midi_file(const std::string& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file.is_open()) return std::nullopt;

    const std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(file)),
                                     std::istreambuf_iterator<char>());
    Cursor c{bytes.data(), bytes.data() + bytes.size()};

    uint32_t chunk_id = 0, header_len = 0;
    uint16_t format = 0, ntracks = 0, division = 0;
    if (!c.u32(chunk_id) || chunk_id != kChunkMThd) return std::nullopt;
    if (!c.u32(header_len) || header_len < 6) return std::nullopt;
    if (!c.u16(format) || !c.u16(ntracks) || !c.u16(division)) return std::nullopt;
    if (!c.skip(header_len - 6)) return std::nullopt;   // tolerate a longer header
    (void)format;

    if (division == 0) return std::nullopt;

    // A negative division is SMPTE timecode: the high byte is a negative frame
    // rate and the low byte is ticks per frame. A tick is then an absolute slice
    // of wall-clock time and the tempo map does not apply to it at all. The file
    // has no quarter note, so ticks_per_quarter carries the default rather than a
    // reinterpretation of a number that does not mean that.
    const bool smpte = (division & 0x8000) != 0;
    double smpte_seconds_per_tick = 0.0;
    if (smpte) {
        const auto fps = static_cast<double>(-static_cast<int8_t>(division >> 8));
        const auto ticks_per_frame = static_cast<double>(division & 0xFF);
        if (!(fps > 0.0) || !(ticks_per_frame > 0.0)) return std::nullopt;
        smpte_seconds_per_tick = 1.0 / (fps * ticks_per_frame);
    }

    MidiFileData result;
    result.ticks_per_quarter = smpte ? 480 : division;

    // Ticks are collected first and converted afterwards, because the tempo that
    // governs a tick can be declared after the events it applies to, and because
    // a tempo change on track 0 retimes every OTHER track too.
    struct RawEvent {
        uint32_t tick = 0;
        MidiEvent event;
    };
    std::vector<std::pair<std::string, std::vector<RawEvent>>> raw_tracks;

    // (tick, microseconds-per-quarter). A file may change tempo as often as it
    // likes; dropping all but the first would silently retime everything after
    // the first change.
    std::vector<std::pair<uint32_t, uint32_t>> tempo_map;

    for (uint16_t t = 0; t < ntracks; ++t) {
        uint32_t track_id = 0, track_len = 0;
        if (!c.u32(track_id) || !c.u32(track_len)) return std::nullopt;
        if (!c.has(track_len)) return std::nullopt;

        // Anything that is not an MTrk is to be skipped, per the specification.
        if (track_id != kChunkMTrk) {
            c.skip(track_len);
            continue;
        }

        Cursor tc{c.p, c.p + track_len};
        c.skip(track_len);

        std::string name;
        std::vector<RawEvent> events;
        uint32_t tick = 0;
        uint8_t running_status = 0;

        while (tc.p < tc.end) {
            uint32_t delta = 0;
            if (!tc.varlen(delta)) return std::nullopt;
            tick += delta;

            uint8_t status;
            if (!tc.u8(status)) return std::nullopt;

            if (status == kMetaPrefix) {
                uint8_t type;
                uint32_t len;
                if (!tc.u8(type) || !tc.varlen(len) || !tc.has(len)) return std::nullopt;

                if (type == kMetaTempo && len == 3) {
                    const uint32_t us = (static_cast<uint32_t>(tc.p[0]) << 16)
                                      | (static_cast<uint32_t>(tc.p[1]) << 8)
                                      |  static_cast<uint32_t>(tc.p[2]);
                    tempo_map.emplace_back(tick, us);
                } else if (type == kMetaTrackName && len > 0) {
                    name.assign(reinterpret_cast<const char*>(tc.p), len);
                } else if (type == kMetaEndOfTrack) {
                    break;
                }
                if (!tc.skip(len)) return std::nullopt;
                continue;
            }

            if (status == 0xF0 || status == 0xF7) {  // sysex: not modeled, but it
                uint32_t len;                        // must be consumed EXACTLY or
                if (!tc.varlen(len)) return std::nullopt;   // the parse desyncs
                if (!tc.skip(len)) return std::nullopt;
                running_status = 0;                  // sysex cancels running status
                continue;
            }

            // Running status: a data byte where a status byte was expected means
            // "same status as last time". A file that uses it and a parser that
            // does not will desync on the very first repeated note.
            uint8_t d1;
            if (status < 0x80) {
                if (running_status == 0) return std::nullopt;
                d1 = status;
                status = running_status;
            } else {
                running_status = status;
                if (!tc.u8(d1)) return std::nullopt;
            }

            uint8_t d2 = 0;
            if (data_bytes_for(status) == 2 && !tc.u8(d2)) return std::nullopt;

            RawEvent re;
            re.tick = tick;
            re.event.message = choc::midi::ShortMessage(status, d1, d2);
            events.push_back(re);
        }

        raw_tracks.emplace_back(std::move(name), std::move(events));
    }

    std::stable_sort(tempo_map.begin(), tempo_map.end(),
                     [](const auto& a, const auto& b) { return a.first < b.first; });

    // MidiFileData carries ONE tempo, so it reports the tempo the file starts at.
    // The conversion below still honours every later change, so a file with a
    // tempo map reads back with correct times even though it round-trips to a
    // single tempo.
    const uint32_t initial_us = tempo_map.empty() ? micros_per_quarter(kDefaultBpm)
                                                  : tempo_map.front().second;
    result.tempo_bpm = smpte ? kDefaultBpm : bpm_from_micros(initial_us);

    // Ticks to seconds, walking the tempo map. Each segment runs at the tempo in
    // force at its start, so the elapsed time accumulates segment by segment
    // rather than being computed from a single rate that was never true.
    const auto to_seconds = [&](uint32_t tick) -> double {
        if (smpte) return static_cast<double>(tick) * smpte_seconds_per_tick;

        double seconds = 0.0;
        uint32_t at = 0;
        double spt = static_cast<double>(micros_per_quarter(kDefaultBpm)) / 1'000'000.0
                     / static_cast<double>(division);

        for (const auto& [change_tick, us] : tempo_map) {
            if (change_tick >= tick) break;
            seconds += static_cast<double>(change_tick - at) * spt;
            at = change_tick;
            spt = static_cast<double>(us) / 1'000'000.0 / static_cast<double>(division);
        }
        return seconds + static_cast<double>(tick - at) * spt;
    };

    for (auto& [name, events] : raw_tracks) {
        MidiTrack track;
        track.name = name;
        track.events.reserve(events.size());
        for (auto& re : events) {
            TimedMidiEvent te;
            te.time_seconds = to_seconds(re.tick);
            te.event = re.event;
            te.event.timestamp = te.time_seconds;
            track.events.push_back(te);
        }
        result.tracks.push_back(std::move(track));
    }

    return result;
}

// ── Write ───────────────────────────────────────────────────────────────────

bool write_midi_file(const std::string& path, const MidiFileData& data) {
    const int division = data.ticks_per_quarter > 0 ? data.ticks_per_quarter : 480;
    const double bpm = data.tempo_bpm > 0.0 ? data.tempo_bpm : kDefaultBpm;

    // Seconds to ticks at the file's OWN division and tempo. This is the whole
    // bug in one line: the header's division and the tick values must be derived
    // from the same two numbers, or the file declares one timebase and is written
    // in another.
    const double ticks_per_second = (bpm / 60.0) * static_cast<double>(division);

    const auto& tracks = data.tracks;
    const auto ntracks = static_cast<uint16_t>(std::max<size_t>(tracks.size(), 1));

    std::vector<uint8_t> out;
    out.reserve(8192);

    put_bytes(out, "MThd", 4);
    put_u32(out, 6);
    put_u16(out, ntracks > 1 ? 1 : 0);   // format 1 = simultaneous tracks
    put_u16(out, ntracks);
    put_u16(out, static_cast<uint16_t>(division));

    for (uint16_t t = 0; t < ntracks; ++t) {
        std::vector<uint8_t> body;

        std::vector<TimedMidiEvent> events;
        std::string name;
        if (t < tracks.size()) {
            events = tracks[t].events;
            name = tracks[t].name;
            // Stable, so two events at the same instant keep the order the caller
            // gave them — a note-off followed by a note-on at one tick stays that
            // way instead of becoming a coin flip.
            std::stable_sort(events.begin(), events.end(),
                             [](const TimedMidiEvent& a, const TimedMidiEvent& b) {
                                 return a.time_seconds < b.time_seconds;
                             });
        }

        if (!name.empty()) {
            put_varlen(body, 0);
            put_u8(body, kMetaPrefix);
            put_u8(body, kMetaTrackName);
            put_varlen(body, static_cast<uint32_t>(name.size()));
            put_bytes(body, name.data(), name.size());
        }

        // The tempo goes on the first track at tick zero. Without it a reader has
        // to assume one, and every event in the file lands at the wrong time.
        if (t == 0) {
            const uint32_t us = micros_per_quarter(bpm);
            put_varlen(body, 0);
            put_u8(body, kMetaPrefix);
            put_u8(body, kMetaTempo);
            put_varlen(body, 3);
            put_u8(body, static_cast<uint8_t>(us >> 16));
            put_u8(body, static_cast<uint8_t>(us >> 8));
            put_u8(body, static_cast<uint8_t>(us));
        }

        uint32_t last_tick = 0;
        for (const auto& te : events) {
            const uint8_t* d = te.event.data();
            const uint8_t status = d[0];
            if (status < 0x80) continue;              // not a channel message

            const double t_sec = te.time_seconds > 0.0 ? te.time_seconds : 0.0;
            const auto tick = static_cast<uint32_t>(std::llround(t_sec * ticks_per_second));

            // A delta is unsigned, so an out-of-order event would wrap into an
            // enormous gap. The sort makes ticks non-decreasing; this makes that
            // guarantee load-bearing rather than assumed.
            const uint32_t delta = tick > last_tick ? tick - last_tick : 0;
            last_tick = std::max(last_tick, tick);

            put_varlen(body, delta);
            put_u8(body, status);                     // explicit: no running status
            put_u8(body, d[1]);
            if (data_bytes_for(status) == 2) put_u8(body, d[2]);
        }

        // Required by the specification. Its absence is what makes a file
        // technically non-conformant, and what some hosts refuse outright.
        put_varlen(body, 0);
        put_u8(body, kMetaPrefix);
        put_u8(body, kMetaEndOfTrack);
        put_varlen(body, 0);

        put_bytes(out, "MTrk", 4);
        put_u32(out, static_cast<uint32_t>(body.size()));
        out.insert(out.end(), body.begin(), body.end());
    }

    std::ofstream file(path, std::ios::binary);
    if (!file.is_open()) return false;
    file.write(reinterpret_cast<const char*>(out.data()),
               static_cast<std::streamsize>(out.size()));
    // Flush/close BEFORE reporting success: ofstream is buffered, so a small
    // payload sits in the buffer and file.good() would report the healthy
    // buffered state while a deferred flush failure (e.g. ENOSPC) only surfaces
    // at close. close() sets failbit on a flush error and preserves any failbit
    // already set by a synchronous write failure.
    file.close();
    return file.good();
}

} // namespace pulp::midi
