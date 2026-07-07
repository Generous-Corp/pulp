#pragma once

#include <pulp/midi/tuning.hpp>

#include <memory>

#ifndef PULP_HAS_MTS_ESP
#define PULP_HAS_MTS_ESP 0
#endif

namespace pulp::midi {

inline constexpr bool kHasMtsEspTuning = static_cast<bool>(PULP_HAS_MTS_ESP);

#if PULP_HAS_MTS_ESP

class MtsEspTuningProvider final : public TuningProvider {
public:
    MtsEspTuningProvider();
    ~MtsEspTuningProvider() override;

    MtsEspTuningProvider(const MtsEspTuningProvider&) = delete;
    MtsEspTuningProvider& operator=(const MtsEspTuningProvider&) = delete;

    MtsEspTuningProvider(MtsEspTuningProvider&&) noexcept;
    MtsEspTuningProvider& operator=(MtsEspTuningProvider&&) noexcept;

    TuningQueryResult note_to_frequency(
        int midi_note,
        int midi_channel = kUnknownMidiChannel) const override;

    TuningNoteResult frequency_to_note(
        double frequency_hz,
        int preferred_midi_channel = kUnknownMidiChannel) const override;

    TuningNoteResult frequency_to_note_and_channel(double frequency_hz) const override;
    TuningStatus status() const override;
    void parse_midi_data(std::span<const std::uint8_t> data) override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

#endif  // PULP_HAS_MTS_ESP

}  // namespace pulp::midi
