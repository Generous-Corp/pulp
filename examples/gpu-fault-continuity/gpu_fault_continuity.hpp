#pragma once

// GPU fault-continuity test plug-in (WP-h1).
//
// A stereo GpuConvolver on the authenticated shared-I/O Dawn path, driven
// through GpuAudioTransport with the continuously primed CPU fallback. It is
// the plug-in tools/testing/daw-smoke/reaper_smoke.py --mode gpu-fault loads
// in REAPER: the harness asks it, through a request file, to fire one GPU
// fault when the host playhead crosses a given project sample, and the
// plug-in records everything needed to prove audio stayed continuous:
//
//   capture.bin   every callback's input and output, with the project
//                 position and flags (format in gpu_fault_continuity.py)
//   events.jsonl  prepared / fault_armed / fault_fired / health snapshots
//
// The callback only copies into a lock-free ring; a host thread owns every
// file, the fault injection and the health snapshots.
//
// Test-only: built only with -DPULP_GPU_AUDIO_FAULT_HOOKS=ON. Without the
// harness's environment it behaves as a plain GPU convolver.

#include <pulp/format/processor.hpp>
#include <pulp/gpu_audio/gpu_audio_transport.hpp>
#include <pulp/gpu_audio/gpu_convolver.hpp>

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace pulp::examples::gpu_fault {

enum class FaultKind : std::uint8_t {
    None,
    ForceLossBefore,
    ForceLossBetween,
    ForceLossAfter,
    DelayCompletion,
    // The provider's worker sleeps inside ProcessEvents: completions arrive
    // late, the GPU itself is fine. (Formerly "hang".)
    WorkerStall,
    // One queue-completion callback is swallowed, so the provider waits on a
    // submission that never reports done: the hung-device route.
    MissingCompletion,
    RenderForceLoss,
    // Positive control: fires at the sample, takes the same snapshots, faults
    // nothing. Named "none" in requests and receipts.
    Control,
};

// Planted negatives, test-only, applied from the fault block on. Each makes the
// audio continuous but wrong (or absent) in a way the null must catch.
enum class Planted : std::uint8_t {
    Off,
    NoFallback,         // fallback blocks are replaced by silence
    FallbackMisaligned, // output is delayed by one extra quantum
    FallbackStale,      // one quantum of input is withheld from the node
};

const char* fault_name(FaultKind kind) noexcept;
FaultKind parse_fault(const std::string& name) noexcept;

// Single-producer (audio thread) / single-consumer (service thread) byte ring.
// A record is written whole or not at all; a dropped record is counted, and a
// non-zero count invalidates the capture.
class CaptureRing {
  public:
    void reset(std::size_t capacity);
    bool push(const void* header, std::size_t header_size, const float* const* planes,
              std::size_t plane_count, std::size_t frames) noexcept;
    std::size_t drain(std::vector<std::uint8_t>& out);
    void note_drop() noexcept {
        dropped_.fetch_add(1, std::memory_order_relaxed);
    }
    std::uint64_t dropped() const noexcept {
        return dropped_.load(std::memory_order_relaxed);
    }

  private:
    void write_bytes(std::size_t at, const void* src, std::size_t size) noexcept;

    std::vector<std::uint8_t> buffer_;
    std::atomic<std::size_t> head_{0}; // written by the producer
    std::atomic<std::size_t> tail_{0}; // written by the consumer
    std::atomic<std::uint64_t> dropped_{0};
};

class GpuFaultContinuityProcessor : public format::Processor {
  public:
    static constexpr std::uint32_t kChannels = 2;
    // Bounds for the transport quantum, which is the host's prepared maximum
    // block. Host callbacks of any size are staged into it, which adds exactly
    // one quantum of latency on top of the GPU lead.
    static constexpr std::uint32_t kMinQuantum = 64;
    static constexpr std::uint32_t kMaxQuantum = 4096;

    GpuFaultContinuityProcessor();
    ~GpuFaultContinuityProcessor() override;

    format::PluginDescriptor descriptor() const override;
    void define_parameters(state::StateStore&) override {}
    void prepare(const format::PrepareContext& context) override;
    void release() override;
    int latency_samples() const override;
    std::uint32_t quantum() const noexcept {
        return quantum_;
    }
    void process(audio::BufferView<float>& output, const audio::BufferView<const float>& input,
                 midi::MidiBuffer&, midi::MidiBuffer&,
                 const format::ProcessContext& context) override;

    std::unique_ptr<view::View> create_view() override;
    std::pair<uint32_t, uint32_t> editor_size() const override {
        return {360, 160};
    }

    // Editor side of render_force_loss. An open editor installs a hook (UI
    // thread) that queues the loss onto the main thread; the service thread
    // calls it when the fault fires, so the loss does not depend on the editor
    // painting during a modal render. The editor reports the outcome back.
    void set_render_loss_hook(std::function<void()> hook);
    bool render_loss_requested() const noexcept {
        return render_loss_requested_.load(std::memory_order_acquire);
    }
    void report_render_loss(bool performed, const std::string& detail);

    // Host thread. Which path delivered each transport quantum so far.
    gpu_audio::GpuAudioTransport::DeliverySnapshot delivery() const;

  private:
    void claim_run_dir(std::uint32_t sample_rate);
    void run_quantum(bool ready, bool offline, Planted planted) noexcept;
    void prepare_engine();
    void release_engine();
    void service_loop();
    void poll_request();
    void fire_pending_fault();
    void maybe_snapshot_after();
    void emit(const std::string& json_line);
    void emit_health(const char* phase);
    std::string status_json() const;

    // Environment, read once at construction.
    std::filesystem::path run_dir_;
    std::filesystem::path request_path_;
    std::filesystem::path ir_path_;
    bool harness_ = false;

    // Engine (host thread; guarded by engine_mutex_ against the service thread).
    mutable std::mutex engine_mutex_;
    std::unique_ptr<gpu_audio::GpuConvolver> node_;
    std::unique_ptr<gpu_audio::GpuAudioTransport> transport_;
    std::vector<float> ir_;
    std::uint32_t sample_rate_ = 48000;
    bool shared_path_ = false;
    std::string provider_name_ = "unknown";
    std::string path_name_ = "unavailable";

    // Audio-thread state.
    std::vector<float> stage_in_;  // [channel][quantum]
    std::vector<float> stage_out_; // [channel][quantum]
    std::uint32_t stage_fill_ = 0;
    std::uint32_t quantum_ = 512;
    std::uint32_t prepare_index_ = 0;
    // Request-selected test shapes; written by the service thread once, read
    // by the audio thread.
    std::atomic<std::uint8_t> planted_{0};
    std::atomic<bool> internal_stimulus_{false};
    std::uint32_t stimulus_state_ = 0x5EEDu;
    std::vector<float> stage_prev_; // FallbackMisaligned: the previous quantum's output
    bool planted_live_ = false;     // audio thread: the fault block has passed
    bool stale_done_ = false;
    std::uint64_t target_device_ = 0; // this instance's provider device, for binding
    bool gpu_off_ = false;            // PULP_GPU_FAULT_PROVIDER=staged: path negative
    std::vector<const float*> in_ptrs_;
    std::vector<float*> out_ptrs_;
    std::vector<float> scratch_in_; // [channel][max block] contiguous copy for capture
    std::vector<float> scratch_out_;
    std::uint32_t max_block_ = 0;
    bool segment_start_ = true;
    CaptureRing ring_;

    // Audio -> service handshake.
    std::atomic<bool> engine_ready_{false};
    std::atomic<std::uint8_t> armed_kind_{0};
    std::atomic<std::int64_t> fault_at_{-1};
    std::atomic<std::int64_t> after_at_{-1};
    std::atomic<bool> fire_pending_{false};
    std::atomic<bool> fire_offline_{false};
    std::atomic<std::int64_t> latest_position_{-1};
    std::atomic<bool> render_loss_requested_{false};

    // Service thread.
    std::thread service_;
    std::atomic<bool> running_{false};
    std::mutex events_mutex_;
    std::vector<std::string> pending_events_;
    bool request_seen_ = false;
    bool fired_ = false;
    bool at_taken_ = false;
    bool after_taken_ = false;
    std::uint64_t fired_ns_ = 0;
    std::uint64_t applied_ns_ = 0;
    std::uint64_t applied_before_ = 0;
    bool applied_reported_ = false;
    std::uint32_t delay_ms_ = 100;
    std::string nonce_;
    std::uint32_t latency_samples_ = 3 * 512;
    std::mutex hook_mutex_;
    std::function<void()> render_loss_hook_;
    std::atomic<int> render_loss_state_{0}; // 0 none, 1 performed, 2 refused
};

std::unique_ptr<format::Processor> create_gpu_fault_continuity();

} // namespace pulp::examples::gpu_fault
