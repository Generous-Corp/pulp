#pragma once

// Minimal 2D GPU AUv3 proof.
//
// A near-silent effect plug-in whose only job is to install a custom
// view that draws ONE animated 2D primitive through the Skia/Dawn path.
// The view sets `requires_gpu_host(true)` so the format adapter's
// `decide_gpu_host()` routes through the Metal/Dawn PluginViewHost; on
// iOS that exercises `core/render/src/gpu_surface_dawn.cpp`'s
// CAMetalLayer surface creation.
//
// Hard fail expectations on success:
//   - log line: `AU iOS ... gpu=1`
//   - log line: `GpuSurface: created Metal surface from CAMetalLayer`
//   - log line: `GpuSurface: Dawn initialized`
//   - log line: `GpuSurface: backend_type=Metal`
//   - visible animated quad rotating at the center of the editor.
//
// Use this example to verify that an AUv3 editor can reach the iOS
// Metal/Dawn path and present visible GPU output inside the host app.

#include <pulp/format/processor.hpp>

namespace pulp::examples::ios_gpu_smoke {

class GpuSmoke : public pulp::format::Processor {
public:
    pulp::format::PluginDescriptor descriptor() const override;
    void define_parameters(pulp::state::StateStore& store) override;
    void prepare(const pulp::format::PrepareContext& ctx) override;
    void process(pulp::audio::BufferView<float>& out,
                 const pulp::audio::BufferView<const float>& in,
                 pulp::midi::MidiBuffer& midi_in,
                 pulp::midi::MidiBuffer& midi_out,
                 const pulp::format::ProcessContext& ctx) override;

    std::pair<uint32_t, uint32_t> editor_size() const override { return {320, 320}; }
    std::unique_ptr<pulp::view::View> create_view() override;
};

} // namespace pulp::examples::ios_gpu_smoke
