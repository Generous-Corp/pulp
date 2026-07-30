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
        temporary.path / "11-session-one-instance-1.token"));
}

TEST_CASE("discovery keeps duplicate session IDs instance-isolated",
          "[inspect][discovery][identity]") {
    TemporaryDirectory temporary;
    const auto first_token = generate_inspector_secret();
    const auto second_token = generate_inspector_secret();
    REQUIRE(first_token.has_value());
    REQUIRE(second_token.has_value());

    auto first_record = fixture_record("shared-session");
    first_record.instance_id = "instance-a";
    auto second_record = fixture_record("shared-session");
    second_record.instance_id = "instance-b";
    InspectorDiscoveryPublisher first(temporary.path);
    InspectorDiscoveryPublisher second(temporary.path);
    REQUIRE(first.publish(first_record, *first_token, 5s));
    REQUIRE(second.publish(second_record, *second_token, 5s));

    InspectorDiscoveryReader reader(temporary.path);
    const auto records = reader.list();
    REQUIRE(records.size() == 2);
    std::string selection_error;
    CHECK_FALSE(select_inspector_session(
        records, "shared-session", &selection_error).has_value());
    CHECK(selection_error.find("Multiple") != std::string::npos);
    CHECK(records[0].record_path != records[1].record_path);
    CHECK(records[0].credential_path != records[1].credential_path);
    for (const auto& record : records) {
        const auto credential = reader.read_credential(record);
        REQUIRE(credential.has_value());
        if (record.instance_id == "instance-a")
            CHECK(*credential == *first_token);
        else if (record.instance_id == "instance-b")
            CHECK(*credential == *second_token);
        else
            FAIL("unexpected instance identity");
    }

    first.remove();
    const auto remaining = reader.list();
    REQUIRE(remaining.size() == 1);
    CHECK(remaining.front().instance_id == "instance-b");
    CHECK(reader.read_credential(remaining.front()) == second_token);
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

TEST_CASE("discovery publisher rejects a runtime symlink without chmod",
          "[inspect][discovery][security][symlink]") {
    TemporaryDirectory temporary;
    const auto target = temporary.path / "target";
    const auto runtime = temporary.path / "runtime";
    std::filesystem::create_directories(target);
    REQUIRE(::chmod(target.c_str(), 0755) == 0);
    std::filesystem::create_directory_symlink(target, runtime);

    const auto token = generate_inspector_secret();
    REQUIRE(token.has_value());
    InspectorDiscoveryPublisher publisher(runtime);
    CHECK_FALSE(publisher.publish(
        fixture_record("symlink-runtime"), *token, 5s));

    struct stat info {};
    REQUIRE(::stat(target.c_str(), &info) == 0);
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
