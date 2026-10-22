#pragma once

// Scriptable render::GpuCompute for GPU-audio tests that must not depend on a
// real adapter: device loss, generation changes, and async readbacks that
// succeed, expire, fail, or never resolve are all set by the test.
//
// Only the operations the GPU-audio nodes use do real work:
// - convolve_batch / convolve_batch_async compute the exact linear convolution
//   of each packed channel slot with the IR given to the constructor, in the
//   interleaved-complex layout the real batch plan returns (real samples at even
//   indices, already time-domain).
// - fft_forward / fft_inverse copy their input (enough to see whether a call
//   reached the device), multi_convolve and spectral_stack_render write a
//   constant marker.
// Every other operation fails. Each call that reaches the device is counted.

#include <pulp/render/gpu_compute.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <limits>
#include <string>
#include <utility>
#include <vector>

namespace pulp::test {

class FakeGpuCompute final : public render::GpuCompute {
  public:
    enum class AsyncMode : std::uint8_t {
        Resolve,     ///< complete Success on the next poll, writing the result
        Expire,      ///< complete Expired on the next poll, dest untouched
        Fail,        ///< complete Failed on the next poll, dest untouched
        Hang,        ///< accept, never complete (a provider that stopped resolving)
        Late,        ///< complete Expired only once the request's deadline has passed
        Delayed,     ///< complete Success on the `set_delay_polls()`-th poll after submit
        DelayedTime, ///< complete Success once `set_delay_time()` has passed since submit
        Reject,      ///< refuse the submit (returns 0)
    };

    static constexpr float kMultiMarker = 0.25f;
    static constexpr float kStackMarker = 0.5f;

    explicit FakeGpuCompute(std::vector<float> ir = {}) : ir_(std::move(ir)) {
        initialized_.store(true, std::memory_order_release);
    }

    // ── Test controls ───────────────────────────────────────────────────────
    /// Scale every convolution result (default 1). A gain of 0 makes GPU
    /// output distinguishable from a CPU continuation in the node's output.
    void set_result_gain(float gain) noexcept {
        result_gain_ = gain;
    }
    void set_async_mode(AsyncMode mode) noexcept {
        async_mode_ = mode;
    }
    /// Skip the reference convolution and return zeros (for timing the node's
    /// own overhead with long IRs, where the direct convolution is too slow).
    void set_compute_results(bool compute) noexcept {
        compute_results_ = compute;
    }
    void set_delay_time(std::chrono::microseconds delay) noexcept {
        delay_time_ = delay;
    }
    std::size_t max_in_flight() const noexcept {
        return max_in_flight_;
    }
    void set_delay_polls(std::uint32_t polls) noexcept {
        delay_polls_ = polls;
    }
    /// Write a NaN into the next successful result (async or blocking).
    void poison_next_result() noexcept {
        poison_next_ = true;
    }
    void set_blocking_ok(bool ok) noexcept {
        blocking_ok_ = ok;
    }
    void lose_device() noexcept {
        lost_.store(true, std::memory_order_release);
    }
    /// A re-initialization on the same object: new generation, not lost.
    void reinitialize() noexcept {
        lost_.store(false, std::memory_order_release);
        generation_.fetch_add(1, std::memory_order_acq_rel);
    }

    std::uint64_t convolve_calls() const noexcept {
        return convolve_calls_;
    }
    std::uint64_t async_submits() const noexcept {
        return async_submits_;
    }
    std::uint64_t fft_calls() const noexcept {
        return fft_calls_;
    }
    std::uint64_t multi_calls() const noexcept {
        return multi_calls_;
    }
    std::uint64_t stack_calls() const noexcept {
        return stack_calls_;
    }

    // ── Device state ────────────────────────────────────────────────────────
    bool initialize_from_surface(render::GpuSurface&) override {
        return false;
    }
    bool initialize_standalone() override {
        return false;
    }
    bool initialize_from_device(void*) override {
        return false;
    }
    bool device_lost() const override {
        return lost_.load(std::memory_order_acquire);
    }
    std::uint64_t device_generation() const override {
        return generation_.load(std::memory_order_acquire);
    }
    void notify_device_lost() override {
        lose_device();
    }

    // ── Operations the GPU-audio nodes use ──────────────────────────────────
    bool fft_forward(const float* in, float* out, uint32_t n) override {
        ++fft_calls_;
        if (device_lost())
            return false;
        std::copy_n(in, static_cast<std::size_t>(n) * 2u, out);
        return true;
    }
    bool fft_inverse(const float* in, float* out, uint32_t n) override {
        return fft_forward(in, out, n);
    }
    bool prepare_convolution_batch(uint32_t, const float*, uint32_t) override {
        return true;
    }
    bool convolve_batch(const float* in, float* out, uint32_t n, uint32_t batch) override {
        ++convolve_calls_;
        if (device_lost() || !blocking_ok_)
            return false;
        convolve_into(in, out, n, batch);
        if (poison_next_) {
            out[0] = std::numeric_limits<float>::quiet_NaN();
            poison_next_ = false;
        }
        return true;
    }
    uint64_t convolve_batch_async(const float* in, float* out, uint32_t n, uint32_t batch,
                                  std::chrono::microseconds deadline,
                                  ReadbackCallback cb) override {
        if (!cb || in == nullptr || out == nullptr)
            return 0;
        if (async_mode_ == AsyncMode::Reject || pending_.size() >= max_in_flight_)
            return 0;
        ++async_submits_;
        Pending p;
        p.id = next_id_++;
        p.dest = out;
        p.mode = device_lost() ? AsyncMode::Fail : async_mode_;
        p.due = std::chrono::steady_clock::now() + deadline;
        p.polls_left = delay_polls_;
        if (p.mode == AsyncMode::DelayedTime)
            p.due = std::chrono::steady_clock::now() + delay_time_;
        p.cb = std::move(cb);
        if (p.mode == AsyncMode::Resolve || p.mode == AsyncMode::Delayed ||
            p.mode == AsyncMode::DelayedTime) {
            p.result.assign(static_cast<std::size_t>(n) * 2u * batch, 0.0f);
            convolve_into(in, p.result.data(), n, batch);
            if (poison_next_) {
                p.result[0] = std::numeric_limits<float>::quiet_NaN();
                poison_next_ = false;
            }
        }
        pending_.push_back(std::move(p));
        return pending_.back().id;
    }
    std::size_t poll_readbacks() override {
        for (auto& pending : pending_)
            if (pending.mode == AsyncMode::Delayed && pending.polls_left > 0)
                --pending.polls_left;
        while (!pending_.empty() && pending_.front().mode != AsyncMode::Hang &&
               (pending_.front().mode != AsyncMode::Delayed || pending_.front().polls_left == 0) &&
               ((pending_.front().mode != AsyncMode::Late &&
                 pending_.front().mode != AsyncMode::DelayedTime) ||
                std::chrono::steady_clock::now() >= pending_.front().due)) {
            Pending p = std::move(pending_.front());
            pending_.pop_front();
            ReadbackResult r;
            r.id = p.id;
            if ((p.mode == AsyncMode::Resolve || p.mode == AsyncMode::Delayed ||
                 p.mode == AsyncMode::DelayedTime) &&
                !device_lost()) {
                std::copy(p.result.begin(), p.result.end(), p.dest);
                r.status = ReadbackStatus::Success;
                r.bytes = static_cast<uint32_t>(p.result.size() * sizeof(float));
            } else {
                r.status = p.mode == AsyncMode::Expire || p.mode == AsyncMode::Late
                               ? ReadbackStatus::Expired
                               : ReadbackStatus::Failed;
            }
            p.cb(r);
        }
        return pending_.size();
    }
    std::size_t readbacks_in_flight() const override {
        return pending_.size();
    }
    void set_max_readbacks_in_flight(std::size_t max_in_flight) override {
        max_in_flight_ = max_in_flight;
    }
    AsyncStats async_stats() const override {
        return {};
    }

    bool prepare_multi_convolution(uint32_t, const float*, uint32_t) override {
        return true;
    }
    bool multi_convolve(const float*, const float*, const float*, float* out_lr, uint32_t n,
                        uint32_t) override {
        ++multi_calls_;
        if (device_lost())
            return false;
        std::fill_n(out_lr, static_cast<std::size_t>(n) * 2u, kMultiMarker);
        return true;
    }

    bool prepare_spectral_stack(uint32_t, uint32_t, uint32_t) override {
        return true;
    }
    bool spectral_stack_set_layer(uint32_t, const float*, const float*, uint32_t,
                                  uint32_t) override {
        ++stack_calls_;
        return !device_lost();
    }
    bool spectral_stack_render(const float*, uint32_t, float, float, uint32_t, float* frame_out,
                               uint32_t n) override {
        ++stack_calls_;
        if (device_lost())
            return false;
        std::fill_n(frame_out, n, kStackMarker);
        return true;
    }

    CapabilityReport capabilities() const override {
        CapabilityReport r;
        r.backend = "Fake";
        return r;
    }

    // ── Everything else fails ───────────────────────────────────────────────
    bool compute_magnitude(const float*, float*, uint32_t) override {
        return false;
    }
    bool complex_multiply(const float*, const float*, float*, uint32_t) override {
        return false;
    }
    bool batch_magnitude(const float*, float*, uint32_t, uint32_t) override {
        return false;
    }
    bool fft_forward_timed(const float*, float*, uint32_t, double*) override {
        return false;
    }
    bool prepare_convolution(uint32_t, const float*) override {
        return false;
    }
    bool convolve(const float*, float*, uint32_t) override {
        return false;
    }
    bool multi_convolve_timed(const float*, const float*, const float*, float*, uint32_t, uint32_t,
                              double*) override {
        return false;
    }
    bool prepare_multi_fdl(uint32_t, const float*, uint32_t, uint32_t) override {
        return false;
    }
    bool multi_fdl_convolve(const float*, const float*, const float*, float*, uint32_t,
                            uint32_t) override {
        return false;
    }
    bool matmul(const float*, const float*, float*, uint32_t, uint32_t, uint32_t) override {
        return false;
    }
    bool additive_synth(const float*, float*, uint32_t, uint32_t, float, float) override {
        return false;
    }
    bool modal_strike(const float*, float*, uint32_t, uint32_t, float, float) override {
        return false;
    }
    bool granular_cloud(const float*, const float*, float*, uint32_t, uint32_t, uint32_t) override {
        return false;
    }
    bool dense_tanh(const float*, const float*, const float*, float*, uint32_t, uint32_t) override {
        return false;
    }
    bool prepare_conv_stack(uint32_t, uint32_t, const uint32_t*, uint32_t, const float*, uint32_t,
                            uint32_t, float) override {
        return false;
    }
    bool conv_stack_forward(const float*, float*, uint32_t) override {
        return false;
    }
    bool prepare_wavenet(const WavenetLayerArraySpec*, uint32_t, const float*, uint32_t, uint32_t,
                         float, uint32_t) override {
        return false;
    }
    bool wavenet_forward(const float*, float*, uint32_t, uint32_t) override {
        return false;
    }
    uint64_t compute_magnitude_async(const float*, float*, uint32_t, std::chrono::microseconds,
                                     ReadbackCallback) override {
        return 0;
    }
    const char* kernel_source(const char*) const override {
        return nullptr;
    }
    bool override_kernel_source(const char*, const char*) override {
        return false;
    }
    DeviceSharingReport verify_device_sharing(render::GpuSurface&) override {
        return {};
    }
    std::vector<BenchmarkResult> benchmark_magnitude(const std::vector<uint32_t>&, int) override {
        return {};
    }
    std::vector<BenchmarkResult> benchmark_complex_multiply(const std::vector<uint32_t>&,
                                                            int) override {
        return {};
    }
#ifdef PULP_BENCHMARK
    void set_bench_counters(render::bench::PerfCounters*) override {}
#endif

  private:
    struct Pending {
        std::uint64_t id = 0;
        float* dest = nullptr;
        AsyncMode mode = AsyncMode::Resolve;
        std::chrono::steady_clock::time_point due{};
        std::uint32_t polls_left = 0;
        ReadbackCallback cb;
        std::vector<float> result;
    };

    // Each slot holds 2*n interleaved floats; only real parts carry input.
    void convolve_into(const float* in, float* out, uint32_t n, uint32_t batch) const {
        const std::size_t cplx = static_cast<std::size_t>(n) * 2u;
        if (!compute_results_) {
            std::fill_n(out, cplx * batch, 0.0f);
            return;
        }
        for (uint32_t b = 0; b < batch; ++b) {
            const float* x = in + b * cplx;
            float* y = out + b * cplx;
            for (uint32_t i = 0; i < n; ++i) {
                double acc = 0.0;
                for (std::size_t k = 0; k < ir_.size() && k <= i; ++k)
                    acc += static_cast<double>(ir_[k]) * x[2u * (i - k)];
                y[2u * i] = static_cast<float>(acc) * result_gain_;
                y[2u * i + 1u] = 0.0f;
            }
        }
    }

    std::vector<float> ir_;
    std::atomic<bool> lost_{false};
    std::atomic<std::uint64_t> generation_{1};
    AsyncMode async_mode_ = AsyncMode::Resolve;
    bool blocking_ok_ = true;
    bool poison_next_ = false;
    std::uint32_t delay_polls_ = 0;
    std::chrono::microseconds delay_time_{0};
    bool compute_results_ = true;
    float result_gain_ = 1.0f;
    std::deque<Pending> pending_;
    std::size_t max_in_flight_ = 4;
    std::uint64_t next_id_ = 1;
    std::uint64_t convolve_calls_ = 0;
    std::uint64_t async_submits_ = 0;
    std::uint64_t fft_calls_ = 0;
    std::uint64_t multi_calls_ = 0;
    std::uint64_t stack_calls_ = 0;
};

} // namespace pulp::test
