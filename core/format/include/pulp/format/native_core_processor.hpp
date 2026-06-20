// pulp/format/native_core_processor.hpp
//
// A C++ `pulp::format::Processor` that owns a native-language DSP core through
// the language-neutral C ABI (pulp/native_components/native_core.h). This is
// the SDK seam for native-language DSP cores: a Rust / C / Zig core
// implements the C ABI vtable; this adapter translates Pulp's C++ Processor
// surface (StateStore params, ParameterEventQueue, BufferView, state blobs)
// into the POD FFI structs and back.
//
// The adapter itself has NO Rust dependency — it bridges any core that exports a
// `pulp_native_core_v1`. It is part of `pulp-format` and always builds; the Rust
// binding + reference cores are the separate opt-in lane.
//
// RT-safety: all scratch (channel-pointer arrays, the native param-event buffer)
// is allocated in prepare(); process() does no allocation, locking, or logging.
#pragma once

#include <pulp/native_components/native_core.h>

#include <pulp/audio/buffer.hpp>
#include <pulp/format/processor.hpp>
#include <pulp/midi/buffer.hpp>
#include <pulp/state/store.hpp>

#include <atomic>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace pulp::format {

/// Owns a native DSP core (via the C ABI vtable) and exposes it as a Processor.
///
/// Construct with a vtable obtained from a core's `pulp_native_core_entry_v1()`.
/// The adapter creates one core instance and drives its lifecycle. Ownership of
/// the vtable pointer stays with the caller (it is process-static); the instance
/// is owned by the adapter.
class NativeCoreProcessor : public Processor {
public:
    /// @param core  Non-null vtable from a native core's entry point. Must
    ///              report a compatible abi_version or construction is inert
    ///              (descriptor() returns a minimal placeholder and process()
    ///              outputs silence).
    explicit NativeCoreProcessor(const pulp_native_core_v1* core);
    ~NativeCoreProcessor() override;

    NativeCoreProcessor(const NativeCoreProcessor&) = delete;
    NativeCoreProcessor& operator=(const NativeCoreProcessor&) = delete;

    PluginDescriptor descriptor() const override;
    void define_parameters(state::StateStore& store) override;
    void prepare(const PrepareContext& context) override;
    void release() override;
    void suspend() override;
    void resume() override;
    void process(audio::BufferView<float>& audio_output,
                 const audio::BufferView<const float>& audio_input,
                 midi::MidiBuffer& midi_in,
                 midi::MidiBuffer& midi_out,
                 const ProcessContext& context) override;
    std::vector<uint8_t> serialize_plugin_state() const override;
    bool deserialize_plugin_state(std::span<const uint8_t> data) override;
    int latency_samples() const override;

    /// Non-RT (host/UI thread) command channel into the native core's domain
    /// logic: send a JSON request, get a JSON reply. This is the seam for
    /// Rust-owned non-real-time work — preset/sample browsers, metadata,
    /// import/export, analysis — never the audio thread.
    ///
    /// Returns std::nullopt when the core declares no editor-command support,
    /// rejects the request (malformed input returns nullopt, not an exception),
    /// or the adapter is inert. A successful command with no payload returns an
    /// empty string.
    std::optional<std::string> editor_command(std::string_view request_json);

    /// True once a compatible core instance was created. When false the adapter
    /// is inert (silent) but safe to use.
    bool valid() const { return instance_ != nullptr; }

private:
    // Host-service callbacks handed to the core. `ctx` is always `this`.
    static void* host_alloc(void* ctx, std::size_t bytes);
    static void host_free(void* ctx, void* ptr);
    static void host_log(void* ctx, int32_t level, const char* utf8, std::size_t len);
    static int32_t host_push_midi_out(void* sink, const pulp_native_midi_event_v1* ev);
    static void host_notify_latency_changed(void* ctx);

    const pulp_native_core_v1* core_ = nullptr;
    pulp_native_instance* instance_ = nullptr;
    pulp_native_host_services_v1 host_{};

    // Native parameter id-hash per Pulp ParamID (ParamID == registration index).
    std::vector<uint64_t> paramid_to_hash_;

    // Preallocated process() scratch (filled per block, never resized in RT).
    std::vector<float*> in_ptrs_;
    std::vector<float*> out_ptrs_;
    std::vector<pulp_native_audio_bus_v1> in_buses_;
    std::vector<pulp_native_audio_bus_v1> out_buses_;
    std::vector<pulp_native_param_event_v1> event_scratch_;

    PrepareContext prepare_ctx_{};
    bool prepared_ = false;
    std::atomic<bool> latency_changed_{false};
};

}  // namespace pulp::format
