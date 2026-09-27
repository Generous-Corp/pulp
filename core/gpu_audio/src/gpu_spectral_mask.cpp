#include <pulp/gpu_audio/gpu_spectral_mask.hpp>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <limits>
#include <vector>
#include <string_view>
#if defined(PULP_GPU_AUDIO_HAS_DAWN_SHARED_IO)
#include "detail/dawn_shared_io_provider.hpp"
#include "detail/shared_io_program_session.hpp"
#endif
namespace pulp::gpu_audio {
#if defined(PULP_GPU_AUDIO_HAS_DAWN_SHARED_IO)
namespace {
std::uint64_t allocate_stream_epoch() noexcept {
    static std::atomic<std::uint64_t> next{1};
    auto value = next.load(std::memory_order_relaxed);
    while (value != std::numeric_limits<std::uint64_t>::max()) {
        if (next.compare_exchange_weak(value, value + 1, std::memory_order_relaxed)) return value;
    }
    return 0; // Never wrap and alias an earlier stream.
}
}
#endif
struct GpuSpectralMaskSession::Impl {
#if defined(PULP_GPU_AUDIO_HAS_DAWN_SHARED_IO)
    detail::SharedIoProgramSession session;
    detail::DawnSharedIoProvider* provider = nullptr;
#endif
    std::uint32_t n=0, h=0, channels=0;
    std::uint64_t submitted=0, last_sequence=0, stream_epoch=0;
    bool failed=false;
    Diagnostics report;
};
GpuSpectralMaskSession::GpuSpectralMaskSession(std::unique_ptr<Impl> p) noexcept : impl_(std::move(p)) {}
GpuSpectralMaskSession::~GpuSpectralMaskSession() { (void)release(); }
GpuSpectralMaskSession::CreateResult GpuSpectralMaskSession::create(const Config& c) noexcept {
    CreateResult result;
    if (c.fft_size < 256 || c.fft_size > 16384 || (c.fft_size & (c.fft_size-1)) ||
        !c.hop || c.hop > c.fft_size/2 || c.fft_size % c.hop || !c.channels ||
        c.channels > 8 || !c.sample_rate || c.slots < 2 || c.slots > 64 ||
        c.gains.size() != c.fft_size/2+1 ||
        !std::all_of(c.gains.begin(),c.gains.end(),[](float v){return std::isfinite(v) && v>=0;})) {
        result.error=Error::InvalidConfig; return result;
    }
#if !defined(PULP_GPU_AUDIO_HAS_DAWN_SHARED_IO)
    result.error=Error::ProviderUnavailable; return result;
#else
#ifndef PULP_GPU_AUDIO_EXPECTED_DAWN_SHA
#define PULP_GPU_AUDIO_EXPECTED_DAWN_SHA ""
#endif
    try {
        auto provider=detail::DawnSharedIoProvider::create(
            {.expected_dawn_revision=PULP_GPU_AUDIO_EXPECTED_DAWN_SHA});
        if (!provider.provider) {result.error=Error::ProviderUnavailable; return result;}
        // The provider's inverse FFT is unnormalized; mask/N gives normalized
        // synthesis. Real symmetric gain preserves conjugate symmetry.
        std::vector<float> mask(2*c.fft_size,0);
        for (unsigned k=0;k<c.fft_size;++k)
            mask[2*k]=c.gains[k<=c.fft_size/2?k:c.fft_size-k]/float(c.fft_size);
        auto program=provider.provider->make_convolution_program({
            .fft_size=c.fft_size,.channels=c.channels,.logical_frames=c.fft_size,
            .ir_length=1,.normalized_ir_spectrum=mask,.spectral_hop=c.hop});
        if (!program) {result.error=Error::PreparationFailed; return result;}
        auto impl=std::make_unique<Impl>();
        impl->stream_epoch = allocate_stream_epoch();
        if (!impl->stream_epoch) { result.error=Error::PreparationFailed; return result; }
        const auto identity = provider.provider->adapter_identity();
        impl->report.dawn_revision = provider.provider->dawn_revision();
        impl->report.configured_revision_verified = std::string_view(PULP_GPU_AUDIO_EXPECTED_DAWN_SHA).size() != 0;
        impl->report.adapter_name = identity.name;
        impl->report.vendor_id = identity.vendor_id;
        impl->report.device_id = identity.device_id;
        impl->report.authenticated_shared_metal = true;
        impl->n=c.fft_size; impl->h=c.hop; impl->channels=c.channels;
        const auto bytes=std::size_t(c.fft_size)*c.channels*2*sizeof(float);
        if (!impl->session.prepare({std::move(provider.provider),std::move(program)},
                {.slots=c.slots,.input_bytes_per_slot=bytes+24,.output_bytes_per_slot=bytes}))
            result.error=Error::PreparationFailed;
        impl->provider = static_cast<detail::DawnSharedIoProvider*>(impl->session.owned_provider());
        result.session=std::unique_ptr<GpuSpectralMaskSession>(new GpuSpectralMaskSession(std::move(impl)));
        return result;
    } catch (...) {result.error=Error::PreparationFailed; return result;}
#endif
}
bool GpuSpectralMaskSession::prepared() const noexcept {
#if defined(PULP_GPU_AUDIO_HAS_DAWN_SHARED_IO)
    return impl_ && impl_->session.prepared() && !impl_->failed;
#else
    return false;
#endif
}
std::uint32_t GpuSpectralMaskSession::latency_samples() const noexcept {return impl_?impl_->n+impl_->h:0;}
std::uint64_t GpuSpectralMaskSession::epoch() const noexcept {
#if defined(PULP_GPU_AUDIO_HAS_DAWN_SHARED_IO)
    return impl_?impl_->stream_epoch:0;
#else
    return 0;
#endif
}
bool GpuSpectralMaskSession::submit_hop(std::span<const float> input,std::uint64_t sequence,
                                       std::uint64_t deadline) noexcept {
#if defined(PULP_GPU_AUDIO_HAS_DAWN_SHARED_IO)
    if (!prepared() || input.size()!=std::size_t(impl_->h)*impl_->channels ||
        impl_->submitted==std::numeric_limits<std::uint64_t>::max() ||
        (impl_->submitted && (impl_->last_sequence==std::numeric_limits<std::uint64_t>::max() ||
                             sequence!=impl_->last_sequence+1))) return false;
    auto lease=impl_->session.acquire_input(sequence,deadline);
    if (!lease) return false;
    std::memcpy(lease->bytes.data(),input.data(),input.size_bytes());
    impl_->report.cpu_input_bytes += input.size_bytes();
    const auto q=impl_->submitted, n=std::uint64_t(impl_->n), h=std::uint64_t(impl_->h);
    const bool frame=q>=n/h-1, output=q>=n/h+1;
    const std::uint32_t metadata[6]={std::uint32_t((q%(n/h))*h),frame?1u:0u,
        frame?std::uint32_t(((q-(n/h-1))%(2*n/h))*h):0u,output?1u:0u,
        output?std::uint32_t(((q-(n/h+1))%(2*n/h))*h):0u,
        output && q-(n/h+1)<n/h?1u:0u};
    std::memcpy(lease->bytes.data()+std::size_t(impl_->n)*impl_->channels*2*sizeof(float),metadata,sizeof(metadata));
    const detail::SharedIoProgramSession::SubmitToken token{lease->token,deadline};
    if (!impl_->session.submit(token)) {(void)impl_->session.cancel(token); return false;}
    ++impl_->submitted; impl_->last_sequence=sequence; return true;
#else
    (void)input;(void)sequence;(void)deadline;return false;
#endif
}
std::size_t GpuSpectralMaskSession::service(std::uint64_t now) noexcept {
#if defined(PULP_GPU_AUDIO_HAS_DAWN_SHARED_IO)
    return impl_?impl_->session.service(now):0;
#else
    (void)now;return 0;
#endif
}
std::optional<GpuSpectralMaskSession::Result> GpuSpectralMaskSession::receive(std::span<float> output) noexcept {
#if defined(PULP_GPU_AUDIO_HAS_DAWN_SHARED_IO)
    if (!impl_ || output.size()<std::size_t(impl_->h)*impl_->channels) return std::nullopt;
    auto completion=impl_->session.pop_completion(); if (!completion) return std::nullopt;
    Result r{impl_->stream_epoch,completion->token.slot.stream_sequence,false,completion->late};
    if(completion->status!=detail::SharedIoArena::CompletionStatus::RetiredSuccess){
        impl_->failed=true; (void)impl_->session.discard_completion(*completion); return r;
    }
    auto lease=impl_->session.acquire_output(*completion);
    if(!lease){impl_->failed=true;(void)impl_->session.discard_completion(*completion);return r;}
    std::memcpy(output.data(),lease->bytes.data(),std::size_t(impl_->h)*impl_->channels*sizeof(float));
    impl_->report.cpu_output_bytes += std::size_t(impl_->h)*impl_->channels*sizeof(float);
    r.delivered=impl_->session.release_output({lease->token});
    if(!r.delivered)impl_->failed=true;
    return r;
#else
    (void)output;return std::nullopt;
#endif
}
bool GpuSpectralMaskSession::release() noexcept {
#if defined(PULP_GPU_AUDIO_HAS_DAWN_SHARED_IO)
    if (!impl_) return true;
    // Snapshot while the provider is still owned; release destroys it only
    // after the arena confirms physical retirement of every slot.
    if (impl_->provider) {
        const auto stats = impl_->provider->stats();
        impl_->report.runtime_write_buffer_calls = stats.runtime_write_buffer_calls;
        impl_->report.runtime_copy_buffer_calls = stats.runtime_copy_buffer_calls;
        impl_->report.runtime_map_async_calls = stats.runtime_map_async_calls;
        impl_->report.imported_allocations = stats.import_successes;
        impl_->report.retired_success = stats.retired_success;
        impl_->report.retired_failure = stats.retired_failure;
    }
    if (!impl_->session.release()) return false;
    impl_->provider = nullptr;
    impl_->report.physical_release_confirmed = true;
    return true;
#else
    return true;
#endif
}
}

namespace pulp::gpu_audio {
GpuSpectralMaskSession::Diagnostics GpuSpectralMaskSession::diagnostics() const {
    if (!impl_) return {};
    auto report = impl_->report;
#if defined(PULP_GPU_AUDIO_HAS_DAWN_SHARED_IO)
    if (impl_->provider) {
        const auto stats = impl_->provider->stats();
        report.runtime_write_buffer_calls = stats.runtime_write_buffer_calls;
        report.runtime_copy_buffer_calls = stats.runtime_copy_buffer_calls;
        report.runtime_map_async_calls = stats.runtime_map_async_calls;
        report.imported_allocations = stats.import_successes;
        report.retired_success = stats.retired_success;
        report.retired_failure = stats.retired_failure;
    }
#endif
    return report;
}
}
