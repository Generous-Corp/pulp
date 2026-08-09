#pragma once

#include <choc/text/choc_JSON.h>

#include <string_view>

namespace pulp::cli::control {

inline choc::value::Value status_permission_terms() {
    auto terms = choc::value::createObject("");
    for (const auto term :
         {"implemented", "built", "host_available", "activated", "session_live"})
        terms.addMember(term, choc::value::createString("not_evaluated"));
    terms.addMember("policy_eligible", choc::value::createString("evaluated_at_call_time"));
    terms.addMember("client_granted", choc::value::createString("grant_required"));
    return terms;
}

inline constexpr std::string_view status_authority_explanation =
    "Authority: registration observed; operation-dependent terms were not evaluated; policy "
    "is evaluated per call and a matching grant is required.";

}  // namespace pulp::cli::control
