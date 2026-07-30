#include <catch2/catch_test_macros.hpp>

#include <pulp/inspect/authentication.hpp>
#include <pulp/inspect/discovery.hpp>
#include <pulp/inspect/discovery_publisher.hpp>

#include <algorithm>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <limits>
#include <thread>
#include <vector>

#ifndef _WIN32
#include <sys/stat.h>
#ifdef __APPLE__
#include <libproc.h>
#include <sys/proc.h>
#include <sys/wait.h>
#include <unistd.h>
#endif
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

#ifdef _WIN32
TEST_CASE("discovery publication preserves Unicode runtime paths on Windows",
          "[inspect][discovery][windows][unicode]") {
    TemporaryDirectory temporary;
    const auto runtime =
        temporary.path / std::filesystem::path(L"runtime-\u03a9-\u65e5\u672c");
    const auto token = generate_inspector_secret();
    REQUIRE(token.has_value());

    InspectorDiscoveryPublisher publisher(runtime);
    REQUIRE(publisher.publish(
        fixture_record("unicode-runtime"), *token, 5s));
    InspectorDiscoveryReader reader(runtime);
    const auto records = reader.list();
    REQUIRE(records.size() == 1);
    CHECK(records.front().session_id == "unicode-runtime");
    CHECK(reader.read_credential(records.front()) == token);
}
#endif

TEST_CASE("discovery rejects records and credentials above their fixed bounds",
          "[inspect][discovery][resource-limit]") {
    TemporaryDirectory temporary;
    const auto token = generate_inspector_secret();
    REQUIRE(token.has_value());
    InspectorDiscoveryPublisher publisher(temporary.path);
    auto oversized = fixture_record("oversized-publication");
    oversized.plugin_id = std::string(2000, 'x');
    CHECK_FALSE(publisher.publish(oversized, *token, 5s));
    REQUIRE(publisher.publish(
        fixture_record("bounded-input"), *token, 5s));
    InspectorDiscoveryReader reader(temporary.path);
    REQUIRE(reader.list().size() == 1);

    {
        std::ofstream record(publisher.record()->record_path,
                             std::ios::binary | std::ios::trunc);
        record << std::string(1025, 'x');
    }
    CHECK(reader.list().empty());

    REQUIRE(publisher.refresh(5s));
    {
        std::ofstream credential(
            publisher.record()->credential_path,
            std::ios::binary | std::ios::app);
        credential << '0';
    }
    CHECK(reader.list().empty());
    CHECK_FALSE(reader.read_credential(*publisher.record()).has_value());
}

TEST_CASE("discovery rejects expiration arithmetic overflow",
          "[inspect][discovery][resource-limit]") {
    TemporaryDirectory temporary;
    const auto token = generate_inspector_secret();
    REQUIRE(token.has_value());
    InspectorDiscoveryPublisher publisher(temporary.path);
    CHECK_FALSE(publisher.publish(
        fixture_record("overflow-publication"), *token,
        std::chrono::milliseconds::max()));
    CHECK_FALSE(publisher.record().has_value());

    REQUIRE(publisher.publish(
        fixture_record("bounded-publication"), *token, 5s));
    const auto original_expiry =
        publisher.record()->expires_at_unix_ms;
    CHECK_FALSE(
        publisher.refresh(std::chrono::milliseconds::max()));
    REQUIRE(publisher.record().has_value());
    CHECK(publisher.record()->expires_at_unix_ms == original_expiry);
}

TEST_CASE("discovery accepts only complete nonzero loopback ports",
          "[inspect][discovery][endpoint]") {
    TemporaryDirectory temporary;
    const auto token = generate_inspector_secret();
    REQUIRE(token.has_value());
    InspectorDiscoveryPublisher publisher(temporary.path);
    for (const auto endpoint : {
             "127.0.0.1:0",
             "127.0.0.1:not-a-port",
             "127.0.0.1:65536",
             "127.0.0.1:123:456",
             "127.0.0.2:32123",
         }) {
        auto record = fixture_record("bad-endpoint");
        record.endpoint = endpoint;
        CHECK_FALSE(publisher.publish(record, *token, 5s));
    }

    REQUIRE(publisher.publish(
        fixture_record("decoded-endpoint"), *token, 5s));
    std::ifstream input(publisher.record()->record_path,
                        std::ios::binary);
    std::string json((std::istreambuf_iterator<char>(input)),
                     std::istreambuf_iterator<char>());
    const auto endpoint = json.find("127.0.0.1:32123");
    REQUIRE(endpoint != std::string::npos);
    json.replace(endpoint, std::string("127.0.0.1:32123").size(),
                 "127.0.0.1:0");
    {
        std::ofstream output(publisher.record()->record_path,
                             std::ios::binary | std::ios::trunc);
        output << json;
    }
    InspectorDiscoveryReader reader(temporary.path);
    CHECK(reader.list().empty());
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
        records, "shared-session", {}, &selection_error).has_value());
    CHECK(selection_error.find("Multiple") != std::string::npos);
    const auto selected = select_inspector_session(
        records, "shared-session", "instance-b", &selection_error);
    REQUIRE(selected.has_value());
    CHECK(selected->instance_id == "instance-b");
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

TEST_CASE("discovery reserves a live session and instance identity",
          "[inspect][discovery][identity][ownership]") {
    TemporaryDirectory temporary;
    const auto first_token = generate_inspector_secret();
    const auto second_token = generate_inspector_secret();
    REQUIRE(first_token.has_value());
    REQUIRE(second_token.has_value());

    InspectorDiscoveryPublisher first(temporary.path);
    InspectorDiscoveryPublisher competing(temporary.path);
    const auto record = fixture_record("owned-session");
    REQUIRE(first.publish(record, *first_token, 5s));
    CHECK_FALSE(competing.publish(record, *second_token, 5s));

    InspectorDiscoveryReader reader(temporary.path);
    auto records = reader.list();
    REQUIRE(records.size() == 1);
    CHECK(reader.read_credential(records.front()) == first_token);
    REQUIRE(first.refresh(5s));
    CHECK(reader.read_credential(reader.list().front()) == first_token);

    first.remove();
    REQUIRE(competing.publish(record, *second_token, 5s));
    records = reader.list();
    REQUIRE(records.size() == 1);
    CHECK(reader.read_credential(records.front()) == second_token);
}

TEST_CASE("stale ownership reclamation has exactly one concurrent winner",
          "[inspect][discovery][identity][ownership][concurrency]") {
    TemporaryDirectory temporary;
    const auto record = fixture_record("raced-session");
    const auto seed_token = generate_inspector_secret();
    REQUIRE(seed_token.has_value());
    {
        InspectorDiscoveryPublisher seed(temporary.path);
        REQUIRE(seed.publish(record, *seed_token, 5s));
    }

    constexpr std::size_t contender_count = 8;
    std::vector<std::vector<std::uint8_t>> tokens;
    std::vector<std::unique_ptr<InspectorDiscoveryPublisher>> publishers;
    tokens.reserve(contender_count);
    publishers.reserve(contender_count);
    for (std::size_t index = 0; index < contender_count; ++index) {
        const auto token = generate_inspector_secret();
        REQUIRE(token.has_value());
        tokens.push_back(*token);
        publishers.push_back(
            std::make_unique<InspectorDiscoveryPublisher>(temporary.path));
    }

    std::atomic<std::size_t> ready{0};
    std::atomic<bool> start{false};
    std::vector<int> published(contender_count, 0);
    std::vector<std::thread> contenders;
    contenders.reserve(contender_count);
    for (std::size_t index = 0; index < contender_count; ++index) {
        contenders.emplace_back([&, index] {
            ready.fetch_add(1, std::memory_order_release);
            while (!start.load(std::memory_order_acquire))
                std::this_thread::yield();
            published[index] =
                publishers[index]->publish(record, tokens[index], 5s) ? 1 : 0;
        });
    }
    while (ready.load(std::memory_order_acquire) != contender_count)
        std::this_thread::yield();
    start.store(true, std::memory_order_release);
    for (auto& contender : contenders)
        contender.join();

    REQUIRE(std::count(published.begin(), published.end(), 1) == 1);
    const auto winner = static_cast<std::size_t>(
        std::find(published.begin(), published.end(), 1) -
        published.begin());
    InspectorDiscoveryReader reader(temporary.path);
    const auto records = reader.list();
    REQUIRE(records.size() == 1);
    CHECK(reader.read_credential(records.front()) == tokens[winner]);
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

#ifdef __APPLE__
TEST_CASE("discovery rejects a zombie publisher on macOS",
          "[inspect][discovery][security][macos]") {
    struct ZombieChild {
        pid_t pid = -1;
        ~ZombieChild() {
            if (pid > 0) {
                int status = 0;
                (void)::waitpid(pid, &status, 0);
            }
        }
    } child{::fork()};
    REQUIRE(child.pid >= 0);
    if (child.pid == 0)
        ::_exit(0);

    proc_bsdinfo child_info{};
    bool observed_exited_child = false;
    int child_info_bytes = 0;
    for (int attempt = 0; attempt < 100 && !observed_exited_child; ++attempt) {
        child_info_bytes =
            proc_pidinfo(child.pid, PROC_PIDTBSDINFO, 0, &child_info,
                         sizeof(child_info));
        observed_exited_child =
            child_info_bytes == 0 ||
            (child_info_bytes == sizeof(child_info) &&
             child_info.pbi_status == SZOMB);
        if (!observed_exited_child)
            std::this_thread::sleep_for(1ms);
    }
    REQUIRE(observed_exited_child);

    TemporaryDirectory temporary;
    const auto token = generate_inspector_secret();
    REQUIRE(token.has_value());
    InspectorDiscoveryPublisher publisher(temporary.path);
    REQUIRE(publisher.publish(fixture_record("zombie-process"), *token, 5s));
    REQUIRE(publisher.record().has_value());

    std::ifstream input(publisher.record()->record_path, std::ios::binary);
    std::string json((std::istreambuf_iterator<char>(input)),
                     std::istreambuf_iterator<char>());
    const auto current_pid = std::to_string(publisher.record()->process_id);
    const auto pid_key = json.find("\"pid\"");
    REQUIRE(pid_key != std::string::npos);
    const auto pid_position = json.find(current_pid, pid_key);
    REQUIRE(pid_position != std::string::npos);
    json.replace(pid_position, current_pid.size(), std::to_string(child.pid));

    const auto current_start = publisher.record()->process_start_id;
    const auto zombie_start = child_info_bytes == sizeof(child_info)
        ? std::to_string(child_info.pbi_start_tvsec) + ":" +
              std::to_string(child_info.pbi_start_tvusec)
        : "0:0";
    const auto start_position = json.find(current_start);
    REQUIRE(start_position != std::string::npos);
    json.replace(start_position, current_start.size(), zombie_start);

    std::ofstream output(publisher.record()->record_path,
                         std::ios::binary | std::ios::trunc);
    REQUIRE(static_cast<bool>(output << json));
    output.close();

    InspectorDiscoveryReader reader(temporary.path);
    CHECK(reader.list().empty());
}
#endif

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

TEST_CASE("discovery publisher does not chmod an existing runtime directory",
          "[inspect][discovery][security]") {
    TemporaryDirectory temporary;
    std::filesystem::create_directories(temporary.path);
    REQUIRE(::chmod(temporary.path.c_str(), 0755) == 0);

    const auto token = generate_inspector_secret();
    REQUIRE(token.has_value());
    InspectorDiscoveryPublisher publisher(temporary.path);
    CHECK_FALSE(publisher.publish(
        fixture_record("insecure-runtime"), *token, 5s));

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
    CHECK_FALSE(select_inspector_session(records, "", {}, &error).has_value());
    CHECK(error.find("Multiple") != std::string::npos);
    REQUIRE(select_inspector_session(
        records, "second", {}, &error).has_value());

#ifndef _WIN32
    REQUIRE(::chmod(first.record()->credential_path.c_str(), 0644) == 0);
    CHECK_FALSE(reader.read_credential(records.front()).has_value());
    records = reader.list();
    REQUIRE(records.size() == 1);
    CHECK(records.front().session_id == "second");
    REQUIRE(::chmod(first.record()->credential_path.c_str(), 0600) == 0);
#endif

    {
        std::ofstream corrupt(first.record()->credential_path,
                              std::ios::binary | std::ios::trunc);
        corrupt << "not-a-credential";
    }
    records = reader.list();
    REQUIRE(records.size() == 1);
    CHECK(records.front().session_id == "second");
    REQUIRE(select_inspector_session(records, "", {}, &error).has_value());

    std::filesystem::remove(first.record()->credential_path);
    records = reader.list();
    REQUIRE(records.size() == 1);
    CHECK(records.front().session_id == "second");
}
