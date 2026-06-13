#pragma once

/// @file audio_scope_json.hpp
/// Versioned JSON serializer for Audio Scope acquisition artifacts.

#include <pulp/audio/audio_scope.hpp>

#include <string>

namespace pulp::audio {

inline constexpr const char* kAudioScopeJsonSchema = "pulp.audio.scope.v1";
inline constexpr int kAudioScopeJsonVersion = 1;

/// Serialize a scope result to the stable v1 JSON shape. Unavailable
/// measurements are serialized as explicit JSON null values.
std::string audio_scope_result_to_json(const AudioScopeResult& result,
                                       bool pretty = true);

}  // namespace pulp::audio
