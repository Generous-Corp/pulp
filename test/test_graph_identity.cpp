#include <catch2/catch_test_macros.hpp>
#include <pulp/host/graph_identity.hpp>

#include <array>

using namespace pulp::host;

TEST_CASE("G3 authored port identity survives canonical reorder") {
    const std::array first{
        PortHandle{1, 0, false, "cutoff", PortKind::Audio},
        PortHandle{1, 1, false, "drive", PortKind::Audio},
        PortHandle{1, 2, false, "mix", PortKind::Audio},
    };
    const std::array reordered{
        PortHandle{1, 9, false, "mix", PortKind::Audio},
        PortHandle{1, 4, false, "cutoff", PortKind::Audio},
        PortHandle{1, 7, false, "drive", PortKind::Audio},
    };
    REQUIRE(validate_port_identities(first).valid);
    REQUIRE(validate_port_identities(reordered).valid);
    CHECK(port_identity_fingerprint(first) == port_identity_fingerprint(reordered));
    const auto diff = diff_port_identities(first, reordered);
    REQUIRE(diff.valid);
    CHECK(diff.added.empty());
    CHECK(diff.removed.empty());
    CHECK(diff.order_changed);
}

TEST_CASE("G3 aliases preserve authored identity and reject ambiguous sets") {
    const std::array ports{PortHandle{1, 0, false, "Drive", PortKind::Audio}};
    const std::array aliases{PortIdentityAlias{"drive", "Drive", 1}};
    const auto report = validate_port_identities(ports, aliases);
    REQUIRE(report.valid);
    REQUIRE(report.normalized_names.size() == 1);
    CHECK(report.normalized_names.front() == "drive");

    const std::array duplicate{
        PortHandle{1, 0, false, "drive", PortKind::Audio},
        PortHandle{1, 1, false, "Drive", PortKind::Audio},
    };
    CHECK_FALSE(validate_port_identities(duplicate, aliases).valid);
}

TEST_CASE("G3 planted negative identity controls fail closed") {
    const std::array renamed{PortHandle{1, 0, false, "gain", PortKind::Audio}};
    const std::array original{PortHandle{1, 0, false, "drive", PortKind::Audio}};
    CHECK(port_identity_fingerprint(original) != port_identity_fingerprint(renamed));
    CHECK_FALSE(
        validate_port_identities(std::array{PortHandle{1, 0, false, "   ", PortKind::Audio}})
            .valid);
    CHECK_FALSE(
        validate_port_identities(
            std::array{PortHandle{1, 0, false, std::string("bad\0name", 8), PortKind::Audio}})
            .valid);
    CHECK_FALSE(
        validate_port_identities(
            std::array{PortHandle{1, 0, false, std::string("\xC2\x85", 2), PortKind::Audio}})
            .valid);
    CHECK_FALSE(
        validate_port_identities(
            std::array{PortHandle{1, 0, false, std::string("\xE2\x80\x83", 3), PortKind::Audio}})
            .valid);
    CHECK_FALSE(validate_port_identities(
                    std::array{PortHandle{1, 0, false, std::string("\xff", 1), PortKind::Audio}})
                    .valid);
    const auto decomposed = std::array{PortHandle{1, 0, false, "e\xCC\x81", PortKind::Audio}};
    const auto composed = std::array{PortHandle{1, 0, false, "\xC3\xA9", PortKind::Audio}};
    CHECK(port_identity_fingerprint(decomposed) == port_identity_fingerprint(composed));
    const auto decomposed_macron =
        std::array{PortHandle{1, 0, false, "a\xCC\x84", PortKind::Audio}};
    const auto composed_macron = std::array{PortHandle{1, 0, false, "\xC4\x81", PortKind::Audio}};
    CHECK(port_identity_fingerprint(decomposed_macron) ==
          port_identity_fingerprint(composed_macron));
    const auto rename = diff_port_identities(original, renamed);
    REQUIRE(rename.valid);
    REQUIRE(rename.renamed.size() == 1);
    CHECK(rename.renamed.front() == "drive -> gain");
}

TEST_CASE("G3 identity undo is value-owned") {
    const std::array before{PortHandle{2, 3, true, "drive", PortKind::Audio}};
    const auto undo = capture_port_identity_undo(before);
    REQUIRE(undo.valid);
    const auto restored = undo_port_identity(undo);
    REQUIRE(restored.size() == 1);
    CHECK(restored.front().name == "drive");
    CHECK(restored.front().index == 3);
}
