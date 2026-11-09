#include <pulp/state/midi_learn_persistence.hpp>

#include <choc/text/choc_JSON.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <exception>

namespace pulp::state {
namespace {

constexpr char kMagic[4] = {'P', 'M', 'L', 'N'};
constexpr std::size_t kHeaderSize = 16;
constexpr uint16_t kRecordSizeV1 = 28;

void put16(std::vector<uint8_t>& out, uint16_t v) {
    out.push_back(static_cast<uint8_t>(v));
    out.push_back(static_cast<uint8_t>(v >> 8));
}
void put32(std::vector<uint8_t>& out, uint32_t v) {
    for (int i = 0; i < 4; ++i)
        out.push_back(static_cast<uint8_t>(v >> (8 * i)));
}
void putf(std::vector<uint8_t>& out, float f) {
    uint32_t v;
    std::memcpy(&v, &f, 4);
    put32(out, v);
}
uint16_t get16(const uint8_t* p) {
    return static_cast<uint16_t>(p[0] | (p[1] << 8));
}
uint32_t get32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}
float getf(const uint8_t* p) {
    const uint32_t v = get32(p);
    float f;
    std::memcpy(&f, &v, 4);
    return f;
}

uint32_t fnv1a(const uint8_t* p, std::size_t n) {
    uint32_t h = 2166136261u;
    for (std::size_t i = 0; i < n; ++i) {
        h ^= p[i];
        h *= 16777619u;
    }
    return h;
}

bool valid_number(MidiSourceKind kind, uint32_t number) {
    switch (kind) {
    case MidiSourceKind::cc7:
    case MidiSourceKind::poly_pressure:
    case MidiSourceKind::program_change:
        return number <= 127;
    case MidiSourceKind::cc14:
        return number <= 31;
    case MidiSourceKind::nrpn:
    case MidiSourceKind::rpn:
        return number <= 16383;
    case MidiSourceKind::pitch_bend:
    case MidiSourceKind::channel_pressure:
        return number == 0;
    }
    return false;
}

float clamp01(float v, float fallback) {
    return std::isfinite(v) ? std::clamp(v, 0.0f, 1.0f) : fallback;
}

const char* kind_name(MidiSourceKind k) {
    switch (k) {
    case MidiSourceKind::cc7:
        return "cc7";
    case MidiSourceKind::cc14:
        return "cc14";
    case MidiSourceKind::nrpn:
        return "nrpn";
    case MidiSourceKind::rpn:
        return "rpn";
    case MidiSourceKind::pitch_bend:
        return "pitch_bend";
    case MidiSourceKind::channel_pressure:
        return "channel_pressure";
    case MidiSourceKind::poly_pressure:
        return "poly_pressure";
    case MidiSourceKind::program_change:
        return "program_change";
    }
    return "cc7";
}
bool kind_from_name(std::string_view s, MidiSourceKind& out) {
    for (uint8_t i = 0; i <= static_cast<uint8_t>(MidiSourceKind::program_change); ++i) {
        const auto k = static_cast<MidiSourceKind>(i);
        if (s == kind_name(k)) {
            out = k;
            return true;
        }
    }
    return false;
}

const char* mode_name(RelativeMode m) {
    switch (m) {
    case RelativeMode::absolute:
        return "absolute";
    case RelativeMode::twos_complement:
        return "twos_complement";
    case RelativeMode::signed_bit:
        return "signed_bit";
    case RelativeMode::binary_offset:
        return "binary_offset";
    }
    return "absolute";
}
bool mode_from_name(std::string_view s, RelativeMode& out) {
    for (uint8_t i = 0; i <= static_cast<uint8_t>(RelativeMode::binary_offset); ++i) {
        const auto m = static_cast<RelativeMode>(i);
        if (s == mode_name(m)) {
            out = m;
            return true;
        }
    }
    return false;
}

const char* takeover_name(TakeoverMode t) {
    switch (t) {
    case TakeoverMode::jump:
        return "jump";
    case TakeoverMode::pickup:
        return "pickup";
    case TakeoverMode::scale:
        return "scale";
    }
    return "jump";
}
bool takeover_from_name(std::string_view s, TakeoverMode& out) {
    for (uint8_t i = 0; i <= static_cast<uint8_t>(TakeoverMode::scale); ++i) {
        const auto t = static_cast<TakeoverMode>(i);
        if (s == takeover_name(t)) {
            out = t;
            return true;
        }
    }
    return false;
}

bool number_of(const choc::value::ValueView& v, double& out) {
    if (v.isFloat64())
        out = v.getFloat64();
    else if (v.isFloat32())
        out = v.getFloat32();
    else if (v.isInt64())
        out = static_cast<double>(v.getInt64());
    else if (v.isInt32())
        out = v.getInt32();
    else
        return false;
    return true;
}

} // namespace

bool is_valid_midi_mapping(const MidiMapping& m) {
    const auto kind = static_cast<uint8_t>(m.source.kind);
    if (kind > static_cast<uint8_t>(MidiSourceKind::program_change))
        return false;
    if (m.source.channel > 15 && m.source.channel != MidiSource::kOmni)
        return false;
    if (!valid_number(m.source.kind, m.source.number))
        return false;
    if (static_cast<uint8_t>(m.mode) > static_cast<uint8_t>(RelativeMode::binary_offset))
        return false;
    if (static_cast<uint8_t>(m.takeover) > static_cast<uint8_t>(TakeoverMode::scale))
        return false;
    return true;
}

std::vector<uint8_t> serialize_midi_mappings(std::span<const MidiMapping> mappings) {
    const std::size_t n = std::min(mappings.size(), kMaxMidiMappings);
    std::vector<uint8_t> body;
    body.reserve(n * kRecordSizeV1);
    for (std::size_t i = 0; i < n; ++i) {
        const MidiMapping& m = mappings[i];
        body.push_back(static_cast<uint8_t>(m.source.kind));
        body.push_back(m.source.channel);
        put16(body, m.source.number);
        put32(body, m.id);
        putf(body, m.scale.min);
        putf(body, m.scale.max);
        body.push_back(static_cast<uint8_t>(m.mode));
        body.push_back(static_cast<uint8_t>(m.takeover));
        body.push_back(m.feedback ? 1 : 0);
        body.push_back(0);
        putf(body, m.relative.step);
        putf(body, m.relative.acceleration);
    }
    std::vector<uint8_t> out;
    out.reserve(kHeaderSize + body.size());
    out.insert(out.end(), kMagic, kMagic + 4);
    put16(out, kMidiMapChunkVersion);
    put16(out, kRecordSizeV1);
    put32(out, static_cast<uint32_t>(n));
    put32(out, fnv1a(body.data(), body.size()));
    out.insert(out.end(), body.begin(), body.end());
    return out;
}

MidiMapParseResult parse_midi_mappings(std::span<const uint8_t> data) {
    MidiMapParseResult r;
    if (data.empty())
        return r;
    auto corrupt = [&] {
        r.status = MidiMapParseResult::Status::corrupt;
        r.mappings.clear();
        return r;
    };
    if (data.size() < kHeaderSize || std::memcmp(data.data(), kMagic, 4) != 0)
        return corrupt();
    r.version = get16(data.data() + 4);
    const uint16_t record_size = get16(data.data() + 6);
    const uint32_t count = get32(data.data() + 8);
    const uint32_t checksum = get32(data.data() + 12);
    if (r.version == 0 || record_size < kRecordSizeV1)
        return corrupt();
    const uint64_t body_bytes = static_cast<uint64_t>(count) * record_size;
    if (body_bytes > data.size() - kHeaderSize)
        return corrupt();
    const uint8_t* body = data.data() + kHeaderSize;

    const bool newer = r.version > kMidiMapChunkVersion;
    if (!newer && fnv1a(body, static_cast<std::size_t>(body_bytes)) != checksum)
        return corrupt();
    if (newer)
        r.status = MidiMapParseResult::Status::newer_version;

    for (uint32_t i = 0; i < count; ++i) {
        if (r.mappings.size() >= kMaxMidiMappings) {
            r.dropped += count - i;
            break;
        }
        const uint8_t* p = body + static_cast<std::size_t>(i) * record_size;
        MidiMapping m;
        m.source.kind = static_cast<MidiSourceKind>(p[0]);
        m.source.channel = p[1];
        m.source.number = get16(p + 2);
        m.id = get32(p + 4);
        m.scale.min = clamp01(getf(p + 8), 0.0f);
        m.scale.max = clamp01(getf(p + 12), 1.0f);
        m.mode = static_cast<RelativeMode>(p[16]);
        m.takeover = static_cast<TakeoverMode>(p[17]);
        m.feedback = (p[18] & 1) != 0;
        m.relative.step = getf(p + 20);
        m.relative.acceleration = getf(p + 24);
        if (!std::isfinite(m.relative.step) || !std::isfinite(m.relative.acceleration) ||
            !is_valid_midi_mapping(m)) {
            ++r.dropped;
            continue;
        }
        r.mappings.push_back(m);
    }
    return r;
}

std::string midi_mappings_to_json(std::span<const MidiMapping> mappings) {
    auto root = choc::value::createObject("");
    root.addMember("version", choc::value::createInt64(kMidiMapChunkVersion));
    auto list = choc::value::createEmptyArray();
    const std::size_t n = std::min(mappings.size(), kMaxMidiMappings);
    for (std::size_t i = 0; i < n; ++i) {
        const MidiMapping& m = mappings[i];
        auto src = choc::value::createObject("");
        src.addMember("kind", choc::value::createString(kind_name(m.source.kind)));
        if (m.source.channel == MidiSource::kOmni)
            src.addMember("channel", choc::value::createString("omni"));
        else
            src.addMember("channel", choc::value::createInt64(m.source.channel));
        src.addMember("number", choc::value::createInt64(m.source.number));
        auto rel = choc::value::createObject("");
        rel.addMember("step", choc::value::createFloat64(m.relative.step));
        rel.addMember("acceleration", choc::value::createFloat64(m.relative.acceleration));
        auto e = choc::value::createObject("");
        e.addMember("source", src);
        e.addMember("param", choc::value::createInt64(m.id));
        e.addMember("min", choc::value::createFloat64(m.scale.min));
        e.addMember("max", choc::value::createFloat64(m.scale.max));
        e.addMember("mode", choc::value::createString(mode_name(m.mode)));
        e.addMember("relative", rel);
        e.addMember("takeover", choc::value::createString(takeover_name(m.takeover)));
        e.addMember("feedback", choc::value::createBool(m.feedback));
        list.addArrayElement(e);
    }
    root.addMember("mappings", list);
    return choc::json::toString(root, true);
}

MidiMapParseResult midi_mappings_from_json(std::string_view text) {
    MidiMapParseResult r;
    try {
        auto root = choc::json::parse(std::string(text));
        if (!root.isObject()) {
            r.status = MidiMapParseResult::Status::corrupt;
            return r;
        }
        double version = 0;
        if (number_of(root["version"], version) && version >= 1)
            r.version = static_cast<uint16_t>(std::min(version, 65535.0));
        else {
            r.status = MidiMapParseResult::Status::corrupt;
            return r;
        }
        if (r.version > kMidiMapChunkVersion)
            r.status = MidiMapParseResult::Status::newer_version;

        auto list = root["mappings"];
        if (!list.isArray()) {
            r.status = MidiMapParseResult::Status::corrupt;
            return r;
        }
        for (uint32_t i = 0; i < list.size(); ++i) {
            if (r.mappings.size() >= kMaxMidiMappings) {
                r.dropped += list.size() - i;
                break;
            }
            auto e = list[i];
            if (!e.isObject()) {
                ++r.dropped;
                continue;
            }
            auto src = e["source"];
            MidiMapping m;
            double v = 0;
            bool ok = e.isObject() && src.isObject() && src["kind"].isString() &&
                      kind_from_name(src["kind"].getString(), m.source.kind);
            if (ok) {
                auto ch = src["channel"];
                if (ch.isString()) {
                    ok = ch.getString() == "omni";
                    m.source.channel = MidiSource::kOmni;
                } else if (number_of(ch, v) && v >= 0 && v <= 15) {
                    m.source.channel = static_cast<uint8_t>(v);
                } else {
                    ok = false;
                }
            }
            if (ok) {
                ok = number_of(src["number"], v) && v >= 0 && v <= 65535;
                m.source.number = static_cast<uint16_t>(v);
            }
            if (ok) {
                ok = number_of(e["param"], v) && v >= 0 && v <= 4294967295.0;
                m.id = static_cast<ParamID>(v);
            }
            if (ok) {
                if (number_of(e["min"], v))
                    m.scale.min = clamp01(static_cast<float>(v), 0.0f);
                if (number_of(e["max"], v))
                    m.scale.max = clamp01(static_cast<float>(v), 1.0f);
                if (e["mode"].isString())
                    ok = mode_from_name(e["mode"].getString(), m.mode);
                if (e["takeover"].isString())
                    ok = ok && takeover_from_name(e["takeover"].getString(), m.takeover);
                if (e["feedback"].isBool())
                    m.feedback = e["feedback"].getBool();
                auto rel = e["relative"];
                if (rel.isObject()) {
                    if (number_of(rel["step"], v) && std::isfinite(v))
                        m.relative.step = static_cast<float>(v);
                    if (number_of(rel["acceleration"], v) && std::isfinite(v))
                        m.relative.acceleration = static_cast<float>(v);
                }
            }
            if (ok && is_valid_midi_mapping(m))
                r.mappings.push_back(m);
            else
                ++r.dropped;
        }
    } catch (const std::exception&) {
        r.status = MidiMapParseResult::Status::corrupt;
        r.mappings.clear();
    }
    return r;
}

} // namespace pulp::state
