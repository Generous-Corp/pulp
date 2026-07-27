#pragma once

#include <pulp/signal/drum/clap.hpp>
#include <pulp/signal/drum/cymbal.hpp>
#include <pulp/signal/drum/fm.hpp>
#include <pulp/signal/drum/fm6.hpp>
#include <pulp/signal/drum/hat.hpp>
#include <pulp/signal/drum/kick.hpp>
#include <pulp/signal/drum/membrane.hpp>
#include <pulp/signal/drum/snare.hpp>
#include <pulp/signal/drum/string.hpp>
#include <pulp/signal/drum/tom.hpp>
#include <pulp/signal/drum/zap.hpp>

#include <array>
#include <memory>
#include <string_view>

namespace pulp::signal::drum {

/// Stable identities for the drum engines Pulp can describe.
///
/// An unavailable identity stays in this list when its provenance is useful to
/// callers. That lets a preset loader distinguish "known but held" from an
/// unknown engine without compiling or vendoring the held implementation.
enum class EngineId {
    kick_oscillator,
    kick_resonant,
    kick_circuit,
    snare,
    hat,
    clap,
    tom_generic,
    tom_simmons,
    cymbal_comb,
    membrane_modal,
    string_karplus_strong,
    zap_cz,
    fm2,
    fm6,
    fm8,
    dx7_msfa,
};

enum class EngineProvenance {
    pulp_original,
    published_technique,
    license_hold,
};

struct EngineMetadata {
    EngineId id;
    std::string_view name;
    std::string_view display_name;
    EngineProvenance provenance;
    std::string_view lineage;
    bool available;
    bool velocity_changes_timbre;
    bool velocity_may_change_decay;
};

/// The authoritative engine inventory. Names are persistence-safe and must not
/// be reused for a different topology.
inline constexpr std::array<EngineMetadata, 16> engine_registry{{
    {EngineId::kick_oscillator, "kick.oscillator", "Oscillator Kick",
     EngineProvenance::pulp_original, "Pulp original implementation", true, true, false},
    {EngineId::kick_resonant, "kick.resonant", "Resonant Kick",
     EngineProvenance::pulp_original, "Pulp original implementation", true, true, false},
    {EngineId::kick_circuit, "kick.circuit", "Bridged-T Circuit Kick",
     EngineProvenance::published_technique,
     "Werner, Abel, and Smith bridged-T model", true, true, false},
    {EngineId::snare, "snare", "Snare",
     EngineProvenance::pulp_original, "Pulp original implementation", true, true, false},
    {EngineId::hat, "hat", "Metallic Hi-Hat",
     EngineProvenance::pulp_original, "Pulp original implementation", true, true, false},
    {EngineId::clap, "clap", "Hand Clap",
     EngineProvenance::pulp_original, "Pulp original implementation", true, true, false},
    {EngineId::tom_generic, "tom.generic", "Generic Tom",
     EngineProvenance::pulp_original, "Pulp original preset and implementation", true, true,
     false},
    {EngineId::tom_simmons, "tom.simmons", "SDS-V-Family Tom",
     EngineProvenance::published_technique,
     "Original implementation of the published Simmons topology", true, true, false},
    {EngineId::cymbal_comb, "cymbal.comb", "Comb Cymbal",
     EngineProvenance::published_technique,
     "Original implementation informed by the zionjaymes comb tutorial", true, true, true},
    {EngineId::membrane_modal, "membrane.modal", "Modal Membrane",
     EngineProvenance::published_technique,
     "Fletcher and Rossing circular-membrane modal ratios", true, true, false},
    {EngineId::string_karplus_strong, "string.karplus-strong", "Karplus-Strong String",
     EngineProvenance::published_technique,
     "Karplus and Strong; Jaffe and Smith extensions", true, true, false},
    {EngineId::zap_cz, "zap.cz", "CZ Phase-Distortion Zap",
     EngineProvenance::published_technique,
     "Original implementation of Casio phase distortion", true, true, false},
    {EngineId::fm2, "fm2", "Two-Operator FM Drum",
     EngineProvenance::published_technique,
     "Original implementation of Chowning FM synthesis", true, true, false},
    {EngineId::fm6, "fm6", "Six-Operator FM Drum",
     EngineProvenance::published_technique,
     "Original implementation of Chowning FM synthesis", true, true, false},
    {EngineId::fm8, "fm8", "Eight-Operator FM Drum",
     EngineProvenance::published_technique,
     "Original implementation of Chowning FM synthesis", true, true, false},
    {EngineId::dx7_msfa, "dx7.msfa", "DX7 (MSFA)",
     EngineProvenance::license_hold,
     "Unavailable: MSFA vendored license-file mismatch must be resolved", false, false,
     false},
}};

inline const EngineMetadata* find_engine(std::string_view name) noexcept {
    for (const auto& engine : engine_registry) {
        if (engine.name == name) return &engine;
    }
    return nullptr;
}

/// Construct a configured engine by stable identity. This is a setup-time
/// operation and may allocate. Held or unknown implementations return null.
inline std::unique_ptr<Voice> create_engine(EngineId id) {
    switch (id) {
        case EngineId::kick_oscillator: {
            auto voice = std::make_unique<KickVoice>();
            voice->set_body(KickBody::oscillator);
            return voice;
        }
        case EngineId::kick_resonant: {
            auto voice = std::make_unique<KickVoice>();
            voice->set_body(KickBody::resonant);
            return voice;
        }
        case EngineId::kick_circuit: {
            auto voice = std::make_unique<KickVoice>();
            voice->set_body(KickBody::circuit);
            return voice;
        }
        case EngineId::snare:
            return std::make_unique<SnareVoice>();
        case EngineId::hat:
            return std::make_unique<HatVoice>();
        case EngineId::clap:
            return std::make_unique<ClapVoice>();
        case EngineId::tom_generic: {
            auto voice = std::make_unique<TomVoice>();
            voice->apply_preset(TomVoice::Preset::generic_tom);
            return voice;
        }
        case EngineId::tom_simmons: {
            auto voice = std::make_unique<TomVoice>();
            voice->apply_preset(TomVoice::Preset::low_tom);
            return voice;
        }
        case EngineId::cymbal_comb:
            return std::make_unique<CymbalVoice>();
        case EngineId::membrane_modal:
            return std::make_unique<MembraneVoice>();
        case EngineId::string_karplus_strong:
            return std::make_unique<StringVoice>();
        case EngineId::zap_cz:
            return std::make_unique<ZapVoice>();
        case EngineId::fm2:
            return std::make_unique<FmDrumVoice>();
        case EngineId::fm6:
            return std::make_unique<Fm6DrumVoice>();
        case EngineId::fm8:
            return std::make_unique<Fm8DrumVoice>();
        case EngineId::dx7_msfa:
            return nullptr;
    }
    return nullptr;
}

}  // namespace pulp::signal::drum
