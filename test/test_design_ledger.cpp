// Tests for the project design ledger (pulp::design::DesignLedger): the pure
// parse/serialize round-trip, version assignment, removal, and reconciliation.

#include <catch2/catch_test_macros.hpp>

#include <pulp/design/design_ledger.hpp>

#include <choc/text/choc_JSON.h>

#include <set>

using namespace pulp::design;

namespace {

LedgerAsset asset(std::string name, std::string path, std::string version = "") {
    LedgerAsset a;
    a.name = std::move(name);
    a.path = std::move(path);
    a.version = std::move(version);
    return a;
}

}  // namespace

TEST_CASE("Review status slugs round-trip and reject unknowns", "[design-ledger]") {
    REQUIRE(std::string(review_status_name(ReviewStatus::needs_review)) == "needs-review");
    REQUIRE(std::string(review_status_name(ReviewStatus::approved)) == "approved");
    REQUIRE(std::string(review_status_name(ReviewStatus::changes_requested)) == "changes-requested");

    REQUIRE(review_status_from_name("approved") == ReviewStatus::approved);
    REQUIRE(review_status_from_name("changes-requested") == ReviewStatus::changes_requested);
    REQUIRE_FALSE(review_status_from_name("bogus").has_value());
}

TEST_CASE("Recording a name auto-increments its version", "[design-ledger]") {
    DesignLedger ledger;
    auto& a1 = upsert_asset(ledger, asset("panel", "ui.js"));
    REQUIRE(a1.version == "v1");
    auto& a2 = upsert_asset(ledger, asset("panel", "ui2.js"));
    REQUIRE(a2.version == "v2");
    // A different name starts its own chain at v1.
    auto& b1 = upsert_asset(ledger, asset("footer", "foot.js"));
    REQUIRE(b1.version == "v1");
    REQUIRE(ledger.assets.size() == 3);
}

TEST_CASE("Recording an explicit existing version replaces it in place", "[design-ledger]") {
    DesignLedger ledger;
    upsert_asset(ledger, asset("panel", "ui.js"));  // v1
    LedgerAsset update = asset("panel", "ui-fixed.js", "v1");
    update.status = ReviewStatus::approved;
    auto& stored = upsert_asset(ledger, update);
    REQUIRE(ledger.assets.size() == 1);  // replaced, not appended
    REQUIRE(stored.path == "ui-fixed.js");
    REQUIRE(stored.status == ReviewStatus::approved);
}

TEST_CASE("Next version is one past the highest, not a gap-filler", "[design-ledger]") {
    DesignLedger ledger;
    upsert_asset(ledger, asset("panel", "a.js"));  // v1
    upsert_asset(ledger, asset("panel", "b.js"));  // v2
    upsert_asset(ledger, asset("panel", "c.js"));  // v3
    remove_asset(ledger, "panel@v2");              // drop the middle revision
    auto& next = upsert_asset(ledger, asset("panel", "d.js"));
    // Must be v4 (past the highest surviving v3), never reuse v2.
    REQUIRE(next.version == "v4");
}

TEST_CASE("design_systems are stored sorted and de-duplicated", "[design-ledger]") {
    DesignLedger ledger;
    LedgerAsset a = asset("panel", "ui.js");
    a.design_systems = {"zebra", "ink-signal", "ink-signal", "alpha"};
    auto& stored = upsert_asset(ledger, a);
    REQUIRE(stored.design_systems == std::vector<std::string>{"alpha", "ink-signal", "zebra"});
}

TEST_CASE("Ledger JSON round-trips and is deterministic", "[design-ledger]") {
    DesignLedger ledger;
    LedgerAsset a = asset("panel", "ui.js");
    a.source = "fig";
    a.viewport = "340x280";
    a.inherit_from = "";
    a.design_systems = {"ink-signal"};
    a.status = ReviewStatus::approved;
    upsert_asset(ledger, a);
    upsert_asset(ledger, asset("footer", "foot.js"));

    const std::string json = ledger_to_json(ledger);
    // Deterministic: same ledger serializes identically.
    REQUIRE(json == ledger_to_json(ledger));

    // Structural round-trip through parse.
    DesignLedger back = parse_ledger(json);
    REQUIRE(back.ledger_version == std::string(kDesignLedgerVersion));
    REQUIRE(back.assets.size() == 2);
    // Sorted by (name, version): footer before panel.
    REQUIRE(back.assets[0].name == "footer");
    REQUIRE(back.assets[1].name == "panel");
    REQUIRE(back.assets[1].source == "fig");
    REQUIRE(back.assets[1].viewport == "340x280");
    REQUIRE(back.assets[1].status == ReviewStatus::approved);
    REQUIRE(back.assets[1].design_systems == std::vector<std::string>{"ink-signal"});

    // Re-serializing the parsed ledger yields byte-identical JSON.
    REQUIRE(ledger_to_json(back) == json);
}

TEST_CASE("Parsing is tolerant of empty and malformed input", "[design-ledger]") {
    REQUIRE(parse_ledger("").assets.empty());
    REQUIRE(parse_ledger("not json at all {[").assets.empty());
    REQUIRE(parse_ledger("[1,2,3]").assets.empty());     // array, not object
    REQUIRE(parse_ledger("{\"assets\": 5}").assets.empty());  // wrong type
    // An unknown status slug falls back to needs-review, not a drop.
    auto l = parse_ledger(R"({"assets":[{"name":"p","version":"v1","status":"weird"}]})");
    REQUIRE(l.assets.size() == 1);
    REQUIRE(l.assets[0].status == ReviewStatus::needs_review);
    // An asset with no name is skipped.
    REQUIRE(parse_ledger(R"({"assets":[{"version":"v1"}]})").assets.empty());
}

TEST_CASE("remove_asset drops a whole name or a single revision", "[design-ledger]") {
    DesignLedger ledger;
    upsert_asset(ledger, asset("panel", "a.js"));  // v1
    upsert_asset(ledger, asset("panel", "b.js"));  // v2
    upsert_asset(ledger, asset("footer", "f.js"));

    // Single revision.
    auto r1 = remove_asset(ledger, "panel@v1");
    REQUIRE(r1 == std::vector<std::string>{"panel@v1"});
    REQUIRE(ledger.assets.size() == 2);

    // Whole name (all remaining versions).
    upsert_asset(ledger, asset("panel", "c.js"));  // v3 now
    auto r2 = remove_asset(ledger, "panel");
    REQUIRE(r2.size() == 2);  // v2 and v3
    REQUIRE(ledger.assets.size() == 1);
    REQUIRE(ledger.assets[0].name == "footer");

    // No match reports empty.
    REQUIRE(remove_asset(ledger, "ghost").empty());
}

TEST_CASE("auto-versioning links the new revision to the prior version", "[design-ledger]") {
    DesignLedger ledger;
    auto& v1 = upsert_asset(ledger, asset("panel", "ui-v1.js"));
    REQUIRE(v1.version == "v1");
    REQUIRE(v1.inherit_from.empty());  // first revision is a root
    auto& v2 = upsert_asset(ledger, asset("panel", "ui-v2.js"));
    REQUIRE(v2.version == "v2");
    REQUIRE(v2.inherit_from == "v1");  // chain is connected by default
    auto& v3 = upsert_asset(ledger, asset("panel", "ui-v3.js"));
    REQUIRE(v3.inherit_from == "v2");
}

TEST_CASE("an explicit inherit_from is not overwritten by auto-linking", "[design-ledger]") {
    DesignLedger ledger;
    upsert_asset(ledger, asset("panel", "a.js"));         // v1
    upsert_asset(ledger, asset("panel", "b.js"));         // v2 -> v1
    LedgerAsset forked = asset("panel", "c.js");
    forked.inherit_from = "v1";                            // deliberately fork off v1
    auto& v3 = upsert_asset(ledger, forked);
    REQUIRE(v3.version == "v3");
    REQUIRE(v3.inherit_from == "v1");  // caller's explicit parent wins
}

TEST_CASE("removing a parent clears the child's dangling inherit_from", "[design-ledger]") {
    DesignLedger ledger;
    upsert_asset(ledger, asset("panel", "a.js"));  // v1
    upsert_asset(ledger, asset("panel", "b.js"));  // v2 -> v1
    remove_asset(ledger, "panel@v1");
    REQUIRE(ledger.assets.size() == 1);
    REQUIRE(ledger.assets[0].version == "v2");
    REQUIRE(ledger.assets[0].inherit_from.empty());  // no longer points at a missing v1
}

TEST_CASE("reconcile clears a child's link to a reconciled-away parent", "[design-ledger]") {
    DesignLedger ledger;
    upsert_asset(ledger, asset("panel", "v1.js"));  // v1
    upsert_asset(ledger, asset("panel", "v2.js"));  // v2 -> v1
    std::set<std::string> on_disk = {"v2.js"};  // v1.js was hand-deleted
    reconcile(ledger, [&](const std::string& p) { return on_disk.count(p) > 0; });
    REQUIRE(ledger.assets.size() == 1);
    REQUIRE(ledger.assets[0].version == "v2");
    REQUIRE(ledger.assets[0].inherit_from.empty());
}

TEST_CASE("reconcile drops entries whose file is gone", "[design-ledger]") {
    DesignLedger ledger;
    upsert_asset(ledger, asset("panel", "present.js"));
    upsert_asset(ledger, asset("footer", "gone.js"));
    // An entry with no path is never reconciled away.
    upsert_asset(ledger, asset("pathless", ""));

    std::set<std::string> on_disk = {"present.js"};
    auto removed = reconcile(ledger, [&](const std::string& p) { return on_disk.count(p) > 0; });

    REQUIRE(removed == std::vector<std::string>{"footer@v1"});
    REQUIRE(ledger.assets.size() == 2);
    // present.js and the pathless entry survive.
    bool has_panel = false, has_pathless = false;
    for (const auto& a : ledger.assets) {
        if (a.name == "panel") has_panel = true;
        if (a.name == "pathless") has_pathless = true;
    }
    REQUIRE(has_panel);
    REQUIRE(has_pathless);
}
