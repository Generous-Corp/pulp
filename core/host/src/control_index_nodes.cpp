#include <pulp/host/control_index_nodes.hpp>
#include <pulp/signal/control_math.hpp>
#include <pulp/signal/indexed_control.hpp>
#include <pulp/signal/mod_tools.hpp>
#include <pulp/signal/trigger.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstring>
#include <limits>

namespace pulp::host {

const char* control_process_result_name(ControlProcessResult result) noexcept {
    switch (result) {
    case ControlProcessResult::Accepted:
        return "accepted";
    case ControlProcessResult::Unbound:
        return "unbound";
    case ControlProcessResult::OutOfRange:
        return "out_of_range";
    case ControlProcessResult::StaleEpoch:
        return "stale_epoch";
    case ControlProcessResult::WrongStart:
        return "wrong_start";
    case ControlProcessResult::ClockExhausted:
        return "clock_exhausted";
    case ControlProcessResult::Busy:
        return "busy";
    case ControlProcessResult::UnsupportedMode:
        return "unsupported_mode";
    case ControlProcessResult::RenderFailed:
        return "render_failed";
    }
    return "unknown";
}

const char* control_process_result_fix(ControlProcessResult result) noexcept {
    switch (result) {
    case ControlProcessResult::Accepted:
        return "none";
    case ControlProcessResult::Unbound:
        return "prepare a graph that holds exactly one Control index input and no trigger "
               "graph before calling process_control_checked()";
    case ControlProcessResult::OutOfRange:
        return "submit between 1 and the prepared block size (at most kControlIndexMaxFrames) "
               "indices, with input and output buffers of the same frame count";
    case ControlProcessResult::StaleEpoch:
        return "use the current epoch, or the next one when the transport requests a "
               "stream-history reset";
    case ControlProcessResult::WrongStart:
        return "set absolute_start to the graph cursor (the previous start plus its frames), "
               "or to the transport position on a reset";
    case ControlProcessResult::ClockExhausted:
        return "reset the stream with a non-negative transport position before the 64-bit "
               "sample clock or epoch overflows";
    case ControlProcessResult::Busy:
        return "call process_control_checked() from one audio thread at a time and do not "
               "nest it inside a graph-event block";
    case ControlProcessResult::UnsupportedMode:
        return "disable parallel routing and anticipation on a graph with a Control index input";
    case ControlProcessResult::RenderFailed:
        return "the block was consumed and silenced; fix the failing node and continue at the "
               "next absolute start";
    }
    return "unknown result";
}

CustomNodeType make_control_index_input_type() {
    CustomNodeType type;
    type.type_id = std::string(kControlIndexInputTypeId);
    type.version = kControlIndexInputVersion;
    type.num_output_ports = 1;
    type.default_name = "Control Index";
    type.output_ports = {{"index", PortKind::Control}};
    // The graph binds this source to snapshot-owned storage at prepare. This
    // fallback is silent if a caller attempts to execute the factory alone.
    type.process = [](audio::BufferView<float>& output, const audio::BufferView<const float>&,
                      int frames) {
        if (output.num_channels() != 0 && frames > 0)
            std::fill_n(output.channel_ptr(0), frames, 0.0f);
    };
    type.rt_contract.declared = true;
    type.rt_contract.safety_class = audio::RtSafetyClass::AudioCallbackSafeAfterPrepare;
    type.rt_contract.allocation_free = true;
    type.rt_contract.worst_case_work = [](int) { return signal::rt::RtWorkCounts{}; };
    type.rt_contract.loop_bound = "one float copy per frame";
    return type;
}

namespace {

enum class Operation : std::uint8_t { MapClip, Hysteresis, Integrator, Slew, IndexedTable };

struct ControlInstance {
    Operation operation;
    std::array<float, 4> parameters{};
    std::vector<float> table;
    signal::MapClip map;
    signal::HystereticTriggerDetect hysteresis;
    signal::ControlIntegrator integrator;
    signal::SlewLimiterT<float> slew;
    signal::IndexedControl indexed;

    explicit ControlInstance(Operation op) : operation(op) {
        if (op == Operation::MapClip) {
            parameters = {0, 1, 0, 1};
            (void)map.configure(0, 1, 0, 1);
        } else if (op == Operation::Hysteresis) {
            parameters = {0.5f, 0.25f, 0, 0};
        } else if (op == Operation::Integrator) {
            parameters = {0, 1, 0, 0};
        }
    }

    void reset() noexcept {
        hysteresis.reset();
        integrator.reset();
        slew.reset();
    }
};

std::size_t parameter_count(Operation op) noexcept {
    switch (op) {
    case Operation::MapClip:
        return 4;
    case Operation::Hysteresis:
        return 2;
    case Operation::Integrator:
        return 3;
    case Operation::Slew:
        return 2;
    case Operation::IndexedTable:
        return 0;
    }
    return 0;
}

void append_u32(std::vector<std::uint8_t>& out, std::uint32_t value) {
    for (unsigned i = 0; i < 4; ++i)
        out.push_back(static_cast<std::uint8_t>(value >> (8 * i)));
}

std::uint32_t read_u32(const std::vector<std::uint8_t>& bytes, std::size_t offset) noexcept {
    std::uint32_t result = 0;
    for (unsigned i = 0; i < 4; ++i)
        result |= static_cast<std::uint32_t>(bytes[offset + i]) << (8 * i);
    return result;
}

std::vector<std::uint8_t> encode_values(std::span<const float> values) {
    std::vector<std::uint8_t> bytes;
    bytes.reserve(5 + values.size() * 4);
    bytes.push_back(1);
    append_u32(bytes, static_cast<std::uint32_t>(values.size()));
    for (float value : values)
        append_u32(bytes, std::bit_cast<std::uint32_t>(value));
    return bytes;
}

bool decode_values(const std::vector<std::uint8_t>& bytes, std::size_t expected,
                   std::vector<float>& values) {
    if (bytes.size() < 5 || bytes[0] != 1 || read_u32(bytes, 1) != expected ||
        bytes.size() != 5 + expected * 4)
        return false;
    values.reserve(expected);
    for (std::size_t i = 0; i < expected; ++i) {
        const auto value = std::bit_cast<float>(read_u32(bytes, 5 + i * 4));
        if (!std::isfinite(value))
            return false;
        values.push_back(value);
    }
    return true;
}

bool load_control_state(ControlInstance& instance, const std::vector<std::uint8_t>& bytes) {
    const auto count = instance.operation == Operation::IndexedTable
                           ? (bytes.size() >= 5 ? read_u32(bytes, 1) : 0)
                           : parameter_count(instance.operation);
    if (instance.operation == Operation::IndexedTable &&
        (count == 0 || count > kControlIndexMaxFrames))
        return false;
    std::vector<float> values;
    if (!decode_values(bytes, count, values))
        return false;

    if (instance.operation == Operation::IndexedTable) {
        instance.table = std::move(values);
        instance.indexed.set_table(instance.table);
        return true;
    }
    if (instance.operation == Operation::MapClip) {
        signal::MapClip candidate;
        if (!candidate.configure(values[0], values[1], values[2], values[3]))
            return false;
        instance.map = candidate;
    } else if (instance.operation == Operation::Hysteresis) {
        if (!(values[0] > values[1]))
            return false;
        instance.hysteresis.set_thresholds(values[0], values[1]);
    } else if (instance.operation == Operation::Integrator) {
        signal::ControlIntegrator candidate;
        if (!candidate.configure(values[0], values[1], values[2]))
            return false;
        instance.integrator = candidate;
    } else if (instance.operation == Operation::Slew) {
        if (values[0] < 0 || values[1] < 0)
            return false;
        instance.slew.set_times_ms(values[0], values[1]);
    }
    std::copy(values.begin(), values.end(), instance.parameters.begin());
    instance.reset();
    return true;
}

CustomNodeType make_binary_type(std::string_view id, float (*rule)(float, float) noexcept) {
    CustomNodeType type;
    type.type_id = std::string(id);
    type.num_input_ports = 2;
    type.num_output_ports = 1;
    type.input_ports = {{"a", PortKind::Control}, {"b", PortKind::Control}};
    type.output_ports = {{"value", PortKind::Control}};
    type.process = [rule](audio::BufferView<float>& out, const audio::BufferView<const float>& in,
                          int frames) {
        if (out.num_channels() == 0 || in.num_channels() < 2)
            return;
        auto* dst = out.channel_ptr(0);
        const auto* a = in.channel_ptr(0);
        const auto* b = in.channel_ptr(1);
        for (int i = 0; i < frames; ++i)
            dst[i] = rule(a[i], b[i]);
    };
    return type;
}

CustomNodeType make_unary_type(std::string_view id, Operation operation) {
    CustomNodeType type;
    type.type_id = std::string(id);
    type.num_input_ports = 1;
    type.num_output_ports = 1;
    type.input_ports = {
        {operation == Operation::IndexedTable ? "index" : "value", PortKind::Control}};
    type.output_ports = {{"value", PortKind::Control}};
    type.create = [operation]() -> void* { return new ControlInstance(operation); };
    type.destroy = [](void* pointer) { delete static_cast<ControlInstance*>(pointer); };
    type.prepare = [](void* pointer, double sample_rate, int) {
        static_cast<ControlInstance*>(pointer)->slew.prepare(sample_rate);
    };
    type.reset = [](void* pointer) { static_cast<ControlInstance*>(pointer)->reset(); };
    type.save_state = [](void* pointer) {
        const auto& state = *static_cast<ControlInstance*>(pointer);
        if (state.operation == Operation::IndexedTable)
            return encode_values(state.table);
        const auto count = parameter_count(state.operation);
        return encode_values(std::span<const float>(state.parameters.data(), count));
    };
    type.load_state = [](void* pointer, const std::vector<std::uint8_t>& bytes) {
        return load_control_state(*static_cast<ControlInstance*>(pointer), bytes);
    };
    type.process_instance = [](void* pointer, audio::BufferView<float>& out,
                               const audio::BufferView<const float>& in, int frames) {
        if (out.num_channels() == 0 || in.num_channels() == 0)
            return;
        auto& state = *static_cast<ControlInstance*>(pointer);
        auto* dst = out.channel_ptr(0);
        const auto* src = in.channel_ptr(0);
        for (int i = 0; i < frames; ++i) {
            const float value = signal::finite_control(src[i]);
            switch (state.operation) {
            case Operation::MapClip:
                dst[i] = signal::finite_control(state.map.process(value));
                break;
            case Operation::Hysteresis:
                (void)state.hysteresis.process(value);
                dst[i] = state.hysteresis.high() ? 1.0f : 0.0f;
                break;
            case Operation::Integrator:
                dst[i] = state.integrator.process(value);
                break;
            case Operation::Slew:
                dst[i] = state.slew.process(value);
                break;
            case Operation::IndexedTable:
                dst[i] = state.indexed.process(src[i]);
                break;
            }
        }
    };
    return type;
}

} // namespace

std::vector<std::uint8_t> make_control_scalar_state(std::span<const float> values) {
    if (values.size() > 4 || std::any_of(values.begin(), values.end(),
                                         [](float value) { return !std::isfinite(value); }))
        return {};
    return encode_values(values);
}

std::vector<std::uint8_t> make_control_table_state(std::span<const float> samples) {
    if (samples.empty() || samples.size() > kControlIndexMaxFrames ||
        std::any_of(samples.begin(), samples.end(),
                    [](float value) { return !std::isfinite(value); }))
        return {};
    return encode_values(samples);
}

CustomNodeType make_control_min_type() {
    return make_binary_type("pulp.control.min", signal::control_min<float>);
}
CustomNodeType make_control_max_type() {
    return make_binary_type("pulp.control.max", signal::control_max<float>);
}
CustomNodeType make_control_safe_divide_type() {
    return make_binary_type("pulp.control.safe-divide", signal::safe_control_divide<float>);
}
CustomNodeType make_control_map_clip_type() {
    return make_unary_type("pulp.control.map-clip", Operation::MapClip);
}
CustomNodeType make_control_hysteresis_type() {
    return make_unary_type("pulp.control.hysteresis", Operation::Hysteresis);
}
CustomNodeType make_control_integrator_type() {
    return make_unary_type("pulp.control.integrator", Operation::Integrator);
}
CustomNodeType make_control_slew_type() {
    return make_unary_type("pulp.control.slew", Operation::Slew);
}
CustomNodeType make_control_indexed_table_type() {
    return make_unary_type("pulp.control.indexed-table", Operation::IndexedTable);
}

bool validate_control_node_state(std::string_view type_id, const std::vector<std::uint8_t>& bytes) {
    Operation operation;
    if (type_id == "pulp.control.map-clip")
        operation = Operation::MapClip;
    else if (type_id == "pulp.control.hysteresis")
        operation = Operation::Hysteresis;
    else if (type_id == "pulp.control.integrator")
        operation = Operation::Integrator;
    else if (type_id == "pulp.control.slew")
        operation = Operation::Slew;
    else if (type_id == "pulp.control.indexed-table")
        operation = Operation::IndexedTable;
    else
        return false;
    ControlInstance candidate(operation);
    return load_control_state(candidate, bytes);
}

} // namespace pulp::host
