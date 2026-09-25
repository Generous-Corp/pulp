#include "forge_catalog_export_detail.hpp"

#include <pulp/host/forge_eurorack_utility_catalog.hpp>
#include <pulp/host/forge_drum_catalog.hpp>

namespace pulp::host::forge_catalog_export_detail {

void append_eurorack_drums(Nodes& nodes) {
    add(nodes, eurorack::attenuverter_descriptor(),
        {realization("default", eurorack::make_attenuverter_node())});
    add(nodes, eurorack::slew_descriptor(),
        {realization("default", eurorack::make_slew_node())});
    add(nodes, eurorack::clock_divider_descriptor(),
        {realization("default", eurorack::make_clock_divider_node())});
    add(nodes, eurorack::sample_hold_descriptor(),
        {realization("default", eurorack::make_sample_hold_node())});

    const auto add_drum = [&nodes](forge_drum::EngineId engine) {
        add(nodes, forge_drum::drum_descriptor(engine),
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
}

} // namespace pulp::host::forge_catalog_export_detail
