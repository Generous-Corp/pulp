#include <pulp/inspect/agent_request_queue.hpp>

#include <choc/text/choc_JSON.h>

#include <cctype>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace pulp::inspect {

namespace {

constexpr std::string_view kSchema = "pulp-design-requests://v1";
constexpr int kVersion = 1;

/// The largest all-digits id present, or 0 when none. Non-numeric ids are
/// ignored for the counter so a hand-authored id never collides with the
/// auto-assigned sequence.
long long max_numeric_id(const std::vector<AgentRequest>& requests) {
    long long max = 0;
    for (const auto& r : requests) {
        if (r.id.empty()) continue;
        bool all_digits = true;
        for (char c : r.id)
            if (!std::isdigit(static_cast<unsigned char>(c))) {
                all_digits = false;
                break;
            }
        if (!all_digits) continue;
        try {
            long long v = std::stoll(r.id);
            if (v > max) max = v;
        } catch (...) {
            // out-of-range id: ignore for counting
        }
    }
    return max;
}

std::string member_str(const choc::value::ValueView& obj, std::string_view key) {
    if (obj.isObject() && obj.hasObjectMember(std::string(key))) {
        auto m = obj[std::string(key).c_str()];
        if (m.isString()) return std::string(m.getString());
    }
    return {};
}

}  // namespace

std::string_view queue_filename() { return ".pulp-design-requests.json"; }

std::string requests_to_json(const std::vector<AgentRequest>& requests) {
    auto root = choc::value::createObject("");
    root.setMember("$schema", std::string(kSchema));
    root.setMember("version", kVersion);
    auto arr = choc::value::createEmptyArray();
    for (const auto& r : requests) {
        auto o = choc::value::createObject("");
        o.setMember("id", r.id);
        o.setMember("text", r.text);
        o.setMember("design", r.design);
        o.setMember("screen", r.screen);
        o.setMember("editmode_state", r.editmode_state);
        o.setMember("screenshot_path", r.screenshot_path);
        o.setMember("created_at", r.created_at);
        o.setMember("consumed", r.consumed);
        arr.addArrayElement(o);
    }
    root.setMember("requests", arr);
    return choc::json::toString(root, true);
}

std::optional<std::vector<AgentRequest>> requests_from_json(std::string_view queue_json) {
    // An empty/whitespace document is an empty queue, not an error.
    bool only_ws = true;
    for (char c : queue_json)
        if (!std::isspace(static_cast<unsigned char>(c))) {
            only_ws = false;
            break;
        }
    if (only_ws) return std::vector<AgentRequest>{};

    choc::value::Value root;
    try {
        root = choc::json::parse(queue_json);
    } catch (...) {
        return std::nullopt;
    }
    if (!root.isObject() || !root.hasObjectMember("requests") || !root["requests"].isArray())
        return std::nullopt;

    std::vector<AgentRequest> out;
    auto arr = root["requests"];
    for (uint32_t i = 0; i < arr.size(); ++i) {
        auto e = arr[i];
        if (!e.isObject()) continue;
        AgentRequest r;
        r.id = member_str(e, "id");
        r.text = member_str(e, "text");
        // A request with neither an id nor text carries nothing actionable.
        if (r.id.empty() && r.text.empty()) continue;
        r.design = member_str(e, "design");
        r.screen = member_str(e, "screen");
        r.editmode_state = member_str(e, "editmode_state");
        r.screenshot_path = member_str(e, "screenshot_path");
        r.created_at = member_str(e, "created_at");
        r.consumed = e.hasObjectMember("consumed") && e["consumed"].getWithDefault<bool>(false);
        out.push_back(std::move(r));
    }
    return out;
}

std::optional<std::string> append_request(std::string_view queue_json, AgentRequest request) {
    auto requests = requests_from_json(queue_json);
    if (!requests) return std::nullopt;
    if (request.id.empty())
        request.id = std::to_string(max_numeric_id(*requests) + 1);
    requests->push_back(std::move(request));
    return requests_to_json(*requests);
}

std::vector<AgentRequest> pending_requests(std::string_view queue_json) {
    auto requests = requests_from_json(queue_json);
    if (!requests) return {};
    std::vector<AgentRequest> pending;
    for (auto& r : *requests)
        if (!r.consumed) pending.push_back(std::move(r));
    return pending;
}

std::optional<std::pair<std::string, bool>> ack_request(std::string_view queue_json,
                                                        std::string_view id) {
    auto requests = requests_from_json(queue_json);
    if (!requests) return std::nullopt;
    bool matched = false;
    for (auto& r : *requests)
        if (r.id == id) {
            r.consumed = true;
            matched = true;
        }
    return std::make_pair(requests_to_json(*requests), matched);
}

// ── file-backed wrappers ────────────────────────────────────────────────────

namespace {

std::optional<std::string> read_file(const std::string& path) {
    std::error_code ec;
    if (!std::filesystem::exists(path, ec)) return std::string{};  // absent → empty queue
    std::ifstream f(path, std::ios::binary);
    if (!f) return std::nullopt;
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

/// Write `content` to `path` atomically: a sibling `<path>.tmp` then a rename
/// over the target, so a crash mid-write never leaves a half-flushed file.
bool write_file_atomic(const std::string& path, const std::string& content) {
    const std::string tmp = path + ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) return false;
        f << content;
        if (!f.good()) return false;
    }
    std::error_code ec;
    std::filesystem::rename(tmp, path, ec);
    if (ec) {
        std::filesystem::remove(tmp, ec);  // don't leak the temp on failure
        return false;
    }
    return true;
}

}  // namespace

std::string queue_path(std::string_view project_dir) {
    return (std::filesystem::path(std::string(project_dir)) / std::string(queue_filename()))
        .string();
}

std::optional<std::string> enqueue_to_file(const std::string& path, AgentRequest request) {
    auto current = read_file(path);
    if (!current) return std::nullopt;
    auto requests = requests_from_json(*current);
    if (!requests) return std::nullopt;
    if (request.id.empty())
        request.id = std::to_string(max_numeric_id(*requests) + 1);
    const std::string assigned = request.id;
    requests->push_back(std::move(request));
    if (!write_file_atomic(path, requests_to_json(*requests))) return std::nullopt;
    return assigned;
}

std::vector<AgentRequest> read_pending_file(const std::string& path) {
    auto current = read_file(path);
    if (!current) return {};
    return pending_requests(*current);
}

bool ack_in_file(const std::string& path, std::string_view id) {
    auto current = read_file(path);
    if (!current) return false;
    auto acked = ack_request(*current, id);
    if (!acked) return false;
    if (!acked->second) return false;  // no id matched — nothing to write
    return write_file_atomic(path, acked->first);
}

}  // namespace pulp::inspect
