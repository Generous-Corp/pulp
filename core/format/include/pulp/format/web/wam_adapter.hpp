#pragma once

// WAMv2 (Web Audio Modules v2) format adapter for Pulp
// Wraps a Pulp Processor as a WAMv2 plugin for browser-based DAWs.
//
// Architecture (WAMv2 spec: https://github.com/webaudiomodules/api):
//   WebAudioModule (JS, main thread)
//     ├── WamDescriptor — plugin metadata from Processor::descriptor()
//     ├── WamNode (AudioWorkletNode) — main-thread proxy
//     │     ├── parameter info/values bridge
//     │     ├── state save/load
//     │     └── event scheduling (MIDI, automation, transport)
//     └── WamProcessor (AudioWorkletProcessor) — audio thread
//           ├── Pulp Processor (WASM-compiled C++)
//           ├── StateStore parameter sync
//           └── MIDI event processing
//
// The C++ side provides:
//   - WamDescriptorData: serializable metadata for JS WamDescriptor
//   - WamProcessorBridge: audio-thread processing callable from AudioWorklet
//   - Emscripten exports for JS interop
//
// The JS side (wam-plugin.js) wraps this as a standard WAMv2 module.

#include <pulp/format/processor.hpp>
#include <string>
#include <vector>
#include <cstdint>

namespace pulp::format::wam {

// Serializable descriptor matching WAMv2 WamDescriptor schema
struct WamDescriptorData {
    std::string name;
    std::string vendor;
    std::string version;
    std::string api_version = "2.0.0";
    std::string description;
    std::string website;
    std::string thumbnail;
    std::vector<std::string> keywords;
    bool is_instrument = false;
    bool has_audio_input = true;
    bool has_audio_output = true;
    bool has_midi_input = false;
    bool has_midi_output = false;
    bool has_automation_input = true;
    bool has_automation_output = false;
    bool has_mpe_input = false;
    bool has_mpe_output = false;

    // Build from a Pulp PluginDescriptor
    static WamDescriptorData from_processor(const PluginDescriptor& desc);

    // Serialize to JSON string for JS consumption
    std::string to_json() const;
};

// WAMv2 parameter info matching WamParameterInfo schema
struct WamParamInfo {
    std::string id;          // string ID (WAMv2 uses string IDs)
    std::string label;
    std::string type;        // "float", "int", "boolean", "choice"
    float default_value;
    float min_value;
    float max_value;
    float step;              // raw parameter step (e.g. 0.1); 0 = continuous
    int discrete_step;       // integer step for int/boolean params; 0 otherwise
    std::string unit;
};

// Audio-thread bridge: wraps a Processor for AudioWorkletProcessor calls
class WamProcessorBridge {
public:
    WamProcessorBridge(ProcessorFactory factory);
    ~WamProcessorBridge();

    // Called once from AudioWorkletProcessor.constructor()
    bool initialize(double sample_rate, int max_block_size);

    // Called each AudioWorkletProcessor.process() frame
    // input/output are interleaved float arrays (Web Audio layout)
    void process(const float* input, float* output,
                 int num_channels, int num_frames);

    // Parameter access (WAMv2 uses string IDs)
    std::vector<WamParamInfo> get_parameter_info() const;
    // Parameter metadata as a JSON array for generated web controls:
    // [{id,label,type,unit,defaultValue,minValue,maxValue,discreteStep}, ...]
    std::string parameters_json() const;
    float get_parameter_value(const std::string& id) const;
    void set_parameter_value(const std::string& id, float value);

    // MIDI events from WAMv2 event system
    void schedule_midi(uint8_t status, uint8_t data1, uint8_t data2,
                       int sample_offset);

    // Drain the MIDI the processor produced during the LAST process() call.
    //
    // Copies up to `cap` bytes into `dst` and returns the number of bytes that
    // were AVAILABLE — so a caller passing too small a buffer sees size > cap
    // and knows the tail was truncated. Records are packed back-to-back:
    //
    //     [int32 sample_offset][uint16 byte_len][byte_len raw MIDI bytes]
    //
    // Short messages carry 1..3 bytes; SysEx carries the full F0..F7 payload,
    // so one drain call handles both and the reader dispatches on the first
    // byte. Records are emitted short-messages-first, then SysEx; consumers
    // that need strict time order should stable-sort on sample_offset.
    //
    // Called from the audio thread immediately after process(): it is a single
    // memcpy out of a buffer serialized during process(), so it neither
    // allocates nor locks.
    int drain_midi_out(uint8_t* dst, int cap) const;

    // State persistence
    std::vector<uint8_t> get_state() const;
    bool set_state(const uint8_t* data, size_t size);

    // Descriptor
    WamDescriptorData descriptor() const;

private:
    ProcessorFactory factory_;
    std::unique_ptr<Processor> processor_;
    state::StateStore store_;

    // De-interleave buffers (Web Audio uses interleaved, Pulp uses planar)
    std::vector<float> input_planar_;
    std::vector<float> output_planar_;
    std::vector<float*> input_ptrs_;
    std::vector<float*> output_ptrs_;

    // Bounds for the adapter-owned MIDI buffers. They are reserved once in
    // initialize() and run with set_realtime_capacity_limit(true), so add()
    // drops on overflow rather than reallocating on the audio thread.
    static constexpr std::size_t kMidiEventCapacity = 256;
    static constexpr std::size_t kSysexCapacity = 16;
    static constexpr std::size_t kSysexPayloadCapacity = 512;

    // Persistent across blocks — never reallocated during process().
    // pending_midi_ is filled by schedule_midi() between render quanta and is
    // handed to the processor as midi_in; midi_out_ receives what it emits.
    midi::MidiBuffer pending_midi_;
    midi::MidiBuffer midi_out_;

    // Serialized midi_out_ records, written at the end of process() and copied
    // out by drain_midi_out(). Sized once; `midi_out_bytes_` is the live length.
    std::vector<uint8_t> midi_out_scratch_;
    std::size_t midi_out_bytes_ = 0;

    double sample_rate_ = 48000.0;
    int num_channels_ = 2;
    int block_size_ = 128;
};

} // namespace pulp::format::wam
