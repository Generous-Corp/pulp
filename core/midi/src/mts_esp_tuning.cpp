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

    return {
        .valid = true,
        .frequency_hz = MTS_NoteToFrequency(impl_->client, note, channel),
        .retuning_semitones = MTS_RetuningInSemitones(impl_->client, note, channel),
        .retuning_ratio = MTS_RetuningAsRatio(impl_->client, note, channel),
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
    result.period_ratio = MTS_GetPeriodRatio(impl_->client);
    result.period_semitones = MTS_GetPeriodSemitones(impl_->client);
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

}  // namespace pulp::midi

#endif  // PULP_HAS_MTS_ESP
