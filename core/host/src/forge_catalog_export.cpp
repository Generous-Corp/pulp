#include "forge_catalog_export_detail.hpp"

#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace pulp::host {
namespace {

const std::vector<std::string_view>& expected_node_keys() {
    static const std::vector<std::string_view> keys{
        "sample_region_input_boundary",
        "sample_region_output_boundary",
        "sample_region_constant",
        "sample_region_parameter",
        "sample_region_add",
        "sample_region_multiply",
        "sample_region_unit_delay",
        "fuzz",
        "saturator",
        "analog_vcf",
        "character_delay",
        "distortion",
        "eurorack_attenuverter",
        "eurorack_slew",
        "eurorack_clock_divider",
        "eurorack_sample_hold",
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
        "true_peak_limiter",
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
        "multiband_compressor",
        "whammy",
        "harmony_engine",
        "stage_seq",
        "cartesian_walk",
        "rungler",
        "quantize_scale",
        "gate_logic",
        "prob_gate",
        "sidechain_compressor",
        "convolution_reverb",
        "nonlin_ambience",
        "speaker_cabinet",
        "additive_bank",
        "vocoder",
        "cyclic_stretch",
        "granular_live",
        "tape_machine",
        "wavetable_oscillator",
    };
    return keys;
}

void append(std::vector<ForgeAuditFinding>& to, std::vector<ForgeAuditFinding> from) {
    to.insert(to.end(), std::make_move_iterator(from.begin()), std::make_move_iterator(from.end()));
}

} // namespace

std::vector<ForgeCatalogExportNode> forge_catalog_export_nodes() {
    std::vector<ForgeCatalogExportNode> nodes;
    // Family order is the export order; each family compiles in its own
    // translation unit (see forge_catalog_export_detail.hpp).
    forge_catalog_export_detail::append_effects(nodes);
    forge_catalog_export_detail::append_eurorack_drums(nodes);
    forge_catalog_export_detail::append_dynamics(nodes);
    forge_catalog_export_detail::append_modulation(nodes);
    forge_catalog_export_detail::append_lofi(nodes);
    forge_catalog_export_detail::append_pitch_sequencing(nodes);
    forge_catalog_export_detail::append_space_synthesis(nodes);

    forge_catalog_export_detail::append_sample_region(nodes);

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
