#pragma once

// Build-time script bytecode blobs: the self-describing container that
// pulp_add_js_bytecode() embeds in a product binary, and the process-wide
// registry the QuickJS bytecode cache consults on a miss. Engine-neutral: the
// QuickJS backend supplies the bytecode and its engine fingerprint.

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace pulp::view::detail {

/// Blob layout, integers little-endian:
///   magic "PULPJSBC" | u32 format | u32 F | F bytes engine fingerprint
///   | u64 source length | 32 B SHA-256(source)
///   | u64 bytecode length | 32 B SHA-256(bytecode) | bytecode
/// The size must match exactly, so a truncated or padded blob is rejected.
std::vector<std::uint8_t> encode_script_bytecode_blob(const std::string& source,
                                                      const std::vector<std::uint8_t>& bytecode,
                                                      std::string_view engine_fingerprint);

struct ScriptBytecodeBlob {
    const std::uint8_t* blob = nullptr;
    std::string fingerprint;
    std::uint64_t source_size = 0;
    const std::uint8_t* source_sha256 = nullptr;
    const std::uint8_t* bytecode_sha256 = nullptr;
    const std::uint8_t* bytecode = nullptr;
    std::size_t bytecode_size = 0;
};

/// Views into `blob` (no copy); nullopt when the blob is malformed.
std::optional<ScriptBytecodeBlob> parse_script_bytecode_blob(const std::uint8_t* blob,
                                                             std::size_t size);

class EmbeddedBytecodeRegistry {
public:
    static EmbeddedBytecodeRegistry& instance();

    /// Registers a blob whose engine fingerprint equals `engine_fingerprint`.
    /// The bytes must outlive the process (they normally live in the binary).
    bool add(const std::uint8_t* blob, std::size_t size, std::string_view engine_fingerprint);

    /// Copies out the bytecode registered for exactly `source`. A blob whose
    /// bytecode fails its SHA-256 (checked on first use) is never returned.
    bool find(const std::string& source, std::vector<std::uint8_t>& out);

    void clear();

private:
    struct Entry {
        ScriptBytecodeBlob blob;
        bool verified = false;
        bool rejected = false;
    };

    std::mutex mutex_;
    std::vector<Entry> entries_;
    std::atomic<std::size_t> count_{0};
};

}  // namespace pulp::view::detail
