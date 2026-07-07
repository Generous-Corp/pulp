#include <pulp/midi/mts_esp_tuning.hpp>

#if PULP_HAS_MTS_ESP

#include <libMTSClient.h>

#include <cmath>
#include <limits>
#include <utility>

namespace pulp::midi {
namespace {

signed char to_mts_channel(int midi_channel) noexcept {
    return is_valid_midi_channel(midi_channel)
        ? static_cast<signed char>(midi_channel)
        : static_cast<signed char>(-1);
}

char to_mts_note(int midi_note) noexcept {
    return static_cast<char>(midi_note);
}

int from_mts_note(char midi_note) noexcept {
    return static_cast<int>(static_cast<unsigned char>(midi_note) & 0x7f);
}

int from_mts_channel(signed char midi_channel) noexcept {
    return is_valid_midi_channel(midi_channel) ? static_cast<int>(midi_channel) : kUnknownMidiChannel;
}

bool is_positive_finite(double value) noexcept {
    return value > 0.0 && std::isfinite(value);
}

double finite_or_default(double value, double fallback) noexcept {
    return std::isfinite(value) ? value : fallback;
}

}  // namespace

struct MtsEspTuningProvider::Impl {
    Impl() : client(MTS_RegisterClient()) {}

    ~Impl() {
        if (client)
            MTS_DeregisterClient(client);
    }

    MTSClient* client = nullptr;
};

MtsEspTuningProvider::MtsEspTuningProvider()
    : impl_(std::make_unique<Impl>()) {}

MtsEspTuningProvider::~MtsEspTuningProvider() = default;
MtsEspTuningProvider::MtsEspTuningProvider(MtsEspTuningProvider&&) noexcept = default;
MtsEspTuningProvider& MtsEspTuningProvider::operator=(MtsEspTuningProvider&&) noexcept = default;

TuningQueryResult MtsEspTuningProvider::note_to_frequency(
    int midi_note,
    int midi_channel) const {
    if (!is_valid_midi_note(midi_note) || !impl_ || !impl_->client)
        return {};

    const auto note = to_mts_note(midi_note);
    const auto channel = to_mts_channel(midi_channel);
    const double frequency_hz = MTS_NoteToFrequency(impl_->client, note, channel);
    if (!is_positive_finite(frequency_hz))
        return {};
    const double retuning_ratio = MTS_RetuningAsRatio(impl_->client, note, channel);

    return {
        .valid = true,
        .frequency_hz = frequency_hz,
        .retuning_semitones = finite_or_default(
            MTS_RetuningInSemitones(impl_->client, note, channel),
            0.0),
        .retuning_ratio = is_positive_finite(retuning_ratio) ? retuning_ratio : 1.0,
        .should_filter_note = MTS_ShouldFilterNote(impl_->client, note, channel),
    };
}

TuningNoteResult MtsEspTuningProvider::frequency_to_note(
    double frequency_hz,
    int preferred_midi_channel) const {
    if (!(frequency_hz > 0.0) || !std::isfinite(frequency_hz) || !impl_ || !impl_->client)
        return {};

    const auto channel = to_mts_channel(preferred_midi_channel);
    const auto note = MTS_FrequencyToNote(impl_->client, frequency_hz, channel);
    return {
        .valid = true,
        .midi_note = from_mts_note(note),
        .midi_channel = normalise_midi_channel(preferred_midi_channel),
    };
}

TuningNoteResult MtsEspTuningProvider::frequency_to_note_and_channel(
    double frequency_hz) const {
    if (!(frequency_hz > 0.0) || !std::isfinite(frequency_hz) || !impl_ || !impl_->client)
        return {};

    signed char channel = static_cast<signed char>(-1);
    const auto note = MTS_FrequencyToNoteAndChannel(impl_->client, frequency_hz, &channel);
    return {
        .valid = true,
        .midi_note = from_mts_note(note),
        .midi_channel = from_mts_channel(channel),
    };
}

TuningStatus MtsEspTuningProvider::status() const {
    TuningStatus result;
    if (!impl_ || !impl_->client)
        return result;

    result.has_external_master = MTS_HasMaster(impl_->client);
    result.has_local_mts_sysex = MTS_HasReceivedMTSSysEx(impl_->client);
    result.library_update_recommended = MTS_Client_ShouldUpdateLibrary(impl_->client);
    if (const char* scale_name = MTS_GetScaleName(impl_->client))
        result.scale_name = scale_name;
    const double period_ratio = MTS_GetPeriodRatio(impl_->client);
    const double period_semitones = MTS_GetPeriodSemitones(impl_->client);
    if (is_positive_finite(period_ratio))
        result.period_ratio = period_ratio;
    if (is_positive_finite(period_semitones))
        result.period_semitones = period_semitones;
    result.map_size = static_cast<int>(MTS_GetMapSize(impl_->client));
    result.map_start_key = static_cast<int>(MTS_GetMapStartKey(impl_->client));
    result.reference_key = static_cast<int>(MTS_GetRefKey(impl_->client));
    return result;
}

void MtsEspTuningProvider::parse_midi_data(std::span<const std::uint8_t> data) {
    if (data.empty() || !impl_ || !impl_->client)
        return;
    if (data.size() > static_cast<std::size_t>(std::numeric_limits<int>::max()))
        return;

    MTS_ParseMIDIDataU(
        impl_->client,
        reinterpret_cast<const unsigned char*>(data.data()),
        static_cast<int>(data.size()));
}

bool MtsEspTuningProvider::has_session_tuning() const {
    if (!impl_ || !impl_->client)
        return false;
    return MTS_HasMaster(impl_->client) || MTS_HasReceivedMTSSysEx(impl_->client);
}

MtsEspFallbackTuningProvider::MtsEspFallbackTuningProvider(
    std::unique_ptr<TuningProvider> local_fallback)
    : fallback_(std::move(local_fallback)) {
    if (!fallback_)
        fallback_ = std::make_unique<EqualTemperamentTuningProvider>();
}

MtsEspFallbackTuningProvider::~MtsEspFallbackTuningProvider() = default;
MtsEspFallbackTuningProvider::MtsEspFallbackTuningProvider(
    MtsEspFallbackTuningProvider&&) noexcept = default;
MtsEspFallbackTuningProvider& MtsEspFallbackTuningProvider::operator=(
    MtsEspFallbackTuningProvider&&) noexcept = default;

bool MtsEspFallbackTuningProvider::using_mts_session() const {
    return mts_.has_session_tuning();
}

TuningQueryResult MtsEspFallbackTuningProvider::note_to_frequency(
    int midi_note,
    int midi_channel) const {
    if (using_mts_session()) {
        auto mts_result = mts_.note_to_frequency(midi_note, midi_channel);
        if (mts_result.valid)
            return mts_result;
    }
    return fallback_ ? fallback_->note_to_frequency(midi_note, midi_channel) : TuningQueryResult{};
}

TuningNoteResult MtsEspFallbackTuningProvider::frequency_to_note(
    double frequency_hz,
    int preferred_midi_channel) const {
    if (using_mts_session()) {
        auto mts_result = mts_.frequency_to_note(frequency_hz, preferred_midi_channel);
        if (mts_result.valid)
            return mts_result;
    }
    return fallback_ ? fallback_->frequency_to_note(frequency_hz, preferred_midi_channel)
                     : TuningNoteResult{};
}

TuningNoteResult MtsEspFallbackTuningProvider::frequency_to_note_and_channel(
    double frequency_hz) const {
    if (using_mts_session()) {
        auto mts_result = mts_.frequency_to_note_and_channel(frequency_hz);
        if (mts_result.valid)
            return mts_result;
    }
    return fallback_ ? fallback_->frequency_to_note_and_channel(frequency_hz)
                     : TuningNoteResult{};
}

TuningStatus MtsEspFallbackTuningProvider::status() const {
    const auto mts_status = mts_.status();
    if (mts_status.has_external_master || mts_status.has_local_mts_sysex)
        return mts_status;
    return fallback_ ? fallback_->status() : TuningStatus{};
}

void MtsEspFallbackTuningProvider::parse_midi_data(std::span<const std::uint8_t> data) {
    mts_.parse_midi_data(data);
    if (fallback_)
        fallback_->parse_midi_data(data);
}

}  // namespace pulp::midi

#endif  // PULP_HAS_MTS_ESP
