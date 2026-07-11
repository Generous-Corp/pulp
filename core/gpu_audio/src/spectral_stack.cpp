#include <pulp/gpu_audio/spectral_stack.hpp>

#include <pulp/gpu_audio/spectral_jitter.hpp>

#include <algorithm>
#include <cmath>

namespace pulp::gpu_audio {

namespace {

constexpr double kTwoPi = 6.283185307179586476925286766559;

bool is_power_of_two(uint32_t n) { return n != 0u && (n & (n - 1u)) == 0u; }

// Smear → (radius, inv_kernel). MUST match the GPU's spectral_stack_render so
// the CPU reference and the GPU engine produce the same smeared magnitude.
void smear_kernel(float smear, uint32_t n, uint32_t& radius, float& inv_kernel) {
    const float sm = smear < 0.0f ? 0.0f : (smear > 1.0f ? 1.0f : smear);
    radius = 0u;
    if (sm > 0.0f) {
        radius = static_cast<uint32_t>(sm * static_cast<float>(n / 32u));
        if (radius < 1u) radius = 1u;
    }
    inv_kernel = 1.0f / static_cast<float>(2u * radius + 1u);
}

}  // namespace

void detail::circular_box_blur(const std::vector<float>& m, std::vector<float>& out,
                               uint32_t radius, float inv_kernel, uint32_t n) {
    const int r = static_cast<int>(radius);
    const int ni = static_cast<int>(n);
    auto wrap = [ni](int j) { j %= ni; return j < 0 ? j + ni : j; };

    // Seed the window centered at k=0: bins [-r, r] mod n.
    float s = 0.0f;
    for (int d = -r; d <= r; ++d) s += m[static_cast<uint32_t>(wrap(d))];
    out[0] = s * inv_kernel;

    // Slide: at each k, add bin (k+r) and drop bin (k-r-1), both mod n.
    for (int k = 1; k < ni; ++k) {
        s += m[static_cast<uint32_t>(wrap(k + r))] -
             m[static_cast<uint32_t>(wrap(k - r - 1))];
        out[static_cast<uint32_t>(k)] = s * inv_kernel;
    }
}

// ───────────────────────────── CpuSpectralStack ────────────────────────────

bool CpuSpectralStack::prepare(uint32_t fft_size, uint32_t hop,
                               uint32_t num_layers) {
    if (!is_power_of_two(fft_size) || hop == 0u || hop > fft_size || num_layers == 0u)
        return false;
    n_ = fft_size;
    hop_ = hop;
    hop_ratio_ = static_cast<float>(hop) / static_cast<float>(fft_size);
    fft_ = std::make_unique<signal::Fft>(static_cast<int>(fft_size));
    scratch_.assign(fft_size, {0.0f, 0.0f});
    smear_scratch_.assign(fft_size, 0.0f);
    weights_.assign(num_layers, 0.0f);
    layers_.clear();
    layers_.resize(num_layers);
    for (auto& l : layers_) {
        l.mag.assign(fft_size, 0.0f);
        l.phase.assign(fft_size, 0.0f);
        l.active = false;
    }
    return true;
}

bool CpuSpectralStack::capture(uint32_t layer, const float* frame_in) {
    if (layer >= layers_.size() || frame_in == nullptr || !fft_) return false;
    fft_->forward_real(frame_in, scratch_.data());
    Layer& L = layers_[layer];
    for (uint32_t k = 0; k < n_; ++k) {
        L.mag[k] = std::abs(scratch_[k]);
        L.phase[k] = std::arg(scratch_[k]);
    }
    L.active = true;
    return true;
}

void CpuSpectralStack::clear(uint32_t layer) {
    if (layer < layers_.size()) layers_[layer].active = false;
}

bool CpuSpectralStack::layer_active(uint32_t layer) const {
    return layer < layers_.size() && layers_[layer].active;
}

bool CpuSpectralStack::render(float* frame_out, const float* weights, float smear,
                             float jitter) {
    if (frame_out == nullptr || n_ == 0u) return false;

    bool any = false;
    for (uint32_t L = 0; L < layers_.size(); ++L) {
        weights_[L] = layers_[L].active ? (weights ? weights[L] : 1.0f) : 0.0f;
        if (layers_[L].active) any = true;
    }
    if (!any) { std::fill(frame_out, frame_out + n_, 0.0f); return false; }

    uint32_t radius = 0u; float inv_kernel = 1.0f;
    smear_kernel(smear, n_, radius, inv_kernel);
    const float jit = jitter < 0.0f ? 0.0f : (jitter > 1.0f ? 1.0f : jitter);
    const uint32_t seed = seed_;
    seed_ = advance_spectral_seed(seed_);  // advance for next render

    // Advance every active layer's persistent phase by the per-bin frequency,
    // plus conjugate-symmetric jitter, wrapped to [-pi, pi].
    const float adv = static_cast<float>(kTwoPi) * hop_ratio_;
    const uint32_t half = n_ / 2u;
    for (uint32_t L = 0; L < layers_.size(); ++L) {
        // Advance every ACTIVE layer, not just the audible ones: a layer parked
        // at weight 0 (muted, e.g. mid-morph) must keep its phase running so it
        // re-enters phase-coherent when its weight comes back up — otherwise the
        // frozen phase makes it re-enter 2*pi*k*hop_ratio*N behind and clicks on
        // un-mute. Gating on the WEIGHT here (the SF-4 regression) diverged from
        // the GPU advance shader, which advances all layers unconditionally (it
        // has no weight binding); gating on `active` restores both the retired
        // GpuHyperFreeze behavior and CPU/GPU parity. Synthesis is still gated by
        // weight below, so a muted layer costs only the phase step, not an FFT.
        if (!layers_[L].active) continue;
        auto& ph = layers_[L].phase;
        for (uint32_t k = 0; k < n_; ++k) {
            float p = ph[k] + adv * static_cast<float>(k);
            if (jit > 0.0f) {
                // Full-turn-at-1 per-hop wander → a random walk that breaks the
                // FFT-period repetition; conjugate-antisymmetric to stay real.
                if (k > 0u && k < half)
                    p += jit * static_cast<float>(kTwoPi) * spectral_phase_hash(k, seed);
                else if (k > half && k < n_)
                    p -= jit * static_cast<float>(kTwoPi) * spectral_phase_hash(n_ - k, seed);
            }
            p -= static_cast<float>(kTwoPi) * std::round(p / static_cast<float>(kTwoPi));
            ph[k] = p;
        }
    }

    // Smear + weighted complex sum across layers, then one inverse FFT.
    //
    // Layer-outer so each layer's circular box blur is computed ONCE in O(n)
    // (circular_box_blur) rather than O(n*radius) recomputed per bin. Each bin's
    // cross-layer accumulation still runs L = 0,1,2,... in order, so the weighted
    // sum is identical to the old bin-outer form up to the box blur's own
    // rounding. w == 0 layers are skipped exactly as before.
    for (uint32_t k = 0; k < n_; ++k) scratch_[k] = {0.0f, 0.0f};
    for (uint32_t L = 0; L < layers_.size(); ++L) {
        const float w = weights_[L];
        if (w == 0.0f) continue;
        const auto& m = layers_[L].mag;
        const auto& ph = layers_[L].phase;
        const float* mag_src = m.data();
        if (radius > 0u) {
            detail::circular_box_blur(m, smear_scratch_, radius, inv_kernel, n_);
            mag_src = smear_scratch_.data();
        }
        for (uint32_t k = 0; k < n_; ++k) {
            const float mag = mag_src[k];
            const float p = ph[k];
            scratch_[k] += std::complex<float>(w * mag * std::cos(p),
                                               w * mag * std::sin(p));
        }
    }
    fft_->inverse(scratch_.data());  // 1/N normalized
    for (uint32_t k = 0; k < n_; ++k) frame_out[k] = scratch_[k].real();
    return true;
}

// ───────────────────────────── GpuSpectralStack ────────────────────────────

bool GpuSpectralStack::prepare(uint32_t fft_size, uint32_t hop,
                               uint32_t num_layers,
                               render::GpuCompute* shared_device) {
    if (!is_power_of_two(fft_size) || hop == 0u || hop > fft_size || num_layers == 0u)
        return false;
    n_ = fft_size;
    hop_ = hop;
    num_layers_ = num_layers;

    if (shared_device) {
        gpu_ = shared_device;
    } else {
        owned_ = render::GpuCompute::create();
        if (owned_ && owned_->initialize_standalone()) gpu_ = owned_.get();
        else { owned_.reset(); gpu_ = nullptr; }
    }
    if (!gpu_) return true;  // prepared, but unavailable() — caller falls back

    if (!gpu_->prepare_spectral_stack(fft_size, hop, num_layers)) {
        gpu_ = nullptr; owned_.reset();
        return true;  // over a device limit / failure → behave as unavailable
    }
    fft_ = std::make_unique<signal::Fft>(static_cast<int>(fft_size));
    cap_scratch_.assign(fft_size, {0.0f, 0.0f});
    mag_up_.assign(fft_size, 0.0f);
    phase_up_.assign(fft_size, 0.0f);
    active_.assign(num_layers, 0u);
    weights_.assign(num_layers, 0.0f);
    return true;
}

bool GpuSpectralStack::capture(uint32_t layer, const float* frame_in) {
    if (!gpu_ || layer >= num_layers_ || frame_in == nullptr || !fft_) return false;
    fft_->forward_real(frame_in, cap_scratch_.data());
    for (uint32_t k = 0; k < n_; ++k) {
        mag_up_[k] = std::abs(cap_scratch_[k]);
        phase_up_[k] = std::arg(cap_scratch_[k]);
    }
    if (!gpu_->spectral_stack_set_layer(layer, mag_up_.data(), phase_up_.data(),
                                        n_, num_layers_))
        return false;
    active_[layer] = 1u;
    return true;
}

void GpuSpectralStack::clear(uint32_t layer) {
    if (layer < num_layers_) active_[layer] = 0u;
}

bool GpuSpectralStack::layer_active(uint32_t layer) const {
    return layer < num_layers_ && active_[layer] != 0u;
}

bool GpuSpectralStack::render(float* frame_out, const float* weights, float smear,
                             float jitter) {
    if (!gpu_ || frame_out == nullptr) return false;
    bool any = false;
    for (uint32_t L = 0; L < num_layers_; ++L) {
        weights_[L] = active_[L] ? (weights ? weights[L] : 1.0f) : 0.0f;
        if (active_[L]) any = true;
    }
    if (!any) { std::fill(frame_out, frame_out + n_, 0.0f); return false; }
    const uint32_t seed = seed_;
    seed_ = advance_spectral_seed(seed_);
    return gpu_->spectral_stack_render(weights_.data(), num_layers_, smear, jitter,
                                       seed, frame_out, n_);
}

std::string GpuSpectralStack::backend() const {
    return gpu_ ? gpu_->capabilities().backend : std::string();
}

// ──────────────────────────── SpectralFreezeFramer ─────────────────────────

bool SpectralFreezeFramer::prepare(SpectralStack* stack, uint32_t fft_size,
                                   uint32_t hop,
                                   signal::WindowFunction::Type window) {
    if (!stack || !stack->available() || stack->fft_size() != fft_size) return false;
    if (hop == 0u || hop > fft_size || (fft_size % hop) != 0u) return false;
    stack_ = stack;
    fft_size_ = fft_size;
    hop_ = hop;
    window_ = signal::WindowFunction::generate(static_cast<int>(fft_size), window);

    // Steady-state overlap-add normalization for analysis×synthesis windowing
    // at this hop: sum of squared window across all overlapping frames at a
    // point. For a constant input it makes unity gain.
    const uint32_t overlap = fft_size / hop;
    double norm = 0.0;
    for (uint32_t o = 0; o < overlap; ++o) {
        const uint32_t idx = o * hop;
        if (idx < fft_size) norm += static_cast<double>(window_[idx]) * window_[idx];
    }
    ola_norm_ = norm > 0.0 ? static_cast<float>(1.0 / norm) : 1.0f;

    reset();
    return true;
}

void SpectralFreezeFramer::reset() {
    in_hist_.assign(fft_size_, 0.0f);
    in_wr_ = 0;
    in_fill_ = 0;
    frame_.assign(fft_size_, 0.0f);
    rendered_.assign(fft_size_, 0.0f);
    ola_.assign(fft_size_, 0.0f);
    // Output ring: out_head_ = read index, out_tail_ = count of valid samples.
    // Primed with fft_size_ zeros so the first real output is one frame delayed
    // (the framer's latency), matching how the stack needs a full frame first.
    out_fifo_.assign(fft_size_ + hop_ + 1u, 0.0f);
    out_head_ = 0;
    out_tail_ = fft_size_;             // fft_size samples of leading silence
    prev_freeze_ = false;
    next_layer_ = 0;
    captured_ = 0;
}

void SpectralFreezeFramer::run_hop(const SpectralFreezeControls& ctl) {
    // Analysis: window the most-recent fft_size input samples, read oldest→newest
    // out of the ring (the next write slot holds the oldest sample).
    for (uint32_t i = 0; i < fft_size_; ++i)
        frame_[i] = in_hist_[(in_wr_ + i) % fft_size_] * window_[i];

    if (ctl.freeze && !prev_freeze_ && stack_->num_layers() > 0) {
        stack_->capture(next_layer_, frame_.data());
        next_layer_ = (next_layer_ + 1u) % stack_->num_layers();
        if (captured_ < stack_->num_layers()) ++captured_;
    }
    prev_freeze_ = ctl.freeze;

    if (!stack_->render(rendered_.data(), ctl.weights, ctl.smear, ctl.jitter))
        std::fill(rendered_.begin(), rendered_.end(), 0.0f);

    // Synthesis window + overlap-add into the rolling accumulator.
    for (uint32_t i = 0; i < fft_size_; ++i)
        ola_[i] += rendered_[i] * window_[i] * ola_norm_;

    // The first hop samples of ola_ are complete → push to the output ring,
    // then shift the accumulator left by hop.
    const uint32_t cap = static_cast<uint32_t>(out_fifo_.size());
    for (uint32_t i = 0; i < hop_; ++i) {
        out_fifo_[(out_head_ + out_tail_) % cap] = ola_[i];
        if (out_tail_ < cap) ++out_tail_;        // ring full only if mis-clocked
        else out_head_ = (out_head_ + 1u) % cap;  // (shouldn't happen) drop oldest
    }
    std::move(ola_.begin() + hop_, ola_.end(), ola_.begin());
    std::fill(ola_.end() - hop_, ola_.end(), 0.0f);
}

void SpectralFreezeFramer::process(const float* in, float* out, uint32_t n,
                                   const SpectralFreezeControls& ctl) {
    if (!stack_ || fft_size_ == 0u) {
        if (out && in) std::copy(in, in + n, out);
        return;
    }
    for (uint32_t s = 0; s < n; ++s) {
        // Write one input sample into the ring (O(1); the slot we overwrite was
        // the oldest sample, so in_wr_ now points at the new oldest).
        in_hist_[in_wr_] = (ctl.active && in) ? in[s] : 0.0f;
        in_wr_ = (in_wr_ + 1u) % fft_size_;

        if (++in_fill_ >= hop_) {
            in_fill_ = 0;
            run_hop(ctl);
        }

        // Consume one output sample from the ring (silence if somehow starved).
        if (out_tail_ > 0u) {
            out[s] = out_fifo_[out_head_];
            out_head_ = (out_head_ + 1u) % static_cast<uint32_t>(out_fifo_.size());
            --out_tail_;
        } else {
            out[s] = 0.0f;
        }
    }
}

}  // namespace pulp::gpu_audio
