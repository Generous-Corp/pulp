#include <pulp/inspect/control_agent_toolkit.hpp>

#include <pulp/format/offline_render_host.hpp>
#include <pulp/state/preset_manager.hpp>
#include <pulp/state/store.hpp>

#include <choc/text/choc_JSON.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <limits>
#include <numbers>
#include <optional>
#include <vector>

namespace pulp::inspect {
namespace {

using Value = choc::value::Value;
using Result = ControlVendorOperationResult;

constexpr std::array<std::string_view, 9> kOperationNames{
    "presets.list", "presets.load",    "presets.save", "patch.schema",   "patch.get",
    "patch.set",    "patch.randomize", "notes.play",   "render.analyze",
};

// dB values are floored so silence stays finite JSON.
constexpr double kFloorDb = -200.0;

Result invalid(std::string explanation) {
    return Result::failure(ControlResultCode::InvalidRequest, std::move(explanation));
}

std::optional<Value> parse_arguments(std::string_view json) {
    try {
        auto value = choc::json::parse(json);
        if (value.isObject())
            return value;
    } catch (...) {
    }
    return std::nullopt;
}

double number(const auto& value, double fallback = 0.0) {
    if (value.isInt32() || value.isInt64())
        return static_cast<double>(value.getInt64());
    if (value.isFloat32() || value.isFloat64())
        return value.getFloat64();
    return fallback;
}

double member(const auto& object, std::string_view name, double fallback) {
    return object.hasObjectMember(name) ? number(object[name], fallback) : fallback;
}

std::string text(const auto& object, std::string_view name) {
    return object.hasObjectMember(name) && object[name].isString()
               ? std::string(object[name].getString())
               : std::string{};
}

Result success(const Value& value) {
    return Result::success(choc::json::toString(value, false));
}

Value value_entry(state::ParamID id, float value) {
    auto entry = choc::value::createObject("");
    entry.addMember("id", static_cast<std::int64_t>(id));
    entry.addMember("value", static_cast<double>(value));
    return entry;
}

Value values_result(const state::StateStore& store, const std::vector<state::ParamID>& ids) {
    auto values = choc::value::createEmptyArray();
    for (const auto id : ids)
        values.addArrayElement(value_entry(id, store.get_value(id)));
    auto result = choc::value::createObject("");
    result.addMember("values", values);
    return result;
}

std::vector<state::ParamID> all_ids(const state::StateStore& store) {
    std::vector<state::ParamID> ids;
    for (const auto& info : store.all_params())
        ids.push_back(info.id);
    return ids;
}

/// Reads `ids` when present; every id must name a parameter.
std::optional<std::vector<state::ParamID>>
requested_ids(const state::StateStore& store, const auto& arguments, std::string& error) {
    if (!arguments.hasObjectMember("ids"))
        return all_ids(store);
    std::vector<state::ParamID> ids;
    for (const auto id : arguments["ids"]) {
        const auto value = static_cast<state::ParamID>(number(id));
        if (!store.info(value)) {
            error = "unknown parameter id " + std::to_string(value);
            return std::nullopt;
        }
        ids.push_back(value);
    }
    return ids;
}

/// A preset name becomes a file name, and the declared schema cannot express a
/// pattern, so the handler rejects path separators, traversal and control
/// characters itself.
bool safe_name(std::string_view name, std::size_t maximum) {
    if (name.empty() || name.size() > maximum || name.front() == '.' || name.front() == ' ' ||
        name.back() == ' ')
        return false;
    return std::ranges::all_of(name, [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
               c == ' ' || c == '_' || c == '-' || c == '.';
    });
}

bool safe_folder(std::string_view folder) {
    if (folder.empty())
        return true;
    if (folder.size() > 128)
        return false;
    std::size_t start = 0;
    while (start <= folder.size()) {
        const auto slash = folder.find('/', start);
        const auto segment = folder.substr(
            start, slash == std::string_view::npos ? std::string_view::npos : slash - start);
        if (!safe_name(segment, 64))
            return false;
        if (slash == std::string_view::npos)
            break;
        start = slash + 1;
    }
    return true;
}

// Deterministic, platform-independent generator so a seed reproduces a patch.
struct SplitMix64 {
    std::uint64_t state;
    std::uint64_t next() {
        auto z = (state += 0x9e3779b97f4a7c15ULL);
        z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
        z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
        return z ^ (z >> 31);
    }
    double unit() {
        return static_cast<double>(next() >> 11) * 0x1.0p-53;
    }
};

struct Biquad {
    double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
    double z1 = 0, z2 = 0;
    double process(double x) {
        const auto y = b0 * x + z1;
        z1 = b1 * x - a1 * y + z2;
        z2 = b2 * x - a2 * y;
        return y;
    }
};

// ITU-R BS.1770 K-weighting: a high shelf, then a high pass.
std::array<Biquad, 2> k_weighting(double sample_rate) {
    std::array<Biquad, 2> filters;
    {
        const double gain_db = 3.999843853973347, f0 = 1681.974450955533, q = 0.7071752369554196;
        const double k = std::tan(std::numbers::pi * f0 / sample_rate);
        const double vh = std::pow(10.0, gain_db / 20.0);
        const double vb = std::pow(vh, 0.4996667741545416);
        const double a0 = 1.0 + k / q + k * k;
        filters[0] = {(vh + vb * k / q + k * k) / a0, 2.0 * (k * k - vh) / a0,
                      (vh - vb * k / q + k * k) / a0, 2.0 * (k * k - 1.0) / a0,
                      (1.0 - k / q + k * k) / a0};
    }
    {
        const double f0 = 38.13547087602444, q = 0.5003270373238773;
        const double k = std::tan(std::numbers::pi * f0 / sample_rate);
        const double a0 = 1.0 + k / q + k * k;
        filters[1] = {1.0, -2.0, 1.0, 2.0 * (k * k - 1.0) / a0, (1.0 - k / q + k * k) / a0};
    }
    return filters;
}

Biquad octave_band(double sample_rate, double centre) {
    // Constant-peak band pass with a one-octave bandwidth.
    const double w0 = 2.0 * std::numbers::pi * centre / sample_rate;
    const double alpha = std::sin(w0) * std::sinh(std::log(2.0) / 2.0 * 1.0 * w0 / std::sin(w0));
    const double a0 = 1.0 + alpha;
    return {alpha / a0, 0.0, -alpha / a0, -2.0 * std::cos(w0) / a0, (1.0 - alpha) / a0};
}

double to_db(double power) {
    return power > 0.0 ? std::max(kFloorDb, 10.0 * std::log10(power)) : kFloorDb;
}

Value analyze(const audio::Buffer<float>& audio, double sample_rate) {
    const auto channels = audio.num_channels();
    const auto frames = audio.num_samples();
    double peak = 0.0, sum_squares = 0.0, k_power = 0.0;
    std::uint64_t clipped = 0;
    std::vector<double> mono(frames, 0.0);
    for (std::size_t channel = 0; channel < channels; ++channel) {
        auto filters = k_weighting(sample_rate);
        double k_sum = 0.0;
        const auto samples = audio.channel(channel);
        for (std::size_t frame = 0; frame < frames; ++frame) {
            const double x = std::isfinite(samples[frame]) ? samples[frame] : 0.0;
            peak = std::max(peak, std::abs(x));
            sum_squares += x * x;
            clipped += std::abs(x) >= 1.0 ? 1 : 0;
            mono[frame] += x / static_cast<double>(channels);
            const auto weighted = filters[1].process(filters[0].process(x));
            k_sum += weighted * weighted;
        }
        if (frames > 0)
            k_power += k_sum / static_cast<double>(frames);
    }
    const auto samples_total = static_cast<double>(frames * channels);
    auto bands = choc::value::createEmptyArray();
    double weighted_hz = 0.0, band_total = 0.0;
    for (const double centre :
         {31.5, 63.0, 125.0, 250.0, 500.0, 1000.0, 2000.0, 4000.0, 8000.0, 16000.0}) {
        if (centre * std::sqrt(2.0) >= sample_rate / 2.0)
            break;
        auto filter = octave_band(sample_rate, centre);
        double power = 0.0;
        for (const auto x : mono) {
            const auto y = filter.process(x);
            power += y * y;
        }
        power = frames > 0 ? power / static_cast<double>(frames) : 0.0;
        weighted_hz += centre * power;
        band_total += power;
        auto band = choc::value::createObject("");
        band.addMember("hz", centre);
        band.addMember("level_db", to_db(power));
        bands.addArrayElement(band);
    }
    auto result = choc::value::createObject("");
    result.addMember("frames", static_cast<std::int64_t>(frames));
    result.addMember("sample_rate", sample_rate);
    result.addMember("channels", static_cast<std::int64_t>(channels));
    result.addMember("peak_dbfs", to_db(peak * peak));
    result.addMember("rms_dbfs", to_db(samples_total > 0 ? sum_squares / samples_total : 0.0));
    result.addMember("loudness_lufs_ungated",
                     k_power > 0.0 ? std::max(kFloorDb, -0.691 + 10.0 * std::log10(k_power))
                                   : kFloorDb);
    result.addMember("clipped_samples", static_cast<std::int64_t>(clipped));
    result.addMember("spectral_centroid_hz", band_total > 0.0 ? weighted_hz / band_total : 0.0);
    result.addMember("bands", bands);
    return result;
}

struct RenderRequest {
    std::uint64_t frames = 0;
    std::vector<format::OfflineMidiEvent> midi;
    std::vector<format::OfflineParameterEvent> parameters;
    std::optional<audio::Buffer<float>> input;
};

/// Renders on a fresh instance holding the live patch's parameter values.
Result render(const ControlAgentToolkitOptions& options, const state::StateStore& store,
              RenderRequest& request, const ControlVendorOperationCall& call) {
    format::OfflineRenderHost host(options.render_factory);
    format::OfflineRenderConfig config;
    config.sample_rate = options.render_sample_rate;
    config.max_block_frames = 512;
    config.input_channels = options.render_channels;
    config.output_channels = options.render_channels;
    if (!host.prepare(config))
        return Result::failure(ControlResultCode::HostUnavailable,
                               "the render instance could not be prepared");
    auto& copy = host.headless().state();
    for (const auto& info : store.all_params())
        if (copy.info(info.id))
            copy.set_value(info.id, store.get_value(info.id));
    std::ranges::stable_sort(request.midi, {}, &format::OfflineMidiEvent::frame);
    std::ranges::stable_sort(request.parameters, {}, &format::OfflineParameterEvent::frame);

    // Render in segments that start at each automation point. Before a
    // segment the point's value is written to the store, as a format adapter
    // does for a block-rate parameter; the same point also reaches the
    // processor as a sample-accurate event. Segments continue one instance,
    // so processor state carries across them.
    std::vector<std::uint64_t> starts{0};
    for (const auto& event : request.parameters)
        if (event.frame > starts.back() && event.frame < request.frames)
            starts.push_back(event.frame);
    audio::Buffer<float> output(options.render_channels, static_cast<std::size_t>(request.frames));
    std::size_t midi_index = 0, parameter_index = 0;
    const auto authorized = [&call] { return !call.still_authorized || call.still_authorized(); };
    for (std::size_t segment = 0; segment < starts.size(); ++segment) {
        const auto begin = starts[segment];
        const auto end = segment + 1 < starts.size() ? starts[segment + 1] : request.frames;
        std::vector<format::OfflineMidiEvent> midi;
        for (; midi_index < request.midi.size() && request.midi[midi_index].frame < end;
             ++midi_index) {
            auto event = request.midi[midi_index];
            event.frame -= begin;
            midi.push_back(event);
        }
        std::vector<format::OfflineParameterEvent> parameters;
        for (; parameter_index < request.parameters.size() &&
               request.parameters[parameter_index].frame < end;
             ++parameter_index) {
            auto event = request.parameters[parameter_index];
            if (event.frame <= begin)
                copy.set_value(event.event.param_id, event.event.value);
            event.frame -= std::min(event.frame, begin);
            parameters.push_back(event);
        }
        format::OfflineRenderOptions render_options;
        render_options.frame_count = end - begin;
        render_options.midi_events = midi;
        render_options.parameter_events = parameters;
        render_options.start_position_samples = static_cast<std::int64_t>(begin);
        if (request.input)
            render_options.input =
                static_cast<const audio::Buffer<float>&>(*request.input)
                    .view()
                    .slice(static_cast<std::size_t>(begin), static_cast<std::size_t>(end - begin));
        const auto rendered = host.render(render_options, authorized);
        if (!rendered.ok) {
            if (!authorized())
                return Result::failure(ControlResultCode::Cancelled, "the render was cancelled");
            return Result::failure(ControlResultCode::InternalError, "the offline render failed");
        }
        for (std::size_t channel = 0; channel < output.num_channels(); ++channel) {
            const auto source = rendered.audio.channel(channel);
            std::ranges::copy(source,
                              output.channel(channel).begin() + static_cast<std::ptrdiff_t>(begin));
        }
    }
    return success(analyze(output, options.render_sample_rate));
}

std::uint64_t to_frames(double seconds, double sample_rate) {
    return static_cast<std::uint64_t>(std::llround(std::max(0.0, seconds) * sample_rate));
}

} // namespace

std::span<const std::string_view> control_agent_toolkit_operation_names() {
    return kOperationNames;
}

bool add_control_agent_toolkit(ControlVendorOperationHandlers& handlers, state::StateStore& store,
                               const ControlAgentToolkitOptions& options) {
    if (!control_vendor_id_is_valid(options.vendor_id))
        return false;
    const auto id = [&](std::string_view name) {
        return "dev." + options.vendor_id + "/" + std::string(name) + "@1";
    };
    auto* const live = &store;

    if (const auto presets = options.presets) {
        handlers.add(id("presets.list"), [presets](const ControlVendorOperationCall&) {
            presets->refresh();
            auto list = choc::value::createEmptyArray();
            for (const auto& preset : presets->all_presets()) {
                if (list.size() >= 1024)
                    break;
                auto entry = choc::value::createObject("");
                entry.addMember("name", preset.name.substr(0, 256));
                entry.addMember("folder", preset.folder.substr(0, 256));
                entry.addMember("factory", preset.is_factory);
                list.addArrayElement(entry);
            }
            auto result = choc::value::createObject("");
            result.addMember("current", presets->current_preset_name().substr(0, 64));
            result.addMember("presets", list);
            return success(result);
        });
        handlers.add(id("presets.load"), [presets](const ControlVendorOperationCall& call) {
            const auto arguments = parse_arguments(call.arguments_json);
            if (!arguments)
                return invalid("arguments must be an object");
            const auto name = text(*arguments, "name");
            const auto folder = text(*arguments, "folder");
            presets->refresh();
            for (const auto& preset : presets->all_presets()) {
                if (preset.name == name && preset.folder == folder) {
                    if (!presets->load(preset))
                        return Result::failure(ControlResultCode::InternalError,
                                               "the preset could not be loaded");
                    auto result = choc::value::createObject("");
                    result.addMember("name", name);
                    return success(result);
                }
            }
            return invalid("no preset has this name and folder");
        });
        handlers.add(id("presets.save"), [presets](const ControlVendorOperationCall& call) {
            const auto arguments = parse_arguments(call.arguments_json);
            if (!arguments)
                return invalid("arguments must be an object");
            const auto name = text(*arguments, "name");
            const auto folder = text(*arguments, "folder");
            if (!safe_name(name, 64) || !safe_folder(folder))
                return invalid("preset names and folders use letters, digits, space, '_', '-' "
                               "and '.', must not start with '.', and folders separate "
                               "segments with '/'");
            if (!presets->save(name, folder))
                return Result::failure(ControlResultCode::InternalError,
                                       "the preset could not be saved");
            auto result = choc::value::createObject("");
            result.addMember("name", name);
            return success(result);
        });
    }

    handlers.add(id("patch.schema"), [live](const ControlVendorOperationCall&) {
        auto parameters = choc::value::createEmptyArray();
        for (const auto& info : live->all_params()) {
            auto entry = choc::value::createObject("");
            entry.addMember("id", static_cast<std::int64_t>(info.id));
            entry.addMember("name", info.name.substr(0, 256));
            entry.addMember("unit", info.unit.substr(0, 64));
            entry.addMember("min", static_cast<double>(info.range.min));
            entry.addMember("max", static_cast<double>(info.range.max));
            entry.addMember("default", static_cast<double>(info.range.default_value));
            entry.addMember("step", static_cast<double>(std::max(0.0f, info.range.step)));
            parameters.addArrayElement(entry);
        }
        auto result = choc::value::createObject("");
        result.addMember("parameters", parameters);
        return success(result);
    });
    handlers.add(id("patch.get"), [live](const ControlVendorOperationCall& call) {
        const auto arguments = parse_arguments(call.arguments_json);
        if (!arguments)
            return invalid("arguments must be an object");
        std::string error;
        const auto ids = requested_ids(*live, *arguments, error);
        return ids ? success(values_result(*live, *ids)) : invalid(error);
    });
    handlers.add(id("patch.set"), [live](const ControlVendorOperationCall& call) {
        const auto arguments = parse_arguments(call.arguments_json);
        if (!arguments)
            return invalid("arguments must be an object");
        // Validate every entry before writing any, so a bad id changes nothing.
        std::vector<std::pair<state::ParamID, float>> writes;
        for (const auto entry : (*arguments)["values"]) {
            const auto param = static_cast<state::ParamID>(number(entry["id"]));
            const auto* info = live->info(param);
            const auto value = number(entry["value"], std::numeric_limits<double>::quiet_NaN());
            if (!info || !std::isfinite(value))
                return invalid("unknown parameter id or non-finite value for id " +
                               std::to_string(param));
            writes.emplace_back(
                param, std::clamp(static_cast<float>(value), info->range.min, info->range.max));
        }
        std::vector<state::ParamID> ids;
        for (const auto& [param, value] : writes) {
            live->set_value(param, value);
            ids.push_back(param);
        }
        return success(values_result(*live, ids));
    });
    handlers.add(id("patch.randomize"), [live](const ControlVendorOperationCall& call) {
        const auto arguments = parse_arguments(call.arguments_json);
        if (!arguments)
            return invalid("arguments must be an object");
        std::string error;
        const auto ids = requested_ids(*live, *arguments, error);
        if (!ids)
            return invalid(error);
        const auto seed = static_cast<std::uint64_t>(member(*arguments, "seed", 0.0));
        const auto amount = std::clamp(member(*arguments, "amount", 1.0), 0.0, 1.0);
        SplitMix64 random{seed};
        std::vector<state::ParamID> changed;
        for (const auto param : *ids) {
            const auto* info = live->info(param);
            // One draw per listed parameter keeps later values stable when an
            // earlier one is skipped.
            const auto target = random.unit();
            if (info->is_trigger || info->designation != state::ParamDesignation::None)
                continue;
            const auto current = static_cast<double>(live->get_normalized(param));
            live->set_normalized(param, static_cast<float>(current + (target - current) * amount));
            changed.push_back(param);
        }
        auto result = values_result(*live, changed);
        result.addMember("seed", static_cast<std::int64_t>(seed));
        return success(result);
    });

    if (options.render_factory) {
        handlers.add(id("notes.play"), [live, options](const ControlVendorOperationCall& call) {
            const auto arguments = parse_arguments(call.arguments_json);
            if (!arguments)
                return invalid("arguments must be an object");
            const auto rate = options.render_sample_rate;
            RenderRequest request;
            request.frames = to_frames(member(*arguments, "duration_seconds", 0.0), rate);
            if (request.frames == 0)
                return invalid("duration_seconds must be positive");
            const auto last = request.frames - 1;
            for (const auto note : (*arguments)["notes"]) {
                const auto channel = static_cast<std::uint8_t>(member(note, "channel", 0.0));
                const auto key = static_cast<std::uint8_t>(member(note, "note", 60.0));
                const auto velocity = static_cast<std::uint8_t>(member(note, "velocity", 100.0));
                const auto start = to_frames(member(note, "start_seconds", 0.0), rate);
                if (start > last)
                    continue;
                const auto stop = std::min(
                    last, start + std::max<std::uint64_t>(
                                      1, to_frames(member(note, "duration_seconds", 0.0), rate)));
                request.midi.push_back({start, midi::MidiEvent::note_on(channel, key, velocity)});
                request.midi.push_back({stop, midi::MidiEvent::note_off(channel, key)});
            }
            if (arguments->hasObjectMember("lanes")) {
                for (const auto lane : (*arguments)["lanes"]) {
                    const auto param = static_cast<state::ParamID>(number(lane["id"]));
                    const auto* info = live->info(param);
                    if (!info)
                        return invalid("unknown automation parameter id " + std::to_string(param));
                    for (const auto point : lane["points"]) {
                        const auto frame =
                            std::min(last, to_frames(member(point, "time_seconds", 0.0), rate));
                        const auto value = std::clamp(
                            static_cast<float>(member(point, "value", info->range.default_value)),
                            info->range.min, info->range.max);
                        format::OfflineParameterEvent event;
                        event.frame = frame;
                        event.event.param_id = param;
                        event.event.value = value;
                        request.parameters.push_back(event);
                    }
                }
            }
            return render(options, *live, request, call);
        });
        handlers.add(id("render.analyze"), [live, options](const ControlVendorOperationCall& call) {
            const auto arguments = parse_arguments(call.arguments_json);
            if (!arguments)
                return invalid("arguments must be an object");
            const auto rate = options.render_sample_rate;
            RenderRequest request;
            request.frames = to_frames(member(*arguments, "duration_seconds", 0.0), rate);
            if (request.frames == 0)
                return invalid("duration_seconds must be positive");
            const auto stimulus = text(*arguments, "stimulus");
            const auto level = std::pow(
                10.0, std::clamp(member(*arguments, "level_dbfs", -12.0), -120.0, 0.0) / 20.0);
            const auto frequency =
                std::clamp(member(*arguments, "frequency_hz", 1000.0), 1.0, rate / 2.0);
            SplitMix64 random{static_cast<std::uint64_t>(member(*arguments, "seed", 1.0))};
            audio::Buffer<float> input(options.render_channels,
                                       static_cast<std::size_t>(request.frames));
            for (std::size_t frame = 0; frame < request.frames; ++frame) {
                double sample = 0.0;
                if (stimulus == "impulse")
                    sample = frame == 0 ? level : 0.0;
                else if (stimulus == "sine")
                    sample = level * std::sin(2.0 * std::numbers::pi * frequency *
                                              static_cast<double>(frame) / rate);
                else if (stimulus == "noise")
                    sample = level * (2.0 * random.unit() - 1.0);
                for (std::uint32_t channel = 0; channel < options.render_channels; ++channel)
                    input.channel(channel)[frame] = static_cast<float>(sample);
            }
            request.input = std::move(input);
            return render(options, *live, request, call);
        });
    }
    return true;
}

} // namespace pulp::inspect
