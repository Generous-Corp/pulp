#pragma once

// Inter-process file lock — prevents multiple processes from accessing
// the same resource simultaneously.

#include <cstdint>
#include <string>
#include <string_view>

namespace pulp::runtime {

class InterProcessLock {

  public:
    /// Create a lock with the given name (used to derive the lock file path).
    explicit InterProcessLock(std::string_view name);
    ~InterProcessLock();

    /// Try to acquire the lock. Returns true if acquired.
    bool try_lock();

    /// Try to acquire a shared/read lock. Multiple shared holders may coexist.
    bool try_lock_shared();

    /// Release the lock.
    void unlock();

    /// Whether this instance holds the lock.
    bool is_locked() const {
        return mode_ != Mode::Unlocked;
    }

    // No copy or move
    InterProcessLock(const InterProcessLock&) = delete;
    InterProcessLock& operator=(const InterProcessLock&) = delete;

  private:
    std::string lock_path_;
    enum class Mode : std::uint8_t { Unlocked, Shared, Exclusive };
    Mode mode_ = Mode::Unlocked;
#ifdef _WIN32
    void* handle_ = nullptr;
#else
    int fd_ = -1;
#endif
};

} // namespace pulp::runtime
