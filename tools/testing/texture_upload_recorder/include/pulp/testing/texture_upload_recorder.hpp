#pragma once

// TextureUploadRecorder: records every texture upload made in this process, so
// a test or harness can prove two editors upload byte-identical textures.
//
// Test-only tooling. It lives under tools/testing, is never linked into a
// shipped plug-in or an SDK runtime library, and records nothing until a host
// or test calls install(). It changes nothing the editor does: every wrapped
// call runs the original implementation with the original arguments.
//
// On Apple platforms install() wraps, through the Objective-C runtime and on
// the system device's own classes:
//   - MTLDevice newTextureWithDescriptor: (size and pixel format),
//   - MTLTexture replaceRegion: (both variants),
//   - MTLBlitCommandEncoder copyFromBuffer:...toTexture: (both variants), the
//     staging-buffer path, read from the buffer's CPU-visible contents when the
//     copy is encoded.
// Nothing is injected into another process.
//
// Each upload becomes one line of <output_dir>/manifest.jsonl and, unless
// write_raw is off, one raw file holding the uploaded pixels with the source's
// row padding stripped, so two uploads of the same pixels hash the same whatever
// their stride. Raw dumps of real products are decoded skin art: output_dir must
// be outside any git working tree unless allow_inside_repository is set.

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace pulp::testing {

struct TextureUploadRecord {
    std::uint64_t seq = 0; ///< 1-based, in upload order
    std::string api;       ///< "metal"
    std::string entry;     ///< the wrapped call, e.g. "replaceRegion"
    /// Numbers textures created through newTextureWithDescriptor: since the
    /// hooks were first installed in this process, from 1; 0 for any other.
    std::uint64_t texture_id = 0;
    std::uint32_t width = 0; ///< the region uploaded
    std::uint32_t height = 0;
    std::uint32_t depth = 0;
    std::uint32_t x = 0;
    std::uint32_t y = 0;
    std::uint32_t z = 0;
    std::uint32_t mip = 0;
    std::uint32_t slice = 0;
    std::string pixel_format; ///< e.g. "RGBA8Unorm"
    std::uint32_t pixel_format_value = 0;
    std::uint64_t source_bytes_per_row = 0;
    bool padding_stripped = false; ///< false when the format's pixel size is unknown
    std::uint64_t byte_length = 0; ///< of the recorded bytes
    std::string sha256;            ///< hex, of the recorded bytes; empty if unreadable
    std::string raw_file;          ///< file name in output_dir; empty if not written
    std::string caller_image;      ///< path of the binary that made the call
};

class TextureUploadRecorder {
  public:
    struct Options {
        std::filesystem::path output_dir; ///< required; created if missing
        bool write_raw = true;
        /// Allow output_dir inside a git working tree. Off by default so
        /// decoded art from a real product never lands under a repository.
        bool allow_inside_repository = false;
    };

    /// Starts recording into options.output_dir. False (with a reason) if the
    /// directory is unusable, inside a repository without permission, or the
    /// platform has no supported graphics API. Calling it again while recording
    /// fails; call stop() first.
    static bool install(const Options& options, std::string* error = nullptr);

    /// Stops recording. The wrapped calls keep passing straight through.
    static void stop();

    [[nodiscard]] static bool recording();

    /// The uploads recorded since the last install().
    [[nodiscard]] static std::vector<TextureUploadRecord> records();
};

} // namespace pulp::testing
