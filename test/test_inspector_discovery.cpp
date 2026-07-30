#include <catch2/catch_test_macros.hpp>

#include <pulp/inspect/authentication.hpp>
#include <pulp/inspect/discovery.hpp>

#include <filesystem>
#include <fstream>
#include <thread>

#ifndef _WIN32
#include <sys/stat.h>
#endif

using namespace std::chrono_literals;
using pulp::inspect::InspectorDiscoveryPublisher;
using pulp::inspect::InspectorDiscoveryReader;
using pulp::inspect::InspectorDiscoveryRecord;
using pulp::inspect::InspectorProfile;
using pulp::inspect::generate_inspector_secret;
using pulp::inspect::select_inspector_session;

namespace {

class TemporaryDirectory {
public:
    TemporaryDirectory() {
        const auto secret = generate_inspector_secret();
        REQUIRE(secret.has_value());
        std::string suffix;
        for (std::size_t index = 0; index < 8; ++index)
            suffix += "0123456789abcdef"[(*secret)[index] & 0xf];
        path = std::filesystem::temp_directory_path() /
               ("pulp-inspector-discovery-test-" + suffix);
    }

    ~TemporaryDirectory() {
        std::error_code error;
        std::filesystem::remove_all(path, error);
    }

    std::filesystem::path path;
};

InspectorDiscoveryRecord fixture_record(std::string session) {
    InspectorDiscoveryRecord record;
    record.session_id = std::move(session);
    record.instance_id = "instance-1";
    record.plugin_id = "com.pulp.fixture";
    record.endpoint = "127.0.0.1:32123";
    record.profile = InspectorProfile::Observe;
    return record;
}

} // namespace

TEST_CASE("discovery publishes owner-private ephemeral credentials and cleans up",
          "[inspect][discovery]") {
    TemporaryDirectory temporary;
    InspectorDiscoveryReader reader(temporary.path);
    CHECK(reader.list().empty());
    CHECK_FALSE(std::filesystem::exists(temporary.path));

    const auto token = generate_inspector_secret();
    REQUIRE(token.has_value());

    InspectorDiscoveryPublisher publisher(temporary.path);
    REQUIRE(publisher.publish(fixture_record("session-one"), *token, 5s));
    REQUIRE(publisher.record().has_value());
    CHECK_FALSE(publisher.record()->process_start_id.empty());
    CHECK(std::filesystem::exists(publisher.record()->record_path));
    CHECK(std::filesystem::exists(publisher.record()->credential_path));

#ifndef _WIN32
    struct stat info {};
    REQUIRE(::stat(publisher.record()->credential_path.c_str(), &info) == 0);
    CHECK((info.st_mode & 077) == 0);
#endif

    const auto records = reader.list();
    REQUIRE(records.size() == 1);
    CHECK(records.front().session_id == "session-one");
    CHECK(reader.read_credential(records.front()) == token);

    publisher.remove();
    CHECK(reader.list().empty());
    CHECK_FALSE(std::filesystem::exists(
        temporary.path / "session-one.token"));
}

TEST_CASE("discovery rejects a stale record after process id reuse",
          "[inspect][discovery][security]") {
    TemporaryDirectory temporary;
    const auto token = generate_inspector_secret();
    REQUIRE(token.has_value());

    InspectorDiscoveryPublisher publisher(temporary.path);
    REQUIRE(publisher.publish(fixture_record("stale-process"), *token, 5s));
    REQUIRE(publisher.record().has_value());

    std::ifstream input(publisher.record()->record_path, std::ios::binary);
    std::string json((std::istreambuf_iterator<char>(input)),
                     std::istreambuf_iterator<char>());
    const auto start = publisher.record()->process_start_id;
    const auto position = json.find(start);
    REQUIRE(position != std::string::npos);
    json.replace(position, start.size(), start + "-reused");
    std::ofstream output(publisher.record()->record_path,
                         std::ios::binary | std::ios::trunc);
    REQUIRE(static_cast<bool>(output << json));
    output.close();

    InspectorDiscoveryReader reader(temporary.path);
    CHECK(reader.list().empty());
}

#ifndef _WIN32
TEST_CASE("discovery reader does not harden an insecure runtime directory",
          "[inspect][discovery][security]") {
    TemporaryDirectory temporary;
    std::filesystem::create_directories(temporary.path);
    REQUIRE(::chmod(temporary.path.c_str(), 0755) == 0);

    InspectorDiscoveryReader reader(temporary.path);
    CHECK(reader.list().empty());

    struct stat info {};
    REQUIRE(::stat(temporary.path.c_str(), &info) == 0);
    CHECK((info.st_mode & 077) == 055);
}
#endif

TEST_CASE("discovery rejects expired, insecure, and ambiguous records",
          "[inspect][discovery][security]") {
    TemporaryDirectory temporary;
    const auto token = generate_inspector_secret();
    REQUIRE(token.has_value());

    InspectorDiscoveryPublisher expired(temporary.path);
    REQUIRE(expired.publish(fixture_record("expired"), *token, 1ms));
    std::this_thread::sleep_for(5ms);
    InspectorDiscoveryReader reader(temporary.path);
    CHECK(reader.list().empty());

    InspectorDiscoveryPublisher first(temporary.path);
    InspectorDiscoveryPublisher second(temporary.path);
    REQUIRE(first.publish(fixture_record("first"), *token, 5s));
    REQUIRE(second.publish(fixture_record("second"), *token, 5s));
    auto records = reader.list();
    REQUIRE(records.size() == 2);
    std::string error;
    CHECK_FALSE(select_inspector_session(records, "", &error).has_value());
    CHECK(error.find("Multiple") != std::string::npos);
    REQUIRE(select_inspector_session(records, "second", &error).has_value());

#ifndef _WIN32
    REQUIRE(::chmod(first.record()->credential_path.c_str(), 0644) == 0);
    CHECK_FALSE(reader.read_credential(records.front()).has_value());
#endif
}
