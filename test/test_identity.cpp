#include <catch2/catch_test_macros.hpp>
#include <pulp/runtime/identity.hpp>
#include <set>
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

    SECTION("Nil UUID") {
        Uuid nil;
        REQUIRE(nil.is_nil());
        REQUIRE(nil.to_string() == "00000000-0000-0000-0000-000000000000");
    }

    SECTION("Invalid string returns nil") {
        auto parsed = Uuid::from_string("not-a-uuid");
        REQUIRE(parsed.is_nil());
    }
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
