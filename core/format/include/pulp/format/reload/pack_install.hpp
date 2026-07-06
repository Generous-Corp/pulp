#pragma once

// Content-addressed, immutable install for a verified swap pack. The trust gate
// verifies bytes on disk, but if the loader later re-reads those paths a local
// attacker (or a buggy installer) can swap the file between verify and load — a
// time-of-check/time-of-use hole that affects the re-read script/theme/asset paths,
// not just native dlopen. The fix: copy the verified files ONCE into a read-only
// directory named by the pack's content hash, re-verify the staged bytes, and have
// the loader read only from there. The installed bytes are the verified bytes.
//
// Control thread only (reads/writes files). Reuses verify_swap_pack (the sole
// sanctioned trust entry) — never the integrity check alone.

#include <pulp/format/reload/swap_pack.hpp>
#include <pulp/runtime/crypto.hpp>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <vector>

namespace pulp::format::reload {

enum class PackInstall {
    Ok = 0,
    NotTrusted,       ///< verify_swap_pack failed (bad signature or integrity)
    SymlinkRejected,  ///< a declared file is a symlink or escapes the pack root
    UnknownKind,      ///< a declared file has an unrecognized kind (consumer packs)
    IoError,          ///< staging copy / rename / permission set failed
};

struct PackInstallResult {
    PackInstall status = PackInstall::Ok;
    std::string detail;                 ///< which file / what went wrong
    std::filesystem::path installed_root;  ///< the immutable dir to load from (on Ok)
    bool ok() const { return status == PackInstall::Ok; }
};

/// The content id is the SHA-256 of the pack's signed message — every signed field
/// (id, plugin, version, policy, per-file hash+kind) feeds it, so two byte-identical
/// packs collide (dedup) and any change yields a new dir. Signature bytes are
/// excluded, so re-signing the same content installs to the same place.
inline std::string swap_pack_content_id(const SwapPackManifest& m) {
    const auto msg = swap_pack_signed_message(m);
    return runtime::sha256_hex(msg.data(), msg.size());
}

/// Verify @p source_root against @p trusted_public_key, then install the pack's
/// files into @p install_base/<content-id>/ as read-only, rejecting symlinks and
/// (for consumer packs) unknown kinds. Idempotent: an already-installed content id
/// is re-verified and reused. On Ok, `installed_root` is the immutable directory the
/// loader must read from. Fail-closed: nothing is published unless the STAGED bytes
/// re-verify.
inline PackInstallResult install_verified_pack(
    const std::filesystem::path& source_root, const SwapPackManifest& manifest,
    const std::vector<std::uint8_t>& trusted_public_key,
    const std::filesystem::path& install_base, bool reject_unknown_kinds = true) {
    namespace fs = std::filesystem;

    // 1. The pack must be trustworthy at the source before we copy anything.
    const auto v = verify_swap_pack(source_root, manifest, trusted_public_key);
    if (!v.ok()) return {PackInstall::NotTrusted, v.detail, {}};

    // 2. Reject symlinks and path escapes up front. A declared file that is a
    //    symlink (or resolves outside the pack root) could redirect the copy to
    //    bytes that were never hashed.
    std::error_code ec;
    const fs::path canon_root = fs::weakly_canonical(source_root, ec);
    for (const auto& f : manifest.files) {
        if (reject_unknown_kinds && f.kind == SwapPackKind::Unknown)
            return {PackInstall::UnknownKind, f.path, {}};
        const fs::path src = source_root / f.path;
        if (fs::is_symlink(src, ec) || ec)
            return {PackInstall::SymlinkRejected, f.path, {}};
        const fs::path canon = fs::weakly_canonical(src, ec);
        if (ec || (!canon_root.empty() &&
                   canon.string().rfind(canon_root.string(), 0) != 0))
            return {PackInstall::SymlinkRejected, f.path, {}};
    }

    const fs::path dest = install_base / swap_pack_content_id(manifest);

    // 3. Already installed → re-verify the immutable copy and reuse it. If a prior
    //    install was corrupted, fall through and re-stage over it.
    if (fs::exists(dest, ec)) {
        if (verify_swap_pack(dest, manifest, trusted_public_key).ok())
            return {PackInstall::Ok, "", dest};
        fs::permissions(dest, fs::perms::owner_all, fs::perm_options::add, ec);
        fs::remove_all(dest, ec);
    }

    // 4. Stage into a sibling temp dir, copy each verified file byte-for-byte
    //    (symlinks already excluded, so a plain read is safe), then re-verify the
    //    STAGED bytes before publishing — the installed bytes are the verified bytes.
    fs::create_directories(install_base, ec);
    const fs::path staging = install_base / (".staging-" + swap_pack_content_id(manifest));
    fs::remove_all(staging, ec);
    if (!fs::create_directories(staging, ec) && ec)
        return {PackInstall::IoError, "create staging dir: " + ec.message(), {}};

    auto fail_io = [&](std::string what) -> PackInstallResult {
        fs::permissions(staging, fs::perms::owner_all, fs::perm_options::add, ec);
        fs::remove_all(staging, ec);
        return {PackInstall::IoError, std::move(what), {}};
    };

    for (const auto& f : manifest.files) {
        const fs::path src = source_root / f.path;
        const fs::path out = staging / f.path;
        fs::create_directories(out.parent_path(), ec);
        std::ifstream in(src, std::ios::binary);
        if (!in) return fail_io("read " + f.path);
        std::vector<char> bytes((std::istreambuf_iterator<char>(in)),
                                std::istreambuf_iterator<char>());
        std::ofstream o(out, std::ios::binary | std::ios::trunc);
        if (!o) return fail_io("write " + f.path);
        o.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        o.close();
        if (!o) return fail_io("flush " + f.path);
        // Read-only file (owner read). The dir is locked down after the rename.
        fs::permissions(out, fs::perms::owner_read, fs::perm_options::replace, ec);
    }

    // 5. Re-verify the staged copy: the bytes we will publish must themselves match
    //    the signed hashes (guards a copy that silently corrupted or truncated).
    if (!verify_swap_pack(staging, manifest, trusted_public_key).ok())
        return fail_io("staged bytes failed re-verification");

    // 6. Publish atomically. If a concurrent installer won the race, its dir is the
    //    same content id (byte-identical), so either result is correct.
    fs::rename(staging, dest, ec);
    if (ec) {
        if (fs::exists(dest) &&
            verify_swap_pack(dest, manifest, trusted_public_key).ok()) {
            fs::permissions(staging, fs::perms::owner_all, fs::perm_options::add, ec);
            fs::remove_all(staging, ec);
            return {PackInstall::Ok, "", dest};
        }
        return fail_io("publish rename: " + ec.message());
    }
    // Lock the published dir read+exec only (owner). Files are already read-only.
    fs::permissions(dest, fs::perms::owner_read | fs::perms::owner_exec,
                    fs::perm_options::replace, ec);
    return {PackInstall::Ok, "", dest};
}

}  // namespace pulp::format::reload
