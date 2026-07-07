#pragma once

#include <pulp/midi/tuning.hpp>

#include <filesystem>
#include <memory>
#include <string>
#include <string_view>

#ifndef PULP_HAS_SCALA_TUNING
#define PULP_HAS_SCALA_TUNING 0
#endif

namespace pulp::midi {

inline constexpr bool kHasScalaTuning = static_cast<bool>(PULP_HAS_SCALA_TUNING);

#if PULP_HAS_SCALA_TUNING

class ScalaTuningProvider final : public TuningProvider {
public:
    ScalaTuningProvider();
    ~ScalaTuningProvider() override;

    ScalaTuningProvider(const ScalaTuningProvider&) = delete;
    ScalaTuningProvider& operator=(const ScalaTuningProvider&) = delete;

    ScalaTuningProvider(ScalaTuningProvider&&) noexcept;
    ScalaTuningProvider& operator=(ScalaTuningProvider&&) noexcept;

    bool load_scl_file(const std::filesystem::path& path, std::string* error = nullptr);
    bool load_kbm_file(const std::filesystem::path& path, std::string* error = nullptr);
    bool load_scl_kbm_files(
        const std::filesystem::path& scl_path,
        const std::filesystem::path& kbm_path,
        std::string* error = nullptr);

    bool load_scl_data(std::string_view scl_data, std::string* error = nullptr);
    bool load_kbm_data(std::string_view kbm_data, std::string* error = nullptr);
    bool load_scl_kbm_data(
        std::string_view scl_data,
        std::string_view kbm_data,
        std::string* error = nullptr);

    bool tune_note_to(int midi_note, double frequency_hz, std::string* error = nullptr);
    void reset_to_equal_temperament();

    bool has_loaded_scale() const noexcept;
    bool has_loaded_keyboard_mapping() const noexcept;

    TuningQueryResult note_to_frequency(
        int midi_note,
        int midi_channel = kUnknownMidiChannel) const override;

    TuningNoteResult frequency_to_note(
        double frequency_hz,
        int preferred_midi_channel = kUnknownMidiChannel) const override;

    TuningNoteResult frequency_to_note_and_channel(double frequency_hz) const override;
    TuningStatus status() const override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

#endif  // PULP_HAS_SCALA_TUNING

}  // namespace pulp::midi
