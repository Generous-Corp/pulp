#include <pulp/inspect/control_offline_render_executor.hpp>

#include <pulp/audio/audio_file.hpp>

#include <choc/text/choc_JSON.h>

#include <cmath>
#include <limits>
#include <sstream>
#include <string>
#include <utility>

namespace pulp::inspect {
namespace {

ControlExecutionOutcome failure(ControlResultCode code, std::string explanation) {
    return {
        .terminal_state = ControlReceiptState::Failed,
        .result = {.result_code = code,
                   .retry = ControlRetryClassification::Never,
                   .explanation = std::move(explanation)},
    };
}

ControlExecutionOutcome checkpoint_failure(ControlExecutionCheckpoint checkpoint) {
    if (checkpoint == ControlExecutionCheckpoint::Cancelled ||
        checkpoint == ControlExecutionCheckpoint::AuthorityRevoked) {
        return {
            .terminal_state = ControlReceiptState::Cancelled,
            .result = {.result_code = ControlResultCode::Cancelled,
                       .explanation = checkpoint == ControlExecutionCheckpoint::Cancelled
                                          ? "offline render cancelled"
                                          : "offline render authority revoked",
                       .cancellation_reason =
                           checkpoint == ControlExecutionCheckpoint::Cancelled
                               ? "client-cancelled"
                               : "authority-revoked"},
        };
    }
    return failure(ControlResultCode::DeadlineExceeded, "offline render deadline exceeded");
}

audio::AudioFileData as_audio_file(const format::OfflineRenderResult& rendered,
                                   std::uint32_t sample_rate) {
    audio::AudioFileData file;
    file.sample_rate = sample_rate;
    file.channels.resize(rendered.audio.num_channels());
    for (std::size_t channel = 0; channel < rendered.audio.num_channels(); ++channel) {
        const auto samples = rendered.audio.channel(channel);
        file.channels[channel].assign(samples.begin(), samples.end());
    }
    return file;
}

} // namespace

ControlOperationExecutor
make_control_offline_render_executor(format::ProcessorFactory factory,
                                     ControlOfflineRenderSourceResolver resolve_source,
                                     ControlOfflineRenderClock clock) {
    if (!clock)
        clock = [] { return std::chrono::steady_clock::now(); };
    return [factory, resolve_source = std::move(resolve_source), clock = std::move(clock)](
               const ControlAdmissionPlan& plan, const ControlRequestEnvelope& request,
               const ControlExecutionContext& context) -> ControlExecutionOutcome {
        if (request.operation_id != "dev.pulp.render/offline@1" ||
            request.operation_version != 1 || !factory || !resolve_source ||
            !context.checkpoint || !context.publish_artifact) {
            return failure(ControlResultCode::InvalidRequest,
                           "offline render executor is unavailable for this operation");
        }

        std::string input_artifact_id;
        std::uint64_t max_frames = 0;
        std::int64_t timeout_ms = 0;
        try {
            const auto params = choc::json::parse(request.params_json);
            if (!params.isObject() || !params["input_artifact_id"].isString() ||
                !params["max_frames"].isInt() || !params["timeout_ms"].isInt()) {
                return failure(ControlResultCode::InvalidRequest,
                               "offline render request was not canonical");
            }
            input_artifact_id = params["input_artifact_id"].getString();
            const auto frames = params["max_frames"].getInt64();
            if (frames <= 0)
                return failure(ControlResultCode::InvalidRequest,
                               "offline render max_frames must be positive");
            max_frames = static_cast<std::uint64_t>(frames);
            timeout_ms = params["timeout_ms"].getInt64();
            if (timeout_ms <= 0 || timeout_ms > 300'000)
                return failure(ControlResultCode::InvalidRequest,
                               "offline render timeout_ms is outside its schema bounds");
        } catch (...) {
            return failure(ControlResultCode::InvalidRequest,
                           "offline render request could not be decoded");
        }

        const auto request_deadline = clock() + std::chrono::milliseconds{timeout_ms};
        const auto runtime_checkpoint = [&] {
            const auto broker_checkpoint = context.checkpoint();
            if (broker_checkpoint != ControlExecutionCheckpoint::Continue)
                return broker_checkpoint;
            return clock() >= request_deadline ? ControlExecutionCheckpoint::DeadlineExceeded
                                               : ControlExecutionCheckpoint::Continue;
        };

        const auto initial = runtime_checkpoint();
        if (initial != ControlExecutionCheckpoint::Continue)
            return checkpoint_failure(initial);

        auto source = resolve_source(plan, input_artifact_id);
        if (!source || source->frame_count == 0 || source->frame_count > max_frames)
            return failure(ControlResultCode::InvalidRequest,
                           "offline render input artifact is unavailable or exceeds max_frames");
        if (!std::isfinite(source->config.sample_rate) ||
            source->config.sample_rate < 1.0 ||
            source->config.sample_rate >
                static_cast<double>(std::numeric_limits<std::uint32_t>::max()) ||
            std::trunc(source->config.sample_rate) != source->config.sample_rate) {
            return failure(ControlResultCode::InvalidRequest,
                           "offline render sample_rate cannot be represented by WAV");
        }
        constexpr std::size_t kWavContainerBudget = 4096;
        const auto output_channels = static_cast<std::size_t>(source->config.output_channels);
        if (context.maximum_artifact_bytes <= kWavContainerBudget || output_channels == 0 ||
            source->frame_count >
                (context.maximum_artifact_bytes - kWavContainerBudget) /
                    (output_channels * sizeof(float))) {
            return failure(ControlResultCode::ResourceExhausted,
                           "offline render output exceeds the broker artifact capacity");
        }

        format::OfflineRenderHost host(factory);
        if (!host.prepare(source->config))
            return failure(ControlResultCode::InternalError,
                           "offline render processor could not be prepared");

        const auto input_view = source->input.num_channels() == 0 ||
                                        source->input.num_samples() == 0
                                    ? audio::BufferView<const float>{}
                                    : std::as_const(source->input).view();
        const format::OfflineRenderOptions options{
            .frame_count = source->frame_count,
            .block_frames = source->block_frames,
            .input = input_view,
            .midi_events = source->midi_events,
            .parameter_events = source->parameter_events,
        };
        const auto rendered = host.render(options, [&runtime_checkpoint] {
            return runtime_checkpoint() == ControlExecutionCheckpoint::Continue;
        });
        if (!rendered.ok) {
            const auto interrupted = runtime_checkpoint();
            if (interrupted != ControlExecutionCheckpoint::Continue)
                return checkpoint_failure(interrupted);
            return failure(ControlResultCode::InternalError, "offline render failed");
        }

        const auto after_render = runtime_checkpoint();
        if (after_render != ControlExecutionCheckpoint::Continue)
            return checkpoint_failure(after_render);

        auto audio_file = as_audio_file(rendered,
                                        static_cast<std::uint32_t>(source->config.sample_rate));
        std::ostringstream encoded(std::ios::binary | std::ios::out);
        if (!audio::write_wav_stream(encoded, audio_file, audio::WavBitDepth::Float32))
            return failure(ControlResultCode::InternalError,
                           "offline render artifact encoding failed");
        const auto wav = std::move(encoded).str();
        const auto before_publish = runtime_checkpoint();
        if (before_publish != ControlExecutionCheckpoint::Continue)
            return checkpoint_failure(before_publish);
        const auto stored = context.publish_artifact(
            {reinterpret_cast<const std::uint8_t*>(wav.data()), wav.size()},
            {.content_type = "audio/wav",
             .sensitivity = ControlArtifactSensitivity::Sensitive,
             .lifetime = std::chrono::hours{1}});
        if (stored.status != ControlArtifactStatus::Stored || !stored.metadata) {
            if (stored.status == ControlArtifactStatus::Unauthorized) {
                const auto interrupted = runtime_checkpoint();
                if (interrupted != ControlExecutionCheckpoint::Continue)
                    return checkpoint_failure(interrupted);
            }
            return failure(stored.status == ControlArtifactStatus::ResourceExhausted
                               ? ControlResultCode::ResourceExhausted
                               : ControlResultCode::InternalError,
                           "offline render artifact publication failed");
        }

        const auto& metadata = *stored.metadata;
        auto detail = choc::value::createObject("OfflineRenderResult");
        detail.setMember("artifact_id", metadata.artifact_id);
        detail.setMember("mime_type", metadata.content_type);
        detail.setMember("sha256", metadata.sha256);
        detail.setMember("byte_count", static_cast<std::int64_t>(metadata.byte_size));
        detail.setMember("frames", static_cast<std::int64_t>(rendered.stats.frames_rendered));
        detail.setMember("blocks", static_cast<std::int64_t>(rendered.stats.blocks_rendered));
        detail.setMember("sample_rate", static_cast<std::int64_t>(audio_file.sample_rate));
        detail.setMember("channels", static_cast<std::int64_t>(audio_file.num_channels()));
        detail.setMember("midi_events",
                         static_cast<std::int64_t>(rendered.stats.midi_events_dispatched));
        detail.setMember("parameter_events",
                         static_cast<std::int64_t>(rendered.stats.parameter_events_dispatched));
        detail.setMember("plugin_id", host.headless().descriptor().bundle_id);
        detail.setMember("parameter_count",
                         static_cast<std::int64_t>(host.headless().state().all_params().size()));
        return {
            .terminal_state = ControlReceiptState::Completed,
            .result = {.detail_json = choc::json::toString(detail, true),
                       .artifacts = {{.artifact_id = metadata.artifact_id,
                                      .media_type = metadata.content_type,
                                      .byte_size = metadata.byte_size}}},
        };
    };
}

} // namespace pulp::inspect
