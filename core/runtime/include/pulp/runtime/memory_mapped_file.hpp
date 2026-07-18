#pragma once

// Memory-mapped file — RAII wrapper for mmap/MapViewOfFile
// Provides read-only or read-write memory-mapped access to files.

#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>

namespace pulp::runtime {

enum class MapMode { ReadOnly, ReadWrite };

struct FileIdentity {
    std::uint64_t volume = 0;
    std::uint64_t file = 0;
    std::uint64_t generation = 0;
    bool valid = false;
    friend bool operator==(const FileIdentity&, const FileIdentity&) = default;
};

FileIdentity file_identity(std::string_view path) noexcept;

class AccessPolicyTarget {
  public:
    AccessPolicyTarget() = default;
    ~AccessPolicyTarget();
    AccessPolicyTarget(AccessPolicyTarget&&) noexcept;
    AccessPolicyTarget& operator=(AccessPolicyTarget&&) noexcept;
    AccessPolicyTarget(const AccessPolicyTarget&) = delete;
    AccessPolicyTarget& operator=(const AccessPolicyTarget&) = delete;

    explicit operator bool() const noexcept;
    // Call after atomically moving the target to its final parent. On Windows
    // this restores the source DACL's inheritance state through the retained
    // handle; on POSIX the copied policy is already final.
    bool finalize_after_move() noexcept;
    bool sync() noexcept;

  private:
    friend class MemoryMappedFile;
#ifdef _WIN32
    void* handle_ = nullptr;
    void* descriptor_ = nullptr;
    void* dacl_ = nullptr;
    bool source_dacl_protected_ = true;
#else
    int fd_ = -1;
#endif
};

class MemoryMappedFile {

  public:
    MemoryMappedFile() = default;
    ~MemoryMappedFile();

    // Open and map a file. Returns false on failure.
    bool open(std::string_view path, MapMode mode = MapMode::ReadOnly);

    bool open(std::string_view path, MapMode mode, std::size_t maximum_bytes);

    // Open only when the final path component is a regular file rather than a
    // symbolic link or Windows reparse point.
    bool open_no_follow(std::string_view path, MapMode mode, std::size_t maximum_bytes);

    // Unmap and close.
    void close();

    // Mapped data pointer (nullptr if not open)
    const uint8_t* data() const {
        return data_;
    }
    uint8_t* mutable_data() {
        return data_;
    }

    // File size in bytes
    size_t size() const {
        return size_;
    }

    // Whether a file is currently mapped
    bool is_open() const {
        return data_ != nullptr;
    }

    // Copy the access policy of the opened file identity to an existing
    // regular file. The source policy is read from this object's retained
    // handle, not by reopening its pathname.
    bool copy_access_policy_to(std::string_view destination) const noexcept;
    AccessPolicyTarget prepare_access_policy_target(std::string_view destination) const noexcept;

    // True only when path currently resolves to the same file generation as
    // the retained open handle.
    bool path_refers_to_open_file(std::string_view path) const noexcept;
    FileIdentity opened_file_identity() const noexcept;

    // Copy exactly the bytes captured by the retained source handle into a new
    // regular file. The destination must not already exist. This avoids
    // reopening a mutable source pathname while establishing a stable snapshot.
    bool copy_contents_to_new_file(std::string_view destination) const noexcept;

    // No copy
    MemoryMappedFile(const MemoryMappedFile&) = delete;
    MemoryMappedFile& operator=(const MemoryMappedFile&) = delete;

    // Move
    MemoryMappedFile(MemoryMappedFile&& other) noexcept;
    MemoryMappedFile& operator=(MemoryMappedFile&& other) noexcept;

  private:
    bool open_impl(std::string_view path, MapMode mode, std::size_t maximum_bytes,
                   bool no_follow);
    uint8_t* data_ = nullptr;
    size_t size_ = 0;
#ifdef _WIN32
    void* file_handle_ = nullptr;
    void* mapping_handle_ = nullptr;
#else
    int fd_ = -1;
#endif
};

} // namespace pulp::runtime
