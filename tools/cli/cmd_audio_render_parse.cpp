// cmd_audio_render_parse.cpp — argument grammar for `pulp audio render`.
//
// Split from cmd_audio_render.cpp (the driver) so the parser is a pure,
// dependency-light translation unit the unit tests can link without dragging in
// the host / plugin-loader / audio-analysis stack. Mirrors the cmd_run_parse.cpp
// split used by `pulp run`.

#include "cmd_audio_render.hpp"

#include <cerrno>
#include <charconv>
#include <cmath>
#include <cstdlib>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace pulp::cli {
namespace {

std::optional<std::uint64_t> parse_u64(std::string_view s) {
    std::uint64_t v = 0;
    const auto* begin = s.data();
    const auto* end = s.data() + s.size();
    auto [ptr, ec] = std::from_chars(begin, end, v);
    if (ec != std::errc{} || ptr != end) return std::nullopt;
    return v;
}

std::optional<std::uint32_t> parse_u32(std::string_view s) {
    auto v = parse_u64(s);
    if (!v || *v > 0xFFFFFFFFull) return std::nullopt;
    return static_cast<std::uint32_t>(*v);
}

std::optional<double> parse_double(std::string_view s) {
    if (s.empty()) return std::nullopt;
    const std::string str(s);
    char* end = nullptr;
    errno = 0;
    const double v = std::strtod(str.c_str(), &end);
    if (end != str.c_str() + str.size() || errno != 0) return std::nullopt;
    if (!std::isfinite(v)) return std::nullopt;
    return v;
}

ParseAudioRenderResult usage_error(std::string message) {
    ParseAudioRenderResult r;
    r.ok = false;
    r.exit_code = 2;
    r.error = std::move(message);
    return r;
}

// --param <id>=<value>[@frame]  (value is PLAIN domain — see header).
bool parse_param(std::string_view spec, ParseAudioRenderResult& out) {
    const auto eq = spec.find('=');
    if (eq == std::string_view::npos) return false;
    const auto id = parse_u32(spec.substr(0, eq));
    if (!id) return false;
    auto rhs = spec.substr(eq + 1);
    std::uint64_t frame = 0;
    if (const auto at = rhs.find('@'); at != std::string_view::npos) {
        const auto f = parse_u64(rhs.substr(at + 1));
        if (!f) return false;
        frame = *f;
        rhs = rhs.substr(0, at);
    }
    const auto value = parse_double(rhs);
    if (!value) return false;
    out.params.push_back({*id, static_cast<float>(*value), frame});
    return true;
}

// --midi note:<note>,<vel>,<on_frame>[,<off_frame>]
bool parse_midi(std::string_view spec, ParseAudioRenderResult& out) {
    constexpr std::string_view kPrefix = "note:";
    if (spec.substr(0, kPrefix.size()) != kPrefix) return false;
    auto body = spec.substr(kPrefix.size());
    std::vector<std::string_view> fields;
    while (!body.empty()) {
        const auto comma = body.find(',');
        if (comma == std::string_view::npos) {
            fields.push_back(body);
            break;
        }
        fields.push_back(body.substr(0, comma));
        body = body.substr(comma + 1);
    }
    if (fields.size() < 3 || fields.size() > 4) return false;
    const auto note = parse_u32(fields[0]);
    const auto vel = parse_u32(fields[1]);
    const auto on = parse_u64(fields[2]);
    if (!note || !vel || !on || *note > 127 || *vel > 127) return false;
    AudioRenderMidi m;
    m.note = static_cast<std::uint8_t>(*note);
    m.velocity = static_cast<std::uint8_t>(*vel);
    m.on_frame = *on;
    if (fields.size() == 4) {
        const auto off = parse_u64(fields[3]);
        if (!off) return false;
        m.has_off = true;
        m.off_frame = *off;
    }
    out.midi.push_back(m);
    return true;
}

// --input-signal silence | sine:<hz>[,<dbfs>] | noise[:<seed>] | impulse[:<frame>]
bool parse_input_signal(std::string_view spec, ParseAudioRenderResult& out) {
    if (spec == "silence") {
        out.input_kind = AudioRenderInputKind::Silence;
        return true;
    }
    // Broadband and aperiodic — the stimulus a delay measurement needs.
    if (spec == "noise" || spec.substr(0, 6) == "noise:") {
        out.input_kind = AudioRenderInputKind::Noise;
        if (spec.size() > 6) {
            const auto seed = parse_u64(spec.substr(6));
            if (!seed) return false;
            out.noise_seed = *seed;
        }
        return true;
    }
    if (spec == "impulse" || spec.substr(0, 8) == "impulse:") {
        out.input_kind = AudioRenderInputKind::Impulse;
        if (spec.size() > 8) {
            const auto frame = parse_u64(spec.substr(8));
            if (!frame) return false;
            out.impulse_frame = *frame;
        }
        return true;
    }
    constexpr std::string_view kSine = "sine:";
    if (spec.substr(0, kSine.size()) == kSine) {
        auto body = spec.substr(kSine.size());
        std::string_view hz = body;
        std::string_view dbfs;
        if (const auto comma = body.find(','); comma != std::string_view::npos) {
            hz = body.substr(0, comma);
            dbfs = body.substr(comma + 1);
        }
        const auto hz_v = parse_double(hz);
        if (!hz_v || *hz_v <= 0.0) return false;
        out.input_kind = AudioRenderInputKind::Sine;
        out.sine_hz = *hz_v;
        if (!dbfs.empty()) {
            const auto db = parse_double(dbfs);
            if (!db) return false;
            out.sine_dbfs = *db;
        }
        return true;
    }
    return false;
}

}  // namespace

ParseAudioRenderResult parse_audio_render_args(const std::vector<std::string>& args) {
    ParseAudioRenderResult r;

    bool has_ms = false, has_frames = false, input_signal_seen = false;
    std::uint64_t duration_ms = 0, duration_frames = 0;

    // Each flag that takes a value accepts both `--flag value` and `--flag=value`.
    auto split = [](const std::string& tok) -> std::pair<std::string, std::optional<std::string>> {
        if (tok.rfind("--", 0) == 0) {
            if (const auto eq = tok.find('='); eq != std::string::npos)
                return {tok.substr(0, eq), tok.substr(eq + 1)};
        }
        return {tok, std::nullopt};
    };

    for (std::size_t i = 0; i < args.size(); ++i) {
        const auto [key, inlinev] = split(args[i]);

        if (key == "--help" || key == "-h") {
            r.help = true;
            r.exit_code = 0;
            return r;
        }
        if (key == "--json") {
            r.json = true;
            continue;
        }

        // Flags below all require a value (inline `=` or the next token).
        auto take_value = [&](std::optional<std::string>& dst) -> bool {
            if (inlinev) { dst = *inlinev; return true; }
            if (i + 1 >= args.size() || (!args[i + 1].empty() && args[i + 1][0] == '-')) {
                return false;
            }
            dst = args[++i];
            return true;
        };
        std::optional<std::string> value;

        if (key == "--plugin") {
            if (!take_value(value)) return usage_error(key + " requires a value");
            r.plugin_path = *value;
        } else if (key == "--format") {
            if (!take_value(value)) return usage_error(key + " requires a value");
            r.format = *value;
        } else if (key == "--id") {
            if (!take_value(value)) return usage_error(key + " requires a value");
            r.unique_id = *value;
        } else if (key == "--sample-rate") {
            if (!take_value(value)) return usage_error(key + " requires a value");
            const auto v = parse_double(*value);
            if (!v || *v <= 0.0) return usage_error("--sample-rate must be a positive number");
            r.sample_rate = *v;
        } else if (key == "--block") {
            if (!take_value(value)) return usage_error(key + " requires a value");
            const auto v = parse_u32(*value);
            if (!v || *v == 0) return usage_error("--block must be a positive integer");
            r.block = *v;
        } else if (key == "--in-channels") {
            if (!take_value(value)) return usage_error(key + " requires a value");
            const auto v = parse_u32(*value);
            if (!v) return usage_error("--in-channels must be a non-negative integer");
            r.in_channels = *v;
        } else if (key == "--out-channels") {
            if (!take_value(value)) return usage_error(key + " requires a value");
            const auto v = parse_u32(*value);
            if (!v || *v == 0) return usage_error("--out-channels must be a positive integer");
            r.out_channels = *v;
        } else if (key == "--duration-ms") {
            if (!take_value(value)) return usage_error(key + " requires a value");
            const auto v = parse_u64(*value);
            if (!v || *v == 0) return usage_error("--duration-ms must be a positive integer");
            duration_ms = *v;
            has_ms = true;
        } else if (key == "--duration-frames") {
            if (!take_value(value)) return usage_error(key + " requires a value");
            const auto v = parse_u64(*value);
            if (!v || *v == 0) return usage_error("--duration-frames must be a positive integer");
            duration_frames = *v;
            has_frames = true;
        } else if (key == "--input") {
            if (!take_value(value)) return usage_error(key + " requires a value");
            r.input_kind = AudioRenderInputKind::Wav;
            r.input_wav = *value;
        } else if (key == "--input-signal") {
            if (!take_value(value)) return usage_error(key + " requires a value");
            if (!parse_input_signal(*value, r))
                return usage_error("--input-signal must be 'silence', 'sine:<hz>[,<dbfs>]', "
                                   "'noise[:<seed>]', or 'impulse[:<frame>]'");
            input_signal_seen = true;
        } else if (key == "--latency-report") {
            if (!take_value(value)) return usage_error(key + " requires a value");
            r.latency_report_path = *value;
        } else if (key == "--latency-policy") {
            if (!take_value(value)) return usage_error(key + " requires a value");
            if (*value == "delayed-null")
                r.latency_policy = AudioRenderLatencyPolicy::DelayedNull;
            else if (*value == "marker")
                r.latency_policy = AudioRenderLatencyPolicy::Marker;
            else
                return usage_error("--latency-policy must be 'delayed-null' or 'marker'");
        } else if (key == "--latency-tolerance") {
            if (!take_value(value)) return usage_error(key + " requires a value");
            const auto v = parse_u64(*value);
            if (!v) return usage_error("--latency-tolerance must be a nonnegative integer");
            r.latency_tolerance = static_cast<std::int64_t>(*v);
        } else if (key == "--latency-intrinsic") {
            if (!take_value(value)) return usage_error(key + " requires a value");
            const auto v = parse_u64(*value);
            if (!v) return usage_error("--latency-intrinsic must be a nonnegative integer");
            r.latency_intrinsic = static_cast<std::int64_t>(*v);
        } else if (key == "--param") {
            if (!take_value(value)) return usage_error(key + " requires a value");
            if (!parse_param(*value, r))
                return usage_error("--param must be '<id>=<value>[@frame]' (value is plain domain)");
        } else if (key == "--midi") {
            if (!take_value(value)) return usage_error(key + " requires a value");
            if (!parse_midi(*value, r))
                return usage_error("--midi must be 'note:<note>,<vel>,<on>[,<off>]'");
        } else if (key == "--out") {
            if (!take_value(value)) return usage_error(key + " requires a value");
            r.out_wav = *value;
        } else if (key == "--manifest") {
            if (!take_value(value)) return usage_error(key + " requires a value");
            r.manifest_path = *value;
        } else {
            return usage_error("unknown flag: " + key);
        }
    }

    // Required + mutually-exclusive validation.
    if (r.plugin_path.empty()) return usage_error("--plugin <bundle> is required");
    if (r.out_wav.empty()) return usage_error("--out <file.wav> is required");
    if (!r.input_wav.empty() && input_signal_seen)
        return usage_error("--input and --input-signal are mutually exclusive");
    if (r.in_channels == 0 && r.input_kind != AudioRenderInputKind::Silence)
        return usage_error("--in-channels 0 cannot be combined with --input or sine --input-signal");

    // Latency probe: the flags have to agree with each other, and with a
    // stimulus that can actually reveal a delay. Failing here beats emitting an
    // "inconclusive" artifact the caller then has to decode.
    const bool wants_latency = !r.latency_report_path.empty() ||
                               r.latency_policy != AudioRenderLatencyPolicy::None;
    if (wants_latency) {
        if (r.latency_report_path.empty())
            return usage_error("--latency-policy requires --latency-report <file.json>");
        // Default the policy from the stimulus the caller chose, since impulse
        // and noise each have one policy that fits them.
        if (r.latency_policy == AudioRenderLatencyPolicy::None) {
            r.latency_policy = r.input_kind == AudioRenderInputKind::Impulse
                                   ? AudioRenderLatencyPolicy::Marker
                                   : AudioRenderLatencyPolicy::DelayedNull;
        }
        // A probe needs input to probe WITH.
        if (r.in_channels == 0)
            return usage_error("--latency-report needs an input bus; --in-channels 0 "
                               "cannot carry a probe stimulus");
        if (r.input_kind == AudioRenderInputKind::Silence)
            return usage_error(
                "--latency-report needs a stimulus that can reveal a delay; silence "
                "cannot. Use --input-signal noise (for --latency-policy delayed-null) "
                "or --input-signal impulse (for --latency-policy marker)");
        if (r.latency_policy == AudioRenderLatencyPolicy::Marker &&
            r.input_kind != AudioRenderInputKind::Impulse)
            return usage_error("--latency-policy marker needs exactly one onset in the "
                               "input; use --input-signal impulse[:<frame>]");
        if (r.latency_policy == AudioRenderLatencyPolicy::DelayedNull &&
            r.input_kind == AudioRenderInputKind::Sine)
            return usage_error(
                "--latency-policy delayed-null cannot measure a delay from a sine: a "
                "tone whose period is a whole number of samples nulls just as well one "
                "period late. Use --input-signal noise");
    }
    if (r.latency_intrinsic != 0 && r.latency_policy != AudioRenderLatencyPolicy::Marker)
        return usage_error("--latency-intrinsic applies only to --latency-policy marker");
    if (has_ms && has_frames)
        return usage_error("--duration-ms and --duration-frames are mutually exclusive");
    if (!has_ms && !has_frames)
        return usage_error("one of --duration-ms / --duration-frames is required");

    r.duration_frames = has_frames
        ? duration_frames
        : static_cast<std::uint64_t>(std::llround(
              static_cast<double>(duration_ms) / 1000.0 * r.sample_rate));
    if (r.duration_frames == 0)
        return usage_error("resolved duration is zero frames");
    if (r.block > r.duration_frames)
        r.block = static_cast<std::uint32_t>(r.duration_frames);

    r.ok = true;
    return r;
}

}  // namespace pulp::cli
