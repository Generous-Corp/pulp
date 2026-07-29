#include <pulp/host/forge_catalog_export.hpp>

#include <pulp/host/forge_catalog_index.hpp>

#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace pulp::host {
namespace {

const std::vector<std::string_view>& expected_node_keys() {
    static const std::vector<std::string_view> keys{
        "fuzz",
        "saturator",
        "analog_vcf",
        "character_delay",
        "distortion",
        "drum_kick_oscillator",
        "drum_kick_resonant",
        "drum_kick_circuit",
        "drum_snare",
        "drum_hat",
        "drum_clap",
        "drum_tom_generic",
        "drum_tom_simmons",
        "drum_cymbal",
        "drum_membrane",
        "drum_string",
        "drum_zap",
        "drum_fm2",
        "drum_fm6",
        "drum_fm8",
        "feedforward_compressor",
        "vca_compressor",
        "fet_compressor",
        "diode_bridge_compressor",
        "frequency_shifter",
        "chorus",
        "phaser_stages",
        "delay_vibrato",
        "phase_vibrato",
        "univibe",
        "flanger",
        "leslie",
        "scanner_vibrato",
        "fdn_reverb",
        "lofi_delay",
        "lofi_filter",
        "lofi_waveshaper",
        "lofi_drywet",
        "lofi_noise",
        "lofi_bitcrush",
        "lofi_trim",
        "lofi_ping_pong",
        "lofi_reverb",
        "lofi_compressor",
        "lofi_gate",
        "lofi_lfo",
        "lofi_vca",
        "lofi_env_follower",
        "lofi_filter_cv",
        "lofi_delay_cv",
        "lofi_auto_pan",
        "lofi_width",
        "lofi_phaser",
        "mod_lfo",
        "mod_lpg",
        "mod_slew",
        "mod_transient",
        "mod_env",
        "whammy",
        "harmony_engine",
        "stage_seq",
        "cartesian_walk",
        "rungler",
        "quantize_scale",
        "gate_logic",
        "prob_gate",
        "convolution_reverb",
        "nonlin_ambience",
        "speaker_cabinet",
        "additive_bank",
        "vocoder",
        "cyclic_stretch",
        "granular_live",
        "tape_machine",
    };
    return keys;
}

ForgeCatalogExportRealization realization(std::string_view mode, CustomNodeType node) {
    return {mode, std::move(node.type_id), std::move(node.baked_params)};
}

void append(std::vector<ForgeAuditFinding>& to, std::vector<ForgeAuditFinding> from) {
    to.insert(to.end(), std::make_move_iterator(from.begin()), std::make_move_iterator(from.end()));
}

} // namespace

std::vector<ForgeCatalogExportNode> forge_catalog_export_nodes() {
    std::vector<ForgeCatalogExportNode> nodes;
    const auto add = [&nodes](ForgeNodeDescriptor descriptor,
                              std::vector<ForgeCatalogExportRealization> realizations) {
        nodes.push_back({std::move(descriptor), std::move(realizations)});
    };

    add(fuzz::descriptor(),
        {
            realization("silicon_x1", fuzz::make_fuzz_node(fuzz::Device::silicon, false)),
            realization("silicon_x4", fuzz::make_fuzz_node(fuzz::Device::silicon, true)),
            realization("germanium_x1", fuzz::make_fuzz_node(fuzz::Device::germanium, false)),
            realization("germanium_x4", fuzz::make_fuzz_node(fuzz::Device::germanium, true)),
        });
    add(saturator::saturator_descriptor(),
        {
            realization("tanh_adaa",
                        saturator::make_saturator_node(saturator::Shape::tanh_soft,
                                                       saturator::AliasPolicy::adaa)),
            realization("tanh_x2",
                        saturator::make_saturator_node(saturator::Shape::tanh_soft,
                                                       saturator::AliasPolicy::oversample_2x)),
            realization("tanh_off",
                        saturator::make_saturator_node(saturator::Shape::tanh_soft,
                                                       saturator::AliasPolicy::off)),
            realization("atan_adaa",
                        saturator::make_saturator_node(saturator::Shape::atan_soft,
                                                       saturator::AliasPolicy::adaa)),
            realization("atan_x2",
                        saturator::make_saturator_node(saturator::Shape::atan_soft,
                                                       saturator::AliasPolicy::oversample_2x)),
            realization("atan_off",
                        saturator::make_saturator_node(saturator::Shape::atan_soft,
                                                       saturator::AliasPolicy::off)),
            realization("cubic_adaa",
                        saturator::make_saturator_node(saturator::Shape::cubic_soft,
                                                       saturator::AliasPolicy::adaa)),
            realization("cubic_x2",
                        saturator::make_saturator_node(saturator::Shape::cubic_soft,
                                                       saturator::AliasPolicy::oversample_2x)),
            realization("cubic_off",
                        saturator::make_saturator_node(saturator::Shape::cubic_soft,
                                                       saturator::AliasPolicy::off)),
            realization("sinh_arc_adaa",
                        saturator::make_saturator_node(saturator::Shape::sinh_arc,
                                                       saturator::AliasPolicy::adaa)),
            realization("sinh_arc_x2",
                        saturator::make_saturator_node(saturator::Shape::sinh_arc,
                                                       saturator::AliasPolicy::oversample_2x)),
            realization("sinh_arc_off",
                        saturator::make_saturator_node(saturator::Shape::sinh_arc,
                                                       saturator::AliasPolicy::off)),
        });

    add(forge_lofi::analog_vcf_descriptor(),
        {
            realization("juno",
                        forge_lofi::make_analog_vcf_node(signal::AnalogVcf::Voicing::juno)),
            realization("jupiter",
                        forge_lofi::make_analog_vcf_node(signal::AnalogVcf::Voicing::jupiter)),
            realization("prophet5",
                        forge_lofi::make_analog_vcf_node(signal::AnalogVcf::Voicing::prophet5)),
            realization("minimoog",
                        forge_lofi::make_analog_vcf_node(signal::AnalogVcf::Voicing::minimoog)),
        });

    add(character_delay::character_delay_descriptor(),
        {
            realization("clean",
                        character_delay::make_character_delay_node(
                            character_delay::Character::clean)),
            realization("vintage",
                        character_delay::make_character_delay_node(
                            character_delay::Character::vintage_digital)),
            realization("tape",
                        character_delay::make_character_delay_node(
                            character_delay::Character::tape,
                            character_delay::TapeTier::standard)),
            realization("tape_physical",
                        character_delay::make_character_delay_node(
                            character_delay::Character::tape,
                            character_delay::TapeTier::physical)),
            realization("bbd",
                        character_delay::make_character_delay_node(
                            character_delay::Character::bbd)),
            realization("diffusion",
                        character_delay::make_character_delay_node(
                            character_delay::Character::diffusion)),
        });

    add(distortion::distortion_descriptor(),
        {
            realization("to_ground_x1",
                        distortion::make_distortion_node(
                            distortion::Topology::to_ground,
                            distortion::OversampleTier::x1)),
            realization("to_ground_x2",
                        distortion::make_distortion_node(
                            distortion::Topology::to_ground,
                            distortion::OversampleTier::x2)),
            realization("to_ground_x4",
                        distortion::make_distortion_node(
                            distortion::Topology::to_ground,
                            distortion::OversampleTier::x4)),
            realization("to_ground_x8",
                        distortion::make_distortion_node(
                            distortion::Topology::to_ground,
                            distortion::OversampleTier::x8)),
            realization("in_loop_x1",
                        distortion::make_distortion_node(
                            distortion::Topology::in_loop,
                            distortion::OversampleTier::x1)),
            realization("in_loop_x2",
                        distortion::make_distortion_node(
                            distortion::Topology::in_loop,
                            distortion::OversampleTier::x2)),
            realization("in_loop_x4",
                        distortion::make_distortion_node(
                            distortion::Topology::in_loop,
                            distortion::OversampleTier::x4)),
            realization("in_loop_x8",
                        distortion::make_distortion_node(
                            distortion::Topology::in_loop,
                            distortion::OversampleTier::x8)),
        });

    const auto add_drum = [&add](forge_drum::EngineId engine) {
        add(forge_drum::drum_descriptor(engine),
            {realization("default", forge_drum::make_drum_node(engine))});
    };
    add_drum(forge_drum::EngineId::kick_oscillator);
    add_drum(forge_drum::EngineId::kick_resonant);
    add_drum(forge_drum::EngineId::kick_circuit);
    add_drum(forge_drum::EngineId::snare);
    add_drum(forge_drum::EngineId::hat);
    add_drum(forge_drum::EngineId::clap);
    add_drum(forge_drum::EngineId::tom_generic);
    add_drum(forge_drum::EngineId::tom_simmons);
    add_drum(forge_drum::EngineId::cymbal_comb);
    add_drum(forge_drum::EngineId::membrane_modal);
    add_drum(forge_drum::EngineId::string_karplus_strong);
    add_drum(forge_drum::EngineId::zap_cz);
    add_drum(forge_drum::EngineId::fm2);
    add_drum(forge_drum::EngineId::fm6);
    add_drum(forge_drum::EngineId::fm8);

    add(dynamics::feedforward_compressor_descriptor(),
        {realization("zero_lookahead", dynamics::make_feedforward_compressor_node())});
    add(dynamics::vca::vca_compressor_descriptor(),
        {realization("default", dynamics::vca::make_vca_compressor_node())});
    add(dynamics::fet::fet_compressor_descriptor(),
        {realization("default", dynamics::fet::make_fet_compressor_node())});
    add(dynamics::diode::diode_bridge_compressor_descriptor(),
        {
            realization("feedback",
                        dynamics::diode::make_diode_bridge_compressor_node(true)),
            realization("feedforward",
                        dynamics::diode::make_diode_bridge_compressor_node(false)),
        });

    add(modulation::frequency_shifter_descriptor(),
        {realization("default", modulation::make_frequency_shifter_node())});
    add(modulation::chorus::chorus_descriptor(),
        {
            realization("ce2_clean",
                        modulation::chorus::make_chorus_node(
                            modulation::chorus::Voicing::ce2,
                            modulation::chorus::JunoMode::mode_I, false)),
            realization("ce2_bbd",
                        modulation::chorus::make_chorus_node(
                            modulation::chorus::Voicing::ce2,
                            modulation::chorus::JunoMode::mode_I, true)),
            realization("juno_i_clean",
                        modulation::chorus::make_chorus_node(
                            modulation::chorus::Voicing::juno_ensemble,
                            modulation::chorus::JunoMode::mode_I, false)),
            realization("juno_i_bbd",
                        modulation::chorus::make_chorus_node(
                            modulation::chorus::Voicing::juno_ensemble,
                            modulation::chorus::JunoMode::mode_I, true)),
            realization("juno_ii_clean",
                        modulation::chorus::make_chorus_node(
                            modulation::chorus::Voicing::juno_ensemble,
                            modulation::chorus::JunoMode::mode_II, false)),
            realization("juno_ii_bbd",
                        modulation::chorus::make_chorus_node(
                            modulation::chorus::Voicing::juno_ensemble,
                            modulation::chorus::JunoMode::mode_II, true)),
            realization("juno_i_ii_clean",
                        modulation::chorus::make_chorus_node(
                            modulation::chorus::Voicing::juno_ensemble,
                            modulation::chorus::JunoMode::mode_I_plus_II, false)),
            realization("juno_i_ii_bbd",
                        modulation::chorus::make_chorus_node(
                            modulation::chorus::Voicing::juno_ensemble,
                            modulation::chorus::JunoMode::mode_I_plus_II, true)),
            realization("dimension_d_clean",
                        modulation::chorus::make_chorus_node(
                            modulation::chorus::Voicing::dimension_d,
                            modulation::chorus::JunoMode::mode_I, false)),
            realization("dimension_d_bbd",
                        modulation::chorus::make_chorus_node(
                            modulation::chorus::Voicing::dimension_d,
                            modulation::chorus::JunoMode::mode_I, true)),
            realization("tri_chorus_clean",
                        modulation::chorus::make_chorus_node(
                            modulation::chorus::Voicing::tri_chorus,
                            modulation::chorus::JunoMode::mode_I, false)),
            realization("tri_chorus_bbd",
                        modulation::chorus::make_chorus_node(
                            modulation::chorus::Voicing::tri_chorus,
                            modulation::chorus::JunoMode::mode_I, true)),
        });
    add(modulation::phaser::phaser_descriptor(),
        {
            realization("four", modulation::phaser::make_phaser_node(4)),
            realization("six", modulation::phaser::make_phaser_node(6)),
            realization("eight", modulation::phaser::make_phaser_node(8)),
            realization("ten", modulation::phaser::make_phaser_node(10)),
            realization("twelve", modulation::phaser::make_phaser_node(12)),
        });
    add(modulation::vibrato::delay_line::delay_vibrato_descriptor(),
        {realization("default",
                     modulation::vibrato::delay_line::make_delay_vibrato_node())});
    add(modulation::vibrato::phase::phase_vibrato_descriptor(),
        {
            realization("one", modulation::vibrato::phase::make_phase_vibrato_node(1)),
            realization("two", modulation::vibrato::phase::make_phase_vibrato_node(2)),
            realization("three", modulation::vibrato::phase::make_phase_vibrato_node(3)),
            realization("four", modulation::vibrato::phase::make_phase_vibrato_node(4)),
        });
    add(modulation::vibrato::univibe::univibe_descriptor(),
        {realization("default", modulation::vibrato::univibe::make_univibe_node())});
    add(modulation::flanger::flanger_descriptor(),
        {
            realization("classic",
                        modulation::flanger::make_flanger_node(
                            modulation::flanger::Mode::classic)),
            realization("through_zero",
                        modulation::flanger::make_flanger_node(
                            modulation::flanger::Mode::through_zero)),
            realization("barberpole",
                        modulation::flanger::make_flanger_node(
                            modulation::flanger::Mode::barberpole)),
        });
    add(modulation::leslie::leslie_descriptor(),
        {realization("default", modulation::leslie::make_leslie_node())});
    add(modulation::leslie::scanner_vibrato_descriptor(),
        {realization("default", modulation::leslie::make_scanner_vibrato_node())});

    add(forge_fdn::fdn_reverb_descriptor(),
        {
            realization("room", forge_fdn::make_fdn_reverb_node(forge_fdn::fdn::Mode::room)),
            realization("hall", forge_fdn::make_fdn_reverb_node(forge_fdn::fdn::Mode::hall)),
            realization("galaxy",
                        forge_fdn::make_fdn_reverb_node(forge_fdn::fdn::Mode::galaxy)),
            realization("shimmer",
                        forge_fdn::make_fdn_reverb_node(forge_fdn::fdn::Mode::shimmer)),
            realization("lofi", forge_fdn::make_fdn_reverb_node(forge_fdn::fdn::Mode::lofi)),
        });

    add(forge_lofi::delay_descriptor(),
        {realization("default", forge_lofi::make_delay_node())});
    add(forge_lofi::filter_descriptor(),
        {
            realization("lowpass",
                        forge_lofi::make_filter_node(signal::Svf::Mode::lowpass)),
            realization("highpass",
                        forge_lofi::make_filter_node(signal::Svf::Mode::highpass)),
            realization("bandpass",
                        forge_lofi::make_filter_node(signal::Svf::Mode::bandpass)),
            realization("notch",
                        forge_lofi::make_filter_node(signal::Svf::Mode::notch)),
        });
    add(forge_lofi::waveshaper_descriptor(),
        {realization("default", forge_lofi::make_waveshaper_node())});
    add(forge_lofi::drywet_descriptor(),
        {realization("default", forge_lofi::make_drywet_node())});
    add(forge_lofi::noise_descriptor(),
        {realization("default", forge_lofi::make_noise_node())});
    add(forge_lofi::bitcrush_descriptor(),
        {realization("default", forge_lofi::make_bitcrush_node())});
    add(forge_lofi::trim_descriptor(),
        {realization("default", forge_lofi::make_trim_node())});
    add(forge_lofi::ping_pong_descriptor(),
        {realization("default", forge_lofi::make_ping_pong_node())});
    add(forge_lofi::reverb_descriptor(),
        {realization("default", forge_lofi::make_reverb_node())});
    add(forge_lofi::compressor_descriptor(),
        {realization("default", forge_lofi::make_compressor_node())});
    add(forge_lofi::gate_descriptor(),
        {realization("default", forge_lofi::make_gate_node())});
    add(forge_lofi::lfo_descriptor(),
        {realization("default", forge_lofi::make_lfo_node())});
    add(forge_lofi::vca_descriptor(),
        {realization("default", forge_lofi::make_vca_node())});
    add(forge_lofi::env_follower_descriptor(),
        {realization("default", forge_lofi::make_env_follower_node())});
    add(forge_lofi::filter_cv_descriptor(),
        {realization("default", forge_lofi::make_filter_cv_node())});
    add(forge_lofi::delay_cv_descriptor(),
        {realization("default", forge_lofi::make_delay_cv_node())});
    add(forge_lofi::auto_pan_descriptor(),
        {realization("default", forge_lofi::make_auto_pan_node())});
    add(forge_lofi::width_descriptor(),
        {realization("default", forge_lofi::make_width_node())});
    add(forge_lofi::phaser_descriptor(),
        {realization("default", forge_lofi::make_phaser_node())});

    add(forge_modulation::mod_lfo_descriptor(),
        {realization("default", forge_modulation::make_mod_lfo_node())});
    add(forge_modulation::mod_lpg_descriptor(),
        {realization("default", forge_modulation::make_mod_lpg_node())});
    add(forge_modulation::mod_slew_descriptor(),
        {realization("default", forge_modulation::make_mod_slew_node())});
    add(forge_modulation::mod_transient_descriptor(),
        {realization("default", forge_modulation::make_mod_transient_node())});
    add(forge_modulation::mod_env_descriptor(),
        {realization("default", forge_modulation::make_mod_env_node())});

    add(pitch::whammy::descriptor(),
        {realization("default", pitch::whammy::make_whammy_node())});
    add(pitch::harmony::descriptor(),
        {realization("default", pitch::harmony::make_harmony_engine_node())});

    add(sequencing::stage_seq::descriptor(),
        {realization("default", sequencing::stage_seq::make_stage_seq_node())});
    add(sequencing::cartesian::descriptor(),
        {
            realization("cartesian",
                        sequencing::cartesian::make_cartesian_walk_node(
                            sequencing::cartesian::default_grid(), false)),
            realization("row_major",
                        sequencing::cartesian::make_cartesian_walk_node(
                            sequencing::cartesian::default_grid(), true)),
        });
    add(sequencing::rungler::descriptor(),
        {realization("default", sequencing::rungler::make_rungler_node())});
    add(sequencing::quantize::descriptor(),
        {realization("default", sequencing::quantize::make_quantize_scale_node())});
    add(sequencing::gate_logic::descriptor(),
        {realization("default", sequencing::gate_logic::make_gate_logic_node())});
    add(sequencing::prob_gate::descriptor(),
        {realization("default", sequencing::prob_gate::make_prob_gate_node())});

    add(space::convolution::descriptor(),
        {realization("default", space::convolution::make_catalog_probe_node())});
    add(space::nonlin_ambience::descriptor(),
        {realization("default", space::nonlin_ambience::make_nonlin_ambience_node())});
    add(space::cabinet::descriptor(),
        {realization("default", space::cabinet::make_speaker_cabinet_node())});

    add(synthesis::additive::descriptor(),
        {
            realization("organ",
                        synthesis::additive::make_additive_bank_node(
                            synthesis::additive::Voice::organ)),
            realization("bell",
                        synthesis::additive::make_additive_bank_node(
                            synthesis::additive::Voice::bell)),
        });
    add(synthesis::vocoder::descriptor(),
        {realization("default", synthesis::vocoder::make_vocoder_node())});
    add(synthesis::cyclic::descriptor(),
        {
            realization("short",
                        synthesis::cyclic::make_cyclic_stretch_node(
                            synthesis::cyclic::Regime::short_frame)),
            realization("long",
                        synthesis::cyclic::make_cyclic_stretch_node(
                            synthesis::cyclic::Regime::long_frame)),
        });
    add(synthesis::granular::descriptor(),
        {realization("default", synthesis::granular::make_granular_node())});

    add(tape::descriptor(),
        {
            realization("ampex",
                        tape::make_tape_machine_node(tape::Archetype::ampex_350_440)),
            realization("studer", tape::make_tape_machine_node(tape::Archetype::studer_a800)),
            realization("cassette",
                        tape::make_tape_machine_node(tape::Archetype::cassette_deck)),
        });

    return nodes;
}

std::vector<ForgeAuditFinding>
audit_forge_catalog_export(const std::vector<ForgeCatalogExportNode>& nodes) {
    std::vector<ForgeNodeDescriptor> descriptors;
    descriptors.reserve(nodes.size());
    std::vector<ForgeAuditFinding> findings;
    std::unordered_map<std::string, int> type_ids;

    for (const auto& node : nodes) {
        descriptors.push_back(node.descriptor);
        std::unordered_map<std::string_view, const ForgeRealization*> declared;
        for (const auto& value : node.descriptor.realizations)
            declared.emplace(value.mode, &value);

        std::unordered_set<std::string_view> constructed_modes;
        for (const auto& built : node.realizations) {
            constructed_modes.insert(built.mode);
            const auto found = declared.find(built.mode);
            if (found == declared.end()) {
                findings.push_back(
                    {ForgeAuditFault::unexpected_realization, std::string(node.descriptor.key),
                     std::string(built.mode),
                     "constructed realization is absent from the semantic descriptor"});
                continue;
            }
            if (built.type_id != found->second->type_id)
                findings.push_back({ForgeAuditFault::mismatched_type_id,
                                    std::string(node.descriptor.key), std::string(built.mode),
                                    "descriptor declares '" + std::string(found->second->type_id) +
                                        "' but the factory constructed '" + built.type_id + "'"});
            ++type_ids[built.type_id];

            append(findings,
                   audit_forge_descriptor(node.descriptor, built.baked_params, built.mode));
        }

        for (const auto& value : node.descriptor.realizations)
            if (!constructed_modes.contains(value.mode))
                findings.push_back({ForgeAuditFault::missing_realization,
                                    std::string(node.descriptor.key), std::string(value.mode),
                                    "declared realization was not constructed for export"});
    }

    for (const auto& [type_id, count] : type_ids)
        if (count > 1)
            findings.push_back({ForgeAuditFault::duplicate_type_id, "<catalog>", type_id,
                                "concrete type id appears " + std::to_string(count) + " times"});

    append(findings, audit_forge_catalog_membership(descriptors, expected_node_keys()));
    return findings;
}

} // namespace pulp::host
