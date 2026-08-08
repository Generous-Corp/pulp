#include <catch2/catch_test_macros.hpp>

#include <pulp/inspect/control_carrier.hpp>

#include <algorithm>
#include <atomic>
#include <filesystem>
#include <fstream>

#ifndef _WIN32
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
#ifdef __APPLE__
#include <fcntl.h>
#include <membership.h>
#include <sys/acl.h>
#endif
#endif

using namespace pulp::inspect;

namespace {

class CarrierFixture {
  public:
    CarrierFixture() {
        static std::atomic<unsigned> next_serial{0};
        const auto serial = next_serial.fetch_add(1, std::memory_order_relaxed);
#ifdef _WIN32
        const auto process_id = static_cast<long long>(::_getpid());
#else
        const auto process_id = static_cast<long long>(::getpid());
#endif
        root = std::filesystem::temp_directory_path() /
               ("pcct-" + std::to_string(process_id) + "-" + std::to_string(serial));
        runtime_directory = default_control_runtime_directory(root);
        endpoint = default_control_endpoint_path(root);
    }

    ~CarrierFixture() {
        std::error_code ignored;
        std::filesystem::remove_all(root, ignored);
    }

    std::filesystem::path root;
    std::filesystem::path runtime_directory;
    std::filesystem::path endpoint;
};

#ifndef _WIN32
int create_socket_endpoint(const std::filesystem::path& endpoint) {
    const int descriptor = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (descriptor < 0)
        return -1;
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    const auto path = endpoint.string();
    if (path.size() >= sizeof(address.sun_path)) {
        ::close(descriptor);
        return -1;
    }
    std::copy(path.begin(), path.end(), address.sun_path);
    address.sun_path[path.size()] = '\0';
    if (::bind(descriptor, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0 ||
        ::chmod(path.c_str(), 0600) != 0) {
        ::close(descriptor);
        return -1;
    }
    return descriptor;
}

#ifdef __APPLE__
bool add_directory_allow_acl(const std::filesystem::path& path) {
    const int descriptor = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (descriptor < 0)
        return false;
    acl_t acl = ::acl_init(1);
    acl_entry_t entry = nullptr;
    uuid_t group{};
    bool valid = acl != nullptr && ::acl_create_entry(&acl, &entry) == 0 &&
                 ::mbr_gid_to_uuid(::getegid(), group) == 0 &&
                 ::acl_set_tag_type(entry, ACL_EXTENDED_ALLOW) == 0 &&
                 ::acl_set_qualifier(entry, group) == 0;
    acl_permset_t permissions = nullptr;
    valid = valid && ::acl_get_permset(entry, &permissions) == 0 &&
            ::acl_add_perm(permissions, ACL_READ_DATA) == 0 &&
            ::acl_set_fd_np(descriptor, acl, ACL_TYPE_EXTENDED) == 0;
    if (acl != nullptr)
        ::acl_free(acl);
    ::close(descriptor);
    return valid;
}
#endif
#endif

} // namespace

TEST_CASE("control carrier paths are deterministic per runtime root",
          "[inspect][control][carrier][security]") {
    CarrierFixture fixture;
    CHECK(default_control_runtime_directory(fixture.root) == fixture.runtime_directory);
    CHECK(default_control_endpoint_path(fixture.root) == fixture.endpoint);
    CHECK(fixture.endpoint.parent_path() == fixture.runtime_directory);
    CHECK(fixture.endpoint.filename() == "broker.sock");

    const auto default_directory = default_control_runtime_directory();
    const auto default_endpoint = default_control_endpoint_path();
    REQUIRE_FALSE(default_directory.empty());
    CHECK(default_directory.is_absolute());
    CHECK(default_endpoint == default_directory / "broker.sock");
}

TEST_CASE("control carrier prepares only an owner-private absolute directory",
          "[inspect][control][carrier][security]") {
    CarrierFixture fixture;
#ifdef _WIN32
    CHECK_FALSE(prepare_control_runtime_directory(fixture.runtime_directory));
#else
    REQUIRE(prepare_control_runtime_directory(fixture.runtime_directory));
    REQUIRE(std::filesystem::is_directory(fixture.runtime_directory));

    struct stat status{};
    REQUIRE(::lstat(fixture.runtime_directory.c_str(), &status) == 0);
    CHECK(status.st_uid == ::geteuid());
    CHECK((status.st_mode & 0777) == 0700);

    REQUIRE(::chmod(fixture.runtime_directory.c_str(), 0755) == 0);
    CHECK_FALSE(prepare_control_runtime_directory(fixture.runtime_directory));
    REQUIRE(::chmod(fixture.runtime_directory.c_str(), 0500) == 0);
    CHECK_FALSE(prepare_control_runtime_directory(fixture.runtime_directory));
#endif

    CHECK_FALSE(prepare_control_runtime_directory("relative-control-runtime"));
}

TEST_CASE("control carrier refuses a symlinked runtime directory",
          "[inspect][control][carrier][security]") {
#ifndef _WIN32
    CarrierFixture fixture;
    const auto target = fixture.root / "target";
    std::filesystem::create_directories(target);
    std::filesystem::permissions(target, std::filesystem::perms::owner_all,
                                 std::filesystem::perm_options::replace);
    std::filesystem::create_directories(fixture.root);
    REQUIRE(::symlink(target.c_str(), fixture.runtime_directory.c_str()) == 0);
    CHECK_FALSE(prepare_control_runtime_directory(fixture.runtime_directory));
#endif
}

TEST_CASE("control carrier refuses an extended ACL on its runtime directory",
          "[inspect][control][carrier][security][macos][acl]") {
#ifdef __APPLE__
    CarrierFixture fixture;
    REQUIRE(prepare_control_runtime_directory(fixture.runtime_directory));
    REQUIRE(add_directory_allow_acl(fixture.runtime_directory));
    CHECK_FALSE(prepare_control_runtime_directory(fixture.runtime_directory));
#endif
}

TEST_CASE("control carrier removes an unchanged stale socket",
          "[inspect][control][carrier][security]") {
#ifndef _WIN32
    CarrierFixture fixture;
    REQUIRE(prepare_control_runtime_directory(fixture.runtime_directory));
    const int descriptor = create_socket_endpoint(fixture.endpoint);
    REQUIRE(descriptor >= 0);
    REQUIRE(::close(descriptor) == 0);

    const auto identity = control_endpoint_identity(fixture.endpoint);
    REQUIRE(identity.has_value());
    CHECK(remove_stale_control_endpoint(fixture.endpoint, *identity));
    CHECK_FALSE(std::filesystem::exists(fixture.endpoint));

    const int second_descriptor = create_socket_endpoint(fixture.endpoint);
    REQUIRE(second_descriptor >= 0);
    REQUIRE(::close(second_descriptor) == 0);
    CHECK(remove_stale_control_endpoint(fixture.endpoint));
    CHECK_FALSE(std::filesystem::exists(fixture.endpoint));
#endif
}

TEST_CASE("control carrier preserves a replacement socket with a new inode",
          "[inspect][control][carrier][security]") {
#ifndef _WIN32
    CarrierFixture fixture;
    REQUIRE(prepare_control_runtime_directory(fixture.runtime_directory));
    const int original = create_socket_endpoint(fixture.endpoint);
    REQUIRE(original >= 0);
    const auto original_identity = control_endpoint_identity(fixture.endpoint);
    REQUIRE(original_identity.has_value());

    const auto parked_original = fixture.runtime_directory / "original-broker.sock";
    REQUIRE(::rename(fixture.endpoint.c_str(), parked_original.c_str()) == 0);
    const int replacement = create_socket_endpoint(fixture.endpoint);
    REQUIRE(replacement >= 0);
    const auto replacement_identity = control_endpoint_identity(fixture.endpoint);
    REQUIRE(replacement_identity.has_value());
    REQUIRE(*replacement_identity != *original_identity);

    CHECK_FALSE(remove_stale_control_endpoint(fixture.endpoint, *original_identity));
    CHECK(control_endpoint_identity(fixture.endpoint) == replacement_identity);

    REQUIRE(::close(replacement) == 0);
    REQUIRE(::close(original) == 0);
    REQUIRE(::unlink(parked_original.c_str()) == 0);
#endif
}

TEST_CASE("control carrier refuses non-socket and non-private endpoints",
          "[inspect][control][carrier][security]") {
#ifndef _WIN32
    CarrierFixture fixture;
    REQUIRE(prepare_control_runtime_directory(fixture.runtime_directory));
    {
        std::ofstream file(fixture.endpoint);
        REQUIRE(file.good());
    }
    CHECK_FALSE(control_endpoint_identity(fixture.endpoint).has_value());
    CHECK_FALSE(remove_stale_control_endpoint(fixture.endpoint));
    CHECK(std::filesystem::is_regular_file(fixture.endpoint));

    REQUIRE(std::filesystem::remove(fixture.endpoint));
    const int descriptor = create_socket_endpoint(fixture.endpoint);
    REQUIRE(descriptor >= 0);
    REQUIRE(::chmod(fixture.endpoint.c_str(), 0660) == 0);
    CHECK_FALSE(control_endpoint_identity(fixture.endpoint).has_value());
    CHECK_FALSE(remove_stale_control_endpoint(fixture.endpoint));
    CHECK(std::filesystem::exists(fixture.endpoint));
    REQUIRE(::close(descriptor) == 0);
#endif
}
