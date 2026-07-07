#include <pulp/midi/scala_tuning.hpp>

#if PULP_HAS_SCALA_TUNING

#include <Tunings.h>

#include <algorithm>
#include <cmath>
#include <fstream>
#include <limits>
#include <sstream>
#include <utility>

namespace pulp::midi {
namespace {

std::string path_string(const std::filesystem::path& path) {
    return path.string();
}

std::string slurp_file(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in)
        throw Tunings::TuningError("Unable to open file '" + path_string(path) + "'");

    std::ostringstream out;
    out << in.rdbuf();
    return out.str();
}

void set_error(std::string* error, const char* prefix, const std::exception& e) {
    if (error)
        *error = std::string(prefix) + e.what();
}

bool is_positive_finite(double value) noexcept {
    return value > 0.0 && std::isfinite(value);
}

double log_distance(double a_hz, double b_hz) noexcept {
    return std::abs(std::log2(a_hz / b_hz));
}

}  // namespace

struct ScalaTuningProvider::Impl {
    Impl()
        : scale(Tunings::evenTemperament12NoteScale()),
          mapping(Tunings::KeyboardMapping()),
          tuning(scale, mapping) {}

    Tunings::Scale scale;
    Tunings::KeyboardMapping mapping;
    Tunings::Tuning tuning;
    bool loaded_scale = false;
    bool loaded_mapping = false;

    void rebuild(Tunings::Scale next_scale,
                 Tunings::KeyboardMapping next_mapping,
                 bool next_loaded_scale,
                 bool next_loaded_mapping) {
        Tunings::Tuning next_tuning(next_scale, next_mapping);
        scale = std::move(next_scale);
        mapping = std::move(next_mapping);
        tuning = std::move(next_tuning);
        loaded_scale = next_loaded_scale;
        loaded_mapping = next_loaded_mapping;
    }
};

ScalaTuningProvider::ScalaTuningProvider()
    : impl_(std::make_unique<Impl>()) {}

ScalaTuningProvider::~ScalaTuningProvider() = default;
ScalaTuningProvider::ScalaTuningProvider(ScalaTuningProvider&&) noexcept = default;
ScalaTuningProvider& ScalaTuningProvider::operator=(ScalaTuningProvider&&) noexcept = default;

bool ScalaTuningProvider::load_scl_file(
    const std::filesystem::path& path,
    std::string* error) {
    try {
        return load_scl_data(slurp_file(path), error);
    } catch (const std::exception& e) {
        set_error(error, "SCL load failed: ", e);
        return false;
    }
}

bool ScalaTuningProvider::load_kbm_file(
    const std::filesystem::path& path,
    std::string* error) {
    try {
        return load_kbm_data(slurp_file(path), error);
    } catch (const std::exception& e) {
        set_error(error, "KBM load failed: ", e);
        return false;
    }
}

bool ScalaTuningProvider::load_scl_kbm_files(
    const std::filesystem::path& scl_path,
    const std::filesystem::path& kbm_path,
    std::string* error) {
    try {
        return load_scl_kbm_data(slurp_file(scl_path), slurp_file(kbm_path), error);
    } catch (const std::exception& e) {
        set_error(error, "SCL/KBM load failed: ", e);
        return false;
    }
}

bool ScalaTuningProvider::load_scl_data(std::string_view scl_data, std::string* error) {
    try {
        auto next_scale = Tunings::parseSCLData(std::string(scl_data));
        impl_->rebuild(
            std::move(next_scale),
            impl_->mapping,
            true,
            impl_->loaded_mapping);
        if (error)
            error->clear();
        return true;
    } catch (const std::exception& e) {
        set_error(error, "SCL parse failed: ", e);
        return false;
    }
}

bool ScalaTuningProvider::load_kbm_data(std::string_view kbm_data, std::string* error) {
    try {
        auto next_mapping = Tunings::parseKBMData(std::string(kbm_data));
        impl_->rebuild(
            impl_->scale,
            std::move(next_mapping),
            impl_->loaded_scale,
            true);
        if (error)
            error->clear();
        return true;
    } catch (const std::exception& e) {
        set_error(error, "KBM parse failed: ", e);
        return false;
    }
}

bool ScalaTuningProvider::load_scl_kbm_data(
    std::string_view scl_data,
    std::string_view kbm_data,
    std::string* error) {
    try {
        auto next_scale = Tunings::parseSCLData(std::string(scl_data));
        auto next_mapping = Tunings::parseKBMData(std::string(kbm_data));
        impl_->rebuild(std::move(next_scale), std::move(next_mapping), true, true);
        if (error)
            error->clear();
        return true;
    } catch (const std::exception& e) {
        set_error(error, "SCL/KBM parse failed: ", e);
        return false;
    }
}

bool ScalaTuningProvider::tune_note_to(
    int midi_note,
    double frequency_hz,
    std::string* error) {
    if (!is_valid_midi_note(midi_note) || !is_positive_finite(frequency_hz)) {
        if (error)
            *error = "Invalid reference note or frequency";
        return false;
    }

    try {
        auto next_mapping = Tunings::tuneNoteTo(midi_note, frequency_hz);
        impl_->rebuild(
            impl_->scale,
            std::move(next_mapping),
            impl_->loaded_scale,
            true);
        if (error)
            error->clear();
        return true;
    } catch (const std::exception& e) {
        set_error(error, "KBM reference tune failed: ", e);
        return false;
    }
}

void ScalaTuningProvider::reset_to_equal_temperament() {
    impl_->rebuild(
        Tunings::evenTemperament12NoteScale(),
        Tunings::KeyboardMapping(),
        false,
        false);
}

bool ScalaTuningProvider::has_loaded_scale() const noexcept {
    return impl_ && impl_->loaded_scale;
}

bool ScalaTuningProvider::has_loaded_keyboard_mapping() const noexcept {
    return impl_ && impl_->loaded_mapping;
}

TuningQueryResult ScalaTuningProvider::note_to_frequency(
    int midi_note,
    int) const {
    if (!is_valid_midi_note(midi_note) || !impl_)
        return {};

    const double frequency_hz = impl_->tuning.frequencyForMidiNote(midi_note);
    if (!is_positive_finite(frequency_hz))
        return {};

    const double twelve_tet_hz = equal_temperament_frequency(midi_note);
    const double retuning_ratio =
        is_positive_finite(twelve_tet_hz) ? frequency_hz / twelve_tet_hz : 1.0;

    return {
        .valid = true,
        .frequency_hz = frequency_hz,
        .retuning_semitones =
            is_positive_finite(retuning_ratio) ? 12.0 * std::log2(retuning_ratio) : 0.0,
        .retuning_ratio = is_positive_finite(retuning_ratio) ? retuning_ratio : 1.0,
        .should_filter_note = !impl_->tuning.isMidiNoteMapped(midi_note),
    };
}

TuningNoteResult ScalaTuningProvider::frequency_to_note(
    double frequency_hz,
    int preferred_midi_channel) const {
    if (!is_positive_finite(frequency_hz) || !impl_)
        return {};

    int best_note = -1;
    double best_distance = std::numeric_limits<double>::infinity();
    for (int note = 0; note <= 127; ++note) {
        if (!impl_->tuning.isMidiNoteMapped(note))
            continue;
        const double candidate_hz = impl_->tuning.frequencyForMidiNote(note);
        if (!is_positive_finite(candidate_hz))
            continue;
        const double distance = log_distance(candidate_hz, frequency_hz);
        if (distance < best_distance) {
            best_distance = distance;
            best_note = note;
        }
    }

    return {
        .valid = is_valid_midi_note(best_note),
        .midi_note = best_note,
        .midi_channel = is_valid_midi_note(best_note)
            ? normalise_midi_channel(preferred_midi_channel)
            : kUnknownMidiChannel,
    };
}

TuningNoteResult ScalaTuningProvider::frequency_to_note_and_channel(double frequency_hz) const {
    auto result = frequency_to_note(frequency_hz, 0);
    if (!result.valid)
        result.midi_channel = kUnknownMidiChannel;
    return result;
}

TuningStatus ScalaTuningProvider::status() const {
    TuningStatus result;
    if (!impl_)
        return result;

    result.has_local_file_tuning = impl_->loaded_scale || impl_->loaded_mapping;
    result.has_keyboard_mapping = impl_->loaded_mapping;
    if (!impl_->scale.description.empty())
        result.scale_name = impl_->scale.description;
    else if (!impl_->scale.name.empty())
        result.scale_name = impl_->scale.name;
    else
        result.scale_name = "Scala tuning";

    if (!impl_->loaded_scale && result.scale_name.find("12 Tone Equal Temperament") != std::string::npos)
        result.scale_name = "12-TET";

    if (!impl_->scale.tones.empty()) {
        const auto& period = impl_->scale.tones.back();
        const double period_log2 = period.floatValue - 1.0;
        const double period_ratio = std::pow(2.0, period_log2);
        if (is_positive_finite(period_ratio)) {
            result.period_ratio = period_ratio;
            result.period_semitones = 12.0 * period_log2;
        }
    }
    result.map_size = impl_->mapping.count;
    result.map_start_key = impl_->mapping.firstMidi;
    result.reference_key = impl_->mapping.tuningConstantNote;
    return result;
}

}  // namespace pulp::midi

#endif  // PULP_HAS_SCALA_TUNING
