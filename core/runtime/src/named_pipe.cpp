#include <pulp/runtime/named_pipe.hpp>
#include <chrono>
#include <cstring>
#include <mutex>
#include <thread>

#ifdef _WIN32
#include <windows.h>
#else
#include <cerrno>
#include <pthread.h>
#include <poll.h>
#include <signal.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#endif

namespace pulp::runtime {

NamedPipe::~NamedPipe() { close(); }

NamedPipe::NamedPipe(NamedPipe&& other) noexcept
    : name_(std::move(other.name_)), is_server_(other.is_server_.exchange(false))
#ifdef _WIN32
    , handle_(other.handle_)
    , closing_(other.closing_.load())
    , connecting_(false)
#else
    , write_name_(std::move(other.write_name_))
    , read_fd_(other.read_fd_.exchange(-1))
    , write_fd_(other.write_fd_.exchange(-1))
    , closing_(other.closing_.load())
    , read_peer_confirmed_(other.read_peer_confirmed_.load())
#endif
{
#ifdef _WIN32
    other.handle_ = nullptr;
    other.closing_.store(true);
    other.connecting_.store(false);
#else
    other.closing_.store(true);
    other.read_peer_confirmed_.store(false);
#endif
}

NamedPipe& NamedPipe::operator=(NamedPipe&& other) noexcept {
    if (this != &other) {
        close();
        name_ = std::move(other.name_);
        is_server_.store(other.is_server_.exchange(false));
#ifdef _WIN32
        handle_ = other.handle_;
        closing_.store(other.closing_.load());
        connecting_.store(false);
        other.handle_ = nullptr;
        other.closing_.store(true);
        other.connecting_.store(false);
#else
        write_name_ = std::move(other.write_name_);
        read_fd_.store(other.read_fd_.exchange(-1));
        write_fd_.store(other.write_fd_.exchange(-1));
        closing_.store(other.closing_.load());
        read_peer_confirmed_.store(other.read_peer_confirmed_.load());
        other.closing_.store(true);
        other.read_peer_confirmed_.store(false);
#endif
    }
    return *this;
}

#ifdef _WIN32

bool NamedPipe::create_server(std::string_view name) {
    close();
    closing_.store(false);
    name_ = "\\\\.\\pipe\\" + std::string(name);
    is_server_.store(true);

    handle_ = CreateNamedPipeA(name_.c_str(),
        PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
        1, 65536, 65536, 0, nullptr);

    if (handle_ == INVALID_HANDLE_VALUE) {
        handle_ = nullptr;
        closing_.store(true);
        is_server_.store(false);
        return false;
    }

    OVERLAPPED overlapped{};
    overlapped.hEvent = CreateEventA(nullptr, TRUE, FALSE, nullptr);
    if (!overlapped.hEvent) {
        CloseHandle(handle_);
        handle_ = nullptr;
        closing_.store(true);
        is_server_.store(false);
        return false;
    }

    connecting_.store(true);
    const auto finish = [&](bool ok) {
        CloseHandle(overlapped.hEvent);
        connecting_.store(false);
        if (!ok || closing_.load()) {
            if (handle_) {
                CloseHandle(handle_);
                handle_ = nullptr;
            }
            closing_.store(true);
            is_server_.store(false);
            return false;
        }
        return true;
    };

    if (ConnectNamedPipe(handle_, &overlapped)) {
        return finish(true);
    }

    DWORD error = GetLastError();
    if (error == ERROR_PIPE_CONNECTED) {
        return finish(true);
    }
    if (error != ERROR_IO_PENDING) {
        return finish(false);
    }

    while (!closing_.load()) {
        const DWORD wait_result = WaitForSingleObject(overlapped.hEvent, 50);
        if (wait_result == WAIT_TIMEOUT) {
            continue;
        }
        if (wait_result != WAIT_OBJECT_0) {
            return finish(false);
        }

        DWORD transferred = 0;
        const BOOL ok = GetOverlappedResult(handle_, &overlapped, &transferred, FALSE);
        return finish(ok != FALSE);
    }

    CancelIoEx(handle_, &overlapped);
    DWORD transferred = 0;
    (void)GetOverlappedResult(handle_, &overlapped, &transferred, TRUE);
    return finish(false);
}

bool NamedPipe::connect_client(std::string_view name) {
    close();
    closing_.store(false);
    name_ = "\\\\.\\pipe\\" + std::string(name);
    is_server_.store(false);

    handle_ = CreateFileA(name_.c_str(), GENERIC_READ | GENERIC_WRITE,
        0, nullptr, OPEN_EXISTING, 0, nullptr);

    if (handle_ == INVALID_HANDLE_VALUE) {
        handle_ = nullptr;
        return false;
    }
    return true;
}

int NamedPipe::write(const uint8_t* data, size_t length) {
    if (!handle_) return -1;
    DWORD written;
    if (!WriteFile(handle_, data, static_cast<DWORD>(length), &written, nullptr))
        return -1;
    return static_cast<int>(written);
}

int NamedPipe::read(uint8_t* buffer, size_t buffer_size) {
    if (!handle_) return -1;
    DWORD bytes_read;
    if (!ReadFile(handle_, buffer, static_cast<DWORD>(buffer_size), &bytes_read, nullptr))
        return -1;
    return static_cast<int>(bytes_read);
}

void NamedPipe::close() {
    closing_.store(true);
    if (handle_) {
        CancelIoEx(handle_, nullptr);
        while (connecting_.load())
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        if (!handle_)
            return;
        const bool was_server = is_server_.exchange(false);
        if (was_server) DisconnectNamedPipe(handle_);
        CloseHandle(handle_);
        handle_ = nullptr;
    }
}

bool NamedPipe::is_open() const { return handle_ != nullptr; }

#else  // POSIX

namespace {

// Bound the client-side grace period for POSIX reply FIFO writer attachment.
constexpr auto kInitialReplyAttachWindow = std::chrono::seconds{2};

std::string reply_fifo_name(std::string_view name) {
    return std::string(name) + ".reply";
}

void close_fd(int fd) {
    if (fd >= 0) {
        ::close(fd);
    }
}

bool unlink_existing_fifo(const std::string& path) {
    struct stat st {};
    if (lstat(path.c_str(), &st) != 0)
        return errno == ENOENT;
    if (!S_ISFIFO(st.st_mode))
        return false;
    return unlink(path.c_str()) == 0 || errno == ENOENT;
}

bool set_nonblocking(int fd) {
    const int flags = fcntl(fd, F_GETFL, 0);
    return flags >= 0 && fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
}

bool set_close_on_exec(int fd) {
    const int flags = fcntl(fd, F_GETFD, 0);
    return flags >= 0 && fcntl(fd, F_SETFD, flags | FD_CLOEXEC) == 0;
}

bool set_no_sigpipe(int fd) {
#ifdef F_SETNOSIGPIPE
    return fcntl(fd, F_SETNOSIGPIPE, 1) == 0;
#else
    (void)fd;
    return true;
#endif
}

bool wait_for_fifo_writer(int& fd, const std::string& path,
                          const std::atomic<bool>& closing,
                          std::chrono::milliseconds timeout = std::chrono::milliseconds{0}) {
    const bool has_timeout = timeout.count() > 0;
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!closing.load()) {
        fd = ::open(path.c_str(), O_WRONLY | O_NONBLOCK);
        if (fd >= 0) {
            if (set_no_sigpipe(fd))
                return true;
            close_fd(fd);
            fd = -1;
            return false;
        }
        if (errno == EINTR)
            continue;
        if (errno == ENXIO) {
            if (has_timeout && std::chrono::steady_clock::now() >= deadline)
                return false;
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }
        return false;
    }
    return false;
}

ssize_t write_no_sigpipe(int fd, const uint8_t* data, size_t length) {
    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, SIGPIPE);

    sigset_t old_mask;
    const bool blocked = pthread_sigmask(SIG_BLOCK, &set, &old_mask) == 0;

    bool sigpipe_was_pending = false;
    if (blocked) {
        sigset_t pending;
        sigpending(&pending);
        sigpipe_was_pending = sigismember(&pending, SIGPIPE) == 1;
    }

    const ssize_t written = ::write(fd, data, length);
    const int saved_errno = errno;

    if (blocked && written < 0 && saved_errno == EPIPE && !sigpipe_was_pending) {
        sigset_t pending;
        sigpending(&pending);
        if (sigismember(&pending, SIGPIPE) == 1) {
            int signal = 0;
            (void)sigwait(&set, &signal);
        }
    }

    if (blocked)
        (void)pthread_sigmask(SIG_SETMASK, &old_mask, nullptr);
    errno = saved_errno;
    return written;
}

}  // namespace

bool NamedPipe::create_server(std::string_view name) {
    close();
    closing_.store(false);
    {
        std::scoped_lock fd_lock(read_fd_mutex_, write_fd_mutex_);
        name_ = std::string(name);
        write_name_ = reply_fifo_name(name_);
        is_server_.store(true);
    }

    // POSIX FIFOs are one-way. Use a pair under one public name:
    // name_ carries client -> server; write_name_ carries server -> client.
    bool public_fifo_created = false;
    bool reply_fifo_created = false;
    auto cleanup_created_fifos = [&] {
        if (reply_fifo_created)
            unlink_existing_fifo(write_name_);
        if (public_fifo_created)
            unlink_existing_fifo(name_);
    };
    if (!unlink_existing_fifo(name_) || !unlink_existing_fifo(write_name_)) {
        closing_.store(true);
        is_server_.store(false);
        return false;
    }

    if (mkfifo(name_.c_str(), 0666) != 0) {
        closing_.store(true);
        is_server_.store(false);
        return false;
    }
    public_fifo_created = true;
    if (mkfifo(write_name_.c_str(), 0666) != 0) {
        unlink_existing_fifo(name_);
        closing_.store(true);
        is_server_.store(false);
        return false;
    }
    reply_fifo_created = true;

    // Keep steady-state descriptors directional so peer shutdown is visible:
    // server reads the public FIFO and writes the reply FIFO; the client uses
    // the opposite direction. The reply writer is opened with retries so
    // create_server() blocks until a client has opened its read side, while
    // close() can still cancel the wait.
    int read_fd = ::open(name_.c_str(), O_RDONLY | O_NONBLOCK);
    int write_fd = -1;
    if (read_fd < 0 || !set_close_on_exec(read_fd) ||
        !wait_for_fifo_writer(write_fd, write_name_, closing_) ||
        !set_close_on_exec(write_fd) ||
        !set_nonblocking(read_fd) || !set_nonblocking(write_fd)) {
        close_fd(read_fd);
        close_fd(write_fd);
        cleanup_created_fifos();
        close();
        return false;
    }
    std::scoped_lock fd_lock(read_fd_mutex_, write_fd_mutex_);
    if (closing_.load()) {
        close_fd(read_fd);
        close_fd(write_fd);
        cleanup_created_fifos();
        is_server_.store(false);
        return false;
    }
    read_fd_.store(read_fd);
    write_fd_.store(write_fd);
    read_peer_confirmed_.store(true);
    return true;
}

bool NamedPipe::connect_client(std::string_view name) {
    close();
    closing_.store(false);
    {
        std::scoped_lock fd_lock(read_fd_mutex_, write_fd_mutex_);
        name_ = std::string(name);
        write_name_ = reply_fifo_name(name_);
        is_server_.store(false);
    }

    int write_fd = -1;
    if (!wait_for_fifo_writer(write_fd, name_, closing_, std::chrono::seconds(2))) {
        close();
        return false;
    }
    int read_fd = ::open(write_name_.c_str(), O_RDONLY | O_NONBLOCK);
    if (read_fd < 0 || write_fd < 0 ||
        !set_close_on_exec(read_fd) || !set_close_on_exec(write_fd) ||
        !set_no_sigpipe(write_fd) ||
        !set_nonblocking(read_fd) || !set_nonblocking(write_fd)) {
        close_fd(read_fd);
        close_fd(write_fd);
        close();
        return false;
    }
    std::scoped_lock fd_lock(read_fd_mutex_, write_fd_mutex_);
    if (closing_.load()) {
        close_fd(read_fd);
        close_fd(write_fd);
        return false;
    }
    read_fd_.store(read_fd);
    write_fd_.store(write_fd);
    read_peer_confirmed_.store(false);
    return true;
}

int NamedPipe::write(const uint8_t* data, size_t length) {
    std::lock_guard fd_lock(write_fd_mutex_);
    const int write_fd = write_fd_.load();
    if (write_fd < 0) return -1;
    while (!closing_.load()) {
        const ssize_t written = write_no_sigpipe(write_fd, data, length);
        if (written >= 0)
            return static_cast<int>(written);
        if (errno == EINTR)
            continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            pollfd pfd{write_fd, POLLOUT, 0};
            (void)::poll(&pfd, 1, 50);
            continue;
        }
        return -1;
    }
    return -1;
}

int NamedPipe::read(uint8_t* buffer, size_t buffer_size) {
    std::lock_guard fd_lock(read_fd_mutex_);
    const int read_fd = read_fd_.load();
    if (read_fd < 0) return -1;
    if (buffer_size == 0) return 0;

    // A POSIX client can open the reply FIFO before the server's writer has
    // attached; the first EOF in that window is not a peer close yet.
    const auto initial_eof_deadline = std::chrono::steady_clock::now() +
                                      kInitialReplyAttachWindow;
    auto wait_through_initial_eof = [&] {
        if (read_peer_confirmed_.load())
            return false;
        if (std::chrono::steady_clock::now() >= initial_eof_deadline)
            return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        return true;
    };
    auto finish_read = [&](ssize_t bytes) {
        if (bytes > 0)
            read_peer_confirmed_.store(true);
        if (bytes == 0 && wait_through_initial_eof())
            return -2;
        return static_cast<int>(bytes);
    };

    while (!closing_.load()) {
        pollfd pfd{read_fd, POLLIN | POLLHUP | POLLERR, 0};
        const int ready = ::poll(&pfd, 1, 50);
        if (ready < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        if (ready == 0) {
            const ssize_t bytes = ::read(read_fd, buffer, buffer_size);
            if (bytes >= 0) {
                const int result = finish_read(bytes);
                if (result != -2)
                    return result;
                continue;
            }
            if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)
                continue;
            return -1;
        }
        if ((pfd.revents & POLLIN) != 0) {
            const ssize_t bytes = ::read(read_fd, buffer, buffer_size);
            if (bytes >= 0) {
                const int result = finish_read(bytes);
                if (result != -2)
                    return result;
                continue;
            }
            if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)
                continue;
            return -1;
        }
        if ((pfd.revents & (POLLHUP | POLLERR | POLLNVAL)) != 0) {
            if (wait_through_initial_eof())
                continue;
            return 0;
        }
    }
    return -1;
}

void NamedPipe::close() {
    closing_.store(true);
    read_peer_confirmed_.store(false);
    std::scoped_lock fd_lock(read_fd_mutex_, write_fd_mutex_);
    close_fd(read_fd_.exchange(-1));
    close_fd(write_fd_.exchange(-1));
    const bool was_server = is_server_.exchange(false);
    if (was_server && !name_.empty()) {
        unlink_existing_fifo(name_);
        if (!write_name_.empty())
            unlink_existing_fifo(write_name_);
    }
}

bool NamedPipe::is_open() const {
    return read_fd_.load() >= 0 && write_fd_.load() >= 0;
}

#endif

int NamedPipe::write(std::string_view data) {
    return write(reinterpret_cast<const uint8_t*>(data.data()), data.size());
}

std::optional<std::string> NamedPipe::read_string(size_t max_size) {
    std::vector<uint8_t> buffer(max_size);
    int n = read(buffer.data(), buffer.size());
    if (n < 0) return std::nullopt;
    return std::string(reinterpret_cast<char*>(buffer.data()), static_cast<size_t>(n));
}

}  // namespace pulp::runtime
