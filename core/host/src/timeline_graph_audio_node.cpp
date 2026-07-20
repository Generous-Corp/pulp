#include "timeline_graph_audio_node.hpp"

#include <utility>

namespace pulp::host::detail {
namespace {

struct AudioNodeInstance {
    AudioNodeInstance(std::shared_ptr<TimelineGraphSharedBlockState> shared,
                      std::shared_ptr<TimelinePdcAudioTransportSlot> transport_slot,
                      std::shared_ptr<playback::ArrangementAudioTrackRenderer> renderer,
                      playback::AudioRendererLimits limits) noexcept
        : shared(std::move(shared)), transport_slot(std::move(transport_slot)),
          renderer(std::move(renderer)), limits(limits) {}

    void process(audio::BufferView<float>& output) noexcept {
        const auto* block = shared->block.load(std::memory_order_acquire);
        const auto* transport = transport_slot->transport.load(std::memory_order_acquire);
        if (block == nullptr || transport == nullptr) {
            output.clear();
            shared->audio_code.store(TimelineGraphProcessCode::MissingProgram,
                                     std::memory_order_relaxed);
            return;
        }
        const auto status = renderer->process(*block, *transport, output, limits);
        if (status != playback::AudioRenderStatus::Rendered &&
            status != playback::AudioRenderStatus::Silent) {
            shared->audio_code.store(TimelineGraphProcessCode::AudioRenderFailed,
                                     std::memory_order_relaxed);
        }
    }

    std::shared_ptr<TimelineGraphSharedBlockState> shared;
    std::shared_ptr<TimelinePdcAudioTransportSlot> transport_slot;
    std::shared_ptr<playback::ArrangementAudioTrackRenderer> renderer;
    playback::AudioRendererLimits limits;
};

} // namespace

CustomNodeType make_timeline_graph_audio_node_type(
    std::string type_id, std::string name, int output_ports,
    const std::shared_ptr<TimelineGraphSharedBlockState>& shared,
    const std::shared_ptr<TimelinePdcAudioTransportSlot>& transport_slot,
    const std::shared_ptr<playback::ArrangementAudioTrackRenderer>& renderer,
    playback::AudioRendererLimits limits) {
    CustomNodeType type;
    type.type_id = std::move(type_id);
    type.version = 1;
    type.num_output_ports = output_ports;
    type.default_name = std::move(name);
    std::weak_ptr<TimelineGraphSharedBlockState> weak_shared = shared;
    std::weak_ptr<TimelinePdcAudioTransportSlot> weak_transport_slot = transport_slot;
    std::weak_ptr<playback::ArrangementAudioTrackRenderer> weak_renderer = renderer;
    type.create = [weak_shared, weak_transport_slot, weak_renderer, limits]() -> void* {
        auto locked_shared = weak_shared.lock();
        auto locked_transport_slot = weak_transport_slot.lock();
        auto locked_renderer = weak_renderer.lock();
        if (!locked_shared || !locked_transport_slot || !locked_renderer)
            return nullptr;
        return new AudioNodeInstance(
            std::move(locked_shared), std::move(locked_transport_slot),
            std::move(locked_renderer), limits);
    };
    type.destroy = [](void* value) { delete static_cast<AudioNodeInstance*>(value); };
    type.process_instance = [](void* value, audio::BufferView<float>& output,
                               const audio::BufferView<const float>&, int) {
        static_cast<AudioNodeInstance*>(value)->process(output);
    };
    type.process_instance_transport = [](
        void* value, audio::BufferView<float>& output,
        const audio::BufferView<const float>&, int,
        const format::ProcessContext&) {
        static_cast<AudioNodeInstance*>(value)->process(output);
    };
    return type;
}

} // namespace pulp::host::detail
