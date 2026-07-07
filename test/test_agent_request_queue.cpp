// Tests for the agent-request queue: pure serialize/parse/append/ack plus the
// atomic file-backed wrappers. No window or render context needed.

#include <pulp/inspect/agent_request_queue.hpp>

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <string>

using namespace pulp::inspect;

namespace {

AgentRequest mk(const std::string& text, const std::string& id = "") {
    AgentRequest r;
    r.id = id;
    r.text = text;
    r.design = "synth-panel";
    r.screen = "main";
    r.editmode_state = R"({"accent":"#33aaff"})";
    r.screenshot_path = "/tmp/shot.png";
    r.created_at = "2026-07-06T18:00:00Z";
    return r;
}

}  // namespace

TEST_CASE("requests_from_json: empty is an empty queue, malformed is an error", "[agent-queue]") {
    REQUIRE(requests_from_json("").has_value());
    CHECK(requests_from_json("").value().empty());
    REQUIRE(requests_from_json("  \n\t ").has_value());  // whitespace-only == empty
    CHECK_FALSE(requests_from_json("not json").has_value());
    CHECK_FALSE(requests_from_json("{\"nope\":1}").has_value());  // no requests array
    CHECK_FALSE(requests_from_json("[1,2,3]").has_value());       // not an object
}

TEST_CASE("append assigns sequential ids and round-trips every field", "[agent-queue]") {
    auto a1 = append_request("", mk("make it minimal"));
    REQUIRE(a1.has_value());
    auto p1 = requests_from_json(*a1);
    REQUIRE(p1.has_value());
    REQUIRE(p1->size() == 1);
    CHECK(p1->at(0).id == "1");
    CHECK(p1->at(0).text == "make it minimal");
    CHECK(p1->at(0).design == "synth-panel");
    CHECK(p1->at(0).screen == "main");
    CHECK(p1->at(0).editmode_state == R"({"accent":"#33aaff"})");
    CHECK(p1->at(0).screenshot_path == "/tmp/shot.png");
    CHECK(p1->at(0).created_at == "2026-07-06T18:00:00Z");
    CHECK(p1->at(0).consumed == false);

    auto a2 = append_request(*a1, mk("tighten the header"));
    REQUIRE(a2.has_value());
    auto p2 = requests_from_json(*a2);
    REQUIRE(p2->size() == 2);
    CHECK(p2->at(1).id == "2");
}

TEST_CASE("a caller id is kept and the counter skips past it", "[agent-queue]") {
    auto a = append_request("", mk("a"));            // "1"
    auto b = append_request(*a, mk("explicit", "99"));
    auto c = append_request(*b, mk("after"));        // max(1,99)+1 == "100"
    auto p = requests_from_json(*c);
    REQUIRE(p.has_value());
    REQUIRE(p->size() == 3);
    CHECK(p->at(1).id == "99");
    CHECK(p->at(2).id == "100");
}

TEST_CASE("append rejects a malformed queue document", "[agent-queue]") {
    CHECK_FALSE(append_request("not json", mk("x")).has_value());
}

TEST_CASE("pending filters consumed; ack marks and reports match", "[agent-queue]") {
    auto a = append_request("", mk("first"));
    auto b = append_request(*a, mk("second"));
    REQUIRE(b.has_value());

    auto acked = ack_request(*b, "1");
    REQUIRE(acked.has_value());
    CHECK(acked->second == true);  // an id matched
    auto pend = pending_requests(acked->first);
    REQUIRE(pend.size() == 1);
    CHECK(pend.at(0).id == "2");

    auto miss = ack_request(*b, "does-not-exist");
    REQUIRE(miss.has_value());
    CHECK(miss->second == false);
    CHECK(pending_requests(miss->first).size() == 2);  // unchanged
}

TEST_CASE("file-backed queue: atomic create, enqueue, read, ack", "[agent-queue]") {
    auto dir = std::filesystem::temp_directory_path() / "pulp_agent_queue_test";
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    const std::string path = queue_path(dir.string());
    CHECK(path == (dir / std::string(queue_filename())).string());

    auto id_a = enqueue_to_file(path, mk("first"));
    REQUIRE(id_a.has_value());
    CHECK(*id_a == "1");
    CHECK(std::filesystem::exists(path));
    CHECK_FALSE(std::filesystem::exists(path + ".tmp"));  // temp cleaned up

    auto id_b = enqueue_to_file(path, mk("second"));
    REQUIRE(id_b.has_value());
    CHECK(*id_b == "2");
    CHECK(read_pending_file(path).size() == 2);

    CHECK(ack_in_file(path, "1") == true);
    auto pend = read_pending_file(path);
    REQUIRE(pend.size() == 1);
    CHECK(pend.at(0).id == "2");
    CHECK(ack_in_file(path, "nope") == false);

    std::filesystem::remove_all(dir);
}

TEST_CASE("an absent queue file reads as empty and acks as no-match", "[agent-queue]") {
    auto dir = std::filesystem::temp_directory_path() / "pulp_agent_queue_absent";
    std::filesystem::remove_all(dir);  // ensure it does not exist
    const std::string path = queue_path(dir.string());
    CHECK(read_pending_file(path).empty());
    CHECK(ack_in_file(path, "1") == false);
}
