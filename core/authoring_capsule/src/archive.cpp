#include <pulp/authoring_capsule/archive.hpp>

#include <pulp/authoring_capsule/safe_path.hpp>
#include <pulp/runtime/crypto.hpp>
#include <pulp/runtime/zip.hpp>

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <memory>
#include <memory_resource>
#include <new>
#include <optional>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#if defined(_WIN32)
#include <fcntl.h>
#include <io.h>
#include <share.h>
#include <sys/stat.h>
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#if defined(__linux__)
#include <linux/fs.h>
#include <sys/syscall.h>
#endif
#endif

// miniz publishes zlib-compatible macros (`crc32`, `deflate`, `compress`, …),
// so it is included after every other header to keep those names from
// rewriting declarations in the headers above.
//
// The vendored copy is compiled into pulp::runtime, which keeps its miniz
// include directory PRIVATE, so this target names external/miniz itself
// rather than widening another target's public surface.
#include <miniz.h>

namespace pulp::authoring_capsule {
namespace fs = std::filesystem;

namespace {

// ── Budget ledger ───────────────────────────────────────────────────────
//
// Admission runs against untrusted bytes, so every allocation it makes —
// miniz's central-directory state, its inflate window, the manifest buffer,
// the member table — is charged against one ceiling and refused rather than
// grown. A capsule cannot make the process allocate more than
// `max_working_set_bytes` no matter what its headers claim.

/// Represents miniz's private CRT stdio buffering, which is not observable
/// through the allocator hooks and so has to be reserved rather than measured.
constexpr std::uint64_t kStdioReserveBytes = 64u * 1024u;
/// Represents the fixed control structures (the `mz_zip_archive` itself and
/// the small API scratch around it) that are likewise not routed through the
/// ledger's allocator.
constexpr std::uint64_t kControlReserveBytes = 4u * 1024u;
/// Per-member allowance for the container overhead the member table costs on
/// top of the struct and the path bytes it stores.
constexpr std::uint64_t kMemberMetadataReserveBytes = 64u;

class BudgetLedger {
public:
    explicit BudgetLedger(std::uint64_t limit) noexcept : limit_(limit) {}

    bool acquire(std::uint64_t bytes) noexcept {
        if (bytes > limit_ || current_ > limit_ - bytes) {
            exceeded_ = true;
            return false;
        }
        current_ += bytes;
        peak_ = std::max(peak_, current_);
        return true;
    }

    void release(std::uint64_t bytes) noexcept {
        // An underflow means the accounting has lost track of a charge, which
        // would silently uncap the budget. Clamp rather than wrap: a wrapped
        // balance would read as "almost nothing outstanding" forever.
        current_ -= std::min(bytes, current_);
    }

    std::uint64_t peak() const noexcept { return peak_; }
    std::uint64_t current() const noexcept { return current_; }
    std::uint64_t remaining() const noexcept { return limit_ - current_; }
    bool exceeded() const noexcept { return exceeded_; }

private:
    std::uint64_t limit_ = 0;
    std::uint64_t current_ = 0;
    std::uint64_t peak_ = 0;
    bool exceeded_ = false;
};

struct AllocationHeader {
    void* raw = nullptr;
    std::uint64_t charge = 0;
    std::size_t requested = 0;
};

void* ledger_allocate(BudgetLedger& ledger, std::size_t requested, std::size_t alignment) {
    alignment = std::max(alignment, alignof(AllocationHeader));
    if (requested > std::numeric_limits<std::size_t>::max() - sizeof(AllocationHeader) - alignment)
        return nullptr;
    const auto charge =
        static_cast<std::uint64_t>(requested + sizeof(AllocationHeader) + alignment);
    if (!ledger.acquire(charge))
        return nullptr;
    void* raw = std::malloc(static_cast<std::size_t>(charge));
    if (raw == nullptr) {
        ledger.release(charge);
        return nullptr;
    }
    void* candidate = static_cast<char*>(raw) + sizeof(AllocationHeader);
    auto space = static_cast<std::size_t>(charge) - sizeof(AllocationHeader);
    void* aligned = std::align(alignment, std::max<std::size_t>(requested, 1), candidate, space);
    if (aligned == nullptr) {
        std::free(raw);
        ledger.release(charge);
        return nullptr;
    }
    auto* header =
        reinterpret_cast<AllocationHeader*>(static_cast<char*>(aligned) - sizeof(AllocationHeader));
    header->raw = raw;
    header->charge = charge;
    header->requested = requested;
    return aligned;
}

void ledger_deallocate(BudgetLedger& ledger, void* address) noexcept {
    if (address == nullptr)
        return;
    const auto* header = reinterpret_cast<const AllocationHeader*>(static_cast<char*>(address) -
                                                                   sizeof(AllocationHeader));
    const auto charge = header->charge;
    void* raw = header->raw;
    std::free(raw);
    ledger.release(charge);
}

class LedgerResource final : public std::pmr::memory_resource {
public:
    explicit LedgerResource(BudgetLedger& ledger) noexcept : ledger_(ledger) {}

private:
    void* do_allocate(std::size_t bytes, std::size_t alignment) override {
        if (auto* result = ledger_allocate(ledger_, bytes, alignment))
            return result;
        throw std::bad_alloc();
    }
    void do_deallocate(void* address, std::size_t, std::size_t) override {
        ledger_deallocate(ledger_, address);
    }
    bool do_is_equal(const std::pmr::memory_resource& other) const noexcept override {
        return this == &other;
    }
    BudgetLedger& ledger_;
};

/// A scoped charge for memory the ledger cannot see directly — a buffer the
/// caller will own, or a fixed overhead that has to be represented rather than
/// measured. Released when the scope ends, so the peak reflects the moment the
/// bytes were actually outstanding.
class Reservation {
public:
    Reservation(BudgetLedger& ledger, std::uint64_t bytes) noexcept
        : ledger_(&ledger), bytes_(bytes), acquired_(ledger.acquire(bytes)) {}
    Reservation(const Reservation&) = delete;
    Reservation& operator=(const Reservation&) = delete;
    ~Reservation() {
        if (acquired_)
            ledger_->release(bytes_);
    }
    explicit operator bool() const noexcept { return acquired_; }

private:
    BudgetLedger* ledger_;
    std::uint64_t bytes_;
    bool acquired_;
};

void* miniz_alloc(void* opaque, std::size_t items, std::size_t size) {
    if (items != 0 && size > std::numeric_limits<std::size_t>::max() / items)
        return nullptr;
    return ledger_allocate(*static_cast<BudgetLedger*>(opaque), items * size,
                           alignof(std::max_align_t));
}

void miniz_free(void* opaque, void* address) {
    ledger_deallocate(*static_cast<BudgetLedger*>(opaque), address);
}

void* miniz_realloc(void* opaque, void* address, std::size_t items, std::size_t size) {
    if (address == nullptr)
        return miniz_alloc(opaque, items, size);
    if (items == 0 || size == 0) {
        miniz_free(opaque, address);
        return nullptr;
    }
    if (size > std::numeric_limits<std::size_t>::max() / items)
        return nullptr;
    auto& ledger = *static_cast<BudgetLedger*>(opaque);
    auto* old_header =
        reinterpret_cast<AllocationHeader*>(static_cast<char*>(address) - sizeof(AllocationHeader));
    const auto old_size = old_header->requested;
    void* replacement = ledger_allocate(ledger, items * size, alignof(std::max_align_t));
    if (replacement == nullptr)
        return nullptr;
    std::memcpy(replacement, address, std::min(old_size, items * size));
    ledger_deallocate(ledger, address);
    return replacement;
}

void install_allocator(mz_zip_archive& zip, BudgetLedger& ledger) noexcept {
    zip.m_pAlloc = miniz_alloc;
    zip.m_pFree = miniz_free;
    zip.m_pRealloc = miniz_realloc;
    zip.m_pAlloc_opaque = &ledger;
}

// ── Error construction ──────────────────────────────────────────────────

CapsuleError error(CapsuleStatus status, std::string subject = {}, std::string required = {},
                   std::string found = {}) {
    return CapsuleError{status, std::move(subject), std::move(required), std::move(found)};
}

template <typename T>
runtime::Result<T, CapsuleError> fail(CapsuleStatus status, std::string subject = {},
                                      std::string required = {}, std::string found = {}) {
    return runtime::Err(
        error(status, std::move(subject), std::move(required), std::move(found)));
}

// ── ZIP container facts ─────────────────────────────────────────────────

constexpr std::uint16_t kMethodStore = 0;
constexpr std::uint16_t kMethodDeflate = 8;

/// Encrypted, strongly encrypted, or patch-data members. A capsule carries no
/// key material, so any of these is a member the reader could never honestly
/// expand — reject the container rather than expand part of it.
constexpr std::uint16_t kFlagEncrypted = 0x0001;
constexpr std::uint16_t kFlagStrongEncryption = 0x0040;
constexpr std::uint16_t kFlagCompressedPatch = 0x0020;
/// Central-directory encryption masks the real local header values, so the
/// sizes admission checked would not be the sizes that get expanded.
constexpr std::uint16_t kFlagMaskedLocalHeader = 0x2000;

/// MS-DOS attribute bits carried in the low byte of the external attributes.
constexpr std::uint32_t kDosAttrVolumeLabel = 0x08;
constexpr std::uint32_t kDosAttrDirectory = 0x10;

constexpr std::uint32_t kUnixFileTypeMask = 0170000u;
constexpr std::uint32_t kUnixRegularFile = 0100000u;
constexpr std::uint32_t kUnixSetIdAndSticky = 07000u;

constexpr const char* kManifestMemberPath = "capsule.json";

/// True when `expanded` claims a larger expansion of `compressed` than the
/// ratio ceiling permits. Guards the multiply so a hostile 64-bit size cannot
/// wrap the comparison into a pass.
bool expansion_ratio_exceeded(std::uint64_t expanded, std::uint64_t compressed,
                              std::uint32_t ratio) noexcept {
    if (ratio == 0)
        return expanded > 0;
    if (compressed == 0)
        return expanded > 0;
    if (compressed > std::numeric_limits<std::uint64_t>::max() / ratio)
        return false;
    return expanded > compressed * ratio;
}

// ── Bounded expansion sink ──────────────────────────────────────────────
//
// The expanded size in a ZIP header is written by whoever built the archive,
// so it is a claim, not a fact. The sink counts the bytes that actually come
// out of the inflater and refuses the moment the real count passes the
// ceiling, which is what stops a member that declares a kilobyte and expands
// to a gigabyte.

struct BoundedSink {
    std::vector<std::uint8_t>* out = nullptr;
    std::uint64_t ceiling = 0;
    std::uint64_t written = 0;
    bool overflowed = false;
    bool malformed = false;
};

std::size_t bounded_sink_write(void* opaque, mz_uint64 offset, const void* buffer,
                               std::size_t count) {
    auto& sink = *static_cast<BoundedSink*>(opaque);
    if (count == 0)
        return 0;
    // miniz streams a member strictly forwards. A non-monotonic offset means
    // the stream is not the one admission measured.
    if (offset != sink.written) {
        sink.malformed = true;
        return 0;
    }
    if (count > sink.ceiling - sink.written) {
        sink.overflowed = true;
        return 0;
    }
    const auto* bytes = static_cast<const std::uint8_t*>(buffer);
    try {
        sink.out->insert(sink.out->end(), bytes, bytes + count);
    } catch (const std::bad_alloc&) {
        sink.overflowed = true;
        return 0;
    }
    sink.written += count;
    return count;
}

// ── Deterministic ZIP emission ──────────────────────────────────────────
//
// The writer emits the container itself rather than driving miniz's writer,
// because miniz stamps every member with the current wall-clock time run
// through `localtime` — which makes the same inventory produce different
// bytes on two machines, or on one machine in two timezones. Every field
// below is either derived from the member's content or a constant.

constexpr std::uint32_t kLocalHeaderSignature = 0x04034b50u;
constexpr std::uint32_t kCentralHeaderSignature = 0x02014b50u;
constexpr std::uint32_t kEndOfCentralDirSignature = 0x06054b50u;
constexpr std::size_t kLocalHeaderBytes = 30;
constexpr std::size_t kCentralHeaderBytes = 46;
constexpr std::size_t kEndOfCentralDirBytes = 22;

/// 1980-01-01 00:00:00, the earliest instant the MS-DOS timestamp can encode.
/// A constant is the only timestamp that keeps a re-export byte-identical.
constexpr std::uint16_t kFixedDosTime = 0x0000;
constexpr std::uint16_t kFixedDosDate = 0x0021;

/// Unix host, ZIP specification 2.0 — the floor that supports deflate.
constexpr std::uint16_t kVersionMadeBy = 0x0314;
constexpr std::uint16_t kVersionNeeded = 20;
/// A constant regular-file mode. The writer records no host permissions: the
/// exporting machine's umask must not reach the receiving machine.
constexpr std::uint32_t kExternalAttrRegularFile = 0100644u << 16;
/// Bit 11 marks the member name as UTF-8 rather than CP437.
constexpr std::uint16_t kFlagUtf8Name = 0x0800;
/// Fixed level, so the compressed bytes are a pure function of the content.
constexpr int kDeflateLevel = 6;

/// ZIP's end-of-central-directory record counts members in 16 bits. A capsule
/// stays inside that without ZIP64, so the writer never emits ZIP64 records
/// and never has to make a version-dependent choice about them.
constexpr std::size_t kMaxZipMembers = 0xFFFFu;
/// ZIP32 records member offsets and sizes in 32 bits. The v1 compressed-archive
/// ceiling sits below that already, but a consumer's limits arrive as data, so
/// the writer refuses rather than silently truncating an offset.
constexpr std::uint64_t kMaxZip32Offset = 0xFFFFFFFFull;

void put_u16(std::vector<std::uint8_t>& out, std::uint16_t value) {
    out.push_back(static_cast<std::uint8_t>(value & 0xFFu));
    out.push_back(static_cast<std::uint8_t>((value >> 8) & 0xFFu));
}

void put_u32(std::vector<std::uint8_t>& out, std::uint32_t value) {
    out.push_back(static_cast<std::uint8_t>(value & 0xFFu));
    out.push_back(static_cast<std::uint8_t>((value >> 8) & 0xFFu));
    out.push_back(static_cast<std::uint8_t>((value >> 16) & 0xFFu));
    out.push_back(static_cast<std::uint8_t>((value >> 24) & 0xFFu));
}

bool name_needs_utf8_flag(std::string_view path) noexcept {
    return std::any_of(path.begin(), path.end(),
                       [](char c) { return static_cast<unsigned char>(c) >= 0x80u; });
}

// ── Owner-private staging file ──────────────────────────────────────────

/// A file created with O_EXCL and owner-only permissions, kept open by both
/// descriptor and stream so the bytes can be fenced to disk before publication.
class StagingFile {
public:
    StagingFile() = default;
    StagingFile(const StagingFile&) = delete;
    StagingFile& operator=(const StagingFile&) = delete;
    ~StagingFile() { close(); }

    bool open_exclusive(const fs::path& path) noexcept {
#if defined(_WIN32)
        int descriptor = -1;
        if (::_wsopen_s(&descriptor, path.c_str(),
                        _O_WRONLY | _O_CREAT | _O_EXCL | _O_BINARY, _SH_DENYNO,
                        _S_IREAD | _S_IWRITE) != 0)
            return false;
        stream_ = ::_fdopen(descriptor, "wb");
#else
        const int descriptor = ::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0600);
        if (descriptor < 0)
            return false;
        stream_ = ::fdopen(descriptor, "wb");
#endif
        if (stream_ == nullptr) {
#if defined(_WIN32)
            ::_close(descriptor);
#else
            ::close(descriptor);
#endif
            return false;
        }
        descriptor_ = descriptor;
        return true;
    }

    bool write(const void* data, std::size_t count) noexcept {
        if (count == 0)
            return true;
        return std::fwrite(data, 1, count, stream_) == count;
    }

    /// Flush through the stream and fence the bytes to the device. A rename
    /// that lands before the data does would publish a file whose contents can
    /// still be lost.
    bool sync_and_close() noexcept {
        if (stream_ == nullptr)
            return false;
        bool ok = std::fflush(stream_) == 0;
        if (ok) {
#if defined(_WIN32)
            ok = ::_commit(descriptor_) == 0;
#elif defined(__APPLE__) && defined(F_FULLFSYNC)
            // fsync alone lets Apple's drives keep the write in their own
            // cache; F_FULLFSYNC is the barrier that actually reaches media.
            ok = ::fcntl(descriptor_, F_FULLFSYNC) == 0 || ::fsync(descriptor_) == 0;
#else
            ok = ::fsync(descriptor_) == 0;
#endif
        }
        const bool closed = std::fclose(stream_) == 0;
        stream_ = nullptr;
        descriptor_ = -1;
        return ok && closed;
    }

    void close() noexcept {
        if (stream_ != nullptr) {
            std::fclose(stream_);
            stream_ = nullptr;
            descriptor_ = -1;
        }
    }

    bool is_open() const noexcept { return stream_ != nullptr; }

private:
    std::FILE* stream_ = nullptr;
    int descriptor_ = -1;
};

enum class PublishOutcome { published, destination_exists, failed };

/// Move a completed staging file onto its destination without ever replacing
/// an existing one. A plain rename would silently overwrite a file that
/// appeared after the caller's existence check, so each platform's
/// no-replace primitive is used and the link/unlink pair is the fallback.
PublishOutcome publish_no_replace(const fs::path& source, const fs::path& destination) noexcept {
#if defined(_WIN32)
    if (::MoveFileExW(source.c_str(), destination.c_str(), MOVEFILE_WRITE_THROUGH) != 0)
        return PublishOutcome::published;
    const auto last_error = ::GetLastError();
    return last_error == ERROR_ALREADY_EXISTS || last_error == ERROR_FILE_EXISTS
               ? PublishOutcome::destination_exists
               : PublishOutcome::failed;
#else
#if defined(__APPLE__)
    if (::renamex_np(source.c_str(), destination.c_str(), RENAME_EXCL) == 0)
        return PublishOutcome::published;
    if (errno == EEXIST)
        return PublishOutcome::destination_exists;
    if (errno != ENOTSUP && errno != ENOSYS && errno != EINVAL)
        return PublishOutcome::failed;
#elif defined(__linux__) && defined(SYS_renameat2)
    if (::syscall(SYS_renameat2, AT_FDCWD, source.c_str(), AT_FDCWD, destination.c_str(),
                  RENAME_NOREPLACE) == 0)
        return PublishOutcome::published;
    if (errno == EEXIST)
        return PublishOutcome::destination_exists;
    if (errno != ENOSYS && errno != EINVAL)
        return PublishOutcome::failed;
#endif
    // link() fails with EEXIST rather than replacing, so it publishes with the
    // same no-replace guarantee on filesystems whose kernel lacks the flag.
    if (::link(source.c_str(), destination.c_str()) != 0)
        return errno == EEXIST ? PublishOutcome::destination_exists : PublishOutcome::failed;
    // The destination is visible the instant link() succeeds. Failing to
    // remove the private source is cleanup debt, not a failed publication.
    ::unlink(source.c_str());
    return PublishOutcome::published;
#endif
}

/// Removes the staging file unless publication took it. A failed or abandoned
/// write must leave nothing behind that a person could mistake for an export.
class StagingGuard {
public:
    StagingGuard(StagingFile& file, fs::path path) : file_(&file), path_(std::move(path)) {}
    StagingGuard(const StagingGuard&) = delete;
    StagingGuard& operator=(const StagingGuard&) = delete;
    ~StagingGuard() {
        if (!armed_)
            return;
        // Windows refuses to unlink a file that is still open, so the stream is
        // closed before the private file is removed.
        file_->close();
        std::error_code ignored;
        fs::remove(path_, ignored);
    }
    void disarm() noexcept { armed_ = false; }

private:
    StagingFile* file_;
    fs::path path_;
    bool armed_ = true;
};

std::string random_staging_suffix() {
    auto bytes = runtime::secure_random_bytes(12);
    if (!bytes)
        return {};
    return runtime::hex_encode(*bytes);
}

}  // namespace

// ── Reader ──────────────────────────────────────────────────────────────

struct CapsuleArchive::Impl {
    explicit Impl(const CapsuleLimits& archive_limits)
        : limits(archive_limits), ledger(archive_limits.max_working_set_bytes), resource(ledger) {
        if (!ledger.acquire(kControlReserveBytes + kStdioReserveBytes))
            throw std::bad_alloc();
        fixed_charge = kControlReserveBytes + kStdioReserveBytes;
    }

    Impl(const Impl&) = delete;
    Impl& operator=(const Impl&) = delete;
    ~Impl() { close(); }

    void close() noexcept {
        if (closed)
            return;
        closed = true;
        // miniz frees its central-directory state through the ledger, so it
        // has to run before the fixed charges are given back.
        if (zip.m_pState != nullptr)
            mz_zip_reader_end(&zip);
        if (stream != nullptr) {
            std::fclose(stream);
            stream = nullptr;
        }
        members.clear();
        members.shrink_to_fit();
        archive_indices.clear();
        archive_indices.shrink_to_fit();
        lookup.clear();
        lookup.shrink_to_fit();
        manifest.clear();
        manifest.shrink_to_fit();
        ledger.release(metadata_charge);
        metadata_charge = 0;
        ledger.release(manifest_charge);
        manifest_charge = 0;
        ledger.release(fixed_charge);
        fixed_charge = 0;
    }

    /// Binary search over the path-ordered view of the member table. Reading
    /// every member of a 20 000-member capsule must not be quadratic.
    std::optional<std::size_t> find(std::string_view path) const noexcept {
        const auto found = std::lower_bound(
            lookup.begin(), lookup.end(), path,
            [this](std::uint32_t index, std::string_view key) {
                return members[index].path < key;
            });
        if (found == lookup.end() || members[*found].path != path)
            return std::nullopt;
        return static_cast<std::size_t>(*found);
    }

    /// Expand one member with the real inflated size bounded by `ceiling`.
    runtime::Result<std::vector<std::uint8_t>, CapsuleError> expand(std::size_t member,
                                                                    std::uint64_t ceiling) {
        const auto& info = members[member];
        // The header may only lower the ceiling, never raise it: a member that
        // claims less than the budget is held to its own claim, and one that
        // claims more is held to the budget.
        const std::uint64_t bound = std::min<std::uint64_t>(ceiling, info.expanded_bytes);
        if constexpr (sizeof(std::size_t) < sizeof(std::uint64_t)) {
            if (bound > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max()))
                return fail<std::vector<std::uint8_t>>(CapsuleStatus::archive_budget_exceeded,
                                                       info.path);
        }

        Reservation charge(ledger, bound + kMemberMetadataReserveBytes);
        if (!charge)
            return fail<std::vector<std::uint8_t>>(CapsuleStatus::archive_budget_exceeded,
                                                   info.path);

        std::vector<std::uint8_t> out;
        try {
            out.reserve(static_cast<std::size_t>(bound));
        } catch (const std::bad_alloc&) {
            return fail<std::vector<std::uint8_t>>(CapsuleStatus::archive_budget_exceeded,
                                                   info.path);
        }

        BoundedSink sink{&out, bound, 0, false, false};
        const bool expanded =
            mz_zip_reader_extract_to_callback(&zip, archive_indices[member], &bounded_sink_write,
                                              &sink, 0) != 0;
        if (!expanded) {
            if (!sink.malformed && (sink.overflowed || ledger.exceeded()))
                return fail<std::vector<std::uint8_t>>(CapsuleStatus::archive_budget_exceeded,
                                                       info.path);
            return fail<std::vector<std::uint8_t>>(CapsuleStatus::unsafe_archive, info.path);
        }
        // A member that expanded to fewer bytes than it declared is as much a
        // lie as one that expanded to more; miniz rejects it too, so this is
        // the belt to that suspenders.
        if (sink.written != info.expanded_bytes)
            return fail<std::vector<std::uint8_t>>(CapsuleStatus::unsafe_archive, info.path,
                                                   std::to_string(info.expanded_bytes),
                                                   std::to_string(sink.written));
        return runtime::Ok(std::move(out));
    }

    CapsuleLimits limits;
    BudgetLedger ledger;
    LedgerResource resource;
    mz_zip_archive zip{};
    std::FILE* stream = nullptr;
    std::vector<MemberInfo> members;
    std::vector<mz_uint> archive_indices;
    /// Member indices ordered by path, for lookup only.
    std::vector<std::uint32_t> lookup;
    std::vector<std::uint8_t> manifest;
    std::uint64_t fixed_charge = 0;
    std::uint64_t metadata_charge = 0;
    std::uint64_t manifest_charge = 0;
    bool closed = false;
};

CapsuleArchive::CapsuleArchive(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
CapsuleArchive::CapsuleArchive(CapsuleArchive&&) noexcept = default;
CapsuleArchive& CapsuleArchive::operator=(CapsuleArchive&&) noexcept = default;
CapsuleArchive::~CapsuleArchive() = default;

std::span<const MemberInfo> CapsuleArchive::members() const noexcept {
    return {impl_->members.data(), impl_->members.size()};
}

std::span<const std::uint8_t> CapsuleArchive::manifest_bytes() const noexcept {
    return {impl_->manifest.data(), impl_->manifest.size()};
}

runtime::Result<std::vector<std::uint8_t>, CapsuleError>
CapsuleArchive::read(std::string_view path) const {
    const auto member = impl_->find(path);
    if (!member)
        return fail<std::vector<std::uint8_t>>(CapsuleStatus::closure_violation,
                                               std::string(path));
    // The manifest is already expanded and charged; hand back the same bytes
    // rather than inflating it twice.
    if (*member == 0)
        return runtime::Ok(impl_->manifest);
    return impl_->expand(*member, impl_->limits.max_member_expanded_bytes);
}

std::uint64_t CapsuleArchive::peak_bytes() const noexcept {
    return impl_->ledger.peak();
}

runtime::Result<CapsuleArchive, CapsuleError> open_archive(const fs::path& path,
                                                           const CapsuleLimits& limits) {
    std::error_code ec;
    const auto status = fs::status(path, ec);
    if (ec || !fs::exists(status))
        return fail<CapsuleArchive>(CapsuleStatus::staging_failed, path.string());
    if (!fs::is_regular_file(status))
        return fail<CapsuleArchive>(CapsuleStatus::unsafe_archive, path.string(), "regular file");

    // Refuse an oversized container before a single header is parsed: the
    // cheapest rejection is the one that never opens the file.
    const auto compressed_size = fs::file_size(path, ec);
    if (ec)
        return fail<CapsuleArchive>(CapsuleStatus::staging_failed, path.string());
    if (compressed_size > limits.max_compressed_bytes)
        return fail<CapsuleArchive>(CapsuleStatus::archive_budget_exceeded, path.string(),
                                    std::to_string(limits.max_compressed_bytes),
                                    std::to_string(compressed_size));

    std::unique_ptr<CapsuleArchive::Impl> impl;
    try {
        impl = std::make_unique<CapsuleArchive::Impl>(limits);
    } catch (const std::bad_alloc&) {
        return fail<CapsuleArchive>(CapsuleStatus::archive_budget_exceeded, path.string());
    }

    install_allocator(impl->zip, impl->ledger);
#if defined(_WIN32)
    impl->stream = ::_wfopen(path.c_str(), L"rb");
#else
    impl->stream = std::fopen(path.c_str(), "rb");
#endif
    if (impl->stream == nullptr)
        return fail<CapsuleArchive>(CapsuleStatus::staging_failed, path.string());
    if (!mz_zip_reader_init_cfile(&impl->zip, impl->stream, 0, 0)) {
        const bool budget = impl->ledger.exceeded();
        return fail<CapsuleArchive>(budget ? CapsuleStatus::archive_budget_exceeded
                                           : CapsuleStatus::unsafe_archive,
                                    path.string());
    }

    const auto count = mz_zip_reader_get_num_files(&impl->zip);
    if (count > limits.max_members)
        return fail<CapsuleArchive>(CapsuleStatus::archive_budget_exceeded, path.string(),
                                    std::to_string(limits.max_members), std::to_string(count));
    if (count == 0)
        return fail<CapsuleArchive>(CapsuleStatus::manifest_not_first, path.string(),
                                    kManifestMemberPath);

    std::uint64_t total_compressed = 0;
    std::uint64_t total_expanded = 0;
    std::vector<std::string> normalized_paths;

    try {
        normalized_paths.reserve(count);
        impl->members.reserve(count);
        impl->archive_indices.reserve(count);
        impl->lookup.reserve(count);

        for (mz_uint index = 0; index < count; ++index) {
            // `mz_zip_archive_file_stat::m_filename` is a fixed 512-byte field
            // that silently truncates, and the path budget is larger than
            // that, so the name is always taken from the sized accessor.
            const auto name_size = mz_zip_reader_get_filename(&impl->zip, index, nullptr, 0);
            if (name_size == 0)
                return fail<CapsuleArchive>(CapsuleStatus::path_rejected, path.string());
            if (name_size - 1 > limits.max_path_bytes)
                return fail<CapsuleArchive>(CapsuleStatus::path_rejected, path.string(),
                                            std::to_string(limits.max_path_bytes),
                                            std::to_string(name_size - 1));

            std::pmr::vector<char> name_buffer(name_size, &impl->resource);
            if (mz_zip_reader_get_filename(&impl->zip, index, name_buffer.data(),
                                           name_buffer.size()) != name_size ||
                name_buffer.back() != '\0')
                return fail<CapsuleArchive>(CapsuleStatus::path_rejected, path.string());
            const std::string raw_name(name_buffer.data(), name_size - 1);

            mz_zip_archive_file_stat entry{};
            if (!mz_zip_reader_file_stat(&impl->zip, index, &entry))
                return fail<CapsuleArchive>(CapsuleStatus::unsafe_archive, raw_name);

            if (entry.m_is_encrypted ||
                (entry.m_bit_flag & (kFlagEncrypted | kFlagStrongEncryption | kFlagCompressedPatch |
                                    kFlagMaskedLocalHeader)) != 0)
                return fail<CapsuleArchive>(CapsuleStatus::unsafe_archive, raw_name, "unencrypted",
                                            "encrypted");
            if (entry.m_method != kMethodStore && entry.m_method != kMethodDeflate)
                return fail<CapsuleArchive>(CapsuleStatus::unsafe_archive, raw_name,
                                            "store or deflate", std::to_string(entry.m_method));
            if (!entry.m_is_supported)
                return fail<CapsuleArchive>(CapsuleStatus::unsafe_archive, raw_name);
            // Every member of a capsule is declared in `files[]`, and a
            // directory cannot be declared there, so a directory entry is a
            // member that could never satisfy closure.
            if (entry.m_is_directory || (!raw_name.empty() && raw_name.back() == '/'))
                return fail<CapsuleArchive>(CapsuleStatus::unsafe_archive, raw_name,
                                            "regular file", "directory");

            const auto dos_attributes = entry.m_external_attr & 0xFFu;
            if ((dos_attributes & (kDosAttrDirectory | kDosAttrVolumeLabel)) != 0)
                return fail<CapsuleArchive>(CapsuleStatus::unsafe_archive, raw_name,
                                            "regular file");
            const auto unix_mode = (entry.m_external_attr >> 16) & 0xFFFFu;
            // Mode 0 is what MS-DOS-host writers record; anything else must
            // describe a plain file. A symlink, device, fifo, or socket member
            // would let an archive reach outside its own staging directory.
            if (unix_mode != 0 && (unix_mode & kUnixFileTypeMask) != kUnixRegularFile)
                return fail<CapsuleArchive>(CapsuleStatus::unsafe_archive, raw_name,
                                            "regular file");
            if ((unix_mode & kUnixSetIdAndSticky) != 0)
                return fail<CapsuleArchive>(CapsuleStatus::unsafe_archive, raw_name,
                                            "no setuid, setgid, or sticky bit");

            // The manifest is fixed at index 0 under its exact name, so a
            // reader can find it without scanning a hostile central directory.
            if (index == 0 && raw_name != kManifestMemberPath)
                return fail<CapsuleArchive>(CapsuleStatus::manifest_not_first, raw_name,
                                            kManifestMemberPath, raw_name);

            auto normalized = admit_member_path(raw_name, limits);
            if (!normalized) {
                auto rejection = std::move(normalized).error();
                if (rejection.subject.empty())
                    rejection.subject = raw_name;
                return runtime::Err(std::move(rejection));
            }

            const std::uint64_t member_compressed = entry.m_comp_size;
            const std::uint64_t member_expanded = entry.m_uncomp_size;

            if (member_expanded > limits.max_member_expanded_bytes)
                return fail<CapsuleArchive>(CapsuleStatus::archive_budget_exceeded, *normalized,
                                            std::to_string(limits.max_member_expanded_bytes),
                                            std::to_string(member_expanded));
            // No compressed bytes cannot produce expanded bytes. The claim is
            // structurally impossible, not merely over budget.
            if (member_compressed == 0 && member_expanded > 0)
                return fail<CapsuleArchive>(CapsuleStatus::unsafe_archive, *normalized);
            if (expansion_ratio_exceeded(member_expanded, member_compressed,
                                         limits.max_expansion_ratio))
                return fail<CapsuleArchive>(CapsuleStatus::archive_budget_exceeded, *normalized,
                                            std::to_string(limits.max_expansion_ratio) + ":1");
            if (index == 0 && member_expanded > limits.max_manifest_bytes)
                return fail<CapsuleArchive>(CapsuleStatus::archive_budget_exceeded, *normalized,
                                            std::to_string(limits.max_manifest_bytes),
                                            std::to_string(member_expanded));

            if (member_compressed > limits.max_compressed_bytes - total_compressed)
                return fail<CapsuleArchive>(CapsuleStatus::archive_budget_exceeded, *normalized,
                                            std::to_string(limits.max_compressed_bytes));
            total_compressed += member_compressed;
            if (member_expanded > limits.max_expanded_bytes - total_expanded)
                return fail<CapsuleArchive>(CapsuleStatus::archive_budget_exceeded, *normalized,
                                            std::to_string(limits.max_expanded_bytes));
            total_expanded += member_expanded;

            const std::uint64_t metadata = sizeof(MemberInfo) + normalized->size() +
                                           kMemberMetadataReserveBytes;
            if (!impl->ledger.acquire(metadata))
                return fail<CapsuleArchive>(CapsuleStatus::archive_budget_exceeded, *normalized);
            impl->metadata_charge += metadata;

            MemberInfo info;
            info.path = *normalized;
            info.compressed_bytes = member_compressed;
            info.expanded_bytes = member_expanded;
            info.method = entry.m_method;
            impl->members.push_back(std::move(info));
            impl->archive_indices.push_back(index);
            impl->lookup.push_back(static_cast<std::uint32_t>(impl->lookup.size()));
            normalized_paths.push_back(std::move(*normalized));
        }
    } catch (const std::bad_alloc&) {
        return fail<CapsuleArchive>(CapsuleStatus::archive_budget_exceeded, path.string());
    }

    // The per-member ratio can be respected by every member while the archive
    // as a whole still expands far past the ceiling.
    if (expansion_ratio_exceeded(total_expanded, total_compressed, limits.max_expansion_ratio))
        return fail<CapsuleArchive>(CapsuleStatus::archive_budget_exceeded, path.string(),
                                    std::to_string(limits.max_expansion_ratio) + ":1");

    if (auto collision = check_collisions(normalized_paths); !collision)
        return runtime::Err(std::move(collision).error());

    std::sort(impl->lookup.begin(), impl->lookup.end(),
              [&impl](std::uint32_t left, std::uint32_t right) {
                  return impl->members[left].path < impl->members[right].path;
              });

    // Expanding the manifest here — and only the manifest — is what lets a
    // preview describe the capsule without touching a single payload byte.
    auto manifest = impl->expand(0, limits.max_manifest_bytes);
    if (!manifest)
        return runtime::Err(std::move(manifest).error());
    const std::uint64_t manifest_charge =
        manifest->size() + kMemberMetadataReserveBytes;
    if (!impl->ledger.acquire(manifest_charge))
        return fail<CapsuleArchive>(CapsuleStatus::archive_budget_exceeded, kManifestMemberPath);
    impl->manifest_charge = manifest_charge;
    impl->manifest = std::move(manifest).value();

    return runtime::Ok(CapsuleArchive(std::move(impl)));
}

// ── Writer ──────────────────────────────────────────────────────────────

runtime::Result<std::uint64_t, CapsuleError>
write_archive_no_replace(const std::vector<WriteMember>& members, const fs::path& destination,
                         const CapsuleLimits& limits) {
    BudgetLedger ledger(limits.max_working_set_bytes);
    Reservation fixed(ledger, kControlReserveBytes + kStdioReserveBytes);
    if (!fixed)
        return fail<std::uint64_t>(CapsuleStatus::archive_budget_exceeded, destination.string());

    if (members.size() > limits.max_members || members.size() > kMaxZipMembers)
        return fail<std::uint64_t>(CapsuleStatus::archive_budget_exceeded, destination.string(),
                                   std::to_string(std::min<std::size_t>(limits.max_members,
                                                                        kMaxZipMembers)),
                                   std::to_string(members.size()));

    // Validate the whole inventory before creating anything on disk, so a
    // rejected export never leaves a staging file behind.
    std::vector<std::string> normalized_paths;
    std::uint64_t total_expanded = 0;
    std::uint64_t central_directory_charge = 0;
    normalized_paths.reserve(members.size());
    for (const auto& member : members) {
        auto normalized = admit_member_path(member.path, limits);
        if (!normalized) {
            auto rejection = std::move(normalized).error();
            if (rejection.subject.empty())
                rejection.subject = member.path;
            return runtime::Err(std::move(rejection));
        }
        // The caller is required to hand over normalized paths. Writing a path
        // that differs from its normal form would produce an archive this
        // module's own reader rewrites on the way back in.
        if (*normalized != member.path)
            return fail<std::uint64_t>(CapsuleStatus::path_rejected, member.path, *normalized,
                                       member.path);

        const std::uint64_t bytes = member.bytes.size();
        if (bytes > limits.max_member_expanded_bytes ||
            bytes > std::numeric_limits<std::uint32_t>::max())
            return fail<std::uint64_t>(CapsuleStatus::archive_budget_exceeded, *normalized,
                                       std::to_string(limits.max_member_expanded_bytes),
                                       std::to_string(bytes));
        if (bytes > limits.max_expanded_bytes - total_expanded)
            return fail<std::uint64_t>(CapsuleStatus::archive_budget_exceeded, *normalized,
                                       std::to_string(limits.max_expanded_bytes));
        total_expanded += bytes;
        central_directory_charge += kCentralHeaderBytes + normalized->size();
        normalized_paths.push_back(std::move(*normalized));
    }

    // The writer holds itself to the reader's rule. Without this a caller could
    // mint an archive that this module's own reader then rejects with
    // manifest_not_first — a producer and consumer in the same library
    // disagreeing about the format, which is the kind of asymmetry that only
    // surfaces on someone else's machine.
    if (normalized_paths.empty() || normalized_paths.front() != kManifestPath)
        return fail<std::uint64_t>(CapsuleStatus::manifest_not_first,
                                   normalized_paths.empty() ? std::string{}
                                                            : normalized_paths.front(),
                                   std::string(kManifestPath),
                                   normalized_paths.empty() ? std::string{}
                                                            : normalized_paths.front());

    if (auto collision = check_collisions(normalized_paths); !collision)
        return runtime::Err(std::move(collision).error());

    Reservation central_reserve(ledger, central_directory_charge + kMemberMetadataReserveBytes);
    if (!central_reserve)
        return fail<std::uint64_t>(CapsuleStatus::archive_budget_exceeded, destination.string());

    // There is deliberately no early "does the destination exist" check here.
    // One would be cheaper on the failure path, but it would also be the check
    // that appears to work: a test can only observe the pre-check, so the
    // exclusive rename below — the only guard that actually holds when the
    // destination appears mid-write — would pass its tests while broken. One
    // guard, and it is the one that is exercised.
    std::error_code ec;
    auto parent = destination.parent_path();
    if (parent.empty())
        parent = fs::path(".");
    if (!fs::is_directory(parent, ec))
        return fail<std::uint64_t>(CapsuleStatus::staging_failed, parent.string());

    // A private sibling: same filesystem as the destination, so publication is
    // a metadata operation rather than a copy that could be seen half-done.
    StagingFile staging;
    fs::path staging_path;
    for (int attempt = 0; attempt < 8 && !staging.is_open(); ++attempt) {
        const auto suffix = random_staging_suffix();
        if (suffix.empty())
            return fail<std::uint64_t>(CapsuleStatus::staging_failed, parent.string());
        staging_path = parent / (".pulp-capsule-" + suffix + ".tmp");
        staging.open_exclusive(staging_path);
    }
    if (!staging.is_open())
        return fail<std::uint64_t>(CapsuleStatus::staging_failed, parent.string());
    StagingGuard guard(staging, staging_path);

    std::vector<std::uint8_t> central_directory;
    std::vector<std::uint8_t> header;
    std::uint64_t offset = 0;

    const auto size_ceiling = std::min<std::uint64_t>(limits.max_compressed_bytes,
                                                     kMaxZip32Offset);
    const auto over_budget = [&](std::uint64_t additional) {
        return additional > size_ceiling - offset;
    };

    for (std::size_t index = 0; index < members.size(); ++index) {
        const auto& path = normalized_paths[index];
        const auto& bytes = members[index].bytes;
        const auto raw_size = static_cast<std::uint32_t>(bytes.size());
        const auto crc = static_cast<std::uint32_t>(
            mz_crc32(MZ_CRC32_INIT, bytes.data(), bytes.size()));

        // Deflate at a fixed level, then keep whichever encoding is smaller.
        // Both halves of that choice are pure functions of the content, so the
        // decision is the same on every machine and every run.
        std::uint16_t method = kMethodStore;
        const std::uint8_t* payload = bytes.data();
        std::uint32_t payload_size = raw_size;
        std::optional<std::vector<std::uint8_t>> deflated;
        if (raw_size > 0) {
            // The deflate bound is the source size plus a small block
            // allowance; charging it keeps the writer inside the same
            // working-set ceiling the reader honours.
            const std::uint64_t compress_charge = static_cast<std::uint64_t>(raw_size) +
                                                  raw_size / 1024 + 128 +
                                                  kMemberMetadataReserveBytes;
            Reservation compress_reserve(ledger, compress_charge);
            // A member too large to compress within the ceiling is stored
            // rather than refused, so a bounded writer never turns a legitimate
            // export into a failure. The branch depends only on the member size
            // and the limits, both inputs, so identical content still writes
            // identical bytes.
            if (compress_reserve)
                deflated = runtime::deflate_compress(bytes.data(), bytes.size(), kDeflateLevel);
            if (deflated && deflated->size() < bytes.size()) {
                method = kMethodDeflate;
                payload = deflated->data();
                payload_size = static_cast<std::uint32_t>(deflated->size());
            } else {
                deflated.reset();
            }
        }

        const auto flags = static_cast<std::uint16_t>(name_needs_utf8_flag(path) ? kFlagUtf8Name : 0);
        const auto local_header_offset = static_cast<std::uint32_t>(offset);

        header.clear();
        put_u32(header, kLocalHeaderSignature);
        put_u16(header, kVersionNeeded);
        put_u16(header, flags);
        put_u16(header, method);
        put_u16(header, kFixedDosTime);
        put_u16(header, kFixedDosDate);
        put_u32(header, crc);
        put_u32(header, payload_size);
        put_u32(header, raw_size);
        put_u16(header, static_cast<std::uint16_t>(path.size()));
        // No extra field: every ZIP extra field in common use carries host
        // state — timestamps, uid/gid, filesystem attributes.
        put_u16(header, 0);

        // A local header is a fixed 30-byte record. A different size means a
        // field was emitted at the wrong width, which would misplace every
        // offset the central directory records.
        if (header.size() != kLocalHeaderBytes)
            return fail<std::uint64_t>(CapsuleStatus::staging_failed, staging_path.string());
        if (over_budget(header.size() + path.size() + payload_size))
            return fail<std::uint64_t>(CapsuleStatus::archive_budget_exceeded, path,
                                       std::to_string(size_ceiling));
        if (!staging.write(header.data(), header.size()) ||
            !staging.write(path.data(), path.size()) || !staging.write(payload, payload_size))
            return fail<std::uint64_t>(CapsuleStatus::staging_failed, staging_path.string());
        offset += header.size() + path.size() + payload_size;

        put_u32(central_directory, kCentralHeaderSignature);
        put_u16(central_directory, kVersionMadeBy);
        put_u16(central_directory, kVersionNeeded);
        put_u16(central_directory, flags);
        put_u16(central_directory, method);
        put_u16(central_directory, kFixedDosTime);
        put_u16(central_directory, kFixedDosDate);
        put_u32(central_directory, crc);
        put_u32(central_directory, payload_size);
        put_u32(central_directory, raw_size);
        put_u16(central_directory, static_cast<std::uint16_t>(path.size()));
        put_u16(central_directory, 0);  // extra field length
        put_u16(central_directory, 0);  // comment length
        put_u16(central_directory, 0);  // disk number start
        put_u16(central_directory, 0);  // internal attributes
        put_u32(central_directory, kExternalAttrRegularFile);
        put_u32(central_directory, local_header_offset);
        central_directory.insert(central_directory.end(), path.begin(), path.end());
    }

    const auto central_offset = static_cast<std::uint32_t>(offset);
    if (over_budget(central_directory.size() + kEndOfCentralDirBytes))
        return fail<std::uint64_t>(CapsuleStatus::archive_budget_exceeded, destination.string(),
                                   std::to_string(size_ceiling));
    if (!staging.write(central_directory.data(), central_directory.size()))
        return fail<std::uint64_t>(CapsuleStatus::staging_failed, staging_path.string());
    offset += central_directory.size();

    header.clear();
    put_u32(header, kEndOfCentralDirSignature);
    put_u16(header, 0);  // this disk
    put_u16(header, 0);  // disk with the central directory
    put_u16(header, static_cast<std::uint16_t>(members.size()));
    put_u16(header, static_cast<std::uint16_t>(members.size()));
    put_u32(header, static_cast<std::uint32_t>(central_directory.size()));
    put_u32(header, central_offset);
    put_u16(header, 0);  // archive comment length
    if (header.size() != kEndOfCentralDirBytes)
        return fail<std::uint64_t>(CapsuleStatus::staging_failed, staging_path.string());
    if (!staging.write(header.data(), header.size()))
        return fail<std::uint64_t>(CapsuleStatus::staging_failed, staging_path.string());
    offset += header.size();

    if (!staging.sync_and_close())
        return fail<std::uint64_t>(CapsuleStatus::staging_failed, staging_path.string());

    switch (publish_no_replace(staging_path, destination)) {
        case PublishOutcome::published:
            guard.disarm();
            break;
        case PublishOutcome::destination_exists:
            return fail<std::uint64_t>(CapsuleStatus::publication_conflict, destination.string());
        case PublishOutcome::failed:
            return fail<std::uint64_t>(CapsuleStatus::staging_failed, destination.string());
    }

    // The peak charge, not the archive size: a budget regression is the fact a
    // receipt cannot recover afterwards, while the size is on the filesystem.
    return runtime::Ok(ledger.peak());
}

}  // namespace pulp::authoring_capsule
