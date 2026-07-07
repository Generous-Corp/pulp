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
    bool has_session_tuning() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

enum class MtsEspFallbackPolicy {
    PreferMtsSession,
    PreferLocalTuning,
};

class MtsEspFallbackTuningProvider final : public TuningProvider {
public:
    explicit MtsEspFallbackTuningProvider(
        std::unique_ptr<TuningProvider> local_fallback =
            std::make_unique<EqualTemperamentTuningProvider>(),
        MtsEspFallbackPolicy policy = MtsEspFallbackPolicy::PreferMtsSession);
    explicit MtsEspFallbackTuningProvider(MtsEspFallbackPolicy policy);
    ~MtsEspFallbackTuningProvider() override;

    MtsEspFallbackTuningProvider(const MtsEspFallbackTuningProvider&) = delete;
    MtsEspFallbackTuningProvider& operator=(const MtsEspFallbackTuningProvider&) = delete;

    MtsEspFallbackTuningProvider(MtsEspFallbackTuningProvider&&) noexcept;
    MtsEspFallbackTuningProvider& operator=(MtsEspFallbackTuningProvider&&) noexcept;

    TuningQueryResult note_to_frequency(
        int midi_note,
        int midi_channel = kUnknownMidiChannel) const override;

    TuningNoteResult frequency_to_note(
        double frequency_hz,
        int preferred_midi_channel = kUnknownMidiChannel) const override;

    TuningNoteResult frequency_to_note_and_channel(double frequency_hz) const override;
    TuningStatus status() const override;
    void parse_midi_data(std::span<const std::uint8_t> data) override;

    bool using_mts_session() const;
    bool mts_session_available() const;
    const MtsEspTuningProvider& mts_provider() const noexcept { return mts_; }
    MtsEspTuningProvider& mts_provider() noexcept { return mts_; }
    const TuningProvider* local_fallback() const noexcept { return fallback_.get(); }
    TuningProvider* local_fallback() noexcept { return fallback_.get(); }

private:
    bool local_tuning_available() const;
    bool should_use_mts_session() const;

    MtsEspTuningProvider mts_;
    std::unique_ptr<TuningProvider> fallback_;
    MtsEspFallbackPolicy policy_ = MtsEspFallbackPolicy::PreferMtsSession;
};

#endif  // PULP_HAS_MTS_ESP

}  // namespace pulp::midi
