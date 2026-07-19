#include <pulp/audio/sample_heritage_json.hpp>

#include <choc/text/choc_JSON.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

namespace pulp::audio {
namespace {

using namespace std::string_view_literals;
using Value = choc::value::ValueView;

template <typename Result>
struct JsonParserBase {
    Result result;

    bool fail(SampleHeritageJsonStatus status, std::string path) {
        result.status = status;
        result.field_path = std::move(path);
        return false;
    }

    template <std::size_t N>
    bool audit_object(Value object,
                      std::string_view path,
                      const std::array<std::string_view, N>& fields) {
        if (!object.isObject()) return fail(SampleHeritageJsonStatus::WrongType,
                                            std::string(path));
        for (std::uint32_t index = 0; index < object.size(); ++index) {
            const auto member = object.getObjectMemberAt(index);
            bool known = false;
            for (const auto field : fields) known = known || member.name == field;
            const auto member_path = std::string(path) + "." + std::string(member.name);
            if (!known) return fail(SampleHeritageJsonStatus::UnknownField, member_path);
            for (std::uint32_t earlier = 0; earlier < index; ++earlier) {
                if (object.getObjectMemberAt(earlier).name == member.name)
                    return fail(SampleHeritageJsonStatus::DuplicateField, member_path);
            }
        }
        for (const auto field : fields) {
            if (!object.hasObjectMember(field))
                return fail(SampleHeritageJsonStatus::MissingField,
                            std::string(path) + "." + std::string(field));
        }
        return true;
    }

    bool string(Value object, std::string_view field, std::string_view path,
                std::string& destination) {
        const auto value = object[field];
        if (!value.isString()) return fail(SampleHeritageJsonStatus::WrongType,
                                           std::string(path));
        destination = value.getString();
        return true;
    }

    template <typename Integer>
    bool integer(Value object, std::string_view field, std::string_view path,
                 std::uint64_t minimum, std::uint64_t maximum,
                 Integer& destination) {
        const auto value = object[field];
        if (!value.isInt()) return fail(SampleHeritageJsonStatus::WrongType,
                                        std::string(path));
        const auto signed_value = value.get<int64_t>();
        if (signed_value < 0 || static_cast<std::uint64_t>(signed_value) < minimum ||
            static_cast<std::uint64_t>(signed_value) > maximum)
            return fail(SampleHeritageJsonStatus::NumberOutOfRange,
                        std::string(path));
        destination = static_cast<Integer>(signed_value);
        return true;
    }
};

struct Parser : JsonParserBase<SampleHeritageJsonParseResult> {
    double record_processing_rate = 0.0;

    bool boolean(Value object, std::string_view field, std::string_view path,
                 bool& destination) {
        const auto value = object[field];
        if (!value.isBool()) return fail(SampleHeritageJsonStatus::WrongType,
                                         std::string(path));
        destination = value.getBool();
        return true;
    }

    bool number(Value object, std::string_view field, std::string_view path,
                double minimum, double maximum, double& destination) {
        const auto value = object[field];
        if (!value.isInt() && !value.isFloat())
            return fail(SampleHeritageJsonStatus::WrongType, std::string(path));
        destination = value.isInt() ? static_cast<double>(value.get<int64_t>())
                                    : value.get<double>();
        if (!std::isfinite(destination) || destination < minimum || destination > maximum)
            return fail(SampleHeritageJsonStatus::NumberOutOfRange, std::string(path));
        return true;
    }

    bool seed(Value object, std::string_view field, std::string_view path,
              std::uint64_t& destination) {
        const auto value = object[field];
        if (!value.isString()) return fail(SampleHeritageJsonStatus::WrongType,
                                           std::string(path));
        const auto text = value.getString();
        if (text.empty() || (text.size() > 1 && text.front() == '0'))
            return fail(SampleHeritageJsonStatus::NumberOutOfRange, std::string(path));
        const auto conversion = std::from_chars(text.data(), text.data() + text.size(),
                                                destination);
        if (conversion.ec != std::errc{} || conversion.ptr != text.data() + text.size())
            return fail(SampleHeritageJsonStatus::NumberOutOfRange, std::string(path));
        return true;
    }

    bool seed_policy(Value object, std::string_view field, std::string_view path,
                     SampleHeritageSeedPolicy& destination) {
        const auto value = object[field];
        if (!value.isString()) return fail(SampleHeritageJsonStatus::WrongType,
                                           std::string(path));
        const auto text = value.getString();
        if (text == "restart_from_profile_seed") {
            destination = SampleHeritageSeedPolicy::RestartFromProfileSeed;
            return true;
        }
        if (text == "continue_serialized_state") {
            destination = SampleHeritageSeedPolicy::ContinueSerializedState;
            return true;
        }
        return fail(SampleHeritageJsonStatus::InvalidEnum, std::string(path));
    }

    bool domain(Value object, std::string_view expected, std::string_view path) {
        const auto value = object["domain"];
        if (!value.isString())
            return fail(SampleHeritageJsonStatus::WrongType, std::string(path));
        if (value.getString() != expected)
            return fail(SampleHeritageJsonStatus::InvalidEnum, std::string(path));
        return true;
    }

    template <typename Enum, std::size_t N>
    bool enumeration(Value object, std::string_view field, std::string_view path,
                     const std::array<std::pair<std::string_view, Enum>, N>& values,
                     Enum& destination) {
        const auto value = object[field];
        if (!value.isString())
            return fail(SampleHeritageJsonStatus::WrongType, std::string(path));
        for (const auto& [name, mapped] : values) {
            if (value.getString() == name) {
                destination = mapped;
                return true;
            }
        }
        return fail(SampleHeritageJsonStatus::InvalidEnum, std::string(path));
    }

    bool voice_block(Value value, std::size_t index,
                     SampleHeritageVoiceBlockSpec& destination) {
        const auto base = "$.voice[" + std::to_string(index) + "]";
        if (!value.isObject()) return fail(SampleHeritageJsonStatus::WrongType, base);
        if (!value.hasObjectMember("type"))
            return fail(SampleHeritageJsonStatus::MissingField, base + ".type");
        if (!value["type"].isString())
            return fail(SampleHeritageJsonStatus::WrongType, base + ".type");
        const auto type = value["type"].getString();
        destination.domain = SampleHeritageBlockDomain::Voice;
        const auto common = [&] {
            return domain(value, "voice", base + ".domain") &&
                   boolean(value, "bypass", base + ".bypass", destination.bypass);
        };
        if (type == "machine_domain") {
            constexpr std::array fields{"domain"sv, "type"sv, "bypass"sv,
                                        "sample_rate"sv};
            double sample_rate{};
            if (!audit_object(value, base, fields) || !common() ||
                !number(value, "sample_rate", base + ".sample_rate", 8000.0,
                        384000.0, sample_rate)) return false;
            destination.parameters = SampleHeritageVoiceMachineDomainBlock{sample_rate};
        } else if (type == "clock") {
            constexpr std::array fields{"domain"sv, "type"sv, "bypass"sv,
                                        "ratio"sv};
            double ratio{};
            if (!audit_object(value, base, fields) || !common() ||
                !number(value, "ratio", base + ".ratio", 0.015625, 64.0, ratio))
                return false;
            destination.parameters = SampleHeritageVoiceClockBlock{ratio};
        } else if (type == "pitch") {
            constexpr std::array fields{"domain"sv, "type"sv, "bypass"sv,
                                        "family"sv};
            constexpr std::array families{
                std::pair{"variable_clock"sv, SampleHeritagePitchFamily::VariableClock},
                std::pair{"drop_repeat"sv, SampleHeritagePitchFamily::DropRepeat},
                std::pair{"early_linear"sv, SampleHeritagePitchFamily::EarlyLinear}};
            SampleHeritagePitchFamily family{};
            if (!audit_object(value, base, fields) || !common() ||
                !enumeration(value, "family", base + ".family", families, family))
                return false;
            destination.parameters = SampleHeritageVoicePitchBlock{family};
        } else if (type == "converter") {
            constexpr std::array fields{"domain"sv, "type"sv, "bypass"sv,
                "family"sv, "bit_depth"sv, "dac_nonlinearity"sv,
                "dither_lsb"sv, "seed"sv, "seed_policy"sv};
            constexpr std::array families{
                std::pair{"linear_pcm"sv, SampleHeritageConverterFamily::LinearPcm},
                std::pair{"mu_law"sv, SampleHeritageConverterFamily::MuLaw},
                std::pair{"a_law"sv, SampleHeritageConverterFamily::ALaw}};
            SampleHeritageConverterFamily family{};
            double bits{}, nonlinearity{}, dither{}; std::uint64_t seed_value{};
            SampleHeritageSeedPolicy policy{};
            if (!audit_object(value, base, fields) || !common() ||
                !enumeration(value, "family", base + ".family", families, family) ||
                !number(value, "bit_depth", base + ".bit_depth", 4.0, 16.0, bits) ||
                !number(value, "dac_nonlinearity", base + ".dac_nonlinearity",
                        0.0, 1.0, nonlinearity) ||
                !number(value, "dither_lsb", base + ".dither_lsb", 0.0, 2.0,
                        dither) || !seed(value, "seed", base + ".seed", seed_value) ||
                !seed_policy(value, "seed_policy", base + ".seed_policy", policy))
                return false;
            if ((dither > 0.0 ||
                 policy == SampleHeritageSeedPolicy::ContinueSerializedState) &&
                seed_value == 0)
                return fail(SampleHeritageJsonStatus::NumberOutOfRange,
                            base + ".seed");
            destination.parameters =
                SampleHeritageVoiceConverterBlock{family, static_cast<float>(bits), static_cast<float>(nonlinearity),
                static_cast<float>(dither), seed_value, policy};
        } else if (type == "hold_droop") {
            constexpr std::array fields{"domain"sv, "type"sv, "bypass"sv,
                                        "mode"sv, "hold_samples"sv, "droop"sv};
            constexpr std::array modes{
                std::pair{"zero_order"sv, SampleHeritageHoldMode::ZeroOrder}};
            SampleHeritageHoldMode mode{};
            std::uint32_t hold{}; double droop{};
            if (!audit_object(value, base, fields) || !common() ||
                !enumeration(value, "mode", base + ".mode", modes, mode) ||
                !integer(value, "hold_samples", base + ".hold_samples", 1, 65536,
                         hold) ||
                !number(value, "droop", base + ".droop", 0.0, 1.0, droop))
                return false;
            destination.parameters =
                SampleHeritageVoiceHoldDroopBlock{mode, hold, static_cast<float>(droop)};
        } else if (type == "reconstruction") {
            constexpr std::array fields{"domain"sv, "type"sv, "bypass"sv,
                "family"sv, "cutoff_law"sv, "cutoff_value"sv, "order"sv,
                "ripple_db"sv, "stopband_attenuation_db"sv};
            constexpr std::array families{
                std::pair{"one_pole"sv, SampleHeritageReconstructionFamily::OnePole},
                std::pair{"butterworth"sv,
                          SampleHeritageReconstructionFamily::Butterworth},
                std::pair{"chebyshev"sv,
                          SampleHeritageReconstructionFamily::Chebyshev},
                std::pair{"elliptic"sv,
                          SampleHeritageReconstructionFamily::Elliptic}};
            constexpr std::array cutoff_laws{
                std::pair{"fixed_hz"sv, SampleHeritageCutoffLaw::FixedHz},
                std::pair{"machine_rate_ratio"sv,
                          SampleHeritageCutoffLaw::MachineRateRatio}};
            SampleHeritageReconstructionFamily family{};
            SampleHeritageCutoffLaw cutoff_law{};
            double cutoff{}, ripple{}, stopband_attenuation{};
            std::uint8_t order{};
            if (!audit_object(value, base, fields) || !common() ||
                !enumeration(value, "family", base + ".family", families, family) ||
                !enumeration(value, "cutoff_law", base + ".cutoff_law",
                             cutoff_laws, cutoff_law) ||
                !number(value, "cutoff_value", base + ".cutoff_value", 0.0,
                        192000.0, cutoff) ||
                !integer(value, "order", base + ".order", 1, 16, order) ||
                !number(value, "ripple_db", base + ".ripple_db", 0.0, 12.0,
                        ripple) ||
                !number(value, "stopband_attenuation_db",
                        base + ".stopband_attenuation_db", 0.0, 180.0,
                        stopband_attenuation)) return false;
            double machine_rate = result.profile.host_sample_rate;
            double clock_ratio = 1.0;
            for (std::size_t earlier = 0; earlier < index; ++earlier) {
                const auto& spec = result.profile.voice[earlier];
                if (spec.bypass) continue;
                if (const auto* machine =
                        std::get_if<SampleHeritageVoiceMachineDomainBlock>(&spec.parameters))
                    machine_rate = machine->sample_rate;
                else if (const auto* clock =
                             std::get_if<SampleHeritageVoiceClockBlock>(&spec.parameters))
                    clock_ratio = clock->ratio;
            }
            if ((cutoff_law == SampleHeritageCutoffLaw::FixedHz &&
                 (cutoff < 1.0 || cutoff >= machine_rate * clock_ratio * 0.5)) ||
                (cutoff_law == SampleHeritageCutoffLaw::MachineRateRatio &&
                 (cutoff <= 0.0 || cutoff >= 0.5)))
                return fail(SampleHeritageJsonStatus::NumberOutOfRange,
                            base + ".cutoff_value");
            const auto even_order = order >= 2 && (order & 1u) == 0;
            if ((family == SampleHeritageReconstructionFamily::OnePole &&
                 order != 1) ||
                (family != SampleHeritageReconstructionFamily::OnePole &&
                 !even_order))
                return fail(SampleHeritageJsonStatus::NumberOutOfRange,
                            base + ".order");
            if (((family == SampleHeritageReconstructionFamily::OnePole ||
                  family == SampleHeritageReconstructionFamily::Butterworth) &&
                 ripple != 0.0) ||
                ((family == SampleHeritageReconstructionFamily::Chebyshev ||
                  family == SampleHeritageReconstructionFamily::Elliptic) &&
                 ripple <= 0.0))
                return fail(SampleHeritageJsonStatus::NumberOutOfRange,
                            base + ".ripple_db");
            if ((family != SampleHeritageReconstructionFamily::Elliptic &&
                 stopband_attenuation != 0.0) ||
                (family == SampleHeritageReconstructionFamily::Elliptic &&
                 stopband_attenuation <= ripple))
                return fail(SampleHeritageJsonStatus::NumberOutOfRange,
                            base + ".stopband_attenuation_db");
            destination.parameters =
                SampleHeritageVoiceReconstructionBlock{family, cutoff_law, cutoff, order, static_cast<float>(ripple),
                static_cast<float>(stopband_attenuation)};
        } else if (type == "analog_color") {
            constexpr std::array fields{"domain"sv, "type"sv, "bypass"sv,
                                        "drive"sv, "asymmetry"sv, "mix"sv};
            double drive{}, asymmetry{}, mix{};
            if (!audit_object(value, base, fields) || !common() ||
                !number(value, "drive", base + ".drive", 0.0, 16.0, drive) ||
                !number(value, "asymmetry", base + ".asymmetry", -1.0, 1.0,
                        asymmetry) ||
                !number(value, "mix", base + ".mix", 0.0, 1.0, mix)) return false;
            destination.parameters = SampleHeritageVoiceAnalogColorBlock{
                static_cast<float>(drive), static_cast<float>(asymmetry),
                static_cast<float>(mix)};
        } else if (type == "live_cyclic_stretch") {
            constexpr std::array fields{"domain"sv, "type"sv, "bypass"sv,
                "factor"sv, "cycle_ms"sv, "splice_ms"sv, "stereo_link"sv,
                "tempo_lock"sv, "shuffle_divisions"sv, "seed"sv,
                "seed_policy"sv};
            double factor{}, cycle{}, splice{}; bool stereo{}, tempo_lock{};
            std::uint16_t shuffle_divisions{};
            std::uint64_t seed_value{}; SampleHeritageSeedPolicy policy{};
            if (!audit_object(value, base, fields) || !common() ||
                !number(value, "factor", base + ".factor", 0.25, 20.0, factor) ||
                !number(value, "cycle_ms", base + ".cycle_ms", 0.0,
                        std::numeric_limits<double>::max(), cycle) ||
                !number(value, "splice_ms", base + ".splice_ms", 0.0, 20.0,
                        splice) ||
                !boolean(value, "stereo_link", base + ".stereo_link", stereo) ||
                !boolean(value, "tempo_lock", base + ".tempo_lock", tempo_lock) ||
                !integer(value, "shuffle_divisions", base + ".shuffle_divisions",
                         0, 64, shuffle_divisions) ||
                !seed(value, "seed", base + ".seed", seed_value) ||
                !seed_policy(value, "seed_policy", base + ".seed_policy", policy))
                return false;
            if (cycle <= 0.0)
                return fail(SampleHeritageJsonStatus::NumberOutOfRange,
                            base + ".cycle_ms");
            if (splice > cycle * 0.5)
                return fail(SampleHeritageJsonStatus::NumberOutOfRange,
                            base + ".splice_ms");
            if (shuffle_divisions == 1)
                return fail(SampleHeritageJsonStatus::NumberOutOfRange,
                            base + ".shuffle_divisions");
            if ((shuffle_divisions != 0 ||
                 policy == SampleHeritageSeedPolicy::ContinueSerializedState) &&
                seed_value == 0)
                return fail(SampleHeritageJsonStatus::NumberOutOfRange,
                            base + ".seed");
            destination.parameters = SampleHeritageVoiceLiveCyclicStretchBlock{
                factor, cycle, splice, stereo, tempo_lock, shuffle_divisions,
                seed_value, policy};
        } else {
            return fail(SampleHeritageJsonStatus::InvalidEnum, base + ".type");
        }
        return true;
    }

    bool bus_block(Value value, std::size_t index,
                   SampleHeritageBusBlockSpec& destination) {
        const auto base = "$.bus[" + std::to_string(index) + "]";
        if (!value.isObject()) return fail(SampleHeritageJsonStatus::WrongType, base);
        if (!value.hasObjectMember("type"))
            return fail(SampleHeritageJsonStatus::MissingField, base + ".type");
        if (!value["type"].isString())
            return fail(SampleHeritageJsonStatus::WrongType, base + ".type");
        const auto type = value["type"].getString();
        destination.domain = SampleHeritageBlockDomain::Bus;
        const auto common = [&] {
            return domain(value, "bus", base + ".domain") &&
                   boolean(value, "bypass", base + ".bypass", destination.bypass);
        };
        if (type == "noise_idle") {
            constexpr std::array fields{"domain"sv, "type"sv, "bypass"sv,
                "noise_amplitude"sv, "idle_amplitude"sv,
                "tilt_db_per_octave"sv, "tilt_reference_hz"sv,
                "tilt_floor_hz"sv, "gate"sv, "seed"sv, "seed_policy"sv};
            constexpr std::array gates{
                std::pair{"always_on"sv, SampleHeritageNoiseGate::AlwaysOn},
                std::pair{"voice_active"sv, SampleHeritageNoiseGate::VoiceActive}};
            double noise{}, idle{}, tilt{}, tilt_reference{}, tilt_floor{};
            std::uint64_t seed_value{};
            SampleHeritageNoiseGate gate{};
            SampleHeritageSeedPolicy policy{};
            if (!audit_object(value, base, fields) || !common() ||
                !number(value, "noise_amplitude", base + ".noise_amplitude", 0.0,
                        1.0, noise) ||
                !number(value, "idle_amplitude", base + ".idle_amplitude", 0.0,
                        1.0, idle) ||
                !number(value, "tilt_db_per_octave", base + ".tilt_db_per_octave",
                        -24.0, 24.0, tilt) ||
                !number(value, "tilt_reference_hz", base + ".tilt_reference_hz",
                        1.0, 192000.0, tilt_reference) ||
                !number(value, "tilt_floor_hz", base + ".tilt_floor_hz", 1.0,
                        192000.0, tilt_floor) ||
                !enumeration(value, "gate", base + ".gate", gates, gate) ||
                !seed(value, "seed", base + ".seed", seed_value) ||
                !seed_policy(value, "seed_policy", base + ".seed_policy", policy))
                return false;
            if ((std::max(noise, idle) > 0.0 ||
                 policy == SampleHeritageSeedPolicy::ContinueSerializedState) &&
                seed_value == 0)
                return fail(SampleHeritageJsonStatus::NumberOutOfRange,
                            base + ".seed");
            if (tilt_floor > tilt_reference)
                return fail(SampleHeritageJsonStatus::NumberOutOfRange,
                            base + ".tilt_floor_hz");
            if (tilt_reference >= result.profile.host_sample_rate * 0.5)
                return fail(SampleHeritageJsonStatus::NumberOutOfRange,
                            base + ".tilt_reference_hz");
            destination.parameters = SampleHeritageBusNoiseIdleBlock{static_cast<float>(noise), static_cast<float>(idle),
                static_cast<float>(tilt), gate, seed_value, policy,
                tilt_reference, tilt_floor};
        } else if (type == "output_drive") {
            constexpr std::array fields{"domain"sv, "type"sv, "bypass"sv,
                                        "drive"sv, "ceiling"sv};
            double drive{}, ceiling{};
            if (!audit_object(value, base, fields) || !common() ||
                !number(value, "drive", base + ".drive", 0.0, 16.0, drive) ||
                !number(value, "ceiling", base + ".ceiling", 0.001, 4.0,
                        ceiling))
                return false;
            destination.parameters = SampleHeritageBusOutputDriveBlock{static_cast<float>(drive), static_cast<float>(ceiling)};
        } else {
            return fail(SampleHeritageJsonStatus::InvalidEnum, base + ".type");
        }
        return true;
    }

    bool record_commit_block(Value value, std::size_t index,
                             SampleHeritageRecordCommitBlockSpec& destination) {
        const auto base = "$.record_commit[" + std::to_string(index) + "]";
        if (!value.isObject()) return fail(SampleHeritageJsonStatus::WrongType, base);
        if (!value.hasObjectMember("type"))
            return fail(SampleHeritageJsonStatus::MissingField, base + ".type");
        if (!value["type"].isString())
            return fail(SampleHeritageJsonStatus::WrongType, base + ".type");
        const auto type = value["type"].getString();
        destination.domain = SampleHeritageBlockDomain::RecordCommit;
        const auto common = [&] {
            return domain(value, "record_commit", base + ".domain") &&
                   boolean(value, "bypass", base + ".bypass", destination.bypass);
        };
        if (type == "input_drive_clip") {
            constexpr std::array fields{"domain"sv, "type"sv, "bypass"sv,
                                        "drive"sv, "clip_level"sv};
            double drive{}, clip{};
            if (!audit_object(value, base, fields) || !common() ||
                !number(value, "drive", base + ".drive", 0.0, 16.0, drive) ||
                !number(value, "clip_level", base + ".clip_level", 0.001, 4.0,
                        clip)) return false;
            destination.parameters = SampleHeritageRecordInputDriveClipBlock{
                static_cast<float>(drive), static_cast<float>(clip)};
        } else if (type == "anti_alias_record_rate") {
            constexpr std::array fields{"domain"sv, "type"sv, "bypass"sv,
                "filter_family"sv, "sample_rate"sv, "cutoff_law"sv,
                "cutoff_value"sv, "order"sv, "ripple_db"sv,
                "stopband_attenuation_db"sv};
            constexpr std::array families{
                std::pair{"one_pole"sv, SampleHeritageRecordFilterFamily::OnePole},
                std::pair{"butterworth"sv,
                          SampleHeritageRecordFilterFamily::Butterworth},
                std::pair{"chebyshev"sv,
                          SampleHeritageRecordFilterFamily::Chebyshev},
                std::pair{"elliptic"sv,
                          SampleHeritageRecordFilterFamily::Elliptic}};
            constexpr std::array cutoff_laws{
                std::pair{"fixed_hz"sv, SampleHeritageCutoffLaw::FixedHz},
                std::pair{"machine_rate_ratio"sv,
                          SampleHeritageCutoffLaw::MachineRateRatio}};
            SampleHeritageRecordFilterFamily family{};
            SampleHeritageCutoffLaw cutoff_law{};
            double rate{}, cutoff{}, ripple{}, stopband_attenuation{};
            std::uint8_t order{};
            if (!audit_object(value, base, fields) || !common() ||
                !enumeration(value, "filter_family", base + ".filter_family",
                             families, family) ||
                !number(value, "sample_rate", base + ".sample_rate", 8000.0,
                        384000.0, rate) ||
                !enumeration(value, "cutoff_law", base + ".cutoff_law",
                             cutoff_laws, cutoff_law) ||
                !number(value, "cutoff_value", base + ".cutoff_value", 0.0,
                        192000.0, cutoff) ||
                !integer(value, "order", base + ".order", 1, 16, order) ||
                !number(value, "ripple_db", base + ".ripple_db", 0.0, 12.0,
                        ripple) ||
                !number(value, "stopband_attenuation_db",
                        base + ".stopband_attenuation_db", 0.0, 180.0,
                        stopband_attenuation)) return false;
            if ((cutoff_law == SampleHeritageCutoffLaw::FixedHz &&
                 (cutoff < 1.0 || cutoff >= record_processing_rate * 0.5)) ||
                (cutoff_law == SampleHeritageCutoffLaw::MachineRateRatio &&
                 (cutoff <= 0.0 || cutoff >= 0.5)))
                return fail(SampleHeritageJsonStatus::NumberOutOfRange,
                            base + ".cutoff_value");
            const auto even_order = order >= 2 && (order & 1u) == 0;
            if ((family == SampleHeritageRecordFilterFamily::OnePole &&
                 order != 1) ||
                (family != SampleHeritageRecordFilterFamily::OnePole &&
                 !even_order))
                return fail(SampleHeritageJsonStatus::NumberOutOfRange,
                            base + ".order");
            if (((family == SampleHeritageRecordFilterFamily::OnePole ||
                  family == SampleHeritageRecordFilterFamily::Butterworth) &&
                 ripple != 0.0) ||
                ((family == SampleHeritageRecordFilterFamily::Chebyshev ||
                  family == SampleHeritageRecordFilterFamily::Elliptic) &&
                 ripple <= 0.0))
                return fail(SampleHeritageJsonStatus::NumberOutOfRange,
                            base + ".ripple_db");
            if ((family != SampleHeritageRecordFilterFamily::Elliptic &&
                 stopband_attenuation != 0.0) ||
                (family == SampleHeritageRecordFilterFamily::Elliptic &&
                 stopband_attenuation <= ripple))
                return fail(SampleHeritageJsonStatus::NumberOutOfRange,
                            base + ".stopband_attenuation_db");
            destination.parameters =
                SampleHeritageRecordRateBlock{family, rate, cutoff_law, cutoff, order,
                static_cast<float>(ripple),
                static_cast<float>(stopband_attenuation)};
            if (!destination.bypass) record_processing_rate = rate;
        } else if (type == "converter") {
            constexpr std::array fields{"domain"sv, "type"sv, "bypass"sv,
                "family"sv, "bit_depth"sv, "dac_nonlinearity"sv,
                "dither_lsb"sv, "seed"sv, "seed_policy"sv};
            constexpr std::array families{
                std::pair{"linear_pcm"sv, SampleHeritageConverterFamily::LinearPcm},
                std::pair{"mu_law"sv, SampleHeritageConverterFamily::MuLaw},
                std::pair{"a_law"sv, SampleHeritageConverterFamily::ALaw}};
            SampleHeritageConverterFamily family{};
            double bits{}, nonlinearity{}, dither{}; std::uint64_t seed_value{};
            SampleHeritageSeedPolicy policy{};
            if (!audit_object(value, base, fields) || !common() ||
                !enumeration(value, "family", base + ".family", families, family) ||
                !number(value, "bit_depth", base + ".bit_depth", 4.0, 16.0, bits) ||
                !number(value, "dac_nonlinearity", base + ".dac_nonlinearity",
                        0.0, 1.0, nonlinearity) ||
                !number(value, "dither_lsb", base + ".dither_lsb", 0.0, 2.0,
                        dither) || !seed(value, "seed", base + ".seed", seed_value) ||
                !seed_policy(value, "seed_policy", base + ".seed_policy", policy))
                return false;
            if ((dither > 0.0 ||
                 policy == SampleHeritageSeedPolicy::ContinueSerializedState) &&
                seed_value == 0)
                return fail(SampleHeritageJsonStatus::NumberOutOfRange,
                            base + ".seed");
            destination.parameters =
                SampleHeritageRecordConverterBlock{family, static_cast<float>(bits), static_cast<float>(nonlinearity),
                static_cast<float>(dither), seed_value, policy};
        } else if (type == "commit_stretch") {
            constexpr std::array families{std::pair{"cyclic"sv, 0u},
                std::pair{"adaptive"sv, 1u}};
            std::uint32_t family{};
            double factor{};
            std::uint64_t zone_start{}, zone_end{};
            if (!common() ||
                !enumeration(value, "family", base + ".family", families, family) ||
                !number(value, "factor", base + ".factor", 0.25, 20.0, factor) ||
                !integer(value, "zone_start_frame", base + ".zone_start_frame", 0,
                         std::numeric_limits<std::int64_t>::max(), zone_start) ||
                !integer(value, "zone_end_frame", base + ".zone_end_frame", 0,
                         std::numeric_limits<std::int64_t>::max(), zone_end))
                return false;
            if (!((zone_start == 0 && zone_end == 0) || zone_start < zone_end))
                return fail(SampleHeritageJsonStatus::NumberOutOfRange,
                            base + ".zone_end_frame");
            if (family == 0) {
                constexpr std::array fields{"domain"sv,
                                            "type"sv,
                                            "bypass"sv,
                                            "family"sv,
                                            "factor"sv,
                                            "cycle_samples"sv,
                                            "crossfade_samples"sv,
                                            "zone_start_frame"sv,
                                            "zone_end_frame"sv};
                std::uint32_t cycle{}, crossfade{};
                if (!audit_object(value, base, fields) ||
                    !integer(value, "cycle_samples", base + ".cycle_samples", 1, 1048576, cycle) ||
                    !integer(value, "crossfade_samples", base + ".crossfade_samples", 0, 524288,
                             crossfade))
                    return false;
                if (crossfade > cycle / 2)
                    return fail(SampleHeritageJsonStatus::NumberOutOfRange,
                                base + ".crossfade_samples");
            destination.parameters = SampleHeritageRecordCommitCyclicStretchBlock{ factor, cycle, crossfade, zone_start, zone_end};
            } else {
                constexpr std::array fields{"domain"sv,
                                            "type"sv,
                                            "bypass"sv,
                                            "family"sv,
                                            "factor"sv,
                                            "decision_hop_samples"sv,
                                            "search_radius_samples"sv,
                                            "search_stride_samples"sv,
                                            "crossfade_samples"sv,
                                            "zone_start_frame"sv,
                                            "zone_end_frame"sv,
                                            "stereo_link"sv};
                std::uint32_t hop{}, radius{}, stride{}, crossfade{};
                bool stereo{};
                if (!audit_object(value, base, fields) ||
                    !integer(value, "decision_hop_samples", base + ".decision_hop_samples", 1,
                             1048576, hop) ||
                    !integer(value, "search_radius_samples", base + ".search_radius_samples", 0,
                             1048576, radius) ||
                    !integer(value, "search_stride_samples", base + ".search_stride_samples", 1,
                             1048576, stride) ||
                    !integer(value, "crossfade_samples", base + ".crossfade_samples", 0, 524288,
                             crossfade) ||
                    !boolean(value, "stereo_link", base + ".stereo_link", stereo))
                    return false;
                if (crossfade > hop)
                    return fail(SampleHeritageJsonStatus::NumberOutOfRange,
                                base + ".crossfade_samples");
                destination.parameters = SampleHeritageRecordCommitAdaptiveStretchBlock{
                    factor, hop, radius, stride, crossfade, zone_start, zone_end,
                stereo};
            }
        } else {
            return fail(SampleHeritageJsonStatus::InvalidEnum, base + ".type");
        }
        return true;
    }
};

struct RuntimeStateParser
    : JsonParserBase<SampleHeritageRuntimeStateJsonParseResult> {

    bool random_state(Value object,
                      std::string_view path,
                      std::uint64_t& destination) {
        const auto value = object["random_state"];
        if (!value.isString())
            return fail(SampleHeritageJsonStatus::WrongType, std::string(path));
        const auto text = value.getString();
        if (text.empty() || text == "0" || (text.size() > 1 && text.front() == '0'))
            return fail(SampleHeritageJsonStatus::NumberOutOfRange,
                        std::string(path));
        const auto conversion =
            std::from_chars(text.data(), text.data() + text.size(), destination);
        if (conversion.ec != std::errc{} ||
            conversion.ptr != text.data() + text.size())
            return fail(SampleHeritageJsonStatus::NumberOutOfRange,
                        std::string(path));
        return true;
    }
};

SampleHeritageRuntimeStateStatus
validate_runtime_state_shape(const SampleHeritageRuntimeState& state) noexcept {
    if (state.schema_version != kSampleHeritageRuntimeStateSchemaVersion)
        return SampleHeritageRuntimeStateStatus::UnsupportedSchemaVersion;
    if (state.profile_schema_version != kSampleHeritageProfileSchemaVersion ||
        state.profile_digest_version != kSampleHeritageProfileDigestVersion ||
        !detail::valid_neutral_profile_id(state.bound_profile_id()))
        return SampleHeritageRuntimeStateStatus::ProfileMismatch;
    if (state.rng_state_count > state.rng_states.size())
        return SampleHeritageRuntimeStateStatus::InvalidStageLayout;
    for (std::size_t index = 0; index < state.rng_state_count; ++index) {
        const auto& rng = state.rng_states[index];
        if (rng.stage_index >= kSampleHeritageMaximumStages ||
            (index != 0 &&
             state.rng_states[index - 1].stage_index >= rng.stage_index) ||
            (rng.stage_type != SampleHeritageRuntimeRngStageType::Quantization &&
             rng.stage_type != SampleHeritageRuntimeRngStageType::Noise))
            return SampleHeritageRuntimeStateStatus::InvalidStageLayout;
        if (rng.random_state == 0)
            return SampleHeritageRuntimeStateStatus::InvalidRandomState;
    }
    return SampleHeritageRuntimeStateStatus::Ok;
}

bool parse_digest_hex(std::string_view text,
                      std::array<std::uint8_t, 32>& digest) noexcept {
    if (text.size() != digest.size() * 2) return false;
    const auto nibble = [](char character) noexcept -> int {
        if (character >= '0' && character <= '9') return character - '0';
        if (character >= 'a' && character <= 'f') return 10 + character - 'a';
        return -1;
    };
    for (std::size_t index = 0; index < digest.size(); ++index) {
        const auto high = nibble(text[index * 2]);
        const auto low = nibble(text[index * 2 + 1]);
        if (high < 0 || low < 0) return false;
        digest[index] = static_cast<std::uint8_t>((high << 4) | low);
    }
    return true;
}

void append_digest_hex(std::string& output,
                       const std::array<std::uint8_t, 32>& digest) {
    constexpr std::string_view digits = "0123456789abcdef";
    output.push_back('"');
    for (const auto byte : digest) {
        output.push_back(digits[byte >> 4]);
        output.push_back(digits[byte & 0x0f]);
    }
    output.push_back('"');
}

template <typename Number>
void append_number(std::string& output, Number value) {
    if (value == Number{}) value = Number{};
    std::array<char, 64> buffer{};
    const auto converted = std::to_chars(buffer.data(), buffer.data() + buffer.size(),
                                         value, std::chars_format::general);
    output.append(buffer.data(), converted.ptr);
}

void append_seed(std::string& output, std::uint64_t seed) {
    std::array<char, 32> buffer{};
    const auto converted = std::to_chars(buffer.data(), buffer.data() + buffer.size(), seed);
    output.push_back('"');
    output.append(buffer.data(), converted.ptr);
    output.push_back('"');
}

std::string_view seed_policy_name(SampleHeritageSeedPolicy policy) {
    return policy == SampleHeritageSeedPolicy::RestartFromProfileSeed
        ? "restart_from_profile_seed"
        : "continue_serialized_state";
}

std::string_view converter_family_name(SampleHeritageConverterFamily family) {
    if (family == SampleHeritageConverterFamily::MuLaw) return "mu_law";
    if (family == SampleHeritageConverterFamily::ALaw) return "a_law";
    return "linear_pcm";
}

std::string_view reconstruction_family_name(SampleHeritageReconstructionFamily family) {
    if (family == SampleHeritageReconstructionFamily::Butterworth)
        return "butterworth";
    if (family == SampleHeritageReconstructionFamily::Chebyshev)
        return "chebyshev";
    if (family == SampleHeritageReconstructionFamily::Elliptic) return "elliptic";
    return "one_pole";
}

std::string_view record_filter_family_name(SampleHeritageRecordFilterFamily family) {
    if (family == SampleHeritageRecordFilterFamily::Butterworth)
        return "butterworth";
    if (family == SampleHeritageRecordFilterFamily::Chebyshev) return "chebyshev";
    if (family == SampleHeritageRecordFilterFamily::Elliptic) return "elliptic";
    return "one_pole";
}

std::string_view cutoff_law_name(SampleHeritageCutoffLaw law) {
    return law == SampleHeritageCutoffLaw::MachineRateRatio
        ? "machine_rate_ratio"
        : "fixed_hz";
}

void append_bool(std::string& output, bool value) {
    output += value ? "true" : "false";
}

}  // namespace

SampleHeritageJsonParseResult parse_sample_heritage_profile_json(std::string_view json) {
    Parser parser;
    try {
        const auto root_value = choc::json::parse(json);
        const Value root = root_value;
        if (!root.isObject()) {
            parser.fail(SampleHeritageJsonStatus::RootNotObject, "$");
            return std::move(parser.result);
        }
        constexpr std::array fields{"schema_version"sv, "profile_id"sv,
                                    "host_sample_rate"sv, "voice"sv, "bus"sv,
                                    "record_commit"sv};
        if (!parser.audit_object(root, "$", fields)) return std::move(parser.result);

        std::uint32_t schema = 0;
        if (!parser.integer(root, "schema_version", "$.schema_version", 0,
                            std::numeric_limits<std::uint32_t>::max(), schema))
            return std::move(parser.result);
        if (schema != kSampleHeritageProfileSchemaVersion) {
            parser.result.profile_status =
                SampleHeritageProfileStatus::UnsupportedSchemaVersion;
            parser.fail(SampleHeritageJsonStatus::ProfileValidationFailed,
                        "$.schema_version");
            return std::move(parser.result);
        }
        parser.result.profile.schema_version = schema;
        if (!parser.string(root, "profile_id", "$.profile_id",
                           parser.result.profile.profile_id))
            return std::move(parser.result);
        if (!parser.number(root, "host_sample_rate", "$.host_sample_rate", 8000.0,
                           384000.0, parser.result.profile.host_sample_rate))
            return std::move(parser.result);
        parser.record_processing_rate = parser.result.profile.host_sample_rate;
        const auto parse_blocks = [&](std::string_view name, std::size_t maximum,
                                      auto& destination, auto parse_block) {
            const auto values = root[name];
            const auto array_path = "$." + std::string(name);
            if (!values.isArray())
                return parser.fail(SampleHeritageJsonStatus::WrongType, array_path);
            if (values.size() > maximum)
                return parser.fail(SampleHeritageJsonStatus::NumberOutOfRange,
                                   array_path);
            destination.resize(values.size());
            for (std::uint32_t index = 0; index < values.size(); ++index) {
                if (!(parser.*parse_block)(values[index], index, destination[index]))
                    return false;
            }
            return true;
        };
        if (!parse_blocks("voice", kSampleHeritageMaximumVoiceBlocks,
                          parser.result.profile.voice, &Parser::voice_block) ||
            !parse_blocks("bus", kSampleHeritageMaximumBusBlocks,
                          parser.result.profile.bus, &Parser::bus_block) ||
            !parse_blocks("record_commit", kSampleHeritageMaximumRecordCommitBlocks,
                          parser.result.profile.record_commit,
                          &Parser::record_commit_block))
            return std::move(parser.result);
        const auto validation = validate_sample_heritage_profile(parser.result.profile);
        parser.result.profile_status = validation.status;
        if (!validation.valid()) {
            std::string path = "$";
            if (validation.status == SampleHeritageProfileStatus::InvalidProfileId)
                path = "$.profile_id";
            else if (validation.status ==
                     SampleHeritageProfileStatus::InvalidHostSampleRate)
                path = "$.host_sample_rate";
            else if (validation.status ==
                     SampleHeritageProfileStatus::UnsupportedSchemaVersion)
                path = "$.schema_version";
            else {
                const auto domain = validation.block_domain ==
                                            SampleHeritageBlockDomain::Voice
                                        ? "voice"
                                    : validation.block_domain ==
                                            SampleHeritageBlockDomain::Bus
                                        ? "bus"
                                        : "record_commit";
                path = "$." + std::string(domain) + "[" +
                       std::to_string(validation.stage_index) + "]";
            }
            parser.fail(SampleHeritageJsonStatus::ProfileValidationFailed,
                        std::move(path));
            return std::move(parser.result);
        }
        parser.result.status = SampleHeritageJsonStatus::Ok;
        parser.result.field_path.clear();
        return std::move(parser.result);
    } catch (const choc::value::Error& error) {
        parser.result.status =
            std::string_view(error.what()) ==
                    "This object already contains a member with the given name"
                ? SampleHeritageJsonStatus::DuplicateField
                : SampleHeritageJsonStatus::InvalidJson;
        // CHOC rejects a duplicate while constructing the value, before it can
        // expose the containing object. The status remains exact; "$" is the
        // narrowest trustworthy path available at that point.
        parser.result.field_path = "$";
        return std::move(parser.result);
    } catch (const choc::json::ParseError&) {
        parser.result.status = SampleHeritageJsonStatus::InvalidJson;
        parser.result.field_path = "$";
        return std::move(parser.result);
    }
}

SampleHeritageJsonWriteResult
write_sample_heritage_profile_json(const SampleHeritageProfile& profile) {
    SampleHeritageJsonWriteResult result;
    const auto validation = validate_sample_heritage_profile(profile);
    result.profile_status = validation.status;
    if (!validation.valid()) return result;
    if (!profile.stages.empty()) {
        result.profile_status =
            SampleHeritageProfileStatus::NonCanonicalProfileRepresentation;
        return result;
    }

    auto& output = result.json;
    output = "{\"schema_version\":" +
             std::to_string(kSampleHeritageProfileSchemaVersion) +
             ",\"profile_id\":\"" + profile.profile_id +
             "\",\"host_sample_rate\":";
    append_number(output, profile.host_sample_rate);
    output += ",\"voice\":[";
    for (std::size_t index = 0; index < profile.voice.size(); ++index) {
        if (index != 0) output.push_back(',');
        const auto& spec = profile.voice[index];
        std::visit(
            [&](const auto& block) {
            using Block = std::decay_t<decltype(block)>;
            output += "{\"domain\":\"voice\",\"type\":\"";
            if constexpr (std::is_same_v<Block, SampleHeritageVoiceMachineDomainBlock>)
                output += "machine_domain";
            else if constexpr (std::is_same_v<Block, SampleHeritageVoiceClockBlock>)
                output += "clock";
            else if constexpr (std::is_same_v<Block, SampleHeritageVoicePitchBlock>)
                output += "pitch";
            else if constexpr (std::is_same_v<Block, SampleHeritageVoiceConverterBlock>)
                output += "converter";
            else if constexpr (std::is_same_v<Block, SampleHeritageVoiceHoldDroopBlock>)
                output += "hold_droop";
            else if constexpr (std::is_same_v<Block,
                                               SampleHeritageVoiceReconstructionBlock>)
                output += "reconstruction";
            else if constexpr (std::is_same_v<Block,
                                               SampleHeritageVoiceAnalogColorBlock>)
                output += "analog_color";
            else
                output += "live_cyclic_stretch";
            output += "\",\"bypass\":";
            append_bool(output, spec.bypass);
            if constexpr (std::is_same_v<Block, SampleHeritageVoiceMachineDomainBlock>) {
                output += ",\"sample_rate\":"; append_number(output, block.sample_rate);
            } else if constexpr (std::is_same_v<Block, SampleHeritageVoiceClockBlock>) {
                output += ",\"ratio\":"; append_number(output, block.ratio);
            } else if constexpr (std::is_same_v<Block, SampleHeritageVoicePitchBlock>) {
                output += ",\"family\":\"";
                output += block.family == SampleHeritagePitchFamily::DropRepeat
                              ? "drop_repeat"
                          : block.family == SampleHeritagePitchFamily::EarlyLinear
                              ? "early_linear"
                              : "variable_clock";
                output.push_back('"');
            } else if constexpr (std::is_same_v<Block,
                                                 SampleHeritageVoiceConverterBlock>) {
                output += ",\"family\":\""; output += converter_family_name(block.family);
                output += "\",\"bit_depth\":"; append_number(output, block.bit_depth);
                output += ",\"dac_nonlinearity\":";
                append_number(output, block.dac_nonlinearity);
                output += ",\"dither_lsb\":"; append_number(output, block.dither_lsb);
                output += ",\"seed\":"; append_seed(output, block.seed);
                output += ",\"seed_policy\":\"";
                output += seed_policy_name(block.seed_policy); output.push_back('"');
            } else if constexpr (std::is_same_v<Block,
                                                 SampleHeritageVoiceHoldDroopBlock>) {
                output += ",\"mode\":\"zero_order\"";
                output += ",\"hold_samples\":" + std::to_string(block.hold_samples);
                output += ",\"droop\":"; append_number(output, block.droop);
            } else if constexpr (std::is_same_v<Block,
                                                 SampleHeritageVoiceReconstructionBlock>) {
                output += ",\"family\":\"";
                output += reconstruction_family_name(block.family);
                output += "\",\"cutoff_law\":\"";
                output += cutoff_law_name(block.cutoff_law);
                output += "\",\"cutoff_value\":";
                append_number(output, block.cutoff_value);
                output += ",\"order\":" + std::to_string(block.order);
                output += ",\"ripple_db\":"; append_number(output, block.ripple_db);
                output += ",\"stopband_attenuation_db\":";
                append_number(output, block.stopband_attenuation_db);
            } else if constexpr (std::is_same_v<Block,
                                                 SampleHeritageVoiceAnalogColorBlock>) {
                output += ",\"drive\":"; append_number(output, block.drive);
                output += ",\"asymmetry\":"; append_number(output, block.asymmetry);
                output += ",\"mix\":"; append_number(output, block.mix);
            } else {
                output += ",\"factor\":"; append_number(output, block.factor);
                output += ",\"cycle_ms\":"; append_number(output, block.cycle_ms);
                output += ",\"splice_ms\":"; append_number(output, block.splice_ms);
                output += ",\"stereo_link\":"; append_bool(output, block.stereo_link);
                output += ",\"tempo_lock\":"; append_bool(output, block.tempo_lock);
                output += ",\"shuffle_divisions\":" +
                          std::to_string(block.shuffle_divisions);
                output += ",\"seed\":"; append_seed(output, block.seed);
                output += ",\"seed_policy\":\"";
                output += seed_policy_name(block.seed_policy); output.push_back('"');
            }
            output.push_back('}');
        }, spec.parameters);
    }
    output += "],\"bus\":[";
    for (std::size_t index = 0; index < profile.bus.size(); ++index) {
        if (index != 0) output.push_back(',');
        const auto& spec = profile.bus[index];
        std::visit(
            [&](const auto& block) {
            using Block = std::decay_t<decltype(block)>;
            output += "{\"domain\":\"bus\",\"type\":\"";
            if constexpr (std::is_same_v<Block, SampleHeritageBusNoiseIdleBlock>)
                output += "noise_idle\",\"bypass\":";
            else
                output += "output_drive\",\"bypass\":";
            append_bool(output, spec.bypass);
            if constexpr (std::is_same_v<Block, SampleHeritageBusNoiseIdleBlock>) {
                output += ",\"noise_amplitude\":";
                append_number(output, block.noise_amplitude);
                output += ",\"idle_amplitude\":";
                append_number(output, block.idle_amplitude);
                output += ",\"tilt_db_per_octave\":";
                append_number(output, block.tilt_db_per_octave);
                output += ",\"tilt_reference_hz\":";
                append_number(output, block.tilt_reference_hz);
                output += ",\"tilt_floor_hz\":";
                append_number(output, block.tilt_floor_hz);
                output += ",\"gate\":\"";
                output += block.gate == SampleHeritageNoiseGate::VoiceActive
                              ? "voice_active"
                              : "always_on";
                output.push_back('"');
                output += ",\"seed\":"; append_seed(output, block.seed);
                output += ",\"seed_policy\":\"";
                output += seed_policy_name(block.seed_policy); output.push_back('"');
            } else {
                output += ",\"drive\":"; append_number(output, block.drive);
                output += ",\"ceiling\":"; append_number(output, block.ceiling);
            }
            output.push_back('}');
        }, spec.parameters);
    }
    output += "],\"record_commit\":[";
    for (std::size_t index = 0; index < profile.record_commit.size(); ++index) {
        if (index != 0) output.push_back(',');
        const auto& spec = profile.record_commit[index];
        std::visit(
            [&](const auto& block) {
            using Block = std::decay_t<decltype(block)>;
            output += "{\"domain\":\"record_commit\",\"type\":\"";
            if constexpr (std::is_same_v<Block,
                                         SampleHeritageRecordInputDriveClipBlock>)
                output += "input_drive_clip";
            else if constexpr (std::is_same_v<Block, SampleHeritageRecordRateBlock>)
                output += "anti_alias_record_rate";
            else if constexpr (std::is_same_v<Block,
                                               SampleHeritageRecordConverterBlock>)
                output += "converter";
            else
                output += "commit_stretch";
            output += "\",\"bypass\":";
            append_bool(output, spec.bypass);
            if constexpr (std::is_same_v<Block,
                                         SampleHeritageRecordInputDriveClipBlock>) {
                output += ",\"drive\":"; append_number(output, block.drive);
                output += ",\"clip_level\":"; append_number(output, block.clip_level);
            } else if constexpr (std::is_same_v<Block,
                                                 SampleHeritageRecordRateBlock>) {
                output += ",\"filter_family\":\"";
                output += record_filter_family_name(block.filter_family);
                output += "\",\"sample_rate\":"; append_number(output, block.sample_rate);
                output += ",\"cutoff_law\":\"";
                output += cutoff_law_name(block.cutoff_law);
                output += "\",\"cutoff_value\":";
                append_number(output, block.cutoff_value);
                output += ",\"order\":" + std::to_string(block.order);
                output += ",\"ripple_db\":"; append_number(output, block.ripple_db);
                output += ",\"stopband_attenuation_db\":";
                append_number(output, block.stopband_attenuation_db);
            } else if constexpr (std::is_same_v<Block,
                                                 SampleHeritageRecordConverterBlock>) {
                output += ",\"family\":\""; output += converter_family_name(block.family);
                output += "\",\"bit_depth\":"; append_number(output, block.bit_depth);
                output += ",\"dac_nonlinearity\":";
                append_number(output, block.dac_nonlinearity);
                output += ",\"dither_lsb\":"; append_number(output, block.dither_lsb);
                output += ",\"seed\":"; append_seed(output, block.seed);
                output += ",\"seed_policy\":\"";
                output += seed_policy_name(block.seed_policy); output.push_back('"');
            } else if constexpr (std::is_same_v<Block,
                                                    SampleHeritageRecordCommitCyclicStretchBlock>) {
                output += ",\"family\":\"cyclic";
                output += "\",\"factor\":"; append_number(output, block.factor);
                output += ",\"cycle_samples\":" + std::to_string(block.cycle_samples);
                output += ",\"crossfade_samples\":" + std::to_string(block.crossfade_samples);
                output += ",\"zone_start_frame\":" + std::to_string(block.zone_start_frame);
                output += ",\"zone_end_frame\":" + std::to_string(block.zone_end_frame);
                } else {
                    output += ",\"family\":\"adaptive\",\"factor\":";
                    append_number(output, block.factor);
                    output +=
                        ",\"decision_hop_samples\":" + std::to_string(block.decision_hop_samples);
                    output +=
                        ",\"search_radius_samples\":" + std::to_string(block.search_radius_samples);
                    output +=
                        ",\"search_stride_samples\":" + std::to_string(block.search_stride_samples);
                    output += ",\"crossfade_samples\":" + std::to_string(block.crossfade_samples);
                output += ",\"zone_start_frame\":" +
                          std::to_string(block.zone_start_frame);
                output += ",\"zone_end_frame\":" +
                          std::to_string(block.zone_end_frame);
                output += ",\"stereo_link\":"; append_bool(output, block.stereo_link);
            }
            output.push_back('}');
        }, spec.parameters);
    }
    output += "]}";
    result.status = SampleHeritageJsonStatus::Ok;
    return result;
}

SampleHeritageRuntimeStateJsonParseResult
parse_sample_heritage_runtime_state_json(std::string_view json) {
    RuntimeStateParser parser;
    try {
        const auto root_value = choc::json::parse(json);
        const Value root = root_value;
        if (!root.isObject()) {
            parser.fail(SampleHeritageJsonStatus::RootNotObject, "$");
            return std::move(parser.result);
        }
        constexpr std::array fields{"schema_version"sv, "profile_schema_version"sv, "profile_id"sv,
            "profile_digest_version"sv, "profile_digest"sv, "rng_states"sv};
        if (!parser.audit_object(root, "$", fields))
            return std::move(parser.result);

        std::uint32_t schema_version = 0;
        if (!parser.integer(root, "schema_version", "$.schema_version", 0,
                            std::numeric_limits<std::uint32_t>::max(),
                            schema_version))
            return std::move(parser.result);
        parser.result.state.schema_version = schema_version;
        if (schema_version != kSampleHeritageRuntimeStateSchemaVersion) {
            parser.result.runtime_status =
                SampleHeritageRuntimeStateStatus::UnsupportedSchemaVersion;
            parser.fail(SampleHeritageJsonStatus::ProfileValidationFailed,
                        "$.schema_version");
            return std::move(parser.result);
        }
        if (!parser.integer(root, "profile_schema_version",
                            "$.profile_schema_version", 0,
                            std::numeric_limits<std::uint32_t>::max(),
                            parser.result.state.profile_schema_version))
            return std::move(parser.result);
        if (parser.result.state.profile_schema_version !=
            kSampleHeritageProfileSchemaVersion) {
            parser.result.runtime_status =
                SampleHeritageRuntimeStateStatus::ProfileMismatch;
            parser.fail(SampleHeritageJsonStatus::ProfileValidationFailed,
                        "$.profile_schema_version");
            return std::move(parser.result);
        }
        std::string profile_id;
        if (!parser.string(root, "profile_id", "$.profile_id", profile_id))
            return std::move(parser.result);
        if (!detail::valid_neutral_profile_id(profile_id)) {
            parser.result.runtime_status =
                SampleHeritageRuntimeStateStatus::ProfileMismatch;
            parser.fail(SampleHeritageJsonStatus::ProfileValidationFailed,
                        "$.profile_id");
            return std::move(parser.result);
        }
        std::copy(profile_id.begin(), profile_id.end(),
                  parser.result.state.profile_id.begin());
        if (!parser.integer(root, "profile_digest_version",
                            "$.profile_digest_version", 0,
                            std::numeric_limits<std::uint32_t>::max(),
                            parser.result.state.profile_digest_version))
            return std::move(parser.result);
        if (parser.result.state.profile_digest_version !=
            kSampleHeritageProfileDigestVersion) {
            parser.result.runtime_status =
                SampleHeritageRuntimeStateStatus::ProfileMismatch;
            parser.fail(SampleHeritageJsonStatus::ProfileValidationFailed,
                        "$.profile_digest_version");
            return std::move(parser.result);
        }
        std::string profile_digest;
        if (!parser.string(root, "profile_digest", "$.profile_digest",
                           profile_digest))
            return std::move(parser.result);
        if (!parse_digest_hex(profile_digest,
                              parser.result.state.profile_digest)) {
            parser.result.runtime_status =
                SampleHeritageRuntimeStateStatus::ProfileMismatch;
            parser.fail(SampleHeritageJsonStatus::ProfileValidationFailed,
                        "$.profile_digest");
            return std::move(parser.result);
        }

        const auto states = root["rng_states"];
        if (!states.isArray()) {
            parser.fail(SampleHeritageJsonStatus::WrongType, "$.rng_states");
            return std::move(parser.result);
        }
        if (states.size() > kSampleHeritageMaximumStages) {
            parser.fail(SampleHeritageJsonStatus::NumberOutOfRange,
                        "$.rng_states");
            return std::move(parser.result);
        }
        parser.result.state.rng_state_count = states.size();
        for (std::uint32_t index = 0; index < states.size(); ++index) {
            const auto value = states[index];
            const auto base = "$.rng_states[" + std::to_string(index) + "]";
            constexpr std::array state_fields{"stage_index"sv, "stage_type"sv, "random_state"sv};
            if (!parser.audit_object(value, base, state_fields))
                return std::move(parser.result);
            std::uint32_t stage_index = 0;
            if (!parser.integer(value, "stage_index", base + ".stage_index", 0,
                                kSampleHeritageMaximumStages - 1, stage_index))
                return std::move(parser.result);
            auto& destination = parser.result.state.rng_states[index];
            destination.stage_index = static_cast<std::uint8_t>(stage_index);
            std::string stage_type;
            if (!parser.string(value, "stage_type", base + ".stage_type",
                               stage_type))
                return std::move(parser.result);
            if (stage_type == "quantization")
                destination.stage_type =
                    SampleHeritageRuntimeRngStageType::Quantization;
            else if (stage_type == "noise")
                destination.stage_type = SampleHeritageRuntimeRngStageType::Noise;
            else {
                parser.fail(SampleHeritageJsonStatus::InvalidEnum,
                            base + ".stage_type");
                return std::move(parser.result);
            }
            if (!parser.random_state(value, base + ".random_state",
                                     destination.random_state))
                return std::move(parser.result);
        }
        parser.result.runtime_status =
            validate_runtime_state_shape(parser.result.state);
        if (parser.result.runtime_status !=
            SampleHeritageRuntimeStateStatus::Ok) {
            parser.fail(SampleHeritageJsonStatus::ProfileValidationFailed,
                        "$.rng_states");
            return std::move(parser.result);
        }
        parser.result.status = SampleHeritageJsonStatus::Ok;
        parser.result.field_path.clear();
        return std::move(parser.result);
    } catch (const choc::value::Error& error) {
        parser.result.status =
            std::string_view(error.what()) ==
                    "This object already contains a member with the given name"
                ? SampleHeritageJsonStatus::DuplicateField
                : SampleHeritageJsonStatus::InvalidJson;
        parser.result.field_path = "$";
        return std::move(parser.result);
    } catch (const choc::json::ParseError&) {
        parser.result.status = SampleHeritageJsonStatus::InvalidJson;
        parser.result.field_path = "$";
        return std::move(parser.result);
    }
}

SampleHeritageRuntimeStateJsonWriteResult
write_sample_heritage_runtime_state_json(const SampleHeritageRuntimeState& state) {
    SampleHeritageRuntimeStateJsonWriteResult result;
    result.runtime_status = validate_runtime_state_shape(state);
    if (result.runtime_status != SampleHeritageRuntimeStateStatus::Ok)
        return result;

    result.json = "{\"schema_version\":" +
                  std::to_string(kSampleHeritageRuntimeStateSchemaVersion) +
                  ",\"profile_schema_version\":" +
                  std::to_string(kSampleHeritageProfileSchemaVersion) +
                  ",\"profile_id\":\"";
    result.json += state.bound_profile_id();
    result.json += "\",\"profile_digest_version\":" +
                   std::to_string(kSampleHeritageProfileDigestVersion) +
                   ",\"profile_digest\":";
    append_digest_hex(result.json, state.profile_digest);
    result.json += ",\"rng_states\":[";
    for (std::size_t index = 0; index < state.rng_state_count; ++index) {
        if (index != 0) result.json.push_back(',');
        const auto& rng = state.rng_states[index];
        result.json += "{\"stage_index\":" + std::to_string(rng.stage_index);
        result.json += ",\"stage_type\":\"";
        result.json +=
            rng.stage_type == SampleHeritageRuntimeRngStageType::Quantization
                ? "quantization"
                : "noise";
        result.json += "\",\"random_state\":";
        append_seed(result.json, rng.random_state);
        result.json.push_back('}');
    }
    result.json += "]}";
    result.status = SampleHeritageJsonStatus::Ok;
    return result;
}

}  // namespace pulp::audio
