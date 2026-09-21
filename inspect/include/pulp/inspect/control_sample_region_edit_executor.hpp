#pragma once
#include <pulp/inspect/control_sample_region_target.hpp>
namespace pulp::inspect {
ControlOperationExecutor
    make_control_sample_region_edit_executor(ControlSampleRegionTargetResolver);
}
