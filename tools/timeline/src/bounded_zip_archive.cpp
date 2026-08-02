#include "bounded_zip_archive.hpp"

#include <pulp/interchange/export_plan.hpp>
#include <pulp/project_package/atomic_publisher.hpp>
#include <pulp/timeline/asset_path.hpp>

#include <miniz.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <map>
#include <memory_resource>
#include <new>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

namespace pulp::tools::timeline::detail {
namespace fs = std::filesystem;

namespace {

class BudgetLedger {
  public:
    explicit BudgetLedger(std::uint64_t limit) : limit_(limit) {}
    ~BudgetLedger() {
        if (current_ != 0)
            std::abort();
    }

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
        if (bytes > current_)
            std::abort();
        current_ -= bytes;
    }
    std::uint64_t peak() const noexcept {
        return peak_;
    }
    std::uint64_t current() const noexcept {
        return current_;
    }
    std::uint64_t remaining() const noexcept {
        return limit_ - current_;
    }
    bool exceeded() const noexcept {
        return exceeded_;
    }

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
    explicit LedgerResource(BudgetLedger& ledger) : ledger_(ledger) {}

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

class Reservation {
  public:
    Reservation(BudgetLedger& ledger, std::uint64_t bytes)
        : ledger_(&ledger), bytes_(ledger.acquire(bytes) ? bytes : 0), acquired_(bytes_ == bytes) {}
    ~Reservation() {
        if (acquired_)
            ledger_->release(bytes_);
    }
    explicit operator bool() const noexcept {
        return acquired_;
    }

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

void install_allocator(mz_zip_archive& zip, BudgetLedger& ledger) {
    zip.m_pAlloc = miniz_alloc;
    zip.m_pFree = miniz_free;
    zip.m_pRealloc = miniz_realloc;
    zip.m_pAlloc_opaque = &ledger;
}

MZ_FILE* open_zip_file(const fs::path& path, bool write) noexcept {
#ifdef _WIN32
    return ::_wfopen(path.c_str(), write ? L"w+b" : L"rb");
#else
    return std::fopen(path.c_str(), write ? "w+b" : "rb");
#endif
}

std::optional<std::uint64_t>
external_artifact_reserve_impl(const pulp::interchange::ExportArtifacts& artifacts) noexcept {
    if (artifacts.artifacts.capacity() > UINT64_MAX / sizeof(pulp::interchange::ExportArtifact))
        return std::nullopt;
    std::uint64_t total = static_cast<std::uint64_t>(artifacts.artifacts.capacity()) *
                          sizeof(pulp::interchange::ExportArtifact);
    if (kExternalArtifactMetadataReserveBytes > UINT64_MAX - total)
        return std::nullopt;
    total += kExternalArtifactMetadataReserveBytes;
    for (const auto& artifact : artifacts.artifacts) {
        const auto logical = static_cast<std::uint64_t>(artifact.name.capacity()) + 1u +
                             artifact.bytes.capacity() + 2u * kExternalArtifactMetadataReserveBytes;
        if (logical > UINT64_MAX - total)
            return std::nullopt;
        total += logical;
    }
    return total;
}

} // namespace

struct AllocationBudget::Impl {
    explicit Impl(std::uint64_t limit) : ledger(limit), resource(ledger) {
        if (!ledger.acquire(kZipControlAndApiReserveBytes))
            throw std::bad_alloc();
    }
    ~Impl() {
        if (external != 0)
            ledger.release(external);
        external = 0;
        ledger.release(kZipControlAndApiReserveBytes);
        if (ledger.current() != 0)
            std::abort();
    }
    BudgetLedger ledger;
    LedgerResource resource;
    std::uint64_t external = 0;
};

AllocationBudget::AllocationBudget(std::uint64_t limit) : impl_(std::make_unique<Impl>(limit)) {}
AllocationBudget::AllocationBudget(AllocationBudget&&) noexcept = default;
AllocationBudget& AllocationBudget::operator=(AllocationBudget&&) noexcept = default;
AllocationBudget::~AllocationBudget() = default;
std::pmr::memory_resource* AllocationBudget::resource() noexcept {
    return &impl_->resource;
}
bool AllocationBudget::acquire_external(std::uint64_t bytes) noexcept {
    if (!impl_->ledger.acquire(bytes))
        return false;
    impl_->external += bytes;
    return true;
}
void AllocationBudget::release_external(std::uint64_t bytes) noexcept {
    if (bytes > impl_->external)
        std::abort();
    impl_->external -= bytes;
    impl_->ledger.release(bytes);
}
std::uint64_t AllocationBudget::peak_bytes() const noexcept {
    return impl_->ledger.peak();
}
std::uint64_t AllocationBudget::balance_bytes() const noexcept {
    return impl_->ledger.current();
}
std::uint64_t AllocationBudget::remaining_bytes() const noexcept {
    return impl_->ledger.remaining();
}

struct BoundedZipArchive::Impl {
    using String = std::pmr::string;
    using Bytes = std::pmr::vector<std::uint8_t>;
    using Entries = std::pmr::map<String, Bytes, std::less<>>;
    using Paths = std::pmr::unordered_set<String>;

    explicit Impl(std::uint64_t limit)
        : ledger(limit), resource(ledger), entries(&resource),
          media_paths(std::in_place, &resource) {
        if (!ledger.acquire(kZipControlAndApiReserveBytes))
            throw std::bad_alloc();
    }
    ~Impl() {
        if (finish().final_balance_bytes != 0)
            std::abort();
    }
    ZipBudgetStats finish() noexcept {
        if (!closed) {
            media_paths.reset();
            entries.clear();
            if (external != 0)
                ledger.release(external);
            external = 0;
            ledger.release(kZipControlAndApiReserveBytes);
            closed = true;
        }
        return {ledger.peak(), ledger.current()};
    }

    BudgetLedger ledger;
    LedgerResource resource;
    Entries entries;
    std::optional<Paths> media_paths;
    std::uint64_t external = 0;
    bool closed = false;
};

BoundedZipArchive::BoundedZipArchive(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
BoundedZipArchive::BoundedZipArchive(BoundedZipArchive&&) noexcept = default;
BoundedZipArchive& BoundedZipArchive::operator=(BoundedZipArchive&&) noexcept = default;
BoundedZipArchive::~BoundedZipArchive() = default;

std::optional<std::span<const std::uint8_t>>
BoundedZipArchive::find(std::string_view name) const noexcept {
    const auto found = impl_->entries.find(name);
    if (found == impl_->entries.end())
        return std::nullopt;
    return std::span<const std::uint8_t>(found->second.data(), found->second.size());
}
bool BoundedZipArchive::retain_media_path(std::string_view name) noexcept {
    try {
        impl_->media_paths->emplace(name.data(), name.size());
        return true;
    } catch (const std::bad_alloc&) {
        return false;
    }
}
bool BoundedZipArchive::publish_retained_media(project_package::AtomicPublisher& publisher) const {
    for (const auto& path : *impl_->media_paths) {
        const auto entry = impl_->entries.find(path);
        if (entry == impl_->entries.end())
            return false;
        auto staged = publisher.write(path, std::span<const std::uint8_t>(entry->second));
        if (!staged || !staged.value())
            return false;
    }
    return true;
}
bool BoundedZipArchive::acquire_external(std::uint64_t bytes) noexcept {
    if (!impl_->ledger.acquire(bytes))
        return false;
    impl_->external += bytes;
    return true;
}
void BoundedZipArchive::release_external(std::uint64_t bytes) noexcept {
    if (bytes > impl_->external)
        std::abort();
    impl_->external -= bytes;
    impl_->ledger.release(bytes);
}
std::uint64_t BoundedZipArchive::peak_bytes() const noexcept {
    return impl_->ledger.peak();
}
ZipBudgetStats BoundedZipArchive::close() noexcept {
    auto stats = impl_->finish();
    impl_.reset();
    return stats;
}

runtime::Result<BoundedZipArchive, std::string>
read_bounded_zip_archive(const fs::path& input, std::uint64_t limit, std::size_t max_files,
                         std::size_t max_entries, std::size_t max_path) {
    std::unique_ptr<BoundedZipArchive::Impl> impl;
    try {
        impl = std::make_unique<BoundedZipArchive::Impl>(limit);
    } catch (const std::bad_alloc&) {
        return runtime::Err(std::string("DAWproject ZIP exceeds the working-set limit"));
    }
    Reservation io(impl->ledger, kZipStdioReserveBytes);
    if (!io)
        return runtime::Err(std::string("DAWproject ZIP exceeds the working-set limit"));
    mz_zip_archive zip{};
    install_allocator(zip, impl->ledger);
    auto* file = open_zip_file(input, false);
    if (file == nullptr || !mz_zip_reader_init_cfile(&zip, file, 0, 0)) {
        if (zip.m_pState != nullptr)
            mz_zip_reader_end(&zip);
        if (file != nullptr)
            std::fclose(file);
        return runtime::Err(std::string(impl->ledger.exceeded()
                                            ? "DAWproject ZIP exceeds the working-set limit"
                                            : "input is not a readable DAWproject ZIP container"));
    }
    bool ok = true;
    std::string_view message;
    const auto count = mz_zip_reader_get_num_files(&zip);
    if (count > max_entries) {
        ok = false;
        message = "DAWproject ZIP has too many entries";
    }
    std::size_t files = 0;
    try {
        for (mz_uint index = 0; ok && index < count; ++index) {
            mz_zip_archive_file_stat stat{};
            const auto name_size = mz_zip_reader_get_filename(&zip, index, nullptr, 0);
            if (!mz_zip_reader_file_stat(&zip, index, &stat) || name_size == 0 ||
                name_size - 1 > max_path) {
                ok = false;
                message = "DAWproject ZIP contains an unsupported or unsafe entry";
                break;
            }
            std::pmr::vector<char> name_buffer(name_size, &impl->resource);
            if (mz_zip_reader_get_filename(&zip, index, name_buffer.data(), name_buffer.size()) !=
                    name_size ||
                name_buffer.back() != '\0' || stat.m_is_encrypted) {
                ok = false;
                message = "DAWproject ZIP contains an unsupported or unsafe entry";
                break;
            }
            BoundedZipArchive::Impl::String name(name_buffer.data(), name_size - 1,
                                                 &impl->resource);
            if (stat.m_is_directory && !name.empty() && name.back() == '/')
                name.pop_back();
            if (name.empty() || !pulp::timeline::package_relative_path_is_portable(name)) {
                ok = false;
                message = "DAWproject ZIP contains an unsupported or unsafe entry";
                break;
            }
            const auto unix_mode = static_cast<unsigned>((stat.m_external_attr >> 16) & 0xffffu);
            if ((unix_mode & 0170000u) == 0120000u) {
                ok = false;
                message = "DAWproject ZIP must not contain symbolic links";
                break;
            }
            if (stat.m_is_directory)
                continue;
            if (++files > max_files ||
                stat.m_uncomp_size > std::numeric_limits<std::size_t>::max()) {
                ok = false;
                message = files > max_files ? "DAWproject ZIP has too many files"
                                            : "DAWproject ZIP entry is too large";
                break;
            }
            BoundedZipArchive::Impl::Bytes bytes(static_cast<std::size_t>(stat.m_uncomp_size),
                                                 &impl->resource);
            if (!mz_zip_reader_extract_to_mem(&zip, index, bytes.data(), bytes.size(), 0)) {
                ok = false;
                message = impl->ledger.exceeded() ? "DAWproject ZIP exceeds the working-set limit"
                                                  : "DAWproject ZIP contains an unreadable entry";
                break;
            }
            if (!impl->entries.emplace(std::move(name), std::move(bytes)).second) {
                ok = false;
                message = "DAWproject ZIP contains a duplicate entry";
                break;
            }
        }
    } catch (const std::bad_alloc&) {
        ok = false;
        message = "DAWproject ZIP exceeds the working-set limit";
    }
    mz_zip_reader_end(&zip);
    if (std::fclose(file) != 0) {
        ok = false;
        message = "could not close DAWproject ZIP input";
    }
    if (!ok)
        return runtime::Err(std::string(message));
    return runtime::Ok(BoundedZipArchive(std::move(impl)));
}

runtime::Result<std::uint64_t, DawProjectArchiveError>
write_dawproject_archive_no_replace(const pulp::interchange::ExportArtifacts& artifacts,
                                    const fs::path& destination, std::uint64_t limit) {
    BudgetLedger ledger(limit);
    const auto external = external_artifact_reserve_impl(artifacts);
    if (!external)
        return runtime::Err(DawProjectArchiveError{DawProjectArchiveErrorCode::Export,
                                                   "DAWproject artifact reserve overflow"});
    Reservation artifacts_reserve(ledger, *external);
    Reservation io(ledger, kZipStdioReserveBytes);
    Reservation control(ledger, kZipControlAndApiReserveBytes);
    if (!artifacts_reserve || !io || !control)
        return runtime::Err(DawProjectArchiveError{
            DawProjectArchiveErrorCode::Export, "DAWproject export exceeds the working-set limit"});
    auto publisher = project_package::AtomicPublisher::create_file(destination);
    if (!publisher)
        return runtime::Err(
            DawProjectArchiveError{DawProjectArchiveErrorCode::Publish,
                                   "output file must not exist and its parent must exist"});
    const auto staging = publisher->staging_file();
    mz_zip_archive zip{};
    install_allocator(zip, ledger);
    auto* file = open_zip_file(staging, true);
    bool ok = file != nullptr && mz_zip_writer_init_cfile(&zip, file, 0) != 0;
    try {
        LedgerResource resource(ledger);
        std::pmr::unordered_set<std::pmr::string> names(&resource);
        names.reserve(artifacts.artifacts.size());
        for (const auto& artifact : artifacts.artifacts) {
            if (!ok)
                break;
            std::pmr::string name(artifact.name, &resource);
            ok = pulp::timeline::package_relative_path_is_portable(name) &&
                 names.emplace(std::move(name)).second &&
                 mz_zip_writer_add_mem(&zip, artifact.name.c_str(), artifact.bytes.data(),
                                       artifact.bytes.size(), MZ_DEFAULT_COMPRESSION) != 0;
        }
        if (ok)
            ok = mz_zip_writer_finalize_archive(&zip) != 0;
    } catch (const std::bad_alloc&) {
        ok = false;
    }
    if (zip.m_pState != nullptr)
        mz_zip_writer_end(&zip);
    if (file != nullptr && std::fclose(file) != 0)
        ok = false;
    if (!ok)
        return runtime::Err(
            DawProjectArchiveError{DawProjectArchiveErrorCode::Export,
                                   "could not build bounded DAWproject ZIP container"});
    const auto peak = ledger.peak();
    auto publication = publisher->commit_file(staging);
    if (!publication || publication.value() == project_package::AtomicPublishOutcome::NotPublished)
        return runtime::Err(DawProjectArchiveError{
            DawProjectArchiveErrorCode::Publish, "output file appeared before atomic publication"});
    if (publication.value() == project_package::AtomicPublishOutcome::PublishedDurabilityUncertain)
        return runtime::Err(
            DawProjectArchiveError{DawProjectArchiveErrorCode::Publish,
                                   "output file publication durability is uncertain"});
    return runtime::Ok(peak);
}

std::optional<std::uint64_t>
retained_external_artifact_reserve(const pulp::interchange::ExportArtifacts& artifacts) noexcept {
    return external_artifact_reserve_impl(artifacts);
}

runtime::Result<ZipBudgetStats, std::string>
inspect_dawproject_archive(const fs::path& input, std::uint64_t limit, std::size_t max_files,
                           std::size_t max_entries, std::size_t max_path) {
    ZipBudgetStats stats;
    {
        auto archive = read_bounded_zip_archive(input, limit, max_files, max_entries, max_path);
        if (!archive)
            return runtime::Err(archive.error());
        stats = archive.value().close();
    }
    return runtime::Ok(stats);
}

} // namespace pulp::tools::timeline::detail
