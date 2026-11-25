#pragma once

#include <vellum/view/design_frame_view.hpp>

namespace pulp::vellum_audio {

/// Waveform Recorder catalog component. Renders the faithful vector export of
/// the design 1:1 through the design-frame view; reskin it through the
/// design import lane, never by hand-painting.
class WaveformRecorderView : public ::vellum::view::DesignFrameView {
  public:
    WaveformRecorderView();
};

} // namespace pulp::vellum_audio
