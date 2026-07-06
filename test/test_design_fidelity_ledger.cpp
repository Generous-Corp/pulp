// Tests for the named fidelity taxonomy + per-run JSON ledger
// (pulp::view::fidelity_taxonomy / fidelity_ledger_json).

#include <catch2/catch_test_macros.hpp>

#include <pulp/view/design_fidelity_ledger.hpp>

#include <choc/text/choc_JSON.h>

using namespace pulp::view;

namespace {

FidelityIssue make_issue(std::string kind, bool informational) {
    FidelityIssue fi;
    fi.node_id = "42:1";
    fi.node_name = "Knob";
    fi.kind = std::move(kind);
    fi.detail = "detail text";
    fi.informational = informational;
    return fi;
}

}  // namespace

TEST_CASE("Taxonomy covers every kind the checks emit with a stable severity",
          "[design-fidelity-ledger]") {
    const auto& tax = fidelity_taxonomy();
    REQUIRE_FALSE(tax.empty());

    // Every entry has a slug, a severity drawn from the closed set, and a summary.
    for (const auto& info : tax) {
        REQUIRE_FALSE(info.kind.empty());
        REQUIRE((info.severity == "warning" || info.severity == "info"));
        REQUIRE_FALSE(info.summary.empty());
    }

    // The kinds design_fidelity.hpp documents are all registered.
    for (const char* kind : {"skew", "aspect-unverified", "gross-size", "widget-size",
                             "widget-undersized", "text-vcenter", "dropped-vector"}) {
        REQUIRE(fidelity_kind_info(kind) != nullptr);
    }
    // An unregistered slug returns nullptr.
    REQUIRE(fidelity_kind_info("no-such-kind") == nullptr);

    // The declared default severity must match how design_fidelity.cpp emits
    // each kind: only widget-undersized is created informational=true (an
    // advisory clamp-up); every other kind is a hard finding that
    // count_strict_fidelity_failures() counts and --strict-fidelity fails on.
    // A future check that flips a kind's informational flag must update both.
    REQUIRE(fidelity_kind_info("widget-undersized")->severity == "info");
    for (const char* hard : {"skew", "aspect-unverified", "gross-size",
                             "widget-size", "text-vcenter", "dropped-vector"}) {
        REQUIRE(fidelity_kind_info(hard)->severity == "warning");
    }
}

TEST_CASE("Ledger JSON is well-formed and reports per-kind + severity counts",
          "[design-fidelity-ledger]") {
    std::vector<FidelityIssue> issues = {
        make_issue("skew", /*informational=*/false),
        make_issue("skew", /*informational=*/false),
        make_issue("aspect-unverified", /*informational=*/true),
    };

    FidelityLedgerMeta meta;
    meta.source = "fig";
    meta.output = "ui.js";

    auto json = fidelity_ledger_json(issues, meta);
    auto v = choc::json::parse(json);

    REQUIRE(v["format_version"].getString() == std::string(kFidelityLedgerVersion));
    REQUIRE(v["source"].getString() == "fig");
    REQUIRE(v["output"].getString() == "ui.js");
    // generated_at omitted when the caller supplies none.
    REQUIRE_FALSE(v.hasObjectMember("generated_at"));

    auto summary = v["summary"];
    REQUIRE(summary["total"].getInt64() == 3);
    // Two skews are warnings (hard), the aspect-unverified is informational.
    REQUIRE(summary["warnings"].getInt64() == 2);
    REQUIRE(summary["informational"].getInt64() == 1);
    REQUIRE(summary["by_kind"]["skew"].getInt64() == 2);
    REQUIRE(summary["by_kind"]["aspect-unverified"].getInt64() == 1);

    REQUIRE(v["findings"].size() == 3);
    // Severity is driven by the informational flag.
    REQUIRE(v["findings"][0]["severity"].getString() == "warning");
    REQUIRE(v["findings"][2]["severity"].getString() == "info");

    // The taxonomy is embedded so a consumer can render kinds it does not know.
    REQUIRE(v["taxonomy"].size() == fidelity_taxonomy().size());
}

TEST_CASE("Empty run yields a valid, zeroed ledger", "[design-fidelity-ledger]") {
    FidelityLedgerMeta meta;
    meta.source = "figma-plugin";
    meta.output = "out.js";
    meta.generated_at = "2026-07-05T00:00:00Z";

    auto v = choc::json::parse(fidelity_ledger_json({}, meta));
    REQUIRE(v["summary"]["total"].getInt64() == 0);
    REQUIRE(v["summary"]["warnings"].getInt64() == 0);
    REQUIRE(v["summary"]["informational"].getInt64() == 0);
    REQUIRE(v["findings"].size() == 0);
    // A supplied timestamp is recorded.
    REQUIRE(v["generated_at"].getString() == "2026-07-05T00:00:00Z");
}

TEST_CASE("Ledger serialization is deterministic", "[design-fidelity-ledger]") {
    std::vector<FidelityIssue> issues = {
        make_issue("dropped-vector", false),
        make_issue("gross-size", false),
        make_issue("skew", false),
    };
    FidelityLedgerMeta meta;
    meta.source = "fig";
    meta.output = "ui.js";
    REQUIRE(fidelity_ledger_json(issues, meta) == fidelity_ledger_json(issues, meta));
}
