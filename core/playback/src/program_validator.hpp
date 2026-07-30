#pragma once

#include <pulp/playback/program_compiler.hpp>

#include <cstdint>
#include <memory>
#include <span>

namespace pulp::playback::detail {

enum class ProgramValidationStatus : std::uint8_t { Progress, Complete, Failed };

struct ProgramValidationStep {
    ProgramValidationStatus status = ProgramValidationStatus::Progress;
    CompileError error;
};

/// Incremental structural validator for a linked program candidate. Each step
/// inspects at most one clip/event or advances one track, preserving compiler
/// slice budgeting without embedding validation substates in the compiler.
class ProgramValidator {
  public:
    ProgramValidationStep step(std::span<const std::shared_ptr<const TrackProgram>> tracks,
                               timeline::ItemId project_id,
                               std::uint64_t document_revision,
                               std::uint64_t generation) noexcept;

  private:
    std::size_t track_ = 0;
    std::size_t clip_ = 0;
    std::size_t audio_clip_ = 0;
    std::size_t note_ = 0;
};

} // namespace pulp::playback::detail
