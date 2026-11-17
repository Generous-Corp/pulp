#pragma once

#include <pulp/host/graph_patch.hpp>
#include <pulp/host/spectral_graph.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// Constructor arguments of notation `# flow 2`: `name : type (key value, ...)`.
// Flow 1 never reaches this module. Every function reports through a
// GraphPatchDiagnostic whose code is Ok on success; the caller adds the line.
namespace pulp::host::patch_constructors {

struct Arguments {
    bool present = false;
    std::vector<std::pair<std::string, std::string>> items;
};

/// Splits a declaration's type text into the type spelling and an optional
/// trailing parenthesized argument list. Parentheses inside quotes are text.
GraphPatchDiagnostic split_arguments(std::string_view type_text, std::string& type,
                                     Arguments& arguments);

/// One document shares one sample rate: the first spectral node fixes it.
struct DocumentRate {
    std::optional<std::uint64_t> bits;
    std::string node;
};

/// True when `key` is a type whose construction flow 2 owns: the audio
/// boundaries (channel count) and the spectral constructors.
bool owns(std::string_view key);

/// Constructs a flow 2 node from its arguments. A boundary without arguments
/// is stereo, as in flow 1; a spectral node requires every identity argument.
GraphPatchDiagnostic construct(SignalGraph& graph, std::string_view key, const Arguments& arguments,
                               const std::string& name, DocumentRate& rate, NodeId& id);

/// Spells a node flow 1 cannot express, as `type (arguments)`. Sets
/// `spelled` empty when flow 1's spelling applies unchanged.
GraphPatchDiagnostic spell(const GraphNode& node, DocumentRate& rate, std::string& spelled);

} // namespace pulp::host::patch_constructors
