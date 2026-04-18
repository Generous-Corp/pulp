#include <pulp/format/aax_entry.hpp>

#include <pulp/audio/buffer.hpp>
#include <pulp/midi/buffer.hpp>
#include <pulp/midi/message.hpp>
#include <pulp/state/store.hpp>

#include <AAX_CEffectParameters.h>
#include <AAX_CParameter.h>
#include <AAX_CString.h>
#include <AAX_ICollection.h>
#include <AAX_IComponentDescriptor.h>
#include <AAX_IController.h>
#include <AAX_IEffectDescriptor.h>
#include <AAX_IMIDINode.h>
#include <AAX_IPropertyMap.h>
#include <AAX_ITransport.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace pulp::format::aax {

namespace {

struct InstanceState;

struct PrivateDataBlock {
    InstanceState* instance = nullptr;
};

struct AlgorithmContext {
    float** audio_input = nullptr;
    float** audio_output = nullptr;
    int32_t* buffer_length = nullptr;
    AAX_CSampleRate* sample_rate = nullptr;
    float* parameter_packet = nullptr;
    PrivateDataBlock* private_data = nullptr;
    AAX_IMIDINode* transport_node = nullptr;
    int32_t* sidechain_start_index = nullptr;
    AAX_IMIDINode* midi_input_node = nullptr;
    AAX_IMIDINode* midi_output_node = nullptr;
};

constexpr AAX_CFieldIndex kAudioInputField = AAX_FIELD_INDEX(AlgorithmContext, audio_input);
constexpr AAX_CFieldIndex kAudioOutputField = AAX_FIELD_INDEX(AlgorithmContext, audio_output);
constexpr AAX_CFieldIndex kBufferLengthField = AAX_FIELD_INDEX(AlgorithmContext, buffer_length);
constexpr AAX_CFieldIndex kSampleRateField = AAX_FIELD_INDEX(AlgorithmContext, sample_rate);
constexpr AAX_CFieldIndex kParameterPacketField = AAX_FIELD_INDEX(AlgorithmContext, parameter_packet);
constexpr AAX_CFieldIndex kPrivateDataField = AAX_FIELD_INDEX(AlgorithmContext, private_data);
constexpr AAX_CFieldIndex kTransportField = AAX_FIELD_INDEX(AlgorithmContext, transport_node);
constexpr AAX_CFieldIndex kSidechainIndexField = AAX_FIELD_INDEX(AlgorithmContext, sidechain_start_index);
constexpr AAX_CFieldIndex kMidiInputField = AAX_FIELD_INDEX(AlgorithmContext, midi_input_node);
constexpr AAX_CFieldIndex kMidiOutputField = AAX_FIELD_INDEX(AlgorithmContext, midi_output_node);

static_assert(kAudioInputField == AAX_FIELD_INDEX(AlgorithmContext, audio_input));

std::string truncate_copy(std::string value, std::size_t max_size) {
    if (value.size() <= max_size) {
        return value;
    }
    value.resize(max_size);
    return value;
}

std::string format_default_value(float value, const std::string& unit) {
    std::ostringstream out;
    out.setf(std::ios::fixed);
    out.precision(std::fabs(value) >= 1000.0f ? 1 : 2);
    out << value;
    if (!unit.empty()) {
        out << ' ' << unit;
    }
    return out.str();
}

bool parse_float_value(const std::string& text, float* value) {
    if (!value) {
        return false;
    }

    char* end = nullptr;
    const double parsed = std::strtod(text.c_str(), &end);
    if (end == text.c_str()) {
        return false;
    }

    *value = static_cast<float>(parsed);
    return true;
}

uint32_t pack_semver(const std::string& version) {
    std::array<uint32_t, 3> parts{1, 0, 0};
    std::stringstream in(version);
    std::string part;
    std::size_t index = 0;
    while (std::getline(in, part, '.') && index < parts.size()) {
        try {
            parts[index] = static_cast<uint32_t>(std::max(0, std::stoi(part)));
        } catch (...) {
            parts[index] = 0;
        }
        ++index;
    }
    return (parts[0] << 16) | (parts[1] << 8) | parts[2];
}

AAX_EStemFormat stem_to_aax(StemKind stem) {
    switch (stem) {
        case StemKind::none: return AAX_eStemFormat_None;
        case StemKind::mono: return AAX_eStemFormat_Mono;
        case StemKind::stereo: return AAX_eStemFormat_Stereo;
        case StemKind::lcr: return AAX_eStemFormat_LCR;
        case StemKind::lcrs: return AAX_eStemFormat_LCRS;
        case StemKind::quad: return AAX_eStemFormat_Quad;
        case StemKind::surround_5_0: return AAX_eStemFormat_5_0;
        case StemKind::surround_5_1: return AAX_eStemFormat_5_1;
        case StemKind::surround_6_0: return AAX_eStemFormat_6_0;
        case StemKind::surround_6_1: return AAX_eStemFormat_6_1;
        case StemKind::surround_7_0: return AAX_eStemFormat_7_0_DTS;
        case StemKind::surround_7_1: return AAX_eStemFormat_7_1_DTS;
    }
    return AAX_eStemFormat_None;
}

uint32_t category_to_aax(PluginCategory category) {
    switch (category) {
        case PluginCategory::Instrument: return AAX_ePlugInCategory_SWGenerators;
        case PluginCategory::MidiEffect: return AAX_ePlugInCategory_MIDIEffect;
        case PluginCategory::Effect: return AAX_ePlugInCategory_Effect;
    }
    return AAX_ePlugInCategory_Effect;
}

void clear_audio(float** channels, int channel_count, int sample_count) {
    if (!channels) {
        return;
    }
    for (int ch = 0; ch < channel_count; ++ch) {
        if (channels[ch]) {
            std::fill_n(channels[ch], sample_count, 0.0f);
        }
    }
}

void copy_audio(float** output,
                int output_channels,
                float** input,
                int input_channels,
                int sample_count)
{
    if (!output) {
        return;
    }

    const int shared_channels = std::min(output_channels, input_channels);
    for (int ch = 0; ch < shared_channels; ++ch) {
        if (!output[ch] || !input || !input[ch]) {
            continue;
        }
        std::memcpy(output[ch], input[ch], static_cast<std::size_t>(sample_count) * sizeof(float));
    }

    for (int ch = shared_channels; ch < output_channels; ++ch) {
        if (output[ch]) {
            std::fill_n(output[ch], sample_count, 0.0f);
        }
    }
}

void copy_midi(const midi::MidiBuffer& in, midi::MidiBuffer& out) {
    for (const auto& event : in) {
        out.add(event);
    }
}

void read_transport(AAX_ITransport* transport, ProcessContext* context) {
    if (!transport || !context) {
        return;
    }

    transport->GetCurrentTempo(&context->tempo_bpm);
    transport->GetCurrentMeter(&context->time_sig_numerator, &context->time_sig_denominator);
    transport->IsTransportPlaying(&context->is_playing);
    transport->GetCurrentNativeSampleLocation(&context->position_samples);

    int64_t tick_position = 0;
    uint32_t ticks_per_quarter = 0;
    if (transport->GetCurrentTickPosition(&tick_position) == AAX_SUCCESS
        && transport->GetTicksPerQuarter(&ticks_per_quarter) == AAX_SUCCESS
        && ticks_per_quarter > 0)
    {
        context->position_beats = static_cast<double>(tick_position) / static_cast<double>(ticks_per_quarter);
    }
}

void decode_midi_node(AAX_IMIDINode* node, midi::MidiBuffer* midi_in) {
    if (!node || !midi_in) {
        return;
    }

    if (auto* stream = node->GetNodeBuffer(); stream && stream->mBuffer) {
        // Sysex accumulator — AAX splits multi-byte sysex into
        // sequential AAX_CMidiPacket entries (mData[4] max) with the
        // F0 status in the first packet and F7 terminator in the last.
        // AAX docs allow continuation packets without a status byte;
        // the accumulator treats every byte while `sysex_in_progress`
        // as payload until F7 is seen. #239 AAX parity.
        std::vector<uint8_t> sysex_buffer;
        bool sysex_in_progress = false;
        int32_t sysex_start_offset = 0;

        for (uint32_t index = 0; index < stream->mBufferSize; ++index) {
            const auto& packet = stream->mBuffer[index];
            if (packet.mLength == 0) {
                continue;
            }

            // Detect sysex start OR continue an already-running sysex.
            const bool starts_sysex =
                (!sysex_in_progress && packet.mData[0] == 0xF0);
            if (starts_sysex || sysex_in_progress) {
                if (starts_sysex) {
                    sysex_buffer.clear();
                    sysex_start_offset =
                        static_cast<int32_t>(packet.mTimestamp);
                    sysex_in_progress = true;
                }
                for (uint32_t b = 0; b < packet.mLength && b < 4; ++b) {
                    const uint8_t byte = packet.mData[b];
                    sysex_buffer.push_back(byte);
                    if (byte == 0xF7) {
                        midi_in->add_sysex(std::move(sysex_buffer),
                                           sysex_start_offset, 0.0);
                        sysex_buffer.clear();
                        sysex_in_progress = false;
                        break;
                    }
                }
                continue;
            }

            // Short channel-voice or system-common (1..3 bytes).
            if (packet.mLength > 3) {
                // Unexpected — AAX short-message packets are 1..3
                // bytes; anything larger outside a sysex is malformed.
                continue;
            }

            unsigned char data1 = packet.mLength > 1 ? packet.mData[1] : 0;
            unsigned char data2 = packet.mLength > 2 ? packet.mData[2] : 0;
            midi::MidiEvent event{
                .message = choc::midi::ShortMessage(packet.mData[0], data1, data2),
                .sample_offset = static_cast<int32_t>(packet.mTimestamp),
                .timestamp = 0.0,
            };
            midi_in->add(std::move(event));
        }

        // Dangling sysex (no F7 before the block ended) — drop rather
        // than deliver a corrupt message. A real device should have
        // emitted F7 within the block; if it didn't, the DAW's own
        // host layer will have flagged the malformed stream.
        midi_in->sort();
    }
}

void encode_midi_node(AAX_IMIDINode* node, const midi::MidiBuffer& midi_out) {
    if (!node) {
        return;
    }

    // Short channel-voice messages first (unchanged).
    for (const auto& event : midi_out) {
        if (event.size() == 0 || event.size() > 3) {
            continue;
        }

        AAX_CMidiPacket packet{};
        packet.mTimestamp = static_cast<uint32_t>(std::max(0, event.sample_offset));
        packet.mLength = event.size();
        std::memcpy(packet.mData, event.data(), packet.mLength);
        packet.mIsImmediate = (packet.mTimestamp == 0);
        node->PostMIDIPacket(&packet);
    }

    // Sysex outbound — chunk each SysexEvent into 4-byte AAX packets.
    // AAX_CMidiPacket::mData is exactly 4 bytes; the full F0..F7 byte
    // stream is delivered as ceil(N / 4) consecutive packets sharing
    // the same timestamp. #239 AAX parity.
    for (const auto& sx : midi_out.sysex()) {
        if (sx.data.empty()) continue;
        const uint32_t ts = static_cast<uint32_t>(
            std::max(0, sx.sample_offset));
        size_t offset = 0;
        while (offset < sx.data.size()) {
            const size_t chunk = std::min<size_t>(4, sx.data.size() - offset);
            AAX_CMidiPacket packet{};
            packet.mTimestamp = ts;
            packet.mLength = static_cast<uint8_t>(chunk);
            std::memcpy(packet.mData, sx.data.data() + offset, chunk);
            // mIsImmediate flags the first chunk only; continuation
            // packets ride the same timestamp and shouldn't re-fire
            // the immediate-dispatch path.
            packet.mIsImmediate = (offset == 0 && ts == 0);
            node->PostMIDIPacket(&packet);
            offset += chunk;
        }
    }
}

struct FloatTaperDelegate final : AAX_ITaperDelegate<float> {
    explicit FloatTaperDelegate(state::ParamRange in_range) : range(in_range) {}

    AAX_ITaperDelegate<float>* Clone() const override { return new FloatTaperDelegate(*this); }
    float GetMaximumValue() const override { return range.max; }
    float GetMinimumValue() const override { return range.min; }
    float ConstrainRealValue(float value) const override {
        return range.denormalize(range.normalize(value));
    }
    float NormalizedToReal(double normalizedValue) const override {
        return range.denormalize(static_cast<float>(normalizedValue));
    }
    double RealToNormalized(float realValue) const override {
        return range.normalize(ConstrainRealValue(realValue));
    }

    state::ParamRange range;
};

struct FloatDisplayDelegate final : AAX_IDisplayDelegate<float> {
    explicit FloatDisplayDelegate(ParameterBinding in_binding) : binding(std::move(in_binding)) {}

    AAX_IDisplayDelegate<float>* Clone() const override { return new FloatDisplayDelegate(*this); }

    bool ValueToString(float value, AAX_CString* valueString) const override {
        if (!valueString) {
            return false;
        }
        if (binding.to_string) {
            *valueString = binding.to_string(value);
            return true;
        }
        *valueString = format_default_value(value, binding.unit);
        return true;
    }

    bool ValueToString(float value, int32_t maxNumChars, AAX_CString* valueString) const override {
        if (!ValueToString(value, valueString)) {
            return false;
        }
        auto text = valueString->StdString();
        if (maxNumChars >= 0 && static_cast<int32_t>(text.size()) > maxNumChars) {
            text.resize(static_cast<std::size_t>(maxNumChars));
            *valueString = text;
        }
        return true;
    }

    bool StringToValue(const AAX_CString& valueString, float* value) const override {
        if (!value) {
            return false;
        }
        if (binding.from_string) {
            *value = binding.from_string(valueString.StdString());
            return true;
        }
        return parse_float_value(valueString.StdString(), value);
    }

    ParameterBinding binding;
};

struct BypassDisplayDelegate final : AAX_IDisplayDelegate<float> {
    AAX_IDisplayDelegate<float>* Clone() const override { return new BypassDisplayDelegate(*this); }

    bool ValueToString(float value, AAX_CString* valueString) const override {
        if (!valueString) {
            return false;
        }
        *valueString = value >= 0.5f ? "On" : "Off";
        return true;
    }

    bool ValueToString(float value, int32_t maxNumChars, AAX_CString* valueString) const override {
        if (!ValueToString(value, valueString)) {
            return false;
        }
        if (maxNumChars >= 0 && valueString->Length() > static_cast<uint32_t>(maxNumChars)) {
            valueString->Set(value >= 0.5f ? "On" : "Off");
        }
        return true;
    }

    bool StringToValue(const AAX_CString& valueString, float* value) const override {
        if (!value) {
            return false;
        }
        auto text = valueString.StdString();
        std::transform(text.begin(), text.end(), text.begin(),
                       [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
        if (text == "on" || text == "1" || text == "true") {
            *value = 1.0f;
            return true;
        }
        if (text == "off" || text == "0" || text == "false") {
            *value = 0.0f;
            return true;
        }
        return parse_float_value(text, value);
    }
};

struct InstanceState {
    explicit InstanceState(const EntryConfig& entry)
        : config(entry)
    {
        auto definition_result = build_plugin_definition(config.factory, config.codes);
        if (definition_result.ok) {
            definition = std::move(definition_result.definition);
        }
        processor = config.factory ? config.factory() : nullptr;
        if (processor) {
            processor->set_state_store(&store);
            processor->define_parameters(store);
            descriptor = processor->descriptor();
            store.reset_all_to_defaults();
        }
    }

    ~InstanceState() {
        if (processor) {
            processor->release();
        }
    }

    bool ensure_prepared(double sample_rate,
                         int max_buffer_size,
                         int input_channels,
                         int output_channels)
    {
        if (!processor) {
            return false;
        }

        if (prepared
            && prepared_sample_rate == sample_rate
            && prepared_max_buffer == max_buffer_size
            && prepared_input_channels == input_channels
            && prepared_output_channels == output_channels)
        {
            return true;
        }

        if (prepared) {
            processor->release();
        }

        processor->prepare({
            .sample_rate = sample_rate,
            .max_buffer_size = max_buffer_size,
            .input_channels = input_channels,
            .output_channels = output_channels,
        });
        prepared = true;
        prepared_sample_rate = sample_rate;
        prepared_max_buffer = max_buffer_size;
        prepared_input_channels = input_channels;
        prepared_output_channels = output_channels;
        return true;
    }

    void sync_from_packet(const PluginDefinition& definition, const float* packet) {
        if (!packet) {
            return;
        }
        for (std::size_t index = 0; index < definition.parameters.size(); ++index) {
            store.set_value(definition.parameters[index].id, packet[index + 1]);
        }
    }

    EntryConfig config;
    PluginDefinition definition;
    std::unique_ptr<Processor> processor;
    state::StateStore store;
    PluginDescriptor descriptor;
    std::vector<const float*> input_ptrs;
    std::vector<float*> output_ptrs;
    std::vector<const float*> sidechain_ptrs;
    midi::MidiBuffer midi_in;
    midi::MidiBuffer midi_out;
    bool prepared = false;
    double prepared_sample_rate = 0.0;
    int prepared_max_buffer = 0;
    int prepared_input_channels = 0;
    int prepared_output_channels = 0;
};

void destroy_instance(PrivateDataBlock* block) {
    if (!block || !block->instance) {
        return;
    }
    delete block->instance;
    block->instance = nullptr;
}

class EffectParameters final : public AAX_CEffectParameters {
public:
    explicit EffectParameters(EntryConfig in_config)
        : config_(in_config)
    {
        auto result = build_plugin_definition(config_.factory, config_.codes);
        if (result.ok) {
            definition_ = std::move(result.definition);
        } else {
            init_error_ = result.error;
        }
    }

    AAX_Result EffectInit() override {
        if (!init_error_.empty()) {
            return AAX_ERROR_UNIMPLEMENTED;
        }

        for (const auto& binding : definition_.parameters) {
            FloatTaperDelegate taper(binding.range);
            FloatDisplayDelegate display(binding);
            auto* parameter = new AAX_CParameter<float>(
                binding.aax_id.c_str(),
                AAX_CString(binding.name),
                binding.range.default_value,
                taper,
                display,
                true);
            parameter->SetType(binding.discrete ? AAX_eParameterType_Discrete : AAX_eParameterType_Continuous);
            if (binding.discrete && binding.step_count > 0) {
                parameter->SetNumberOfSteps(binding.step_count);
            }
            mParameterManager.AddParameter(parameter);
        }

        {
            state::ParamRange bypass_range{
                .min = 0.0f,
                .max = 1.0f,
                .default_value = 0.0f,
                .step = 1.0f,
            };
            FloatTaperDelegate taper(bypass_range);
            BypassDisplayDelegate display;
            auto* bypass = new AAX_CParameter<float>(
                cDefaultMasterBypassID,
                AAX_CString("Master Bypass"),
                0.0f,
                taper,
                display,
                true);
            bypass->SetType(AAX_eParameterType_Discrete);
            bypass->SetNumberOfSteps(2);
            mParameterManager.AddParameter(bypass);
            FilterParameterIDOnSave(cDefaultMasterBypassID);
        }

        if (auto* controller = Controller()) {
            controller->SetSignalLatency(definition_.latency_samples);
        }

        return GenerateCoefficients();
    }

    AAX_Result UpdateParameterNormalizedValue(AAX_CParamID parameter_id,
                                              double value,
                                              AAX_EUpdateSource source) override
    {
        return AAX_CEffectParameters::UpdateParameterNormalizedValue(parameter_id, value, source);
    }

    AAX_Result GenerateCoefficients() override {
        auto* controller = Controller();
        if (!controller) {
            return AAX_SUCCESS;
        }

        std::vector<float> packet(definition_.packet_float_count, 0.0f);
        if (auto* bypass = mParameterManager.GetParameterByID(cDefaultMasterBypassID)) {
            float bypass_value = 0.0f;
            bypass->GetValueAsFloat(&bypass_value);
            packet[0] = bypass_value >= 0.5f ? 1.0f : 0.0f;
        }

        for (std::size_t index = 0; index < definition_.parameters.size(); ++index) {
            if (auto* parameter = mParameterManager.GetParameterByID(definition_.parameters[index].aax_id.c_str())) {
                float value = definition_.parameters[index].range.default_value;
                parameter->GetValueAsFloat(&value);
                packet[index + 1] = value;
            }
        }

        controller->SetSignalLatency(definition_.latency_samples);
        return controller->PostPacket(
            kParameterPacketField,
            packet.data(),
            static_cast<uint32_t>(packet.size() * sizeof(float)));
    }

    AAX_Result ResetFieldData(AAX_CFieldIndex field_index, void* data, uint32_t data_size) const override {
        if (field_index == kPrivateDataField && data && data_size >= sizeof(PrivateDataBlock)) {
            destroy_instance(static_cast<PrivateDataBlock*>(data));
        }
        return AAX_CEffectParameters::ResetFieldData(field_index, data, data_size);
    }

    AAX_Result SetChunk(AAX_CTypeID chunk_id, const AAX_SPlugInChunk* chunk) override {
        const auto result = AAX_CEffectParameters::SetChunk(chunk_id, chunk);
        if (result == AAX_SUCCESS) {
            return GenerateCoefficients();
        }
        return result;
    }

private:
    EntryConfig config_{};
    PluginDefinition definition_{};
    std::string init_error_;
};

int32_t AAX_CALLBACK instance_init(const AlgorithmContext* context,
                                   AAX_EComponentInstanceInitAction action)
{
    if (!context || !context->private_data) {
        return 0;
    }

    switch (action) {
        case AAX_eComponentInstanceInitAction_AddingNewInstance:
        case AAX_eComponentInstanceInitAction_ResetInstance:
        case AAX_eComponentInstanceInitAction_RemovingInstance:
            destroy_instance(context->private_data);
            break;
        default:
            break;
    }

    return 0;
}

void AAX_CALLBACK process_callback(AlgorithmContext* const instances_begin[],
                                   const void* instances_end)
{
    const AlgorithmContext* const* current = instances_begin;
    const AlgorithmContext* const* end = static_cast<const AlgorithmContext* const*>(instances_end);
    while (current != end) {
        auto* context = *current++;
        if (!context || !context->private_data) {
            continue;
        }

        auto* block = context->private_data;
        if (!block->instance) {
            continue;
        }

        auto& state = *block->instance;
        if (!state.processor || state.definition.components.empty()) {
            continue;
        }
        const auto& definition = state.definition;

        const int sample_count = context->buffer_length ? std::max(0, *context->buffer_length) : 0;
        const double sample_rate = context->sample_rate ? static_cast<double>(*context->sample_rate) : 48000.0;

        const auto& component = definition.components.front();
        state.sync_from_packet(definition, context->parameter_packet);
        state.ensure_prepared(sample_rate,
                              sample_count,
                              component.main_input_channels,
                              component.main_output_channels);

        state.input_ptrs.clear();
        if (component.main_input_channels > 0 && context->audio_input) {
            state.input_ptrs.reserve(static_cast<std::size_t>(component.main_input_channels));
            for (int ch = 0; ch < component.main_input_channels; ++ch) {
                state.input_ptrs.push_back(context->audio_input[ch]);
            }
        }

        state.output_ptrs.clear();
        if (component.main_output_channels > 0 && context->audio_output) {
            state.output_ptrs.reserve(static_cast<std::size_t>(component.main_output_channels));
            for (int ch = 0; ch < component.main_output_channels; ++ch) {
                state.output_ptrs.push_back(context->audio_output[ch]);
            }
        }

        audio::BufferView<const float> audio_in;
        if (!state.input_ptrs.empty()) {
            audio_in = audio::BufferView<const float>(state.input_ptrs.data(),
                                                      state.input_ptrs.size(),
                                                      static_cast<std::size_t>(sample_count));
        }

        audio::BufferView<float> audio_out;
        if (!state.output_ptrs.empty()) {
            audio_out = audio::BufferView<float>(state.output_ptrs.data(),
                                                 state.output_ptrs.size(),
                                                 static_cast<std::size_t>(sample_count));
        }

        std::optional<audio::BufferView<const float>> sidechain_view;
        state.sidechain_ptrs.clear();
        if (component.sidechain_channels > 0
            && context->sidechain_start_index
            && *context->sidechain_start_index > 0
            && context->audio_input)
        {
            state.sidechain_ptrs.push_back(context->audio_input[*context->sidechain_start_index]);
            sidechain_view.emplace(state.sidechain_ptrs.data(),
                                   state.sidechain_ptrs.size(),
                                   static_cast<std::size_t>(sample_count));
            state.processor->set_sidechain(&*sidechain_view);
        } else {
            state.processor->set_sidechain(nullptr);
        }

        state.midi_in.clear();
        state.midi_out.clear();
        if (definition.supports_midi_input) {
            decode_midi_node(context->midi_input_node, &state.midi_in);
        }

        ProcessContext process_context{
            .sample_rate = sample_rate,
            .num_samples = sample_count,
        };
        if (definition.uses_transport && context->transport_node) {
            read_transport(context->transport_node->GetTransport(), &process_context);
        }

        const bool bypass = context->parameter_packet && context->parameter_packet[0] >= 0.5f;
        if (bypass) {
            if (component.main_output_channels > 0) {
                if (component.main_input_channels > 0) {
                    copy_audio(context->audio_output,
                               component.main_output_channels,
                               context->audio_input,
                               component.main_input_channels,
                               sample_count);
                } else {
                    clear_audio(context->audio_output, component.main_output_channels, sample_count);
                }
            }
            if (definition.supports_midi_output) {
                copy_midi(state.midi_in, state.midi_out);
            }
        } else {
            state.processor->process(audio_out, audio_in, state.midi_in, state.midi_out, process_context);
        }

        if (definition.supports_midi_output) {
            encode_midi_node(context->midi_output_node, state.midi_out);
        }
        state.processor->set_sidechain(nullptr);
    }
}

AAX_Result add_component_ports(AAX_IComponentDescriptor* component,
                               const PluginDefinition& definition,
                               const ComponentLayout& layout)
{
    if (layout.main_input_channels > 0 || layout.sidechain_channels > 0) {
        auto result = component->AddAudioIn(kAudioInputField);
        if (result != AAX_SUCCESS) return result;
    }
    if (layout.main_output_channels > 0) {
        auto result = component->AddAudioOut(kAudioOutputField);
        if (result != AAX_SUCCESS) return result;
    }

    if (auto result = component->AddAudioBufferLength(kBufferLengthField); result != AAX_SUCCESS) return result;
    if (auto result = component->AddSampleRate(kSampleRateField); result != AAX_SUCCESS) return result;
    if (auto result = component->AddDataInPort(
            kParameterPacketField,
            static_cast<uint32_t>(definition.packet_float_count * sizeof(float)),
            AAX_eDataInPortType_Buffered);
        result != AAX_SUCCESS)
    {
        return result;
    }
    if (auto result = component->AddPrivateData(kPrivateDataField, sizeof(PrivateDataBlock)); result != AAX_SUCCESS) {
        return result;
    }

    if (definition.uses_transport) {
        if (auto result = component->AddMIDINode(kTransportField,
                                                 AAX_eMIDINodeType_Transport,
                                                 "Transport",
                                                 0);
            result != AAX_SUCCESS)
        {
            return result;
        }
    }

    if (layout.sidechain_channels > 0) {
        if (auto result = component->AddSideChainIn(kSidechainIndexField); result != AAX_SUCCESS) return result;
    }

    if (definition.supports_midi_input) {
        if (auto result = component->AddMIDINode(kMidiInputField,
                                                 AAX_eMIDINodeType_LocalInput,
                                                 "MIDI In",
                                                 0xffffu);
            result != AAX_SUCCESS)
        {
            return result;
        }
    }

    if (definition.supports_midi_output) {
        if (auto result = component->AddMIDINode(kMidiOutputField,
                                                 AAX_eMIDINodeType_LocalOutput,
                                                 "MIDI Out",
                                                 0xffffu);
            result != AAX_SUCCESS)
        {
            return result;
        }
    }

    return AAX_SUCCESS;
}

AAX_Result add_component_process(AAX_IComponentDescriptor* component,
                                 const PluginDefinition& definition,
                                 const ComponentLayout& layout)
{
    auto* properties = component->NewPropertyMap();
    if (!properties) {
        return AAX_ERROR_UNIMPLEMENTED;
    }

    auto add = [&](AAX_EProperty property, AAX_CPropertyValue value) {
        return properties->AddProperty(property, value);
    };

    if (auto result = add(AAX_eProperty_ManufacturerID,
                          static_cast<AAX_CPropertyValue>(definition.codes.manufacturer_id));
        result != AAX_SUCCESS) return result;
    if (auto result = add(AAX_eProperty_ProductID,
                          static_cast<AAX_CPropertyValue>(definition.codes.product_id));
        result != AAX_SUCCESS) return result;
    if (auto result = add(AAX_eProperty_PlugInID_Native,
                          static_cast<AAX_CPropertyValue>(layout.native_plugin_id));
        result != AAX_SUCCESS) return result;
    if (auto result = add(AAX_eProperty_InputStemFormat,
                          static_cast<AAX_CPropertyValue>(stem_to_aax(layout.input_stem)));
        result != AAX_SUCCESS) return result;
    if (auto result = add(AAX_eProperty_OutputStemFormat,
                          static_cast<AAX_CPropertyValue>(stem_to_aax(layout.output_stem)));
        result != AAX_SUCCESS) return result;
    if (auto result = add(AAX_eProperty_LatencyContribution,
                          static_cast<AAX_CPropertyValue>(definition.latency_samples));
        result != AAX_SUCCESS) return result;
    if (auto result = add(AAX_eProperty_CanBypass, 1); result != AAX_SUCCESS) return result;
    if (auto result = add(AAX_eProperty_Constraint_MultiMonoSupport, 0); result != AAX_SUCCESS) return result;
    if (definition.supports_sidechain) {
        if (auto result = add(AAX_eProperty_SupportsSideChainInput, 1); result != AAX_SUCCESS) return result;
        if (auto result = add(AAX_eProperty_SideChainStemFormat,
                              static_cast<AAX_CPropertyValue>(AAX_eStemFormat_Mono));
            result != AAX_SUCCESS) return result;
    }

    return component->AddProcessProc_Native<AlgorithmContext>(
        &process_callback,
        properties,
        &instance_init);
}

} // namespace

IACFUnknown* create_effect_parameters(const EntryConfig& config) {
    return static_cast<IACFUnknown*>(new EffectParameters(config));
}

AAX_Result get_effect_descriptions(AAX_ICollection* out_collection,
                                   const EntryConfig& config,
                                   AAXCreateObjectProc create_proc)
{
    if (!out_collection || !create_proc) {
        return AAX_ERROR_UNIMPLEMENTED;
    }

    auto result = build_plugin_definition(config.factory, config.codes);
    if (!result.ok) {
        return AAX_ERROR_UNIMPLEMENTED;
    }
    const auto& definition = result.definition;

    if (auto status = out_collection->SetManufacturerName(definition.descriptor.manufacturer.c_str());
        status != AAX_SUCCESS)
    {
        return status;
    }

    if (auto status = out_collection->AddPackageName(definition.descriptor.name.c_str()); status != AAX_SUCCESS) {
        return status;
    }
    const auto short_package_name = truncate_copy(definition.descriptor.name, 16);
    if (short_package_name != definition.descriptor.name) {
        if (auto status = out_collection->AddPackageName(short_package_name.c_str()); status != AAX_SUCCESS) {
            return status;
        }
    }

    if (auto status = out_collection->SetPackageVersion(pack_semver(definition.descriptor.version));
        status != AAX_SUCCESS)
    {
        return status;
    }

    auto* effect = out_collection->NewDescriptor();
    if (!effect) {
        return AAX_ERROR_UNIMPLEMENTED;
    }

    if (auto status = effect->AddName(definition.descriptor.name.c_str()); status != AAX_SUCCESS) {
        return status;
    }
    const auto short_effect_name = truncate_copy(definition.descriptor.name, 31);
    if (short_effect_name != definition.descriptor.name) {
        if (auto status = effect->AddName(short_effect_name.c_str()); status != AAX_SUCCESS) {
            return status;
        }
    }

    if (auto status = effect->AddCategory(category_to_aax(definition.descriptor.category)); status != AAX_SUCCESS) {
        return status;
    }
    if (auto status = effect->SetRole(AAX_ePlugInRole_InsertOrAudioSuite); status != AAX_SUCCESS) {
        return status;
    }
    if (auto status = effect->AddProcPtr(reinterpret_cast<void*>(create_proc),
                                         kAAX_ProcPtrID_Create_EffectParameters);
        status != AAX_SUCCESS)
    {
        return status;
    }

    if (auto* effect_properties = effect->NewPropertyMap()) {
        if (auto status = effect_properties->AddProperty(AAX_eProperty_SupportsSaveRestore, 1);
            status != AAX_SUCCESS)
        {
            return status;
        }
        if (auto status = effect_properties->AddProperty(AAX_eProperty_UsesTransport,
                                                         definition.uses_transport ? 1 : 0);
            status != AAX_SUCCESS)
        {
            return status;
        }
        if (auto status = effect->SetProperties(effect_properties); status != AAX_SUCCESS) {
            return status;
        }
    }

    for (const auto& component_layout : definition.components) {
        auto* component = effect->NewComponentDescriptor();
        if (!component) {
            return AAX_ERROR_UNIMPLEMENTED;
        }
        if (auto status = add_component_ports(component, definition, component_layout); status != AAX_SUCCESS) {
            return status;
        }
        if (auto status = add_component_process(component, definition, component_layout); status != AAX_SUCCESS) {
            return status;
        }
        if (auto status = effect->AddComponent(component); status != AAX_SUCCESS) {
            return status;
        }
    }

    return out_collection->AddEffect(definition.descriptor.bundle_id.c_str(), effect);
}

} // namespace pulp::format::aax
