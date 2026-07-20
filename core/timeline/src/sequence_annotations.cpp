#include <pulp/timeline/sequence_annotations.hpp>

namespace pulp::timeline {

runtime::Result<MarkerTypeId, MarkerTypeIdError> MarkerTypeId::create(std::string value) {
    if (value.empty() || value.size() > 128)
        return runtime::Err(MarkerTypeIdError::InvalidValue);
    bool segment_start = true;
    bool saw_dot = false;
    for (const auto raw : value) {
        const auto character = static_cast<unsigned char>(raw);
        if (character == '.') {
            if (segment_start)
                return runtime::Err(MarkerTypeIdError::InvalidValue);
            segment_start = true;
            saw_dot = true;
            continue;
        }
        const bool lower = character >= 'a' && character <= 'z';
        const bool digit = character >= '0' && character <= '9';
        if ((segment_start && !lower) || (!segment_start && !lower && !digit && character != '_'))
            return runtime::Err(MarkerTypeIdError::InvalidValue);
        segment_start = false;
    }
    if (!saw_dot || segment_start)
        return runtime::Err(MarkerTypeIdError::InvalidValue);
    return runtime::Ok(MarkerTypeId(std::move(value)));
}

MarkerTypeId MarkerTypeId::cue() {
    return MarkerTypeId("pulp.marker.cue");
}

} // namespace pulp::timeline
