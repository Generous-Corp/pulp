#include "gpu_artifact_publication.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <string>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#include <winioctl.h>
#endif

namespace {

namespace fs = std::filesystem;
using pulp::cli::gpu_artifacts::PinnedArtifactDirectory;

struct TemporaryDirectory {
    fs::path path;

    TemporaryDirectory() {
        static std::atomic<std::uint64_t> sequence{0};
        path = fs::weakly_canonical(fs::temp_directory_path()) /
               ("pulp-gpu-artifact-publication-" +
                std::to_string(sequence.fetch_add(1, std::memory_order_relaxed)) + "-" +
                std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        fs::create_directories(path);
    }

    ~TemporaryDirectory() {
        std::error_code ignored;
        fs::remove_all(path, ignored);
    }
};

std::vector<std::uint8_t> read_bytes(const fs::path& path) {
    std::ifstream input(path, std::ios::binary);
    REQUIRE(input.good());
    return {std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
}

TEST_CASE("GPU artifact publication creates and reuses a pinned nested directory",
          "[cli][gpu][artifact-publication]") {
    TemporaryDirectory temporary;
    const auto artifacts = temporary.path / "nested" / "artifacts";
    auto pinned = PinnedArtifactDirectory::open_or_create(artifacts);
    const std::vector<std::uint8_t> first{'f', 'i', 'r', 's', 't'};
    const std::vector<std::uint8_t> second{'s', 'e', 'c', 'o', 'n', 'd'};

    pinned.publish("observed.bin", first);
    pinned.publish("observed.bin", second);

    CHECK(read_bytes(artifacts / "observed.bin") == second);
}

#if !defined(_WIN32)
TEST_CASE("GPU artifact publication confines a planted directory symlink swap",
          "[cli][gpu][artifact-publication][symlink]") {
    TemporaryDirectory temporary;
    const auto artifacts = temporary.path / "artifacts";
    const auto pinned_identity = temporary.path / "pinned-identity";
    const auto escape = temporary.path / "escape";
    fs::create_directory(escape);
    auto pinned = PinnedArtifactDirectory::open_or_create(artifacts);

    fs::rename(artifacts, pinned_identity);
    fs::create_directory_symlink(escape, artifacts);
    const std::vector<std::uint8_t> evidence{'p', 'i', 'n', 'n', 'e', 'd'};
    pinned.publish("observed.bin", evidence);

    CHECK(read_bytes(pinned_identity / "observed.bin") == evidence);
    CHECK_FALSE(fs::exists(escape / "observed.bin"));
}

TEST_CASE("GPU artifact publication never follows an existing artifact symlink",
          "[cli][gpu][artifact-publication][symlink]") {
    TemporaryDirectory temporary;
    const auto artifacts = temporary.path / "artifacts";
    const auto outside = temporary.path / "outside.bin";
    {
        std::ofstream output(outside, std::ios::binary);
        output << "sentinel";
    }
    auto pinned = PinnedArtifactDirectory::open_or_create(artifacts);
    fs::create_symlink(outside, artifacts / "observed.bin");
    const std::vector<std::uint8_t> replacement{'r', 'e', 'p', 'l', 'a', 'c', 'e'};

    CHECK_THROWS_WITH(pinned.publish("observed.bin", replacement),
                      Catch::Matchers::ContainsSubstring("refusing to replace symlink"));
    const std::vector<std::uint8_t> sentinel{'s', 'e', 'n', 't', 'i', 'n', 'e', 'l'};
    CHECK(read_bytes(outside) == sentinel);
}
#else
struct MountPointReparseData {
    DWORD tag;
    WORD data_length;
    WORD reserved;
    WORD substitute_name_offset;
    WORD substitute_name_length;
    WORD print_name_offset;
    WORD print_name_length;
    WCHAR path_buffer[1];
};

static_assert(offsetof(MountPointReparseData, path_buffer) == 16);

std::wstring nt_mount_point_target(const fs::path& target) {
    const auto native = fs::absolute(target).native();
    if (native.starts_with(L"\\\\"))
        return L"\\??\\UNC\\" + native.substr(2);
    return L"\\??\\" + native;
}

TEST_CASE("GPU artifact publication confines a Windows in-place reparse mutation",
          "[cli][gpu][artifact-publication][reparse]") {
    TemporaryDirectory temporary;
    const auto artifacts = temporary.path / "artifacts";
    const auto escape = temporary.path / "escape";
    fs::create_directory(escape);
    auto pinned = PinnedArtifactDirectory::open_or_create(artifacts);

    const HANDLE reparse_mutator =
        CreateFileW(artifacts.c_str(), FILE_WRITE_ATTRIBUTES,
                    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                    FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    REQUIRE(reparse_mutator != INVALID_HANDLE_VALUE);

    const auto substitute = nt_mount_point_target(escape);
    const auto print = fs::absolute(escape).native();
    const auto path_bytes = (substitute.size() + 1 + print.size() + 1) * sizeof(wchar_t);
    REQUIRE(path_bytes + sizeof(WORD) * 4 <= (std::numeric_limits<WORD>::max)());
    std::vector<std::byte> storage(offsetof(MountPointReparseData, path_buffer) + path_bytes);
    auto* reparse = reinterpret_cast<MountPointReparseData*>(storage.data());
    reparse->tag = IO_REPARSE_TAG_MOUNT_POINT;
    reparse->data_length = static_cast<WORD>(sizeof(WORD) * 4 + path_bytes);
    reparse->reserved = 0;
    reparse->substitute_name_offset = 0;
    reparse->substitute_name_length = static_cast<WORD>(substitute.size() * sizeof(wchar_t));
    reparse->print_name_offset = static_cast<WORD>((substitute.size() + 1) * sizeof(wchar_t));
    reparse->print_name_length = static_cast<WORD>(print.size() * sizeof(wchar_t));
    std::memcpy(reparse->path_buffer, substitute.c_str(),
                (substitute.size() + 1) * sizeof(wchar_t));
    std::memcpy(reinterpret_cast<std::byte*>(reparse->path_buffer) + reparse->print_name_offset,
                print.c_str(), (print.size() + 1) * sizeof(wchar_t));

    DWORD returned = 0;
    REQUIRE(DeviceIoControl(reparse_mutator, FSCTL_SET_REPARSE_POINT, reparse,
                            static_cast<DWORD>(storage.size()), nullptr, 0, &returned, nullptr));
    REQUIRE((GetFileAttributesW(artifacts.c_str()) & FILE_ATTRIBUTE_REPARSE_POINT) != 0);

    const std::vector<std::uint8_t> evidence{'p', 'i', 'n', 'n', 'e', 'd'};
    pinned.publish("observed.bin", evidence);
    CHECK_FALSE(fs::exists(escape / "observed.bin"));

    struct {
        DWORD tag{IO_REPARSE_TAG_MOUNT_POINT};
        WORD data_length{0};
        WORD reserved{0};
    } deletion;
    REQUIRE(DeviceIoControl(reparse_mutator, FSCTL_DELETE_REPARSE_POINT, &deletion,
                            sizeof(deletion), nullptr, 0, &returned, nullptr));
    REQUIRE(CloseHandle(reparse_mutator));
    CHECK(read_bytes(artifacts / "observed.bin") == evidence);
}
#endif

TEST_CASE("GPU artifact publication rejects non-basename payload names",
          "[cli][gpu][artifact-publication]") {
    TemporaryDirectory temporary;
    auto pinned = PinnedArtifactDirectory::open_or_create(temporary.path / "artifacts");
    const std::vector<std::uint8_t> evidence{'x'};

    CHECK_THROWS(pinned.publish("../escape.bin", evidence));
    CHECK_FALSE(fs::exists(temporary.path / "escape.bin"));
}

} // namespace
