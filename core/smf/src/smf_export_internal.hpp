#pragma once

#include <pulp/timeline/smf.hpp>

namespace pulp::timeline::detail {

struct SmfExportLossPolicy {
    bool drop_media_clips = false;
    bool drop_registered_content = false;
    bool drop_opaque_content = false;
    bool drop_nested_sequences = false;
    bool drop_absolute_clips = false;
    bool strip_note_modifiers = false;
    bool drop_midi_expression_lanes = false;
    bool quantize_note_velocity = false;
    bool drop_non_root_sequences = false;
    bool step_tempo_ramps = false;
};

runtime::Result<SmfExport, SmfError>
export_smf_with_policy(const Project& project, const SmfExportOptions& options,
                       const SmfExportLossPolicy& loss_policy);

} // namespace pulp::timeline::detail
