#include <pulp/host/forge_catalog_export.hpp>

#include <pulp/host/forge_catalog_index.hpp>
#include <pulp/host/forge_descriptor_audit.hpp>

#include <algorithm>
#include <iomanip>
#include <limits>
#include <locale>
#include <sstream>
#include <unordered_set>

namespace pulp::host {
namespace {

template <typename Range> void append(std::vector<ForgeNodeDescriptor>& out, Range&& range) {
    out.insert(out.end(), range.begin(), range.end());
}

void add_node(std::vector<CustomNodeType>& out, CustomNodeType node) {
    out.push_back(std::move(node));
}

void merge_baked(std::vector<CustomNodeBakedParam>& into,
                 const std::vector<CustomNodeBakedParam>& from) {
    for (const auto& candidate : from) {
        const auto found = std::find_if(
            into.begin(), into.end(), [&](const auto& param) { return param.id == candidate.id; });
        if (found == into.end()) {
            into.push_back(candidate);
        } else {
            found->min_value = std::min(found->min_value, candidate.min_value);
            found->max_value = std::max(found->max_value, candidate.max_value);
        }
    }
}

std::string escaped(std::string_view value) {
    std::string out;
    out.reserve(value.size() + 8);
    for (const unsigned char c : value) {
        switch (c) {
        case '"':
            out += "\\\"";
            break;
        case '\\':
            out += "\\\\";
            break;
        case '\b':
            out += "\\b";
            break;
        case '\f':
            out += "\\f";
            break;
        case '\n':
            out += "\\n";
            break;
        case '\r':
            out += "\\r";
            break;
        case '\t':
            out += "\\t";
            break;
        default:
            if (c < 0x20) {
                constexpr char hex[] = "0123456789abcdef";
                out += "\\u00";
                out += hex[c >> 4];
                out += hex[c & 0x0f];
            } else {
                out += static_cast<char>(c);
            }
        }
    }
    return out;
}

void quote(std::ostringstream& out, std::string_view value) {
    out << '"' << escaped(value) << '"';
}

const CustomNodeBakedParam* baked_param(const std::vector<CustomNodeBakedParam>& baked,
                                        state::ParamID id) {
    const auto found =
        std::find_if(baked.begin(), baked.end(), [&](const auto& param) { return param.id == id; });
    return found == baked.end() ? nullptr : &*found;
}

void write_choice(std::ostringstream& out, const ForgeParamChoice& choice) {
    out << "{\"token\":";
    quote(out, choice.token);
    out << ",\"label\":";
    quote(out, choice.label);
    out << ",\"value\":" << choice.value << ",\"realization_modes\":[";
    for (std::size_t mode_index = 0; mode_index < choice.realization_modes.size(); ++mode_index) {
        if (mode_index != 0)
            out << ',';
        quote(out, choice.realization_modes[mode_index]);
    }
    out << "]}";
}

std::vector<CustomNodeType> all_realization_nodes() {
    std::vector<CustomNodeType> out;

    for (auto voicing :
         {pulp::signal::AnalogVcf::Voicing::juno, pulp::signal::AnalogVcf::Voicing::jupiter,
          pulp::signal::AnalogVcf::Voicing::prophet5, pulp::signal::AnalogVcf::Voicing::minimoog})
        add_node(out, forge_lofi::make_analog_vcf_node(voicing));

    using DelayCharacter = character_delay::Character;
    add_node(out, character_delay::make_character_delay_node(DelayCharacter::clean));
    add_node(out, character_delay::make_character_delay_node(DelayCharacter::vintage_digital));
    add_node(out, character_delay::make_character_delay_node(DelayCharacter::tape));
    add_node(out, character_delay::make_character_delay_node(DelayCharacter::tape,
                                                             character_delay::TapeTier::physical));
    add_node(out, character_delay::make_character_delay_node(DelayCharacter::bbd));
    add_node(out, character_delay::make_character_delay_node(DelayCharacter::diffusion));

    for (auto topology : {distortion::Topology::to_ground, distortion::Topology::in_loop})
        for (auto tier : {distortion::OversampleTier::x1, distortion::OversampleTier::x2,
                          distortion::OversampleTier::x4, distortion::OversampleTier::x8})
            add_node(out, distortion::make_distortion_node(topology, tier));

    for (const auto& engine : pulp::signal::drum::engine_registry)
        if (engine.available)
            add_node(out, forge_drum::make_drum_node(engine.id));

    for (float lookahead : {0.0f, 3.0f, 10.0f})
        add_node(out, dynamics::make_feedforward_compressor_node(lookahead));
    add_node(out, dynamics::vca::make_vca_compressor_node());
    add_node(out, dynamics::vca::make_vca_compressor_node(3.0f, 4.0));
    add_node(out, dynamics::vca::make_vca_compressor_node(10.0f, 4.0));
    add_node(out, dynamics::vca::make_vca_compressor_node(0.0f, 2.0));
    add_node(out, dynamics::vca::make_vca_compressor_node(0.0f, 8.0));
    add_node(out, dynamics::fet::make_fet_compressor_node());
    add_node(out, dynamics::diode::make_diode_bridge_compressor_node(true));
    add_node(out, dynamics::diode::make_diode_bridge_compressor_node(false));

    add_node(out, modulation::make_frequency_shifter_node());
    using ChorusVoicing = modulation::chorus::Voicing;
    using JunoMode = modulation::chorus::JunoMode;
    for (bool bbd : {false, true}) {
        add_node(out,
                 modulation::chorus::make_chorus_node(ChorusVoicing::ce2, JunoMode::mode_I, bbd));
        add_node(out, modulation::chorus::make_chorus_node(ChorusVoicing::juno_ensemble,
                                                           JunoMode::mode_I, bbd));
        add_node(out, modulation::chorus::make_chorus_node(ChorusVoicing::juno_ensemble,
                                                           JunoMode::mode_II, bbd));
        add_node(out, modulation::chorus::make_chorus_node(ChorusVoicing::juno_ensemble,
                                                           JunoMode::mode_I_plus_II, bbd));
        add_node(out, modulation::chorus::make_chorus_node(ChorusVoicing::dimension_d,
                                                           JunoMode::mode_I, bbd));
        add_node(out, modulation::chorus::make_chorus_node(ChorusVoicing::tri_chorus,
                                                           JunoMode::mode_I, bbd));
    }
    for (int stages : {4, 6, 8, 10, 12})
        add_node(out, modulation::phaser::make_phaser_node(stages));
    for (float rate : {4.0f, 8.0f})
        add_node(out, modulation::vibrato::delay_line::make_delay_vibrato_node(rate));
    for (int stages : {1, 2, 3, 4})
        add_node(out, modulation::vibrato::phase::make_phase_vibrato_node(stages));
    add_node(out, modulation::vibrato::univibe::make_univibe_node());
    add_node(out, modulation::flanger::make_flanger_node(modulation::flanger::Mode::classic));
    add_node(out, modulation::flanger::make_flanger_node(modulation::flanger::Mode::barberpole));
    for (double offset : {4.0, 1.0, 2.0, 8.0})
        add_node(out, modulation::flanger::make_flanger_node(
                          modulation::flanger::Mode::through_zero, offset));
    add_node(out, modulation::leslie::make_leslie_node());
    add_node(out, modulation::leslie::make_scanner_vibrato_node());

    for (auto mode :
         {forge_fdn::fdn::Mode::room, forge_fdn::fdn::Mode::hall, forge_fdn::fdn::Mode::galaxy,
          forge_fdn::fdn::Mode::shimmer, forge_fdn::fdn::Mode::lofi})
        add_node(out, forge_fdn::make_fdn_reverb_node(mode));

    for (auto device : {fuzz::Device::silicon, fuzz::Device::germanium})
        for (bool oversampled : {false, true})
            add_node(out, fuzz::make_fuzz_node(device, oversampled));

    add_node(out, forge_lofi::make_delay_node());
    for (auto mode : {pulp::signal::Svf::Mode::lowpass, pulp::signal::Svf::Mode::highpass,
                      pulp::signal::Svf::Mode::bandpass, pulp::signal::Svf::Mode::notch})
        add_node(out, forge_lofi::make_filter_node(mode));
    add_node(out, forge_lofi::make_waveshaper_node());
    add_node(out, forge_lofi::make_drywet_node());
    add_node(out, forge_lofi::make_noise_node());
    add_node(out, forge_lofi::make_bitcrush_node());
    add_node(out, forge_lofi::make_trim_node());
    add_node(out, forge_lofi::make_ping_pong_node());
    add_node(out, forge_lofi::make_reverb_node());
    add_node(out, forge_lofi::make_compressor_node());
    add_node(out, forge_lofi::make_gate_node());
    add_node(out, forge_lofi::make_lfo_node());
    add_node(out, forge_lofi::make_vca_node());
    add_node(out, forge_lofi::make_env_follower_node());
    add_node(out, forge_lofi::make_filter_cv_node());
    add_node(out, forge_lofi::make_delay_cv_node());
    add_node(out, forge_lofi::make_auto_pan_node());
    add_node(out, forge_lofi::make_width_node());
    add_node(out, forge_lofi::make_phaser_node());

    add_node(out, forge_modulation::make_mod_lfo_node());
    add_node(out, forge_modulation::make_mod_lpg_node());
    add_node(out, forge_modulation::make_mod_slew_node());
    add_node(out, forge_modulation::make_mod_transient_node());
    add_node(out, forge_modulation::make_mod_env_node());
    add_node(out, pitch::whammy::make_whammy_node());
    add_node(out, pitch::harmony::make_harmony_engine_node());
    for (auto shape : {saturator::Shape::tanh_soft, saturator::Shape::atan_soft,
                       saturator::Shape::cubic_soft, saturator::Shape::sinh_arc})
        add_node(out, saturator::make_saturator_node(shape));

    add_node(out, sequencing::stage_seq::make_stage_seq_node());
    add_node(out, sequencing::cartesian::make_cartesian_walk_node());
    add_node(out, sequencing::cartesian::make_cartesian_walk_node(
                      sequencing::cartesian::default_grid(), true));
    add_node(out, sequencing::rungler::make_rungler_node());
    add_node(out, sequencing::quantize::make_quantize_scale_node());
    add_node(out, sequencing::gate_logic::make_gate_logic_node());
    add_node(out, sequencing::prob_gate::make_prob_gate_node());

    add_node(out, space::convolution::make_convolution_reverb_node({{{1.0f}}, 48000.0}));
    add_node(out, space::nonlin_ambience::make_nonlin_ambience_node());
    add_node(out, space::cabinet::make_speaker_cabinet_node());

    add_node(out, synthesis::additive::make_additive_bank_node(synthesis::additive::Voice::organ));
    add_node(out, synthesis::additive::make_additive_bank_node(synthesis::additive::Voice::bell));
    add_node(out, synthesis::vocoder::make_vocoder_node());
    add_node(out,
             synthesis::cyclic::make_cyclic_stretch_node(synthesis::cyclic::Regime::short_frame));
    add_node(out,
             synthesis::cyclic::make_cyclic_stretch_node(synthesis::cyclic::Regime::long_frame));
    add_node(out, synthesis::granular::make_granular_node());

    using TapeArchetype = tape::Archetype;
    for (bool pre_echo : {false, true}) {
        add_node(out, tape::make_tape_machine_node(TapeArchetype::ampex_350_440, 7.5, pre_echo));
        add_node(out, tape::make_tape_machine_node(TapeArchetype::ampex_350_440, 15.0, pre_echo));
        add_node(out, tape::make_tape_machine_node(TapeArchetype::studer_a800, 7.5, pre_echo));
        add_node(out, tape::make_tape_machine_node(TapeArchetype::studer_a800, 15.0, pre_echo));
        add_node(out, tape::make_tape_machine_node(TapeArchetype::studer_a800, 30.0, pre_echo));
        add_node(out, tape::make_tape_machine_node(TapeArchetype::cassette_deck, 1.875, pre_echo));
    }
    return out;
}

} // namespace

std::vector<ForgeNodeDescriptor> forge_catalog_descriptors() {
    std::vector<ForgeNodeDescriptor> out;
    append(out, forge_lofi::analog_vcf_descriptors());
    append(out, character_delay::character_delay_descriptors());
    append(out, distortion::distortion_descriptors());
    append(out, forge_drum::drum_descriptors());
    append(out, dynamics::dynamics_descriptors());
    append(out, modulation::effect_modulation_descriptors());
    append(out, forge_fdn::fdn_reverb_descriptors());
    out.push_back(fuzz::descriptor());
    append(out, forge_lofi::lofi_descriptors());
    append(out, forge_modulation::modulation_descriptors());
    append(out, pitch::pitch_descriptors());
    append(out, saturator::saturator_descriptors());
    append(out, sequencing::sequencing_descriptors());
    append(out, space::space_descriptors());
    append(out, synthesis::synthesis_descriptors());
    append(out, tape::tape_descriptors());
    return out;
}

std::vector<ForgeNodeRealizationBakedParams> forge_catalog_realization_baked_params() {
    const auto descriptors = forge_catalog_descriptors();
    auto nodes = all_realization_nodes();
    std::vector<ForgeNodeRealizationBakedParams> out;
    out.reserve(descriptors.size());
    for (const auto& descriptor : descriptors) {
        ForgeNodeRealizationBakedParams contracts;
        contracts.reserve(descriptor.realizations.size());
        for (const auto& realization : descriptor.realizations) {
            const auto node = std::find_if(nodes.begin(), nodes.end(), [&](const auto& candidate) {
                return candidate.type_id == realization.type_id;
            });
            contracts.push_back(
                {std::string(realization.mode),
                 node == nodes.end() ? std::vector<CustomNodeBakedParam>{} : node->baked_params});
        }
        out.push_back(std::move(contracts));
    }
    return out;
}

std::vector<std::vector<CustomNodeBakedParam>> forge_catalog_baked_params() {
    std::vector<std::vector<CustomNodeBakedParam>> out;
    for (const auto& contracts : forge_catalog_realization_baked_params()) {
        std::vector<CustomNodeBakedParam> merged;
        for (const auto& contract : contracts)
            merge_baked(merged, contract.params);
        out.push_back(std::move(merged));
    }
    return out;
}

ForgeCatalogExportResult export_forge_catalog_json() {
    return export_forge_catalog_json(forge_catalog_descriptors(),
                                     forge_catalog_realization_baked_params(),
                                     kForgeCatalogNodeCount);
}

ForgeCatalogExportResult
export_forge_catalog_json(const std::vector<ForgeNodeDescriptor>& descriptors,
                          const std::vector<ForgeNodeRealizationBakedParams>& baked_per_node,
                          std::size_t expected_node_count) {
    ForgeCatalogExportResult result;
    result.node_count = descriptors.size();
    if (descriptors.size() != expected_node_count) {
        result.error = "Forge catalog node count mismatch: expected " +
                       std::to_string(expected_node_count) + ", found " +
                       std::to_string(descriptors.size());
        return result;
    }
    if (baked_per_node.size() != descriptors.size()) {
        result.error = "Forge catalog descriptor/baked-node count mismatch";
        return result;
    }

    std::vector<std::vector<CustomNodeBakedParam>> audit_baked;
    audit_baked.reserve(baked_per_node.size());
    for (std::size_t i = 0; i < baked_per_node.size(); ++i) {
        const auto& descriptor = descriptors[i];
        const auto& contracts = baked_per_node[i];
        if (contracts.size() != descriptor.realizations.size()) {
            result.error = "Forge catalog realization contract count mismatch for " +
                           std::string(descriptor.key);
            return result;
        }
        std::vector<CustomNodeBakedParam> merged;
        for (std::size_t j = 0; j < contracts.size(); ++j) {
            if (contracts[j].mode != descriptor.realizations[j].mode ||
                contracts[j].params.empty()) {
                result.error = "Forge catalog missing built realization contract for " +
                               std::string(descriptor.key) + "." +
                               std::string(descriptor.realizations[j].mode);
                return result;
            }
            merge_baked(merged, contracts[j].params);
        }
        audit_baked.push_back(std::move(merged));
    }

    const auto findings = audit_forge_catalog(descriptors, audit_baked);
    if (!findings.empty()) {
        result.error = describe_forge_audit_finding(findings.front());
        return result;
    }

    std::unordered_set<std::string_view> type_ids;
    for (const auto& descriptor : descriptors) {
        std::size_t realization_count = 1;
        for (const auto& axis : descriptor.axes) {
            if (axis.values.empty()) {
                result.error =
                    "Forge catalog realization axis has no values: " + std::string(descriptor.key) +
                    "." + std::string(axis.key);
                return result;
            }
            realization_count *= axis.values.size();
        }
        if (realization_count != descriptor.realizations.size()) {
            result.error = "Forge catalog realization/axis product mismatch for " +
                           std::string(descriptor.key);
            return result;
        }
        for (const auto& realization : descriptor.realizations) {
            if (realization.mode.empty() || realization.type_id.empty()) {
                result.error = "Forge catalog realization has an empty mode or type_id";
                return result;
            }
            if (!type_ids.insert(realization.type_id).second) {
                result.error = "Forge catalog duplicate realization type_id: " +
                               std::string(realization.type_id);
                return result;
            }
        }
    }

    std::ostringstream out;
    out.imbue(std::locale::classic());
    out << std::setprecision(std::numeric_limits<float>::max_digits10);
    out << "{\"schema_version\":1,\"node_count\":" << descriptors.size() << ",\"nodes\":[";
    for (std::size_t i = 0; i < descriptors.size(); ++i) {
        if (i != 0)
            out << ',';
        const auto& descriptor = descriptors[i];
        const auto& contracts = baked_per_node[i];
        out << "{\"key\":";
        quote(out, descriptor.key);
        out << ",\"label\":";
        quote(out, descriptor.label);
        out << ",\"description\":";
        quote(out, descriptor.description);
        out << ",\"axes\":[";
        for (std::size_t axis_index = 0; axis_index < descriptor.axes.size(); ++axis_index) {
            if (axis_index != 0)
                out << ',';
            const auto& axis = descriptor.axes[axis_index];
            out << "{\"key\":";
            quote(out, axis.key);
            out << ",\"label\":";
            quote(out, axis.label);
            out << ",\"description\":";
            quote(out, axis.description);
            out << ",\"values\":[";
            for (std::size_t value_index = 0; value_index < axis.values.size(); ++value_index) {
                if (value_index != 0)
                    out << ',';
                write_choice(out, axis.values[value_index]);
            }
            out << "]}";
        }
        out << "],\"realizations\":[";
        for (std::size_t realization_index = 0; realization_index < descriptor.realizations.size();
             ++realization_index) {
            if (realization_index != 0)
                out << ',';
            const auto& realization = descriptor.realizations[realization_index];
            out << "{\"mode\":";
            quote(out, realization.mode);
            out << ",\"type_id\":";
            quote(out, realization.type_id);
            out << ",\"axis_values\":{";
            std::size_t ordinal = realization_index;
            std::vector<std::size_t> choice_indices(descriptor.axes.size());
            for (std::size_t axis_index = descriptor.axes.size(); axis_index-- > 0;) {
                const auto count = descriptor.axes[axis_index].values.size();
                choice_indices[axis_index] = ordinal % count;
                ordinal /= count;
            }
            for (std::size_t axis_index = 0; axis_index < descriptor.axes.size(); ++axis_index) {
                if (axis_index != 0)
                    out << ',';
                quote(out, descriptor.axes[axis_index].key);
                out << ':';
                quote(out, descriptor.axes[axis_index].values[choice_indices[axis_index]].token);
            }
            out << '}';
            out << '}';
        }
        out << "],\"params\":[";
        for (std::size_t param_index = 0; param_index < descriptor.params.size(); ++param_index) {
            if (param_index != 0)
                out << ',';
            const auto& param = descriptor.params[param_index];
            out << "{\"key\":";
            quote(out, param.key);
            out << ",\"id\":" << param.id << ",\"label\":";
            quote(out, param.label);
            out << ",\"unit\":";
            quote(out, param.unit);
            out << ",\"kind\":";
            switch (param.kind) {
            case ForgeParamKind::continuous:
                quote(out, "continuous");
                break;
            case ForgeParamKind::stepped:
                quote(out, "stepped");
                break;
            case ForgeParamKind::bitmask:
                quote(out, "bitmask");
                break;
            }
            out << ",\"curve\":";
            quote(out, param.curve == ForgeParamCurve::logarithmic ? "logarithmic" : "linear");
            out << ",\"description\":";
            quote(out, param.description);
            out << ",\"numeric_contracts\":[";
            bool wrote_numeric = false;
            for (const auto& contract : contracts) {
                if (!param_applies(param, contract.mode))
                    continue;
                const auto* numeric = baked_param(contract.params, param.id);
                if (!numeric) {
                    result.error = "Forge catalog parameter join failed for " +
                                   std::string(descriptor.key) + "." + std::string(param.key) +
                                   " in " + contract.mode;
                    return result;
                }
                if (wrote_numeric)
                    out << ',';
                wrote_numeric = true;
                out << "{\"realization_mode\":";
                quote(out, contract.mode);
                out << ",\"min\":" << numeric->min_value << ",\"max\":" << numeric->max_value
                    << ",\"default\":" << numeric->default_value << '}';
            }
            if (!wrote_numeric) {
                result.error = "Forge catalog parameter has no applicable numeric contract for " +
                               std::string(descriptor.key) + "." + std::string(param.key);
                return result;
            }
            for (const auto& choice : param.choices) {
                bool checked_choice = false;
                for (const auto& contract : contracts) {
                    if (!param_applies(param, contract.mode) ||
                        !choice_applies(choice, contract.mode))
                        continue;
                    checked_choice = true;
                    const auto* numeric = baked_param(contract.params, param.id);
                    if (!numeric || choice.value < numeric->min_value ||
                        choice.value > numeric->max_value) {
                        result.error =
                            "Forge catalog choice is outside its realization contract for " +
                            std::string(descriptor.key) + "." + std::string(param.key) + "." +
                            choice.token + " in " + contract.mode;
                        return result;
                    }
                }
                if (!checked_choice) {
                    result.error = "Forge catalog choice has no applicable numeric contract for " +
                                   std::string(descriptor.key) + "." + std::string(param.key) +
                                   "." + choice.token;
                    return result;
                }
            }
            out << "],\"choices\":[";
            for (std::size_t choice_index = 0; choice_index < param.choices.size();
                 ++choice_index) {
                if (choice_index != 0)
                    out << ',';
                write_choice(out, param.choices[choice_index]);
            }
            out << "],\"realization_modes\":[";
            for (std::size_t mode_index = 0; mode_index < param.realization_modes.size();
                 ++mode_index) {
                if (mode_index != 0)
                    out << ',';
                quote(out, param.realization_modes[mode_index]);
            }
            out << "]}";
        }
        out << "]}";
    }
    out << "]}\n";
    result.ok = true;
    result.json = out.str();
    return result;
}

} // namespace pulp::host
