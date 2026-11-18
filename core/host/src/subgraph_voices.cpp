#include <pulp/host/signal_graph.hpp>
#include <pulp/host/subgraph_persistence.hpp>

#include "subgraph_voice_records.hpp"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace pulp::host {
namespace detail {

bool voice_declaration_identity_valid(std::string_view identity) noexcept {
    if (identity.empty() || identity.size() > 128)
        return false;
    for (const unsigned char c : identity)
        if (c < 0x20 || c == 0x7f)
            return false;
    return true;
}

namespace {

constexpr std::string_view kDeclarationMembers[] = {"identity", "template_identity", "voice_count",
                                                    "voices"};

bool is_integer(const choc::value::ValueView& value) {
    return value.isInt32() || value.isInt64();
}

bool is_nonempty_string(const choc::value::ValueView& value) {
    return value.isString() && !value.getString().empty();
}

bool template_declares_event(const SubgraphTemplate& definition,
                             std::unordered_set<const SubgraphTemplate*>& active) {
    if (!active.insert(&definition).second)
        return false;
    if (definition.event_edge)
        return true;
    for (const auto& child : definition.children)
        if (child.definition && template_declares_event(*child.definition, active))
            return true;
    active.erase(&definition);
    return false;
}

} // namespace

GraphDocumentRefusal validate_voice_declarations(const choc::value::ValueView& root, bool saved_v7,
                                                 std::string& detail) {
    const auto& declarations = root["voice_declarations"];
    if (!saved_v7) {
        detail = "voice_declarations requires the saved-identity v7 envelope";
        return GraphDocumentRefusal::SubgraphRecordInvalid;
    }
    if (!declarations.isArray()) {
        detail = "voice_declarations must be an array of declarations";
        return GraphDocumentRefusal::SubgraphRecordInvalid;
    }
    // Each voice is an ordinary subgraph record, found by instance identity.
    std::unordered_map<std::string, std::pair<std::string, std::int64_t>> records;
    if (root.hasObjectMember("subgraphs") && root["subgraphs"].isArray()) {
        const auto& subgraphs = root["subgraphs"];
        for (std::uint32_t i = 0; i < subgraphs.size(); ++i) {
            const auto& record = subgraphs[i];
            records.emplace(std::string(record["instance_identity"].getString()),
                            std::pair{std::string(record["template_identity"].getString()),
                                      record["instance_index"].getInt64()});
        }
    }
    std::unordered_set<std::string> identities;
    std::unordered_set<std::string> voices_seen;
    for (std::uint32_t i = 0; i < declarations.size(); ++i) {
        const auto& declaration = declarations[i];
        const auto where = "voice declaration " + std::to_string(i);
        if (!declaration.isObject() || declaration.size() != std::size(kDeclarationMembers)) {
            detail =
                where + " requires exactly identity, template_identity, voice_count and voices";
            return GraphDocumentRefusal::SubgraphRecordInvalid;
        }
        for (const auto name : kDeclarationMembers) {
            if (!declaration.hasObjectMember(name)) {
                detail = where + " is missing " + std::string(name);
                return GraphDocumentRefusal::SubgraphRecordInvalid;
            }
        }
        if (!declaration["identity"].isString() ||
            !voice_declaration_identity_valid(declaration["identity"].getString()) ||
            !is_nonempty_string(declaration["template_identity"])) {
            detail = where + " identity must be 1..128 bytes without control characters and "
                             "template_identity a non-empty string";
            return GraphDocumentRefusal::SubgraphRecordInvalid;
        }
        const auto identity = std::string(declaration["identity"].getString());
        const auto template_identity = std::string(declaration["template_identity"].getString());
        if (!is_integer(declaration["voice_count"]) || declaration["voice_count"].getInt64() < 1 ||
            declaration["voice_count"].getInt64() > kMaxSubgraphVoices) {
            detail = where + " voice_count must be an integer in 1.." +
                     std::to_string(kMaxSubgraphVoices);
            return GraphDocumentRefusal::SubgraphRecordInvalid;
        }
        if (!identities.insert(identity).second) {
            detail = "duplicate voice declaration identity: " + identity;
            return GraphDocumentRefusal::SubgraphBindingDuplicate;
        }
        const auto count = static_cast<std::uint32_t>(declaration["voice_count"].getInt64());
        const auto& voices = declaration["voices"];
        if (!voices.isArray() || voices.size() != count) {
            detail = where + " voices must list exactly voice_count identities";
            return GraphDocumentRefusal::SubgraphRecordInvalid;
        }
        for (std::uint32_t k = 0; k < count; ++k) {
            if (!is_nonempty_string(voices[k])) {
                detail = where + " voice " + std::to_string(k) + " must be an identity string";
                return GraphDocumentRefusal::SubgraphRecordInvalid;
            }
            const auto voice = std::string(voices[k].getString());
            if (!voices_seen.insert(voice).second) {
                detail = "voice identity " + voice + " is claimed twice";
                return GraphDocumentRefusal::SubgraphBindingDuplicate;
            }
            // The identity is derived from the declaration, never chosen.
            if (voice != subgraph_voice_identity(identity, k)) {
                detail = where + " voice " + std::to_string(k) + " is " + voice +
                         ", not the derived identity " + subgraph_voice_identity(identity, k);
                return GraphDocumentRefusal::SubgraphRecordInvalid;
            }
            const auto record = records.find(voice);
            if (record == records.end()) {
                detail = where + " voice " + voice + " has no subgraph record";
                return GraphDocumentRefusal::SubgraphBindingDangling;
            }
            if (record->second.first != template_identity ||
                record->second.second != static_cast<std::int64_t>(k)) {
                detail = "voice " + voice + " record must use template " + template_identity +
                         " at instance index " + std::to_string(k);
                return GraphDocumentRefusal::SubgraphRecordInvalid;
            }
        }
    }
    return GraphDocumentRefusal::None;
}

} // namespace detail

std::string subgraph_voice_identity(std::string_view declaration, std::uint32_t voice) {
    return std::string(declaration) + ".voice." + std::to_string(voice);
}

SubgraphVoiceExpansion instantiate_subgraph_voices(SignalGraph& graph,
                                                   const SubgraphTemplate& definition,
                                                   std::string identity,
                                                   std::uint32_t voice_count) {
    SubgraphVoiceExpansion out;
    const auto refuse = [&](GraphResult::Code code, std::string rule, std::string fix = {}) {
        out.result = {code, 0, 0, "subgraph boundary: " + std::move(rule), std::move(fix)};
        return std::move(out);
    };
    if (!detail::voice_declaration_identity_valid(identity))
        return refuse(GraphResult::Code::UnknownNode,
                      "voice declaration identity must be 1..128 bytes without control "
                      "characters");
    if (voice_count < 1 || voice_count > kMaxSubgraphVoices)
        return refuse(GraphResult::Code::UnknownNode,
                      "voice count must be in 1.." + std::to_string(kMaxSubgraphVoices),
                      "declare between one and kMaxSubgraphVoices voices");
    if (definition.identity.empty())
        return refuse(GraphResult::Code::UnknownNode, "voice template has no identity");
    std::unordered_set<const SubgraphTemplate*> active;
    if (detail::template_declares_event(definition, active))
        return refuse(GraphResult::Code::Duplicate,
                      "a voice template cannot declare an event connection; a graph holds one",
                      "remove the template's event_edge or expand it once as a subgraph");

    out.bank.declaration = {identity, definition.identity, voice_count};
    auto checkpoint = graph.mutation_checkpoint();
    const auto rollback = [&] {
        for (auto voice = out.bank.voices.rbegin(); voice != out.bank.voices.rend(); ++voice)
            for (auto node = voice->nodes.rbegin(); node != voice->nodes.rend(); ++node)
                graph.remove_node(*node);
        out.bank.voices.clear();
    };
    for (std::uint32_t k = 0; k < voice_count; ++k) {
        auto expansion =
            instantiate_subgraph(graph, definition, subgraph_voice_identity(identity, k), k);
        if (!expansion) {
            rollback();
            out.result = std::move(expansion.result);
            return std::move(out);
        }
        out.bank.voices.push_back(std::move(expansion.instance));
    }
    checkpoint.commit();
    return out;
}

} // namespace pulp::host
