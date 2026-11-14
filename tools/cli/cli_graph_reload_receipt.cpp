#include "cli_graph_reload_receipt.hpp"

#include <choc/text/choc_JSON.h>

GraphReloadOutcome classify_graph_reload_receipt(int exit_code, const std::string& output,
                                                 std::uint64_t& generation) {
    try {
        const auto response = choc::json::parse(output);
        if (!response.isObject())
            return GraphReloadOutcome::Unknown;
        // A CLI error document has no receipt: the request never reached the
        // executor. Admission refusal means this grant is no longer usable.
        if (!response.hasObjectMember("state"))
            return graph_reload_authority_refused(output) ? GraphReloadOutcome::AuthorityRefused
                                                          : GraphReloadOutcome::Unknown;
        if (!response["state"].isString())
            return GraphReloadOutcome::Unknown;
        const auto state = response["state"].getString();
        const auto detail = response["detail"];
        const bool typed =
            detail.isObject() && detail.hasObjectMember("code") && detail["code"].isString();
        if ((state == "completed" || state == "completed_after_revocation") && exit_code == 0) {
            if (detail.isObject() && detail.hasObjectMember("generation") &&
                detail["generation"].isInt())
                generation = static_cast<std::uint64_t>(detail["generation"].getInt64());
            return GraphReloadOutcome::Published;
        }
        if (state == "failed" && typed && detail["code"].getString() == "stale-base")
            return GraphReloadOutcome::StaleGeneration;
        if (state == "failed" && typed)
            return GraphReloadOutcome::Refused;
        if (state == "failed" || state == "cancelled")
            return GraphReloadOutcome::NotApplied;
        return GraphReloadOutcome::Unknown;
    } catch (...) {
        return GraphReloadOutcome::Unknown;
    }
}

namespace {
std::string member(const choc::value::ValueView& object, const char* name) {
    if (!object.hasObjectMember(name) || !object[name].isString())
        return {};
    return std::string(object[name].getString());
}
} // namespace

std::string graph_reload_refusal_key(const std::string& output) {
    try {
        const auto response = choc::json::parse(output);
        if (response.isObject())
            return member(response, "error") + "|" + member(response, "explanation") + "|" +
                   member(response, "state");
    } catch (...) {
    }
    return output;
}

bool graph_reload_authority_refused(const std::string& output) {
    try {
        const auto response = choc::json::parse(output);
        return response.isObject() && !response.hasObjectMember("state") &&
               member(response, "error") == "admission-denied";
    } catch (...) {
        return false;
    }
}
