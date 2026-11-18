#pragma once

#include <pulp/host/sample_region_proof.hpp>
#include <pulp/host/signal_graph.hpp>

#include <span>
#include <string>

namespace pulp::host::detail {

// Checks each persisted region's authored limits, member kernel contracts,
// boundary indexes and promoted-parameter bindings against the restored
// graph. On refusal writes the reason to `error` and returns false.
bool validate_persisted_region_metadata(const SignalGraph& graph,
                                        std::span<const SampleRegionDefinition> definitions,
                                        std::string& error);

// Folds one region definition into the parser shape the load proof bounds.
void accumulate_parser_region_shape(const SignalGraph& graph,
                                    const SampleRegionDefinition& definition,
                                    SampleRegionParserShape& shape);

// Checks persisted connections against each region: boundary-only crossings,
// one producer per internal input, reachability, no instantaneous cycle and
// the known work/state budgets. On refusal writes the reason to `error`.
bool validate_persisted_region_topology(const SignalGraph& graph,
                                        std::span<const SampleRegionDefinition> definitions,
                                        std::span<const Connection> connections,
                                        std::string& error);

} // namespace pulp::host::detail
