#pragma once

/// @file peak_rms_kernels.hpp
/// The two implementations behind buffer_ops::measure_peak_rms, exposed so
/// tests can hold them to the documented bit-for-bit agreement. Not part of
/// the public API.

#include <pulp/audio/buffer_ops.hpp>

#include <span>

namespace pulp::audio::buffer_ops::detail {

/// Array-of-partials kernel; compiled on every platform. MSVC builds use it
/// for measure_peak_rms.
PeakRms measure_peak_rms_portable(std::span<const float> samples) noexcept;

/// Register-resident kernel built on the GCC/Clang vector extension. Where
/// that extension is unavailable it forwards to the portable kernel.
PeakRms measure_peak_rms_vector(std::span<const float> samples) noexcept;

/// True when measure_peak_rms_vector is a distinct vector implementation.
bool has_vector_peak_rms_kernel() noexcept;

} // namespace pulp::audio::buffer_ops::detail
