#include "forge_catalog_export_detail.hpp"

#include <pulp/host/forge_space_catalog.hpp>
#include <pulp/host/forge_synthesis_catalog.hpp>
#include <pulp/host/forge_tape_catalog.hpp>
#include <pulp/host/forge_wavetable_catalog.hpp>

namespace pulp::host::forge_catalog_export_detail {

void append_space_synthesis(Nodes& nodes) {
    add(nodes, space::convolution::descriptor(),
        {realization("default", space::convolution::catalog_probe_node())});
    add(nodes, space::nonlin_ambience::descriptor(),
        {realization("default", space::nonlin_ambience::make_nonlin_ambience_node())});
    add(nodes, space::cabinet::descriptor(),
        {realization("default", space::cabinet::make_speaker_cabinet_node())});

    add(nodes, synthesis::additive::descriptor(),
        {
            realization("organ", synthesis::additive::make_additive_bank_node(
                                     synthesis::additive::Voice::organ)),
            realization("bell", synthesis::additive::make_additive_bank_node(
                                    synthesis::additive::Voice::bell)),
        });
    add(nodes, synthesis::vocoder::descriptor(),
        {realization("default", synthesis::vocoder::make_vocoder_node())});
    add(nodes, synthesis::cyclic::descriptor(),
        {
            realization("short", synthesis::cyclic::make_cyclic_stretch_node(
                                     synthesis::cyclic::Regime::short_frame)),
            realization("long", synthesis::cyclic::make_cyclic_stretch_node(
                                    synthesis::cyclic::Regime::long_frame)),
        });
    add(nodes, synthesis::granular::descriptor(),
        {realization("default", synthesis::granular::make_granular_node())});

    add(nodes, tape::descriptor(),
        {
            realization("ampex_7_5ips",
                        tape::make_tape_machine_node(tape::Archetype::ampex_350_440, 7.5)),
            realization("ampex_7_5ips_pre_echo",
                        tape::make_tape_machine_node(tape::Archetype::ampex_350_440, 7.5, true)),
            realization("ampex",
                        tape::make_tape_machine_node(tape::Archetype::ampex_350_440, 15.0)),
            realization("ampex_pre_echo",
                        tape::make_tape_machine_node(tape::Archetype::ampex_350_440, 15.0, true)),
            realization("studer_7_5ips",
                        tape::make_tape_machine_node(tape::Archetype::studer_a800, 7.5)),
            realization("studer_7_5ips_pre_echo",
                        tape::make_tape_machine_node(tape::Archetype::studer_a800, 7.5, true)),
            realization("studer", tape::make_tape_machine_node(tape::Archetype::studer_a800, 15.0)),
            realization("studer_pre_echo",
                        tape::make_tape_machine_node(tape::Archetype::studer_a800, 15.0, true)),
            realization("studer_30ips",
                        tape::make_tape_machine_node(tape::Archetype::studer_a800, 30.0)),
            realization("studer_30ips_pre_echo",
                        tape::make_tape_machine_node(tape::Archetype::studer_a800, 30.0, true)),
            realization("cassette",
                        tape::make_tape_machine_node(tape::Archetype::cassette_deck, 1.875)),
            realization("cassette_pre_echo",
                        tape::make_tape_machine_node(tape::Archetype::cassette_deck, 1.875, true)),
        });

    add(nodes, wavetable::descriptor(),
        {realization("default", wavetable::make_wavetable_oscillator_node())});
}

} // namespace pulp::host::forge_catalog_export_detail
