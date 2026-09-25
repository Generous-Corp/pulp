#include "forge_catalog_export_detail.hpp"

#include <pulp/host/forge_effect_modulation_catalog.hpp>

namespace pulp::host::forge_catalog_export_detail {

void append_modulation(Nodes& nodes) {
    add(nodes, modulation::frequency_shifter_descriptor(),
        {realization("default", modulation::make_frequency_shifter_node())});
    add(nodes, modulation::chorus::chorus_descriptor(),
        {
            realization("ce2_clean", modulation::chorus::make_chorus_node(
                                         modulation::chorus::Voicing::ce2,
                                         modulation::chorus::JunoMode::mode_I, false)),
            realization("ce2_bbd", modulation::chorus::make_chorus_node(
                                       modulation::chorus::Voicing::ce2,
                                       modulation::chorus::JunoMode::mode_I, true)),
            realization("juno_i_clean", modulation::chorus::make_chorus_node(
                                            modulation::chorus::Voicing::juno_ensemble,
                                            modulation::chorus::JunoMode::mode_I, false)),
            realization("juno_i_bbd", modulation::chorus::make_chorus_node(
                                          modulation::chorus::Voicing::juno_ensemble,
                                          modulation::chorus::JunoMode::mode_I, true)),
            realization("juno_ii_clean", modulation::chorus::make_chorus_node(
                                             modulation::chorus::Voicing::juno_ensemble,
                                             modulation::chorus::JunoMode::mode_II, false)),
            realization("juno_ii_bbd", modulation::chorus::make_chorus_node(
                                           modulation::chorus::Voicing::juno_ensemble,
                                           modulation::chorus::JunoMode::mode_II, true)),
            realization("juno_i_ii_clean",
                        modulation::chorus::make_chorus_node(
                            modulation::chorus::Voicing::juno_ensemble,
                            modulation::chorus::JunoMode::mode_I_plus_II, false)),
            realization("juno_i_ii_bbd", modulation::chorus::make_chorus_node(
                                             modulation::chorus::Voicing::juno_ensemble,
                                             modulation::chorus::JunoMode::mode_I_plus_II, true)),
            realization("dimension_d_clean", modulation::chorus::make_chorus_node(
                                                 modulation::chorus::Voicing::dimension_d,
                                                 modulation::chorus::JunoMode::mode_I, false)),
            realization("dimension_d_bbd", modulation::chorus::make_chorus_node(
                                               modulation::chorus::Voicing::dimension_d,
                                               modulation::chorus::JunoMode::mode_I, true)),
            realization("tri_chorus_clean", modulation::chorus::make_chorus_node(
                                                modulation::chorus::Voicing::tri_chorus,
                                                modulation::chorus::JunoMode::mode_I, false)),
            realization("tri_chorus_bbd", modulation::chorus::make_chorus_node(
                                              modulation::chorus::Voicing::tri_chorus,
                                              modulation::chorus::JunoMode::mode_I, true)),
        });
    add(nodes, modulation::phaser::phaser_descriptor(),
        {
            realization("four", modulation::phaser::make_phaser_node(4)),
            realization("six", modulation::phaser::make_phaser_node(6)),
            realization("eight", modulation::phaser::make_phaser_node(8)),
            realization("ten", modulation::phaser::make_phaser_node(10)),
            realization("twelve", modulation::phaser::make_phaser_node(12)),
        });
    add(nodes, modulation::vibrato::delay_line::delay_vibrato_descriptor(),
        {realization("default", modulation::vibrato::delay_line::make_delay_vibrato_node(4.0f)),
         realization("8", modulation::vibrato::delay_line::make_delay_vibrato_node(8.0f))});
    add(nodes, modulation::vibrato::phase::phase_vibrato_descriptor(),
        {
            realization("one", modulation::vibrato::phase::make_phase_vibrato_node(1)),
            realization("two", modulation::vibrato::phase::make_phase_vibrato_node(2)),
            realization("three", modulation::vibrato::phase::make_phase_vibrato_node(3)),
            realization("four", modulation::vibrato::phase::make_phase_vibrato_node(4)),
        });
    add(nodes, modulation::vibrato::univibe::univibe_descriptor(),
        {realization("default", modulation::vibrato::univibe::make_univibe_node())});
    add(nodes, modulation::flanger::flanger_descriptor(),
        {
            realization("classic",
                        modulation::flanger::make_flanger_node(modulation::flanger::Mode::classic)),
            realization("through_zero", modulation::flanger::make_flanger_node(
                                            modulation::flanger::Mode::through_zero)),
            realization("barberpole", modulation::flanger::make_flanger_node(
                                          modulation::flanger::Mode::barberpole)),
            realization("through_zero_1ms", modulation::flanger::make_flanger_node(
                                                modulation::flanger::Mode::through_zero, 1.0)),
            realization("through_zero_2ms", modulation::flanger::make_flanger_node(
                                                modulation::flanger::Mode::through_zero, 2.0)),
            realization("through_zero_8ms", modulation::flanger::make_flanger_node(
                                                modulation::flanger::Mode::through_zero, 8.0)),
        });
    add(nodes, modulation::leslie::leslie_descriptor(),
        {realization("default", modulation::leslie::make_leslie_node())});
    add(nodes, modulation::leslie::scanner_vibrato_descriptor(),
        {realization("default", modulation::leslie::make_scanner_vibrato_node())});
}

} // namespace pulp::host::forge_catalog_export_detail
