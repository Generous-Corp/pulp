// SPDX-License-Identifier: MIT
//
// IMPORTER_TERMS accept-to-run gate — implementation. See import_terms.hpp for
// the contract and the vendor-agnostic rule (this unit names no vendor / no
// framework; the terms body is runtime DATA carried by the add-on importer).

#include "import_terms.hpp"

#include "json_parser.hpp"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <istream>
#include <ostream>
#include <sstream>

namespace pulp::cli::import_terms {

namespace {

namespace pkg = pulp::cli::pkg;

std::string json_escape(const std::string& s) {
    std::string out;
    for (char c : s) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default: out += c;
        }
    }
    return out;
}

std::string trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return {};
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

std::string to_lower(std::string s) {
    for (auto& c : s) c = static_cast<char>(::tolower(static_cast<unsigned char>(c)));
    return s;
}

}  // namespace

bool has_terms(const TermsDescriptor& td) {
    return !trim(td.terms_text).empty();
}

std::string terms_hash(const std::string& terms_text) {
    std::uint64_t h = 1469598103934665603ull;  // FNV-1a offset basis
    for (unsigned char c : terms_text) {
        h ^= c;
        h *= 1099511628211ull;
    }
    char out[19];
    std::snprintf(out, sizeof(out), "0x%016llx",
                  static_cast<unsigned long long>(h));
    return out;
}

bool is_accepted(const std::vector<AcceptanceRecord>& records,
                 const std::string& importer_id,
                 const std::string& current_hash) {
    for (const auto& r : records)
        if (r.importer_id == importer_id && r.terms_hash == current_hash)
            return true;
    return false;
}

std::vector<AcceptanceRecord> parse_acceptance_store(const std::string& json_text) {
    std::vector<AcceptanceRecord> out;
    if (trim(json_text).empty()) return out;
    pkg::JsonParser parser{json_text};
    auto root = parser.parse();
    if (root.type != pkg::JsonValue::Array) return out;
    for (const auto& entry : root.arr()) {
        if (entry.type != pkg::JsonValue::Object) continue;
        AcceptanceRecord r;
        if (auto v = entry.get("importer")) r.importer_id = v->as_string();
        if (auto v = entry.get("terms_version")) r.terms_version = v->as_string();
        if (auto v = entry.get("acceptance_hash")) r.terms_hash = v->as_string();
        if (auto v = entry.get("vendor_id")) r.vendor_id = v->as_string();
        if (auto v = entry.get("timestamp")) r.accepted_at = v->as_string();
        if (auto v = entry.get("method")) r.method = v->as_string();
        // An entry with neither importer nor hash is malformed — skip it.
        if (r.importer_id.empty() && r.terms_hash.empty()) continue;
        out.push_back(std::move(r));
    }
    return out;
}

std::string format_acceptance_store(const std::vector<AcceptanceRecord>& records) {
    std::string j = "[\n";
    for (size_t i = 0; i < records.size(); ++i) {
        const auto& r = records[i];
        j += "  {\n";
        j += "    \"importer\": \"" + json_escape(r.importer_id) + "\",\n";
        j += "    \"terms_version\": \"" + json_escape(r.terms_version) + "\",\n";
        j += "    \"acceptance_hash\": \"" + json_escape(r.terms_hash) + "\",\n";
        j += "    \"vendor_id\": \"" + json_escape(r.vendor_id) + "\",\n";
        j += "    \"timestamp\": \"" + json_escape(r.accepted_at) + "\",\n";
        j += "    \"method\": \"" + json_escape(r.method) + "\"\n";
        j += "  }";
        j += (i + 1 < records.size()) ? ",\n" : "\n";
    }
    j += "]\n";
    return j;
}

std::vector<AcceptanceRecord> upsert_acceptance(
    std::vector<AcceptanceRecord> records, const AcceptanceRecord& rec) {
    for (auto& r : records) {
        if (r.importer_id == rec.importer_id) {
            r = rec;
            return records;
        }
    }
    records.push_back(rec);
    return records;
}

fs::path acceptance_store_path() {
    if (const char* pulp_home_env = std::getenv("PULP_HOME"))
        return fs::path(pulp_home_env) / "importer-terms-accepted.json";
    const char* home = std::getenv("HOME");
#ifdef _WIN32
    if (!home) home = std::getenv("USERPROFILE");
#endif
    if (!home) return {};
    return fs::path(home) / ".pulp" / "importer-terms-accepted.json";
}

std::vector<AcceptanceRecord> load_acceptance_store(const fs::path& path) {
    std::error_code ec;
    if (path.empty() || !fs::exists(path, ec)) return {};
    std::ifstream f(path, std::ios::binary);
    if (!f) return {};
    std::stringstream ss;
    ss << f.rdbuf();
    return parse_acceptance_store(ss.str());
}

bool save_acceptance_store(const fs::path& path,
                           const std::vector<AcceptanceRecord>& records) {
    if (path.empty()) return false;
    std::error_code ec;
    if (path.has_parent_path()) fs::create_directories(path.parent_path(), ec);
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f) return false;
    f << format_acceptance_store(records);
    return f.good();
}

std::string render_gate(const TermsDescriptor& td) {
    std::string s;
    s += "\n";
    s += "================ Importer Terms of Use ================\n";
    s += "Importer:      " + td.importer_id + "\n";
    if (!td.terms_version.empty())
        s += "Terms version: " + td.terms_version + "\n";
    s += "-------------------------------------------------------\n";
    s += trim(td.terms_text) + "\n";
    s += "-------------------------------------------------------\n";
    s += "Type \"accept\" to confirm you have read and accept these\n";
    s += "terms, or anything else to abort: ";
    return s;
}

GateResult run_gate(const TermsDescriptor& td,
                    const fs::path& store_path,
                    bool accept_flag,
                    const std::string& now_utc,
                    GateIo io) {
    if (!has_terms(td)) return GateResult::NoTermsToAccept;

    const std::string hash = terms_hash(td.terms_text);
    auto records = load_acceptance_store(store_path);
    if (is_accepted(records, td.importer_id, hash))
        return GateResult::Accepted;

    auto record_and_persist = [&](const char* method) -> GateResult {
        AcceptanceRecord rec;
        rec.importer_id = td.importer_id;
        rec.terms_version = td.terms_version;
        rec.terms_hash = hash;
        rec.vendor_id = td.vendor_id;
        rec.accepted_at = now_utc;
        rec.method = method;
        records = upsert_acceptance(std::move(records), rec);
        if (!save_acceptance_store(store_path, records))
            return GateResult::StoreError;
        return GateResult::Accepted;
    };

    // Non-interactive acceptance (CI / scripts): the flag IS the affirmative
    // action, and it is still recorded.
    if (accept_flag) return record_and_persist("flag");

    // Interactive type-to-accept. Without a TTY we cannot prompt — fail loud so
    // CI uses --accept-importer-terms deliberately rather than hanging.
    if (!io.interactive) return GateResult::NonInteractive;

    io.out << render_gate(td);
    io.out.flush();
    std::string line;
    if (!std::getline(io.in, line)) return GateResult::Declined;
    if (to_lower(trim(line)) != "accept") return GateResult::Declined;
    return record_and_persist("interactive");
}

}  // namespace pulp::cli::import_terms
