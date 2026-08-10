// SPDX-License-Identifier: MIT
#include <catch2/catch_test_macros.hpp>
#include <pulp/platform/child_process.hpp>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <fcntl.h>
#include <signal.h>
#include <unistd.h>
#endif

using namespace pulp::platform;

namespace {

std::vector<std::uint8_t> bytes_of(std::string_view text) {
    return {text.begin(), text.end()};
}

bool process_is_alive(int process_id) {
#ifdef _WIN32
    const auto process = OpenProcess(SYNCHRONIZE, FALSE, static_cast<DWORD>(process_id));
    if (!process)
        return false;
    const bool alive = WaitForSingleObject(process, 0) == WAIT_TIMEOUT;
    CloseHandle(process);
    return alive;
#else
    return ::kill(process_id, 0) == 0 || errno == EPERM;
#endif
}

std::size_t open_handle_count() {
#ifdef _WIN32
    DWORD count = 0;
    return GetProcessHandleCount(GetCurrentProcess(), &count) ? count : 0;
#else
    std::size_t count = 0;
    const auto configured_limit = ::sysconf(_SC_OPEN_MAX);
    const auto upper = configured_limit > 0 ? std::min<long>(configured_limit, 4096) : 4096;
    for (int descriptor = 0; descriptor < upper; ++descriptor) {
        errno = 0;
        if (::fcntl(descriptor, F_GETFD) >= 0 || errno != EBADF)
            ++count;
    }
    return count;
#endif
}

void require_failed_child_was_joined(ChildProcess& child, int observed_process_id) {
    REQUIRE(observed_process_id > 0);
    CHECK_FALSE(child.is_running());
    (void)child.wait();
    CHECK_FALSE(process_is_alive(observed_process_id));
}

struct InheritableSentinel {
#ifdef _WIN32
    HANDLE read_end = INVALID_HANDLE_VALUE;
    HANDLE write_end = INVALID_HANDLE_VALUE;

    InheritableSentinel() {
        SECURITY_ATTRIBUTES attributes{};
        attributes.nLength = sizeof(attributes);
        attributes.bInheritHandle = TRUE;
        REQUIRE(CreatePipe(&read_end, &write_end, &attributes, 0));
    }
    ~InheritableSentinel() {
        if (read_end != INVALID_HANDLE_VALUE)
            CloseHandle(read_end);
        if (write_end != INVALID_HANDLE_VALUE)
            CloseHandle(write_end);
    }
    std::string encoded_read_end() const {
        return std::to_string(reinterpret_cast<std::uintptr_t>(read_end));
    }
#else
    int descriptors[2] = {-1, -1};

    InheritableSentinel() { REQUIRE(::pipe(descriptors) == 0); }
    ~InheritableSentinel() {
        if (descriptors[0] >= 0)
            ::close(descriptors[0]);
        if (descriptors[1] >= 0)
            ::close(descriptors[1]);
    }
    std::string encoded_read_end() const { return std::to_string(descriptors[0]); }
#endif
};

} // namespace

TEST_CASE("post-spawn input provider binds bytes to the actual blocked child",
          "[child_process][standard-input][process-id]") {
    ChildProcess child;
    ProcessOptions options;
    options.timeout_ms = 5000;
    options.standard_input_timeout_ms = 5000;
    options.max_output_bytes = 4u * 1024u * 1024u;
    int provided_process_id = -1;
    int observer_process_id = -1;
    bool observer_completed_while_provider_active = false;
    std::string readiness;

    const StandardInputByteProvider provider =
        [&](int process_id) -> std::optional<std::vector<std::uint8_t>> {
        provided_process_id = process_id;

        std::atomic<bool> observer_done = false;
        std::thread observer([&] {
            observer_process_id = child.process_id();
            observer_done.store(true, std::memory_order_release);
        });
        const auto observer_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
        while (!observer_done.load(std::memory_order_acquire) &&
               std::chrono::steady_clock::now() < observer_deadline)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        observer_completed_while_provider_active = observer_done.load(std::memory_order_acquire);
        if (observer_completed_while_provider_active)
            observer.join();
        else
            observer.detach();

        const auto ready_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(4);
        while (readiness.find("ready:") == std::string::npos &&
               std::chrono::steady_clock::now() < ready_deadline) {
            readiness += child.read_available_output();
            if (readiness.find("ready:") == std::string::npos)
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        return bytes_of(std::to_string(process_id));
    };

    REQUIRE(child.start_with_standard_input(PULP_CHILD_PROCESS_INPUT_FIXTURE, {"--pid-bound"},
                                            provider, options));
    const auto result = child.wait();
    CHECK(result.exit_code == 0);
    CHECK(provided_process_id == child.process_id());
    CHECK(observer_process_id == provided_process_id);
    CHECK(observer_completed_while_provider_active);
    CHECK(readiness.find("ready:" + std::to_string(provided_process_id)) != std::string::npos);
}

TEST_CASE("suspended spawn validation runs before child code and controls resume",
          "[child_process][standard-input][security][spawn]") {
#if defined(__APPLE__)
    SECTION("accepted exact process resumes") {
        ChildProcess child;
        ProcessOptions options;
        int validated_process_id = -1;
        options.suspended_process_validator = [&](int process_id) {
            validated_process_id = process_id;
            return true;
        };
        REQUIRE(child.start_with_standard_input(
            PULP_CHILD_PROCESS_INPUT_FIXTURE, {"--pid-bound"},
            [](int process_id) -> std::optional<std::vector<std::uint8_t>> {
                return bytes_of(std::to_string(process_id));
            },
            options));
        CHECK(child.wait().exit_code == 0);
        CHECK(validated_process_id == child.process_id());
    }

    SECTION("rejected process is joined without executing") {
        ChildProcess child;
        ProcessOptions options;
        int validated_process_id = -1;
        options.suspended_process_validator = [&](int process_id) {
            validated_process_id = process_id;
            return false;
        };
        CHECK_FALSE(child.start_with_standard_input(
            PULP_CHILD_PROCESS_INPUT_FIXTURE, {"--pid-bound"},
            [](int process_id) -> std::optional<std::vector<std::uint8_t>> {
                return bytes_of(std::to_string(process_id));
            },
            options));
        require_failed_child_was_joined(child, validated_process_id);
        CHECK(child.read_available_output().empty());
    }
#else
    SUCCEED("suspended spawn validation is macOS-only");
#endif
}

TEST_CASE("post-spawn input bytes cannot be replayed to a sibling child",
          "[child_process][standard-input][process-id][security]") {
    int bound_process_id = -1;
    ChildProcess bound;
    REQUIRE(bound.start_with_standard_input(
        PULP_CHILD_PROCESS_INPUT_FIXTURE, {"--pid-bound"},
        [&](int process_id) -> std::optional<std::vector<std::uint8_t>> {
            bound_process_id = process_id;
            return bytes_of(std::to_string(process_id));
        }));
    CHECK(bound.wait().exit_code == 0);

    int sibling_process_id = -1;
    ChildProcess sibling;
    REQUIRE(sibling.start_with_standard_input(
        PULP_CHILD_PROCESS_INPUT_FIXTURE, {"--pid-bound"},
        [&](int process_id) -> std::optional<std::vector<std::uint8_t>> {
            sibling_process_id = process_id;
            return bytes_of(std::to_string(bound_process_id));
        }));
    CHECK(sibling.wait().exit_code == 65);
    CHECK(sibling_process_id > 0);
    CHECK(sibling_process_id != bound_process_id);
}

TEST_CASE("post-spawn input inherits only its standard handles",
          "[child_process][standard-input][security]") {
    InheritableSentinel sentinel;
    ChildProcess child;
    REQUIRE(child.start_with_standard_input(
        PULP_CHILD_PROCESS_INPUT_FIXTURE,
        {"--pid-bound-no-inherit", sentinel.encoded_read_end()},
        [](int process_id) -> std::optional<std::vector<std::uint8_t>> {
            return bytes_of(std::to_string(process_id));
        }));
    CHECK(child.wait().exit_code == 0);
}

TEST_CASE("post-spawn input provider failures terminate and join the child",
          "[child_process][standard-input][security]") {
    ProcessOptions options;
    options.standard_input_timeout_ms = 100;
    options.max_standard_input_provider_bytes = 8;

    SECTION("provider rejection") {
        ChildProcess child;
        int pid = -1;
        CHECK_FALSE(child.start_with_standard_input(
            PULP_CHILD_PROCESS_INPUT_FIXTURE, {"--pid-bound"},
            [&](int process_id) -> std::optional<std::vector<std::uint8_t>> {
                pid = process_id;
                return std::nullopt;
            },
            options));
        require_failed_child_was_joined(child, pid);
    }

    SECTION("provider exception") {
        ChildProcess child;
        int pid = -1;
        CHECK_FALSE(child.start_with_standard_input(
            PULP_CHILD_PROCESS_INPUT_FIXTURE, {"--pid-bound"},
            [&](int process_id) -> std::optional<std::vector<std::uint8_t>> {
                pid = process_id;
                throw std::runtime_error("provider failure");
            },
            options));
        require_failed_child_was_joined(child, pid);
    }

    SECTION("provider output exceeds its bound") {
        ChildProcess child;
        int pid = -1;
        CHECK_FALSE(child.start_with_standard_input(
            PULP_CHILD_PROCESS_INPUT_FIXTURE, {"--pid-bound"},
            [&](int process_id) -> std::optional<std::vector<std::uint8_t>> {
                pid = process_id;
                return std::vector<std::uint8_t>(9, 'x');
            },
            options));
        require_failed_child_was_joined(child, pid);
    }

    SECTION("provider exhausts the delivery deadline") {
        ChildProcess child;
        int pid = -1;
        CHECK_FALSE(child.start_with_standard_input(
            PULP_CHILD_PROCESS_INPUT_FIXTURE, {"--pid-bound"},
            [&](int process_id) -> std::optional<std::vector<std::uint8_t>> {
                pid = process_id;
                std::this_thread::sleep_for(std::chrono::milliseconds(150));
                return bytes_of(std::to_string(process_id));
            },
            options));
        require_failed_child_was_joined(child, pid);
    }

    SECTION("child exits while provider is running") {
        ChildProcess child;
        int pid = -1;
        CHECK_FALSE(child.start_with_standard_input(
            PULP_CHILD_PROCESS_INPUT_FIXTURE, {"--exit-immediately"},
            [&](int process_id) -> std::optional<std::vector<std::uint8_t>> {
                pid = process_id;
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
                return std::nullopt;
            },
            options));
        require_failed_child_was_joined(child, pid);
    }
}

TEST_CASE("post-spawn input write timeout cleans the child and parent handles",
          "[child_process][standard-input][security]") {
    const auto handles_before = open_handle_count();
    ChildProcess child;
    ProcessOptions options;
    options.standard_input_timeout_ms = 50;
    options.max_standard_input_provider_bytes = 8u * 1024u * 1024u;
    int pid = -1;
    const auto started_at = std::chrono::steady_clock::now();
    CHECK_FALSE(child.start_with_standard_input(
        PULP_CHILD_PROCESS_INPUT_FIXTURE, {"--stall"},
        [&](int process_id) -> std::optional<std::vector<std::uint8_t>> {
            pid = process_id;
            return std::vector<std::uint8_t>(8u * 1024u * 1024u, 'x');
        },
        options));
    CHECK(std::chrono::steady_clock::now() - started_at < std::chrono::seconds(2));
    require_failed_child_was_joined(child, pid);
    CHECK(open_handle_count() == handles_before);
}

TEST_CASE("post-spawn input stays off argv environment and filesystem",
          "[child_process][standard-input][security]") {
    constexpr std::string_view secret = "provider-secret-7f3a";
    const auto working_directory =
        std::filesystem::temp_directory_path() /
        ("pulp-child-input-" +
         std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(working_directory);

    ChildProcess child;
    ProcessOptions options;
    options.timeout_ms = 5000;
    options.max_output_bytes = 4u * 1024u * 1024u;
    options.working_directory = working_directory.string();
    REQUIRE(child.start_with_standard_input(
        PULP_CHILD_PROCESS_INPUT_FIXTURE, {"--private-input"},
        [secret](int) -> std::optional<std::vector<std::uint8_t>> { return bytes_of(secret); },
        options));
    const auto result = child.wait();
    CHECK(result.exit_code == 0);
    CHECK(result.stdout_output.find("secret-received") != std::string::npos);
    CHECK(std::filesystem::directory_iterator(working_directory) ==
          std::filesystem::directory_iterator{});
    std::filesystem::remove(working_directory);
}

#ifndef _WIN32
TEST_CASE("post-spawn input honors a pinned working-directory descriptor",
          "[child_process][standard-input][working-directory]") {
    const auto working_directory =
        std::filesystem::temp_directory_path() /
        ("pulp-child-input-descriptor-" +
         std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(working_directory);

    const auto descriptor = ::open(working_directory.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    REQUIRE(descriptor >= 0);

    ChildProcess child;
    ProcessOptions options;
    options.timeout_ms = 5000;
    options.max_output_bytes = 4u * 1024u * 1024u;
    options.working_directory_descriptor = descriptor;
    REQUIRE(child.start_with_standard_input(
        PULP_CHILD_PROCESS_INPUT_FIXTURE, {"--pid-bound-current-directory"},
        [](int process_id) -> std::optional<std::vector<std::uint8_t>> {
            return bytes_of(std::to_string(process_id));
        },
        options));
    const auto result = child.wait();
    CHECK(result.exit_code == 0);
    REQUIRE_FALSE(result.stdout_output.empty());
    REQUIRE(result.stdout_output.back() == '\n');
    const auto observed_directory =
        std::filesystem::path(result.stdout_output.substr(0, result.stdout_output.size() - 1));
    CHECK(std::filesystem::equivalent(observed_directory, working_directory));

    ::close(descriptor);
    std::filesystem::remove_all(working_directory);
}
#endif
