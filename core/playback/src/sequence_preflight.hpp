#pragma once

#include <pulp/playback/program_compiler.hpp>

#include <cstdint>
#include <vector>

namespace pulp::playback {

runtime::Result<std::vector<timeline::ItemId>, CompileError>
collect_reachable_asset_ids(const timeline::Project& project, const timeline::Sequence& root,
                            const timebase::CompiledTempoMap& tempo_map,
                            std::uint64_t max_expanded_note_events,
                            std::uint64_t max_expanded_clips);

} // namespace pulp::playback
