#pragma once

#include <cstdint>

/// @file spectral_jitter.hpp
/// The single, canonical spectral phase-jitter contract shared by every
/// spectral-freeze engine in this library.
///
/// `jitter` (0..1) is a per-hop phase-wander depth: at each render hop every
/// bin's running phase is perturbed by `jitter * 2*pi * spectral_phase_hash(...)`
/// so the wander reaches a full turn at jitter == 1. Applied
/// conjugate-antisymmetrically (bin k gets +h, bin n-k gets -h, DC and Nyquist
/// none) so a magnitude-symmetric spectrum stays Hermitian and the synthesized
/// frame stays real. The hash is *stateless* in the bin index — it depends only
/// on (bin, seed) — so the same seed reproduces the same wander regardless of
/// the order bins are visited, and it can be bit-matched between the CPU
/// reference and the GPU compute shader.
///
/// This is the ONE meaning of the `jitter` knob across CpuSpectralStack,
/// GpuSpectralStack, and GpuSpectralFreeze. The GPU-resident equivalent lives in
/// the `spectral_advance` WGSL shader (core/render/src/gpu_compute.cpp): the
/// `hash01` + seed-advance math there MUST stay identical to the functions
/// below, or the CPU reference and GPU engine will diverge.
namespace pulp::gpu_audio {

constexpr float kSpectralTwoPi = 6.2831853071795864f;

/// Integer hash → float in [-0.5, 0.5], deterministic per (bin, seed). Matches
/// the `hash01` function in the GPU spectral_advance shader bit-for-bit.
inline float spectral_phase_hash(std::uint32_t bin, std::uint32_t seed) {
    std::uint32_t h = bin * 2654435761u + seed * 40503u;
    h = (h ^ (h >> 15u)) * 2246822519u;
    h = (h ^ (h >> 13u)) * 3266489917u;
    h = h ^ (h >> 16u);
    return static_cast<float>(h >> 8u) / 16777216.0f - 0.5f;
}

/// Advance the jitter seed one render hop (the classic Numerical-Recipes LCG).
/// The seed a render *uses* is the value BEFORE this advance, so successive
/// hops draw a fresh, deterministic wander. Matches the GPU seed advance.
inline std::uint32_t advance_spectral_seed(std::uint32_t seed) {
    return seed * 1664525u + 1013904223u;
}

/// Shared initial jitter seed so every engine starts the same wander sequence.
constexpr std::uint32_t kSpectralSeedInit = 0x9E3779B9u;

} // namespace pulp::gpu_audio
