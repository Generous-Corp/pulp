#pragma once

// Load an impulse-response audio file into a convolution-ready mono buffer:
// decode (WAV / AIFF / FLAC via the FormatRegistry) → sum to mono → resample to
// the target rate → optional unit-energy normalize. This is the shared loader
// behind Pulp's convolution demos (SuperConvolver's cabinet/room IRs and the
// GPU NAM cabinet IR); the convolver itself is pulp::signal::PartitionedConvolver.
//
// The FFT/convolution primitive and the resampler already live in core; this is
// the file-to-buffer glue they share. Header-only so it adds no link edge to
// pulp-audio — a consumer already linking pulp::audio + pulp::signal gets it for
// free.
//
// Off the audio thread only (it allocates and decodes).

#include <pulp/audio/audio_file.hpp>
#include <pulp/audio/format_registry.hpp>
#include <pulp/signal/resampler.hpp>

#include <algorithm>
#include <cmath>
#include <optional>
#include <string>
#include <vector>

namespace pulp::audio {

struct ImpulseResponseLoadOptions {
    /// Truncate the IR to at most this many seconds (measured at both the source
    /// and the target rate so a huge file can't OOM the decode or the FFT). 0
    /// disables the cap.
    double max_seconds = 0.0;
    /// Scale so the IR has unit energy (‖ir‖₂ = 1), keeping a dry/wet mix sane
    /// regardless of the file's recorded level.
    bool normalize_unit_energy = true;
    /// Refuse a file whose duration exceeds this many seconds before decoding
    /// (fat-finger guard). 0 disables the preflight.
    double reject_longer_than_seconds = 300.0;
};

/// Read an IR file → mono → resampled to `target_sample_rate` → (optionally)
/// unit-energy normalized. Returns nullopt on any failure (missing/empty file,
/// silent or non-finite content, absurd length). Off-audio-thread only.
inline std::optional<std::vector<float>> read_impulse_response(
    const std::string& path, double target_sample_rate,
    const ImpulseResponseLoadOptions& opts = {}) {
    const double session_sr = target_sample_rate > 0.0 ? target_sample_rate : 48000.0;

    // Preflight on metadata (no decode) to reject a pathologically large file
    // before read() pulls the whole thing into memory.
    if (opts.reject_longer_than_seconds > 0.0) {
        if (auto info = FormatRegistry::instance().read_info(path)) {
            if (info->num_frames == 0) return std::nullopt;
            const double info_sr =
                info->sample_rate > 0 ? static_cast<double>(info->sample_rate) : session_sr;
            const double dur = static_cast<double>(info->num_frames) / info_sr;
            if (dur > opts.reject_longer_than_seconds) return std::nullopt;
        }
    }

    const auto data = FormatRegistry::instance().read(path);
    if (!data || data->channels.empty() || data->num_frames() == 0) return std::nullopt;
    const double file_sr =
        data->sample_rate > 0 ? static_cast<double>(data->sample_rate) : session_sr;

    // Truncate at the source rate so the resampled result also fits the cap.
    std::size_t frames = static_cast<std::size_t>(data->num_frames());
    if (opts.max_seconds > 0.0) {
        const std::size_t max_file_frames =
            static_cast<std::size_t>(opts.max_seconds * file_sr);
        if (frames > max_file_frames) frames = max_file_frames;
    }
    const std::uint32_t ch = data->num_channels();

    // Sum to mono (mono IR; true-stereo IR is a future extension).
    std::vector<float> mono(frames, 0.0f);
    for (std::uint32_t c = 0; c < ch; ++c) {
        const auto& src = data->channels[c];
        const std::size_t m = std::min<std::size_t>(frames, src.size());
        for (std::size_t i = 0; i < m; ++i) mono[i] += src[i];
    }
    if (ch > 1) {
        const float inv = 1.0f / static_cast<float>(ch);
        for (float& v : mono) v *= inv;
    }

    // Resample from the file's rate to the target rate so the IR length and tone
    // are correct regardless of the file's recording rate.
    std::vector<float> ir;
    if (std::abs(file_sr - session_sr) > 1e-3 && file_sr > 0.0) {
        signal::Resampler rs;
        rs.prepare(file_sr, session_sr, 1, frames);
        // Pad with one filter span of zeros so the resampler can flush its
        // group-delay tail and the full IR is captured.
        const std::size_t pad = rs.taps_per_phase();
        std::vector<float> padded = std::move(mono);
        padded.resize(frames + pad, 0.0f);
        const std::size_t cap = rs.max_output_for(padded.size());
        ir.assign(cap, 0.0f);
        const std::size_t produced =
            rs.process_block_mono(padded.data(), padded.size(), ir.data(), cap);
        ir.resize(produced);

        // Drop the resampler's leading group delay so the IR onset (and the
        // dry/wet alignment) is not shifted by an artificial silence. Group delay
        // is (taps_per_phase-1)/2 at the INPUT rate; convert to output samples by
        // the resample ratio.
        const double ratio = session_sr / file_sr;
        const std::size_t gd_out = static_cast<std::size_t>(
            (static_cast<double>(rs.taps_per_phase()) - 1.0) * 0.5 * ratio);
        if (gd_out > 0 && gd_out < ir.size())
            ir.erase(ir.begin(), ir.begin() + static_cast<std::ptrdiff_t>(gd_out));
    } else {
        ir = std::move(mono);
    }
    if (ir.empty()) return std::nullopt;

    // Final clamp at the target rate (resampler margin + rounding can nudge the
    // length just past the cap).
    if (opts.max_seconds > 0.0) {
        const std::size_t max_session_frames =
            static_cast<std::size_t>(opts.max_seconds * session_sr);
        if (ir.size() > max_session_frames) ir.resize(max_session_frames);
    }

    // Reject non-finite content regardless of the normalize flag — a NaN/Inf
    // sample would otherwise spread through the convolver's FFT. Zero energy is
    // only fatal when normalizing (division by zero); an all-zero IR is a valid,
    // if pointless, silence when normalization is off.
    double energy = 0.0;
    for (float v : ir) {
        if (!std::isfinite(v)) return std::nullopt;
        energy += static_cast<double>(v) * v;
    }
    if (opts.normalize_unit_energy) {
        if (energy <= 0.0) return std::nullopt;
        const float g = static_cast<float>(1.0 / std::sqrt(energy));
        for (float& v : ir) v *= g;
    }
    return ir;
}

}  // namespace pulp::audio
