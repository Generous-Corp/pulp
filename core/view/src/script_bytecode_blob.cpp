// script_bytecode_blob.cpp - build-time script bytecode container and registry.

#include "script_bytecode_blob.hpp"

#include <pulp/runtime/crypto.hpp>
#include <pulp/runtime/log.hpp>

#include <array>
#include <cstring>

namespace pulp::view::detail {
namespace {

constexpr std::array<char, 8> kMagic = {'P', 'U', 'L', 'P', 'J', 'S', 'B', 'C'};
constexpr std::uint32_t kFormat = 1;
constexpr std::size_t kDigestBytes = 32;

void append_le(std::vector<std::uint8_t>& out, std::uint64_t value, int bytes) {
    for (int i = 0; i < bytes; ++i)
        out.push_back(static_cast<std::uint8_t>(value >> (8 * i)));
}

} // namespace

std::vector<std::uint8_t> encode_script_bytecode_blob(const std::string& source,
                                                      const std::vector<std::uint8_t>& bytecode,
                                                      std::string_view engine_fingerprint) {
    const auto source_digest = runtime::sha256(source);
    const auto bytecode_digest = runtime::sha256(bytecode.data(), bytecode.size());
    std::vector<std::uint8_t> blob;
    blob.reserve(kMagic.size() + 8 + engine_fingerprint.size() + 2 * (8 + kDigestBytes) +
                 bytecode.size());
    blob.insert(blob.end(), kMagic.begin(), kMagic.end());
    append_le(blob, kFormat, 4);
    append_le(blob, engine_fingerprint.size(), 4);
    blob.insert(blob.end(), engine_fingerprint.begin(), engine_fingerprint.end());
    append_le(blob, source.size(), 8);
    blob.insert(blob.end(), source_digest.begin(), source_digest.end());
    append_le(blob, bytecode.size(), 8);
    blob.insert(blob.end(), bytecode_digest.begin(), bytecode_digest.end());
    blob.insert(blob.end(), bytecode.begin(), bytecode.end());
    return blob;
}

std::optional<ScriptBytecodeBlob> parse_script_bytecode_blob(const std::uint8_t* blob,
                                                             std::size_t size) {
    if (!blob)
        return std::nullopt;
    std::size_t at = 0;
    auto take = [&](std::size_t n) -> const std::uint8_t* {
        if (n > size - at)
            return nullptr;
        const auto* p = blob + at;
        at += n;
        return p;
    };
    auto read_le = [&](int bytes, std::uint64_t& value) {
        const auto* p = take(static_cast<std::size_t>(bytes));
        if (!p)
            return false;
        value = 0;
        for (int i = 0; i < bytes; ++i)
            value |= std::uint64_t(p[i]) << (8 * i);
        return true;
    };
    const auto* magic = take(kMagic.size());
    if (!magic || std::memcmp(magic, kMagic.data(), kMagic.size()) != 0)
        return std::nullopt;
    std::uint64_t format = 0, fingerprint_size = 0, bytecode_size = 0;
    if (!read_le(4, format) || format != kFormat)
        return std::nullopt;
    if (!read_le(4, fingerprint_size))
        return std::nullopt;
    const auto* fingerprint = take(static_cast<std::size_t>(fingerprint_size));
    if (!fingerprint)
        return std::nullopt;
    ScriptBytecodeBlob out;
    out.blob = blob;
    out.fingerprint.assign(reinterpret_cast<const char*>(fingerprint),
                           static_cast<std::size_t>(fingerprint_size));
    if (!read_le(8, out.source_size))
        return std::nullopt;
    if (!(out.source_sha256 = take(kDigestBytes)))
        return std::nullopt;
    if (!read_le(8, bytecode_size))
        return std::nullopt;
    if (!(out.bytecode_sha256 = take(kDigestBytes)))
        return std::nullopt;
    if (bytecode_size == 0 || bytecode_size != size - at)
        return std::nullopt;
    out.bytecode = take(static_cast<std::size_t>(bytecode_size));
    out.bytecode_size = static_cast<std::size_t>(bytecode_size);
    return out;
}

EmbeddedBytecodeRegistry& EmbeddedBytecodeRegistry::instance() {
    static EmbeddedBytecodeRegistry registry;
    return registry;
}

bool EmbeddedBytecodeRegistry::add(const std::uint8_t* blob, std::size_t size,
                                   std::string_view engine_fingerprint) {
    auto parsed = parse_script_bytecode_blob(blob, size);
    if (!parsed)
        return false;
    if (parsed->fingerprint != engine_fingerprint) {
        runtime::log_warn("JS bytecode: ignoring build-time bytecode compiled by another "
                          "engine build ({} != {}); the script will compile from source",
                          parsed->fingerprint, engine_fingerprint);
        return false;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& existing : entries_)
        if (existing.blob.blob == blob)
            return true;
    entries_.push_back({std::move(*parsed)});
    count_.store(entries_.size(), std::memory_order_release);
    return true;
}

bool EmbeddedBytecodeRegistry::find(const std::string& source, std::vector<std::uint8_t>& out) {
    if (count_.load(std::memory_order_acquire) == 0)
        return false;
    std::optional<std::vector<std::uint8_t>> digest;
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& entry : entries_) {
        const auto& blob = entry.blob;
        if (entry.rejected || blob.source_size != source.size())
            continue;
        if (!digest)
            digest = runtime::sha256(source);
        if (std::memcmp(digest->data(), blob.source_sha256, kDigestBytes) != 0)
            continue;
        if (!entry.verified) {
            // Once per blob: the bytes the linker placed are the bytes the
            // generator hashed. A mismatch is a broken build artifact.
            const auto bytecode_digest = runtime::sha256(blob.bytecode, blob.bytecode_size);
            if (std::memcmp(bytecode_digest.data(), blob.bytecode_sha256, kDigestBytes) != 0) {
                entry.rejected = true;
                runtime::log_warn("JS bytecode: build-time bytecode failed its integrity check; "
                                  "compiling from source");
                continue;
            }
            entry.verified = true;
        }
        out.assign(blob.bytecode, blob.bytecode + blob.bytecode_size);
        return true;
    }
    return false;
}

void EmbeddedBytecodeRegistry::clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    entries_.clear();
    count_.store(0, std::memory_order_release);
}

} // namespace pulp::view::detail
