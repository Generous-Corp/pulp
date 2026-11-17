#pragma once

// Authored subgraph persistence in the saved-identity document (v7 written,
// v6 still restored).
//
// A SubgraphInstance expands into ordinary graph nodes, so the flattened
// graph already round-trips through GraphSerializer. What a plain document
// loses is the instance itself: which nodes belong to which template
// instance, its identity and index, its nested child identities, and its
// declared parameter values. These functions record that binding beside the
// flattened graph and rebuild live SubgraphInstance values on restore.
//
// A record never contains a runtime NodeId. Members are named by the
// logical identities in the envelope (authored IDs in v7, structural
// identities in v6), in expansion order. On restore the
// caller supplies the templates; each record is checked against a fresh
// expansion of its template (member count, node type and name, port kinds,
// exact Custom type and version, exact Processor graph type, nested child
// identities, every semantic field of each internal edge, declared parameter
// set and bounds, and Custom member state the registered type accepts), and
// any mismatch refuses the whole document.
// The whole document is first restored and checked on a candidate graph that
// carries the caller's custom node types, and only then loaded into the
// caller's graph. Template expansions run on scratch graphs too, so Processor
// factories, restore registries and Custom create/load_state/destroy probes
// run several times during one restore, all on the calling (control) thread.
// Records carry authored membership and identity only: no runtime NodeId,
// prepared instance, voice generation or pending event is persisted.

#include <pulp/host/graph_serializer.hpp>
#include <pulp/host/signal_graph_subgraph.hpp>

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace pulp::host {

// ── Fixed-count voice templates ─────────────────────────────────────────────
//
// A voice declaration names one template expanded as N identical voices. The
// declaration is the authored identity: voice k is an ordinary subgraph
// instance whose instance identity is subgraph_voice_identity(identity, k)
// and whose instance index is k, so every voice identity is derived from the
// declaration and never chosen by the loader. Only the declaration and each
// voice's declared parameter values persist. Voice allocation, sounding
// voices, note tokens, epochs, pedal holds and release tails are runtime
// state and are never written. A graph holds one event connection, so a
// template that declares one (itself or in a child) cannot be expanded as
// voices; voices carry audio and parameters only.

/// Upper bound on voice_count. A declaration must have 1..kMaxSubgraphVoices.
inline constexpr std::uint32_t kMaxSubgraphVoices = 16;

struct SubgraphVoiceDeclaration {
    // Non-empty, at most 128 bytes, no control characters.
    std::string identity;
    std::string template_identity;
    std::uint32_t voice_count = 0;
};

/// The instance identity of voice `voice` of a declaration:
/// "<declaration>.voice.<voice>". Distinct (declaration, voice) pairs always
/// give distinct identities.
std::string subgraph_voice_identity(std::string_view declaration, std::uint32_t voice);

struct SubgraphVoiceBank {
    SubgraphVoiceDeclaration declaration;
    // Voice k at index k: instance identity subgraph_voice_identity(
    // declaration.identity, k) and instance index k.
    std::vector<SubgraphInstance> voices;
};

struct SubgraphVoiceExpansion {
    GraphResult result;
    SubgraphVoiceBank bank;
    explicit operator bool() const noexcept {
        return static_cast<bool>(result);
    }
};

/// Expand `definition` as `voice_count` voices. The declaration (identity
/// shape, count within 1..kMaxSubgraphVoices, a template without an event
/// connection) is checked before the graph is touched. Any later failure
/// removes every node this call created, so a refused expansion holds no
/// NodeIds. Diagnostics name the subgraph boundary.
SubgraphVoiceExpansion instantiate_subgraph_voices(SignalGraph& graph,
                                                   const SubgraphTemplate& definition,
                                                   std::string identity, std::uint32_t voice_count);

/// Write a saved-identity v7 document that also records each instance.
/// Parameter values are read from the live graph (the target Gain node's
/// gain or the target Processor parameter), not from the instance's cached
/// `parameters`. Returns an empty string when an instance is not part of
/// `graph` (a stale or foreign NodeId, or an exposed port whose owner or
/// incarnation no longer matches the live node), two instances share an
/// identity or a node, a live parameter value is non-finite or outside its
/// declared bounds, a Custom member is unresolved or holds state its
/// registered type refuses, or the graph itself cannot be serialized.
std::string to_saved_subgraph_document(
    const SignalGraph& graph, std::span<const SubgraphInstance> subgraphs,
    const std::unordered_map<NodeId, std::pair<float, float>>& editor_layout = {});

/// As above, and also record each voice bank: every voice as a subgraph
/// record and the bank as a `voice_declarations` entry (identity, template,
/// count, and the derived voice identities). Returns an empty string, in
/// addition to the cases above, when a bank's declaration is malformed, its
/// count is outside 1..kMaxSubgraphVoices or differs from its voices, a voice
/// does not carry its derived identity, index and the declared template, a
/// voice holds an event connection, two banks share an identity, or a voice
/// shares an identity or a node with any other instance.
std::string to_saved_subgraph_document(
    const SignalGraph& graph, std::span<const SubgraphInstance> subgraphs,
    std::span<const SubgraphVoiceBank> voice_banks,
    const std::unordered_map<NodeId, std::pair<float, float>>& editor_layout = {});

struct SubgraphDocumentLoad {
    GraphSerializer::LoadResult load;
    // Restored instances, ordered by instance identity, bound to the live
    // NodeIds of `graph`. Empty whenever load.ok is false.
    std::vector<SubgraphInstance> subgraphs;
    // Restored voice banks, ordered by declaration identity, each with its
    // voices in voice order. Voices appear here and not in `subgraphs`.
    // Empty whenever load.ok is false.
    std::vector<SubgraphVoiceBank> voice_banks;

    bool ok() const noexcept {
        return load.ok;
    }
};

/// Restore a saved-identity v6 or v7 document and rebind its subgraph records to
/// `templates`. Template lookup, document inspection, record shape, the
/// flattened load and conformance to each template are all checked on a
/// candidate graph first, and those refusals leave `graph` unchanged. Only a
/// candidate that passes is loaded into `graph`; should that second load or
/// rebind still fail (a factory or restore callback that behaves differently
/// the second time), the refusal clears `graph` and empties every map and
/// list `load` returned, as any refused LoadResult. Every refusal names its
/// rule in load.error (`subgraph_template_unknown`,
/// `subgraph_template_duplicate`, `subgraph_instance_mismatch`,
/// `subgraph_state_rejected`, or a document inspection or load refusal).
/// Voice declarations are checked by inspection (count bound, derived and
/// unique voice identities, each voice record present with the declared
/// template and its index) and their template is looked up like any record's;
/// a template that declares an event connection is refused as
/// `subgraph_instance_mismatch`. A v6 document cannot carry declarations.
SubgraphDocumentLoad from_saved_subgraph_document(SignalGraph& graph, const std::string& json,
                                                  std::span<const SubgraphTemplate> templates,
                                                  const GraphLoadOptions& options = {});

} // namespace pulp::host
