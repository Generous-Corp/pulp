#pragma once

// Shared plumbing for the `pulp audio spectrogram` and `pulp audio waveform`
// image verbs: common flag parsing, memory-mapped windowed input, the
// `pulp.audio.image.v1` sidecar, and PNG/JSON output.
//
// Long inputs are read through `MemoryMappedAudioReader` in fixed windows and
// never through `decode_wav` (which caps at 100,000,000 frames), so a
// multi-hour WAV or AIFF renders with memory bounded by the window. Formats the
// mapped reader cannot seek (FLAC, MP3, Ogg, platform decoders) fall back to
// the registry's one-time whole-file decode, and the verbs print a one-line
// note when that happens.
//
// Paths in the sidecar are basenames unless `--absolute-paths` is given, so a
// sidecar attached to a public CI artifact never leaks a checkout layout.

#include <pulp/audio/analysis/raster_image.hpp>
#include <pulp/audio/audio_file.hpp>
#include <pulp/audio/mmap_reader.hpp>

#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace pulp::cli::audio_image {

inline constexpr const char* kSidecarSchema = "pulp.audio.image.v1";

/// Flags both verbs share. Verb-specific settings live beside this in the
/// verb's own struct.
struct ImageArgs {
    std::string input;
    std::string png;
    std::string json; ///< sidecar path; empty means `<png>.json`
    bool absolute_paths = false;
    std::optional<double> start_s;
    std::optional<double> end_s;
    std::uint32_t channel = 0;
    std::uint32_t width = 0;  ///< plot width in pixels; 0 = verb default
    std::uint32_t height = 0; ///< plot height in pixels; 0 = verb default
};

enum class CommonArg { not_common, consumed, error };

/// Consumes one common flag at `args[index]` (advancing `index` past its
/// value). `not_common` leaves `index` untouched so the verb can try its own
/// flags; `error` has already printed the reason to stderr.
CommonArg parse_common_arg(const std::vector<std::string>& args, std::size_t& index,
                           ImageArgs& out);

/// Help text for the common flags, appended to each verb's usage.
const char* common_flags_help();

/// The opened input plus the resolved range and identity facts.
struct OpenedInput {
    pulp::audio::MemoryMappedAudioReader reader;
    pulp::audio::AudioFileInfo info;
    std::string path;         ///< input path as given
    std::string decoder_name; ///< registry reader's `format_name()`
    std::string sha256;       ///< hex SHA-256 of the input bytes
    std::uint64_t start_frame = 0;
    std::uint64_t end_frame = 0; ///< exclusive
    double start_s = 0.0;
    double end_s = 0.0;

    std::uint64_t range_frames() const noexcept {
        return end_frame - start_frame;
    }
};

/// Validates the common arguments, opens the input, hashes it and resolves
/// the time range and channel. Returns a process exit code: 0 on success,
/// 1 for a usage or I/O error, 2 when there is nothing to render (an input
/// with no frames). Errors are printed to stderr.
int open_input(const ImageArgs& args, OpenedInput& out);

/// What the streamed pass observed over the rendered range.
struct StreamStats {
    double peak_abs = 0.0; ///< max |sample| over the range, unclamped
    std::uint64_t frames = 0;
};

/// Streams the selected channel of the resolved range to `sink` in windows
/// of at most `window_frames`, tracking the sample peak. Returns false and
/// prints to stderr if a read fails.
bool stream_channel(OpenedInput& input, std::uint32_t channel, std::uint64_t window_frames,
                    const std::function<void(std::span<const float>)>& sink, StreamStats& stats);

/// Settings and image facts the verb contributes to the sidecar.
struct SidecarFields {
    std::string verb;          ///< "spectrogram" or "waveform"
    std::string settings_json; ///< a serialized JSON object
    std::uint32_t image_width = 0;
    std::uint32_t image_height = 0;
};

/// Builds the `pulp.audio.image.v1` sidecar document.
std::string build_sidecar_json(const ImageArgs& args, const OpenedInput& input,
                               const SidecarFields& fields, const StreamStats& stats);

/// Footer burned into the image: `<basename> sha256:<12 hex> <summary> pulp <version>`.
std::string footer_text(const OpenedInput& input, const std::string& settings_summary);

/// Basename, or the path as given when `--absolute-paths` is set.
std::string display_path(const ImageArgs& args, const std::string& path);

/// Resolved sidecar path (`args.json`, or `<png>.json`).
std::string sidecar_path(const ImageArgs& args);

/// Encodes the image as PNG and writes it plus the sidecar. Returns a process
/// exit code (0 or 1) and prints errors to stderr.
int write_outputs(const ImageArgs& args, const pulp::test::audio::RasterImage& image,
                  const std::string& sidecar_json);

} // namespace pulp::cli::audio_image
