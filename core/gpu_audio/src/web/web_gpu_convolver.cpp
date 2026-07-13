#include <pulp/gpu_audio/web/web_gpu_convolver.hpp>

#include <pulp/gpu_audio/detail/gpu_ola.hpp>
#include <pulp/signal/fft.hpp>

#include <algorithm>
#include <complex>

namespace pulp::gpu_audio {

WebGpuConvolver::WebGpuConvolver(uint32_t depth)
    : state_(std::make_shared<State>()), depth_(depth == 0 ? 1u : depth) {}

WebGpuConvolver::~WebGpuConvolver() = default;

bool WebGpuConvolver::prepare(render::GpuCompute* gpu, int sample_rate, int block,
                              int channels, const float* ir, int ir_len) {
    prepared_ = false;
    gpu_ = nullptr;

    if (gpu == nullptr || !gpu->is_initialized() || gpu->device_lost()) return false;
    if (sample_rate <= 0 || block <= 0 || channels <= 0) return false;
    if (ir == nullptr || ir_len <= 0) return false;

    channels_ = static_cast<uint32_t>(channels);
    block_ = static_cast<uint32_t>(block);
    sample_rate_ = static_cast<uint32_t>(sample_rate);

    // Matches GpuConvolver / signal::Convolver: the smallest power of two that
    // holds one block plus the full IR tail, so a single FFT-length convolution
    // is linear (not circular).
    fft_size_ = 1;
    while (fft_size_ < block_ + static_cast<uint32_t>(ir_len)) fft_size_ <<= 1;

    const uint32_t cplx = fft_size_ * 2u;

    // The IR spectrum on the CPU — deliberately, not on the GPU. See the header:
    // the GPU forward-FFT blocks on a map that a browser event loop cannot
    // resolve while the caller is spinning inside it. This is a one-time cost.
    signal::FftT<float> fft(static_cast<int>(fft_size_));
    std::vector<std::complex<float>> ir_freq(fft_size_, {0.0f, 0.0f});
    for (uint32_t i = 0; i < static_cast<uint32_t>(ir_len) && i < fft_size_; ++i)
        ir_freq[i] = {ir[i], 0.0f};
    fft.forward(ir_freq.data());

    ir_spec_.assign(cplx, 0.0f);
    for (uint32_t i = 0; i < fft_size_; ++i) {
        ir_spec_[2u * i] = ir_freq[i].real();
        ir_spec_[2u * i + 1u] = ir_freq[i].imag();
    }

    if (!gpu->prepare_convolution_batch(fft_size_, ir_spec_.data(), channels_))
        return false;

    // Keep GpuCompute's admission cap and this pipeline's depth cap the same, so
    // "at the cap" is decided in one place. Otherwise a submit could pass our cap
    // and be rejected by the device's, which is the same miss reported twice.
    gpu->set_max_readbacks_in_flight(depth_);

    // One scratch pair per pipeline slot: depth_ blocks may be in flight, and a
    // slot cannot be reused until its readback has landed. depth_ + 1 slots keep
    // the seq -> slot mapping (seq % slots) collision-free at the cap.
    auto st = std::make_shared<State>();
    const uint32_t slots = depth_ + 1u;
    st->slots.resize(slots);
    for (auto& slot : st->slots) {
        slot.in_pad.assign(static_cast<std::size_t>(cplx) * channels_, 0.0f);
        slot.time.assign(static_cast<std::size_t>(cplx) * channels_, 0.0f);
    }
    st->pending.assign(slots, 0u);
    state_ = std::move(st);

    carry_.assign(channels_, std::vector<float>(fft_size_, 0.0f));
    out_.assign(static_cast<std::size_t>(channels_) * block_, 0.0f);

    gpu_ = gpu;
    prepared_ = true;
    return true;
}

bool WebGpuConvolver::submit(uint32_t seq, const float* in_planar, int frames,
                             std::chrono::microseconds deadline) {
    if (!prepared_ || in_planar == nullptr || gpu_->device_lost()) return false;
    if (frames != static_cast<int>(block_)) return false;

    State& st = *state_;
    if (st.count >= depth_) return false;  // at the cap — the caller misses this block

    const uint32_t slots = static_cast<uint32_t>(st.slots.size());
    const uint32_t index = seq % slots;
    Slot& slot = st.slots[index];
    if (slot.state != SlotState::Free) return false;

    const uint32_t cplx = fft_size_ * 2u;
    std::fill(slot.in_pad.begin(), slot.in_pad.end(), 0.0f);
    for (uint32_t ch = 0; ch < channels_; ++ch) {
        const float* x = in_planar + static_cast<std::size_t>(ch) * block_;
        float* dst = slot.in_pad.data() + static_cast<std::size_t>(ch) * cplx;
        for (uint32_t i = 0; i < block_; ++i) dst[2u * i] = x[i];  // imag stays 0
    }

    auto st_ref = state_;  // keeps the slot storage (the readback's dest) alive
    const uint64_t id = gpu_->convolve_batch_async(
        slot.in_pad.data(), slot.time.data(), fft_size_, channels_, deadline,
        [st_ref, index](const render::GpuCompute::ReadbackResult& res) {
            Slot& s = st_ref->slots[index];
            switch (res.status) {
                case render::GpuCompute::ReadbackStatus::Success:
                    s.state = SlotState::Ready;
                    ++st_ref->resolves;
                    break;
                case render::GpuCompute::ReadbackStatus::Expired:
                    s.state = SlotState::Bad;
                    ++st_ref->expired;
                    break;
                case render::GpuCompute::ReadbackStatus::Failed:
                    s.state = SlotState::Bad;
                    ++st_ref->failed;
                    break;
            }
        });

    // Rejected before it was queued (no plan, device gone, device-side cap): the
    // completion does NOT fire, so there is nothing to unwind — the caller counts
    // a miss, exactly as it would for an expired block.
    if (id == 0) return false;

    slot.state = SlotState::InFlight;
    slot.seq = seq;
    st.pending[(st.head + st.count) % slots] = index;
    ++st.count;
    ++st.submits;
    return true;
}

uint32_t WebGpuConvolver::collect(const Sink& sink) {
    if (!prepared_) return 0;

    // ONE pump. Never a spin: in a browser worker the event loop — not this
    // function — is what lets the map callbacks resolve, so a loop here would
    // starve the very thing it is waiting for.
    gpu_->poll_readbacks();

    State& st = *state_;
    const uint32_t slots = static_cast<uint32_t>(st.slots.size());
    const uint32_t cplx = fft_size_ * 2u;
    uint32_t delivered = 0;

    // Completions fire in submission order, so the pending ring is drained from
    // the head: the first still-in-flight block ends the harvest.
    while (st.count > 0) {
        const uint32_t index = st.pending[st.head];
        Slot& slot = st.slots[index];
        if (slot.state == SlotState::InFlight) break;

        st.head = (st.head + 1u) % slots;
        --st.count;

        bool ok = (slot.state == SlotState::Ready);
        if (ok) {
            for (uint32_t ch = 0; ch < channels_; ++ch) {
                // Guarded overlap-add: a non-finite readback resets that
                // channel's carry and silences the block rather than poisoning
                // every block that follows.
                const bool finite = detail::overlap_add_block(
                    carry_[ch].data(),
                    slot.time.data() + static_cast<std::size_t>(ch) * cplx,
                    /*src_stride=*/2, out_.data() + static_cast<std::size_t>(ch) * block_,
                    fft_size_, block_);
                if (!finite) ok = false;
            }
            if (!ok) ++st.failed;
        } else {
            // Expired or failed: the block never landed. Emit silence and leave
            // the carry untouched — the next block still convolves correctly
            // against the history it already had.
            std::fill(out_.begin(), out_.end(), 0.0f);
        }

        const uint32_t seq = slot.seq;
        slot.state = SlotState::Free;
        sink(seq, out_.data(), ok);
        ++delivered;
    }

    return delivered;
}

uint32_t WebGpuConvolver::in_flight() const { return state_->count; }
uint64_t WebGpuConvolver::submits() const { return state_->submits; }
uint64_t WebGpuConvolver::resolves() const { return state_->resolves; }
uint64_t WebGpuConvolver::expired() const { return state_->expired; }
uint64_t WebGpuConvolver::failed() const { return state_->failed; }

}  // namespace pulp::gpu_audio
