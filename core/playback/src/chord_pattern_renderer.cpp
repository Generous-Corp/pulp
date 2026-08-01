#include <pulp/playback/chord_pattern_renderer.hpp>

#include <pulp/timeline/schema_json.hpp>

#include <algorithm>
#include <array>
#include <limits>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace pulp::playback {
namespace {

using timeline::BoundedJsonSink;
using timeline::JsonValue;
using timeline::PersistenceError;
using timeline::PersistenceErrorCode;
using timeline::SchemaWriteSuccess;

template <typename T>
runtime::Result<T, PersistenceError> persistence_fail(PersistenceErrorCode code,
                                                      std::string path = {}) {
    return runtime::Err(PersistenceError{code, 0, 0, 0, std::move(path)});
}

bool valid_payload(const ChordPatternContent& content) noexcept {
    return content.step.value > 0 && content.gate.value > 0 &&
           content.gate.value <= content.step.value && content.octave >= -1 &&
           content.octave <= 7 && content.velocity != 0;
}

runtime::Result<std::shared_ptr<const void>, PersistenceError>
decode_chord_pattern(const JsonValue& data, const void*) noexcept {
    if (data.kind != JsonValue::Kind::Object)
        return persistence_fail<std::shared_ptr<const void>>(PersistenceErrorCode::UnexpectedType);
    if (data.object.size() != 5)
        return persistence_fail<std::shared_ptr<const void>>(PersistenceErrorCode::InvalidSchema);

    const auto* seed = data.find("seed");
    const auto* step = data.find("step_ticks");
    const auto* gate = data.find("gate_ticks");
    const auto* octave = data.find("octave");
    const auto* velocity = data.find("velocity");
    if (!seed || !step || !gate || !octave || !velocity)
        return persistence_fail<std::shared_ptr<const void>>(PersistenceErrorCode::MissingField);

    auto decoded_seed = timeline::parse_canonical_u64_string(*seed, "/seed");
    auto decoded_step = timeline::parse_canonical_u64_string(*step, "/step_ticks");
    auto decoded_gate = timeline::parse_canonical_u64_string(*gate, "/gate_ticks");
    auto decoded_octave = timeline::parse_canonical_i64_string(*octave, "/octave");
    auto decoded_velocity = timeline::parse_u32_number(*velocity, "/velocity");
    if (!decoded_seed || !decoded_step || !decoded_gate || !decoded_octave || !decoded_velocity)
        return persistence_fail<std::shared_ptr<const void>>(PersistenceErrorCode::InvalidSchema);
    if (decoded_step.value() >
            static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) ||
        decoded_gate.value() >
            static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) ||
        decoded_octave.value() < std::numeric_limits<std::int8_t>::min() ||
        decoded_octave.value() > std::numeric_limits<std::int8_t>::max() ||
        decoded_velocity.value() > std::numeric_limits<std::uint16_t>::max())
        return persistence_fail<std::shared_ptr<const void>>(PersistenceErrorCode::InvalidSchema);

    ChordPatternContent content{
        .seed = decoded_seed.value(),
        .step = {static_cast<std::int64_t>(decoded_step.value())},
        .gate = {static_cast<std::int64_t>(decoded_gate.value())},
        .octave = static_cast<std::int8_t>(decoded_octave.value()),
        .velocity = static_cast<std::uint16_t>(decoded_velocity.value()),
    };
    if (!valid_payload(content))
        return persistence_fail<std::shared_ptr<const void>>(PersistenceErrorCode::InvalidSchema);
    return runtime::Ok(
        std::shared_ptr<const void>(std::make_shared<const ChordPatternContent>(content)));
}

runtime::Result<SchemaWriteSuccess, PersistenceError>
encode_chord_pattern(const std::shared_ptr<const void>& value, BoundedJsonSink& output,
                     const void*) noexcept {
    if (!value)
        return persistence_fail<SchemaWriteSuccess>(PersistenceErrorCode::InvalidSchema);
    const auto& content = *static_cast<const ChordPatternContent*>(value.get());
    if (!valid_payload(content))
        return persistence_fail<SchemaWriteSuccess>(PersistenceErrorCode::InvalidSchema);

    output.append("{\"seed\":\"");
    output.append(std::to_string(content.seed));
    output.append("\",\"step_ticks\":\"");
    output.append(std::to_string(content.step.value));
    output.append("\",\"gate_ticks\":\"");
    output.append(std::to_string(content.gate.value));
    output.append("\",\"octave\":\"");
    output.append(std::to_string(content.octave));
    output.append("\",\"velocity\":");
    output.append(std::to_string(content.velocity));
    output.append("}");
    return runtime::Ok(SchemaWriteSuccess{});
}

std::size_t retained_chord_pattern(const std::shared_ptr<const void>&, const void*) noexcept {
    return sizeof(ChordPatternContent);
}

struct ChordIntervals {
    std::array<std::uint8_t, 4> values{};
    std::uint8_t count = 0;
};

constexpr ChordIntervals intervals_for(timeline::ChordQuality quality) noexcept {
    using timeline::ChordQuality;
    switch (quality) {
    case ChordQuality::Major:
        return {{0, 4, 7, 0}, 3};
    case ChordQuality::Minor:
        return {{0, 3, 7, 0}, 3};
    case ChordQuality::Diminished:
        return {{0, 3, 6, 0}, 3};
    case ChordQuality::Augmented:
        return {{0, 4, 8, 0}, 3};
    case ChordQuality::Dominant7:
        return {{0, 4, 7, 10}, 4};
    case ChordQuality::Major7:
        return {{0, 4, 7, 11}, 4};
    case ChordQuality::Minor7:
        return {{0, 3, 7, 10}, 4};
    case ChordQuality::HalfDiminished7:
        return {{0, 3, 6, 10}, 4};
    case ChordQuality::Suspended2:
        return {{0, 2, 7, 0}, 3};
    case ChordQuality::Suspended4:
        return {{0, 5, 7, 0}, 3};
    }
    return {};
}

runtime::Result<ContentProgramFragment, ContentFragmentError>
compile_chord_pattern(const RegisteredContentCompileInput& input, const void*) noexcept {
    const auto& schema = input.content.schema();
    if (schema.type_name != kChordPatternContentType ||
        schema.version != kChordPatternContentSchemaVersion || input.clip_duration.value <= 0)
        return runtime::Err(ContentFragmentError{});
    const auto* content = input.content.value_as<ChordPatternContent>();
    if (!content || !valid_payload(*content) || input.maximum_fragment_notes == 0)
        return runtime::Err(ContentFragmentError{});

    const auto duration = input.clip_duration.value;
    const auto step = content->step.value;
    const auto step_count = 1u + static_cast<std::uint64_t>((duration - 1) / step);
    if (step_count > input.maximum_fragment_notes)
        return runtime::Err(ContentFragmentError{});

    std::vector<ContentFragmentNote> notes;
    notes.reserve(static_cast<std::size_t>(step_count));
    std::int64_t local_tick = 0;
    for (std::uint64_t step_index = 0; step_index < step_count; ++step_index) {
        if (input.context_start.value > std::numeric_limits<std::int64_t>::max() - local_tick)
            return runtime::Err(ContentFragmentError{});
        const auto* harmony =
            input.context.chord_scale_at({input.context_start.value + local_tick});
        if (harmony) {
            const auto intervals = intervals_for(harmony->chord_quality);
            if (intervals.count == 0)
                return runtime::Err(ContentFragmentError{});
            const auto tone =
                static_cast<std::size_t>(content->seed + step_index) % intervals.count;
            const auto pitch = (static_cast<int>(content->octave) + 1) * 12 + harmony->chord_root +
                               intervals.values[tone];
            if (pitch < 0 || pitch > 127)
                return runtime::Err(ContentFragmentError{});
            notes.push_back({
                .start = {local_tick},
                .duration = {std::min(content->gate.value, duration - local_tick)},
                .velocity = content->velocity,
                .pitch = static_cast<std::uint8_t>(pitch),
                .channel = 0,
            });
        }
        if (step_index + 1 < step_count)
            local_tick += step;
    }
    return ContentProgramFragment::create(std::move(notes), input.clip_duration);
}

} // namespace

runtime::Result<timeline::SchemaRegistration, timeline::SchemaError>
register_chord_pattern_content_schema(timeline::SchemaRegistryBuilder& builder) {
    timeline::TypeSchema schema;
    schema.type_name = kChordPatternContentType;
    schema.domain = timeline::SchemaDomain::Content;
    schema.current_version = kChordPatternContentSchemaVersion;
    schema.fields = {
        {"gate_ticks", timeline::SchemaValueKind::U64String},
        {"octave", timeline::SchemaValueKind::I64String},
        {"seed", timeline::SchemaValueKind::U64String},
        {"step_ticks", timeline::SchemaValueKind::U64String},
        {"velocity", timeline::SchemaValueKind::U32},
    };
    schema.codec = {{}, decode_chord_pattern, encode_chord_pattern, retained_chord_pattern};
    return builder.register_type(std::move(schema));
}

runtime::Result<timeline::RegisteredContent, timeline::PersistenceError>
create_chord_pattern_content(const ChordPatternContent& content,
                             const timeline::SchemaRegistry& schemas,
                             std::size_t maximum_json_bytes) {
    return schemas.create_registered_no_owned_ids(
        {kChordPatternContentType, kChordPatternContentSchemaVersion},
        std::make_shared<const ChordPatternContent>(content), maximum_json_bytes);
}

std::optional<ContextRegistrationError>
declare_chord_pattern_renderer(CompileContextRegistry& registry,
                               const timeline::SchemaRegistry& schemas) {
    auto subscriptions = timeline::CompileContextSubscriptions::none();
    subscriptions.subscribe(timeline::CompileContextKind::ChordScale);
    return registry.declare(
        {.content_type_name = kChordPatternContentType,
         .subscriptions = subscriptions,
         .schema_version = kChordPatternContentSchemaVersion,
         .output_kind = ContentProgramOutputKind::Notes,
         .maximum_fragment_notes = kMaximumChordPatternNotes,
         .state_policy = RegisteredRendererStatePolicy::Reset,
         .production = {.mode = timeline::ProductionMode::Synchronous,
                        .reproducibility = timeline::ReproducibilityClass::Deterministic,
                        .lookahead_ms = 0},
         .compile = compile_chord_pattern},
        schemas);
}

} // namespace pulp::playback
