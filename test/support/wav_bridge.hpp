#pragma once

/// @file wav_bridge.hpp
/// Write an offline render to a WAV file the Python Quality Lab can read.
///
/// The C++ harness renders in-tree Processors through RenderScenario /
/// HeadlessHost, but its output lives only as an in-memory buffer. The offline
/// Python analysis lane (tools/audio/quality-lab, soundfile) reads WAVs. This
/// bridge closes the gap: a rendered ScenarioResult (or any owning float
/// Buffer) is written to disk as a standard PCM/float WAV, so an offline script
/// can analyze in-tree oscillator output without a plugin bundle.
///
/// Encoding reuses the shipped `pulp::audio::write_wav_file` (CHOC's WAV
/// writer, per the repo's CHOC-first policy) — no hand-rolled WAV encoding.
/// Float32 is the default so the file carries the exact rendered samples with
/// no quantization; Int24 is available for a compact integer file that any DAW
/// or tool reads. The harness is deterministic, so the same render produces
/// byte-identical WAV bytes.
///
/// Test/tool layer only. Layering (see README.md): this sits alongside
/// scenarios; nothing below the harness may include it.

#include "render_scenario.hpp"

#include <pulp/audio/audio_file.hpp>
#include <pulp/audio/buffer.hpp>

#include <string>

namespace pulp::test::audio {

/// Write an owning float Buffer to a WAV file at `path`, deinterleaving into an
/// AudioFileData and calling `pulp::audio::write_wav_file`.
///
/// `sample_rate` is rounded to the nearest integer for the WAV header (the WAV
/// format carries an integer rate). Float32 preserves the exact samples;
/// Int24 quantizes to the integer PCM grid. Returns false on an empty buffer,
/// a non-positive sample rate, or a write failure.
bool write_buffer_wav(const pulp::audio::Buffer<float>& buffer,
                      double sample_rate, const std::string& path,
                      pulp::audio::WavBitDepth bit_depth =
                          pulp::audio::WavBitDepth::Float32);

/// Write a rendered ScenarioResult's output to a WAV file, tagging the file
/// with the render's own sample rate. Thin wrapper over `write_buffer_wav`.
bool write_scenario_wav(const ScenarioResult& result, const std::string& path,
                        pulp::audio::WavBitDepth bit_depth =
                            pulp::audio::WavBitDepth::Float32);

} // namespace pulp::test::audio
