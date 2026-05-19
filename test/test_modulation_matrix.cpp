// ModulationMatrix data-model tests (workstream 07 slice 7.1).

#include <catch2/catch_test_macros.hpp>
#include <pulp/view/modulation_matrix.hpp>

using namespace pulp::view;

TEST_CASE("add/find/size", "[view][mod-matrix]") {
    ModulationMatrix m;
    REQUIRE(m.empty());
    ModRoute lfo_to_cutoff{.source = 1, .destination = 100, .depth = 0.5f};
    auto idx = m.add(lfo_to_cutoff);
    REQUIRE(idx == 0);
    REQUIRE(m.size() == 1);
    REQUIRE(m.find(1, 100).has_value());
    REQUIRE_FALSE(m.find(1, 999).has_value());
}

TEST_CASE("add with same source+destination replaces in place",
          "[view][mod-matrix]") {
    ModulationMatrix m;
    m.add({1, 100, 0.25f, false, ModCurve::Linear});
    auto idx = m.add({1, 100, 0.75f, true, ModCurve::Exponential});
    REQUIRE(idx == 0);
    REQUIRE(m.size() == 1);
    REQUIRE(m.routes()[0].depth == 0.75f);
    REQUIRE(m.routes()[0].bipolar);
    REQUIRE(m.routes()[0].curve == ModCurve::Exponential);
}

TEST_CASE("remove + remove_by_destination", "[view][mod-matrix]") {
    ModulationMatrix m;
    m.add({1, 100, 0.5f, false, ModCurve::Linear});
    m.add({2, 100, 0.25f, true, ModCurve::Quadratic});
    m.add({1, 200, 0.5f, false, ModCurve::Linear});
    REQUIRE(m.size() == 3);
    m.remove(0);
    REQUIRE(m.size() == 2);
    m.remove_by_destination(100);
    REQUIRE(m.size() == 1);
    REQUIRE(m.routes()[0].destination == 200);
}

TEST_CASE("remove out of range and clear keep matrix reusable",
          "[view][mod-matrix]") {
    ModulationMatrix m;
    m.add({1, 100, 0.5f, false, ModCurve::Linear});
    m.remove(99);
    REQUIRE(m.size() == 1);
    REQUIRE(m.routes()[0].source == 1);

    m.clear();
    REQUIRE(m.empty());

    m.add({2, 200, -0.25f, true, ModCurve::SCurve});
    REQUIRE(m.size() == 1);
    REQUIRE(m.find(2, 200).has_value());
}

TEST_CASE("serialize + deserialize round-trip", "[view][mod-matrix]") {
    ModulationMatrix a;
    a.add({1, 100, 0.5f, false, ModCurve::Linear});
    a.add({2, 200, -0.25f, true, ModCurve::SCurve});
    a.add({3, 300, 1.0f, false, ModCurve::Exponential});

    auto blob = a.serialize();
    ModulationMatrix b;
    REQUIRE(b.deserialize(blob.data(), blob.size()));
    REQUIRE(b.size() == a.size());
    for (std::size_t i = 0; i < a.size(); ++i) {
        REQUIRE(a.routes()[i] == b.routes()[i]);
    }
}

TEST_CASE("deserialize rejects malformed blobs", "[view][mod-matrix]") {
    ModulationMatrix b;
    // Empty
    REQUIRE_FALSE(b.deserialize(nullptr, 0));
    // Wrong magic
    uint8_t bad[] = {0, 0, 0, 0, 0, 0, 0, 0};
    REQUIRE_FALSE(b.deserialize(bad, sizeof(bad)));
    // Valid magic, but claims 1000 routes in 8 bytes
    uint8_t short_hdr[] = {0x31, 0x4D, 0x4D, 0x50, 0xE8, 0x03, 0, 0};
    REQUIRE_FALSE(b.deserialize(short_hdr, sizeof(short_hdr)));
}

TEST_CASE("empty matrix round-trips", "[view][mod-matrix]") {
    ModulationMatrix a;
    auto blob = a.serialize();
    ModulationMatrix b;
    b.add({99, 999, 0.1f});          // must be cleared by deserialize
    REQUIRE(b.deserialize(blob.data(), blob.size()));
    REQUIRE(b.empty());
}
