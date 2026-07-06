// design_ledger.cpp — project design ledger: parse/serialize + in-memory ops.

#include <pulp/design/design_ledger.hpp>

#include <choc/text/choc_JSON.h>

#include <algorithm>
#include <charconv>

namespace pulp::design {

namespace {

// (name, version) is the asset key. Sorting by it keeps the serialized ledger
// stable so it diffs cleanly across records.
bool asset_less(const LedgerAsset& a, const LedgerAsset& b) {
    if (a.name != b.name) return a.name < b.name;
    return a.version < b.version;
}

void sort_ledger(DesignLedger& ledger) {
    std::sort(ledger.assets.begin(), ledger.assets.end(), asset_less);
}

void sort_dedupe(std::vector<std::string>& v) {
    std::sort(v.begin(), v.end());
    v.erase(std::unique(v.begin(), v.end()), v.end());
}

std::string slug_of(const LedgerAsset& a) { return a.name + "@" + a.version; }

// Next "v<N>" for a name: one past the highest existing v-number, so removing a
// middle revision never reuses a slug already seen in review history.
std::string next_version(const DesignLedger& ledger, const std::string& name) {
    int highest = 0;
    for (const auto& a : ledger.assets) {
        if (a.name != name) continue;
        if (a.version.size() < 2 || a.version[0] != 'v') continue;
        int n = 0;
        const char* first = a.version.data() + 1;
        const char* last = a.version.data() + a.version.size();
        if (std::from_chars(first, last, n).ec == std::errc{} && n > highest) highest = n;
    }
    return "v" + std::to_string(highest + 1);
}

std::string string_member(const choc::value::ValueView& obj, const char* key) {
    if (!obj.isObject() || !obj.hasObjectMember(key)) return {};
    auto v = obj[key];
    return v.isString() ? std::string(v.getString()) : std::string{};
}

}  // namespace

const char* review_status_name(ReviewStatus status) {
    switch (status) {
        case ReviewStatus::approved: return "approved";
        case ReviewStatus::changes_requested: return "changes-requested";
        case ReviewStatus::needs_review: break;
    }
    return "needs-review";
}

std::optional<ReviewStatus> review_status_from_name(std::string_view name) {
    if (name == "needs-review") return ReviewStatus::needs_review;
    if (name == "approved") return ReviewStatus::approved;
    if (name == "changes-requested") return ReviewStatus::changes_requested;
    return std::nullopt;
}

DesignLedger parse_ledger(const std::string& json) {
    DesignLedger ledger;
    if (json.empty()) return ledger;

    choc::value::Value root;
    try {
        root = choc::json::parse(json);
    } catch (...) {
        return ledger;  // tolerant: unreadable ledger reads as empty
    }
    if (!root.isObject()) return ledger;

    ledger.ledger_version = string_member(root, "ledger_version");

    if (root.hasObjectMember("assets") && root["assets"].isArray()) {
        auto arr = root["assets"];
        for (uint32_t i = 0; i < arr.size(); ++i) {
            auto a = arr[i];
            if (!a.isObject()) continue;
            LedgerAsset asset;
            asset.name = string_member(a, "name");
            asset.version = string_member(a, "version");
            asset.path = string_member(a, "path");
            asset.inherit_from = string_member(a, "inherit_from");
            asset.source = string_member(a, "source");
            asset.viewport = string_member(a, "viewport");
            asset.status =
                review_status_from_name(string_member(a, "status")).value_or(ReviewStatus::needs_review);
            if (a.hasObjectMember("design_systems") && a["design_systems"].isArray()) {
                auto ds = a["design_systems"];
                for (uint32_t j = 0; j < ds.size(); ++j)
                    if (ds[j].isString()) asset.design_systems.push_back(std::string(ds[j].getString()));
            }
            sort_dedupe(asset.design_systems);
            if (!asset.name.empty()) ledger.assets.push_back(std::move(asset));
        }
    }
    sort_ledger(ledger);
    return ledger;
}

std::string ledger_to_json(const DesignLedger& ledger) {
    DesignLedger sorted = ledger;
    sort_ledger(sorted);

    auto root = choc::value::createObject("");
    root.addMember("ledger_version",
                   sorted.ledger_version.empty() ? std::string(kDesignLedgerVersion)
                                                 : sorted.ledger_version);

    auto assets = choc::value::createEmptyArray();
    for (const auto& a : sorted.assets) {
        auto obj = choc::value::createObject("");
        obj.addMember("name", a.name);
        obj.addMember("version", a.version);
        obj.addMember("path", a.path);
        obj.addMember("inherit_from", a.inherit_from);
        obj.addMember("source", a.source);
        obj.addMember("viewport", a.viewport);
        obj.addMember("status", std::string(review_status_name(a.status)));
        auto ds = choc::value::createEmptyArray();
        auto systems = a.design_systems;
        sort_dedupe(systems);
        for (const auto& s : systems) ds.addArrayElement(s);
        obj.addMember("design_systems", ds);
        assets.addArrayElement(obj);
    }
    root.addMember("assets", assets);

    return choc::json::toString(root, /*pretty=*/true);
}

LedgerAsset& upsert_asset(DesignLedger& ledger, const LedgerAsset& incoming) {
    LedgerAsset entry = incoming;
    sort_dedupe(entry.design_systems);
    if (entry.version.empty()) entry.version = next_version(ledger, entry.name);
    const std::string key_name = entry.name;
    const std::string key_version = entry.version;

    bool replaced = false;
    for (auto& a : ledger.assets) {
        if (a.name == key_name && a.version == key_version) {
            a = entry;  // replace an existing revision in place
            replaced = true;
            break;
        }
    }
    if (!replaced) ledger.assets.push_back(std::move(entry));
    sort_ledger(ledger);

    // Return the stored entry located by its stable (name, version) key.
    for (auto& a : ledger.assets)
        if (a.name == key_name && a.version == key_version) return a;
    return ledger.assets.back();  // unreachable: we just inserted it
}

std::vector<std::string> remove_asset(DesignLedger& ledger, const std::string& selector) {
    std::string name = selector;
    std::string version;
    if (auto at = selector.find('@'); at != std::string::npos) {
        name = selector.substr(0, at);
        version = selector.substr(at + 1);
    }
    std::vector<std::string> removed;
    auto it = std::remove_if(ledger.assets.begin(), ledger.assets.end(), [&](const LedgerAsset& a) {
        const bool match = a.name == name && (version.empty() || a.version == version);
        if (match) removed.push_back(slug_of(a));
        return match;
    });
    ledger.assets.erase(it, ledger.assets.end());
    std::sort(removed.begin(), removed.end());
    return removed;
}

std::vector<std::string> reconcile(DesignLedger& ledger,
                                   const std::function<bool(const std::string&)>& exists) {
    std::vector<std::string> removed;
    auto it = std::remove_if(ledger.assets.begin(), ledger.assets.end(), [&](const LedgerAsset& a) {
        const bool gone = !a.path.empty() && !exists(a.path);
        if (gone) removed.push_back(slug_of(a));
        return gone;
    });
    ledger.assets.erase(it, ledger.assets.end());
    std::sort(removed.begin(), removed.end());
    return removed;
}

}  // namespace pulp::design
