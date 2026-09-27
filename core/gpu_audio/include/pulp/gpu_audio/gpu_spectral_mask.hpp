#pragma once
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>

namespace pulp::gpu_audio {
// First shared spectral seam: immutable real gains, Hann WOLA, fixed planar
// hops. Every method is serialized non-RT; callers own callback bridging and
// continuously advanced CPU fallback. The session performs ordinary CPU copies
// into/out of shared slots, but no WebGPU payload upload/readback copy.
class GpuSpectralMaskSession {
public:
    struct Config {
        std::uint32_t fft_size = 0, hop = 0, channels = 0, sample_rate = 0;
        std::uint32_t slots = 3;
        std::span<const float> gains; // DC through Nyquist, fft_size/2+1 values
    };
    enum class Error { None, InvalidConfig, ProviderUnavailable, PreparationFailed };
    struct CreateResult {
        std::unique_ptr<GpuSpectralMaskSession> session;
        Error error = Error::None;
        explicit operator bool() const noexcept { return session && error == Error::None; }
    };
    struct Result {
        std::uint64_t epoch = 0, sequence = 0;
        // Physical success only: the callback owner must reject late output.
        bool delivered = false, late = false;
    };
    struct Diagnostics {
        std::string dawn_revision, adapter_name;
        std::uint32_t vendor_id = 0, device_id = 0;
        // Header/native/proc revision agreement plus Metal host-pointer import.
        // This does not imply a separately configured manifest pin was checked.
        bool authenticated_shared_metal = false, configured_revision_verified = false;
        bool physical_release_confirmed = false;
        std::uint64_t cpu_input_bytes = 0, cpu_output_bytes = 0;
        std::uint64_t runtime_write_buffer_calls = 0, runtime_copy_buffer_calls = 0,
                      runtime_map_async_calls = 0, imported_allocations = 0,
                      retired_success = 0, retired_failure = 0;
    };
    // Non-RT snapshot. Transfer counters exclude resource initialization.
    Diagnostics diagnostics() const;
    static CreateResult create(const Config&) noexcept;
    ~GpuSpectralMaskSession();
    bool prepared() const noexcept;
    std::uint32_t latency_samples() const noexcept; // intrinsic FFT+hop only
    // Process-unique stream identity, never reused by a recreated session.
    std::uint64_t epoch() const noexcept;
    // A refusal does not advance history. Retry the same sequence/input or
    // drain and recreate; silently dropping hops invalidates causal history.
    bool submit_hop(std::span<const float> planar, std::uint64_t sequence,
                    std::uint64_t deadline_ns = 0) noexcept;
    std::size_t service(std::uint64_t now_ns) noexcept;
    // A failed result poisons this epoch: prepared() becomes false, future
    // submissions are refused, and remaining completions are retired without
    // delivery. Continue service/receive or release, then recreate the session
    // with a new epoch; a physical success cannot repair missing DSP history.
    std::optional<Result> receive(std::span<float> planar) noexcept;
    // Retain the session and retry if its physical drain cannot yet finish.
    bool release() noexcept;
private:
    struct Impl;
    explicit GpuSpectralMaskSession(std::unique_ptr<Impl>) noexcept;
    std::unique_ptr<Impl> impl_;
};
}
