#pragma once

#include <pulp/audio/sample_mip_sidecar.hpp>

namespace pulp::examples {

using SamplerStreamMipSidecarStatus = audio::SampleMipSidecarStatus;
using SamplerStreamMipSidecar = audio::SampleMipSidecar;

inline bool sampler_stream_mip_sidecar_exists(std::string_view source_path) noexcept {
    return audio::sample_mip_sidecar_exists(source_path);
}

inline SamplerStreamMipSidecar load_sampler_stream_mip_sidecar(
    std::string_view source_path, const audio::FileFrameReader& source,
    const std::shared_ptr<audio::MemoryMappedAudioReader>& retained_source) {
    return audio::load_sample_mip_sidecar(source_path, source, retained_source);
}

} // namespace pulp::examples
