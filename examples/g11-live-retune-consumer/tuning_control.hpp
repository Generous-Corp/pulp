#pragma once

#include <pulp/format/processor_node_adapter.hpp>
#include <pulp/midi/tuning.hpp>

#include <cmath>
#include <string_view>

namespace g11 {

enum class RetuneResult { Applied, Unsupported, InvalidNote, InvalidFrequency, UnknownParameter };

// Product-owned boundary adapter, not a graph scheduler. The host calls this
// on its control thread between callbacks at a boundary it has already chosen.
inline RetuneResult retune(pulp::format::ProcessorNodeInstance& instrument,
                           const pulp::midi::TuningProvider* provider, int note,
                           std::string_view parameter_path, double sample_rate) {
    if (!provider)
        return RetuneResult::Unsupported;
    if (!pulp::midi::is_valid_midi_note(note))
        return RetuneResult::InvalidNote;
    const auto pitch = provider->note_to_frequency(note, 0);
    if (!pitch.valid || pitch.should_filter_note)
        return RetuneResult::InvalidNote;
    if (!std::isfinite(sample_rate) || sample_rate <= 0.0 ||
        !std::isfinite(pitch.frequency_hz) || pitch.frequency_hz <= 0.0 ||
        pitch.frequency_hz >= sample_rate / 2.0)
        return RetuneResult::InvalidFrequency;
    for (const auto& parameter : instrument.parameter_catalog()) {
        if (parameter.name != parameter_path)
            continue;
        if (pitch.frequency_hz < parameter.range.min || pitch.frequency_hz > parameter.range.max)
            return RetuneResult::InvalidFrequency;
        const float frequency = static_cast<float>(pitch.frequency_hz);
        if (!std::isfinite(frequency))
            return RetuneResult::InvalidFrequency;
#if !defined(G11_MUTATE_IGNORE_WRITE)
        instrument.state_store().set_value(parameter.id, frequency);
#endif
        return RetuneResult::Applied;
    }
    return RetuneResult::UnknownParameter;
}

} // namespace g11
