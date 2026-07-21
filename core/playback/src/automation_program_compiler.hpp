#pragma once

#include <pulp/playback/automation_program.hpp>
#include <pulp/runtime/result.hpp>

#include <memory>
#include <vector>

namespace pulp::playback::detail {

enum class AutomationProgramCompileStatus { Pending, Complete };

class AutomationProgramCompiler {
  public:
    void reset(const timeline::AutomationLane& lane,
               std::shared_ptr<const timebase::CompiledTempoMap> tempo_map,
               ProgramGeneration generation);
    runtime::Result<AutomationProgramCompileStatus, AutomationProgramError> step();
    std::shared_ptr<const AutomationProgram> take_result() noexcept;

  private:
    struct CompiledKnot {
        timebase::TickPosition tick;
        timebase::SamplePosition sample;
        float value = 0.0f;
        timeline::AutomationInterpolation interpolation =
            timeline::AutomationInterpolation::Continuous;
        float curvature = 0.0f;
    };

    enum class Stage { Validate, Knots, Segments, Finalize, Complete };

    static AutomationProgramSegment make_segment(const CompiledKnot& start,
                                                  const CompiledKnot& end) noexcept;

    const timeline::AutomationLane* lane_ = nullptr;
    std::shared_ptr<const timebase::CompiledTempoMap> tempo_map_;
    ProgramGeneration generation_ = 0;
    Stage stage_ = Stage::Complete;
    std::size_t index_ = 0;
    timeline::DeviceParameterTarget target_;
    std::vector<CompiledKnot> knots_;
    std::vector<AutomationProgramSegment> segments_;
    std::shared_ptr<const AutomationProgram> result_;
};

} // namespace pulp::playback::detail
