#pragma once

// approach after SimZhou/vscode-audiolens c5c5a24; re-authored

/// @file audio_file_layout_json.hpp
/// Versioned JSON serialization of an AudioFileLayoutReport.

#include <pulp/audio/audio_file_layout.hpp>

#include <string>

namespace pulp::audio {

inline constexpr const char* kAudioFileInspectJsonSchema = "pulp.audio.file_inspect.v1";

struct LayoutJsonOptions {
    /// Value of `input.name`: the caller decides between a basename and a
    /// full path, so the serializer never sees a policy decision.
    std::string input_name;
    /// Emit `metadata.text`. Absent (not empty) when false, so a consumer
    /// can tell "nothing lifted" from "not requested".
    bool include_metadata_text = false;
    bool pretty = true;
};

[[nodiscard]] std::string audio_file_layout_to_json(const AudioFileLayoutReport& report,
                                                    const LayoutJsonOptions& options);

} // namespace pulp::audio
