#include <catch2/catch_test_macros.hpp>
#include <pulp/runtime/identity.hpp>
#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>

using namespace pulp::runtime;

TEST_CASE("Uuid generation", "[runtime][identity]") {
    SECTION("Generated UUIDs are non-nil") {
        auto id = Uuid::generate();
        REQUIRE_FALSE(id.is_nil());
    }

    SECTION("Generated UUIDs are unique") {
        std::set<std::string> ids;
        for (int i = 0; i < 1000; ++i) {
            ids.insert(Uuid::generate().to_string());
        }
        REQUIRE(ids.size() == 1000);
    }

    SECTION("UUIDv4 version bits are set") {
        auto id = Uuid::generate();
        // Version 4: bits 48-51 of hi should be 0100
        uint8_t version = static_cast<uint8_t>((id.hi >> 12) & 0x0F);
        REQUIRE(version == 4);
    }

    SECTION("UUIDv4 variant bits are set") {
        auto id = Uuid::generate();
        // Variant 1: bits 62-63 of lo should be 10
        uint8_t variant = static_cast<uint8_t>((id.lo >> 62) & 0x03);
        REQUIRE(variant == 2); // binary 10
    }
}

TEST_CASE("Uuid string round-trip", "[runtime][identity]") {
    SECTION("to_string produces correct format") {
        auto id = Uuid::generate();
        auto str = id.to_string();
        REQUIRE(str.size() == 36);
        REQUIRE(str[8] == '-');
        REQUIRE(str[13] == '-');
        REQUIRE(str[18] == '-');
        REQUIRE(str[23] == '-');
    }

    SECTION("from_string round-trips") {
        auto original = Uuid::generate();
        auto str = original.to_string();
        auto parsed = Uuid::from_string(str);
        REQUIRE(parsed == original);
    }

    SECTION("to_hex produces 32 chars") {
        auto id = Uuid::generate();
        auto hex = id.to_hex();
        REQUIRE(hex.size() == 32);
        // No dashes
        REQUIRE(hex.find('-') == std::string::npos);
    }

    SECTION("from_string handles compact hex") {
        auto original = Uuid::generate();
        auto hex = original.to_hex();
        auto parsed = Uuid::from_string(hex);
        REQUIRE(parsed == original);
    }

    SECTION("from_string accepts uppercase hex") {
        auto parsed = Uuid::from_string("00112233-4455-6677-8899-AABBCCDDEEFF");
        REQUIRE_FALSE(parsed.is_nil());
        REQUIRE(parsed.to_string() == "00112233-4455-6677-8899-aabbccddeeff");
    }

    SECTION("Nil UUID") {
        Uuid nil;
        REQUIRE(nil.is_nil());
        REQUIRE(nil.to_string() == "00000000-0000-0000-0000-000000000000");
    }

    SECTION("Invalid string returns nil") {
        auto parsed = Uuid::from_string("not-a-uuid");
        REQUIRE(parsed.is_nil());
    }

    SECTION("Invalid hex digits and misplaced dashes return nil") {
        REQUIRE(Uuid::from_string("00112233-4455-6677-8899-aabbccddeefg").is_nil());
        REQUIRE(Uuid::from_string("00112233445566778899aabb-ccddeeff").is_nil());
        REQUIRE(Uuid::from_string("001122334455-6677-8899-aabbccddeeff").is_nil());
        REQUIRE(Uuid::from_string("0011223344556677-8899-aabb-ccdd-eeff").is_nil());
        REQUIRE(Uuid::from_string("00112233445566778899aabbccddeeff00").is_nil());
    }
}

TEST_CASE("Uuid ordering and hashing cover deterministic values", "[runtime][identity][coverage][issue-656]") {
    auto low = Uuid::from_string("00000000-0000-0000-0000-000000000001");
    auto high = Uuid::from_string("00000000-0000-0000-0000-000000000002");

    REQUIRE(low < high);
    REQUIRE_FALSE(high < low);

    std::unordered_set<Uuid> ids;
    ids.insert(low);
    ids.insert(high);
    ids.insert(Uuid::from_string(low.to_hex()));
    REQUIRE(ids.size() == 2);
}

TEST_CASE("Uuid parsing rejects malformed dashed layout",
          "[runtime][identity][coverage][issue-656]") {
    auto parsed = Uuid::from_string("0011223344556677-8899-aabb-ccdd-eeff");
    REQUIRE(parsed.is_nil());
}

TEST_CASE("Uuid parsing accepts uppercase dashed and compact hex",
          "[runtime][identity][issue-641]") {
    Uuid id;
    id.hi = 0x0123456789abcdefULL;
    id.lo = 0xfedcba9876543210ULL;

    REQUIRE(Uuid::from_string("01234567-89AB-CDEF-FEDC-BA9876543210") == id);
    REQUIRE(Uuid::from_string("0123456789ABCDEFFEDCBA9876543210") == id);
}

TEST_CASE("Uuid parsing rejects short, long, and whitespace-padded values",
          "[runtime][identity][coverage][phase3]") {
    REQUIRE(Uuid::from_string("").is_nil());
    REQUIRE(Uuid::from_string("00112233445566778899aabbccddee").is_nil());
    REQUIRE(Uuid::from_string("00112233445566778899aabbccddeeff00").is_nil());
    REQUIRE(Uuid::from_string(" 00112233-4455-6677-8899-aabbccddeeff").is_nil());
    REQUIRE(Uuid::from_string("00112233-4455-6677-8899-aabbccddeeff ").is_nil());
    REQUIRE(Uuid::from_string("00112233-4455-6677-8899-aabbccddee-ff").is_nil());
}

TEST_CASE("Uuid formatting preserves leading zero bytes",
          "[runtime][identity][coverage][phase3]") {
    Uuid id;
    id.hi = 0x0001020304050607ULL;
    id.lo = 0x08090a0b0c0d0e0fULL;

    REQUIRE(id.to_hex() == "000102030405060708090a0b0c0d0e0f");
    REQUIRE(id.to_string() == "00010203-0405-0607-0809-0a0b0c0d0e0f");
    REQUIRE(Uuid::from_string(id.to_string()) == id);
    REQUIRE(Uuid::from_string(id.to_hex()) == id);
}

TEST_CASE("Uuid ordering sorts by high word before low word",
          "[runtime][identity][issue-641]") {
    Uuid low{1, 999};
    Uuid middle{2, 1};
    Uuid high{2, 2};

    REQUIRE(low < middle);
    REQUIRE(middle < high);
    REQUIRE_FALSE(high < middle);
}

TEST_CASE("Typed identity wrappers", "[runtime][identity]") {
    SECTION("SessionId") {
        auto sid = SessionId::generate();
        REQUIRE_FALSE(sid.is_nil());
        REQUIRE(SessionId::nil().is_nil());
        REQUIRE(sid != SessionId::generate()); // unique
    }

    SECTION("RunId") {
        auto rid = RunId::generate();
        REQUIRE_FALSE(rid.is_nil());
        REQUIRE(RunId::nil().is_nil());
    }

    SECTION("ObjectId") {
        auto oid = ObjectId::generate();
        REQUIRE_FALSE(oid.is_nil());

        // Round-trip through string
        auto str = oid.to_string();
        auto parsed = ObjectId::from_string(str);
        REQUIRE(parsed == oid);
    }

    SECTION("CorrelationId") {
        auto cid = CorrelationId::generate();
        REQUIRE_FALSE(cid.is_nil());
    }

    SECTION("Type safety — different ID types are distinct") {
        // This test verifies the design; types are checked at compile time.
        auto sid = SessionId::generate();
        auto oid = ObjectId::generate();
        // These should NOT compile: sid == oid (different types)
        // Instead, verify they can coexist in separate containers
        std::unordered_set<SessionId> sessions;
        std::unordered_set<ObjectId> objects;
        sessions.insert(sid);
        objects.insert(oid);
        REQUIRE(sessions.size() == 1);
        REQUIRE(objects.size() == 1);
    }
}

TEST_CASE("Typed identity nil and hashing are stable", "[runtime][identity][issue-641]") {
    REQUIRE(SessionId::nil() == SessionId::nil());
    REQUIRE(RunId::nil() == RunId::nil());
    REQUIRE(ObjectId::nil() == ObjectId::from_string("00000000000000000000000000000000"));
    REQUIRE(CorrelationId::nil() == CorrelationId::nil());

    auto object = ObjectId::generate();
    std::unordered_set<ObjectId> objects;
    objects.insert(object);
    objects.insert(object);
    REQUIRE(objects.size() == 1);
}

TEST_CASE("Typed identity wrappers compare and hash deterministic values",
          "[runtime][identity][coverage][phase3]") {
    auto first = ObjectId::from_string("00000000-0000-0000-0000-000000000001");
    auto second = ObjectId::from_string("00000000-0000-0000-0000-000000000002");

    REQUIRE(first != second);
    REQUIRE(first < second);
    REQUIRE_FALSE(second < first);
    REQUIRE(first.to_string() == "00000000-0000-0000-0000-000000000001");

    std::unordered_map<ObjectId, std::string> objects;
    objects[first] = "one";
    objects[second] = "two";
    objects[first] = "updated";

    REQUIRE(objects.size() == 2);
    REQUIRE(objects[first] == "updated");
    REQUIRE(objects[second] == "two");
}

TEST_CASE("Typed transient identity wrappers hash deterministic values",
          "[runtime][identity][codecov]") {
    SessionId session{Uuid::from_string("10000000-0000-0000-0000-000000000001")};
    RunId run{Uuid::from_string("20000000-0000-0000-0000-000000000002")};
    CorrelationId correlation{Uuid::from_string("30000000-0000-0000-0000-000000000003")};

    REQUIRE(session.to_string() == "10000000-0000-0000-0000-000000000001");
    REQUIRE(run.to_string() == "20000000-0000-0000-0000-000000000002");
    REQUIRE(correlation.to_string() == "30000000-0000-0000-0000-000000000003");

    std::unordered_map<SessionId, std::string> sessions;
    sessions[session] = "session";
    sessions[SessionId{session.value}] = "updated";
    REQUIRE(sessions.size() == 1);
    REQUIRE(sessions[session] == "updated");

    std::unordered_set<RunId> runs;
    runs.insert(run);
    runs.insert(RunId{run.value});
    REQUIRE(runs.size() == 1);

    std::unordered_set<CorrelationId> correlations;
    correlations.insert(correlation);
    correlations.insert(CorrelationId{Uuid::from_string(correlation.to_string())});
    REQUIRE(correlations.size() == 1);
}

TEST_CASE("EventEnvelope defaults are nil and empty before attribution",
          "[runtime][identity][coverage][phase3]") {
    EventEnvelope env;

    REQUIRE(env.timestamp == 0.0);
    REQUIRE(env.session_id.is_nil());
    REQUIRE(env.object_id.is_nil());
    REQUIRE(env.run_id.is_nil());
    REQUIRE(env.correlation_id.is_nil());
    REQUIRE(env.actor.empty());
    REQUIRE(env.action.empty());
}

TEST_CASE("EventEnvelope", "[runtime][identity]") {
    EventEnvelope env;
    env.timestamp = 1711411200.0; // 2024-03-26T00:00:00Z
    env.session_id = SessionId::generate();
    env.object_id = ObjectId::generate();
    env.run_id = RunId::generate();
    env.correlation_id = CorrelationId::generate();
    env.actor = "ai";
    env.action = "modify";

    REQUIRE_FALSE(env.session_id.is_nil());
    REQUIRE_FALSE(env.object_id.is_nil());
    REQUIRE_FALSE(env.run_id.is_nil());
    REQUIRE(env.actor == "ai");
    REQUIRE(env.action == "modify");
}
