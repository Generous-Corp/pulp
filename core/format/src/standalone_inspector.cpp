#include <pulp/format/detail/standalone_inspector.hpp>

#include "standalone_inspector_capture.hpp"

#include <pulp/events/main_thread_dispatcher.hpp>
#include <pulp/format/processor.hpp>
#include <pulp/format/standalone.hpp>
#include <pulp/format/view_bridge.hpp>
#include <pulp/inspect/agent_context.hpp>
#include <pulp/inspect/audio_inspector.hpp>
#include <pulp/inspect/authentication.hpp>
#include <pulp/inspect/capabilities.hpp>
#include <pulp/inspect/capture_source.hpp>
#include <pulp/inspect/console_capture.hpp>
#include <pulp/inspect/discovery_publisher.hpp>
#include <pulp/inspect/domain_handler.hpp>
#include <pulp/inspect/inspector_overlay.hpp>
#include <pulp/inspect/inspector_server.hpp>
#include <pulp/inspect/main_thread_rpc.hpp>
#include <pulp/inspect/session.hpp>
#include <pulp/inspect/state_inspector.hpp>
#include <pulp/inspect/test_input.hpp>
#include <pulp/inspect/tweak_store.hpp>
#include <pulp/inspect/value_channel_telemetry_broker.hpp>
#include <pulp/runtime/build_info.hpp>
#include <pulp/runtime/crypto.hpp>
#include <pulp/runtime/log.hpp>
#include <pulp/view/scripted_ui.hpp>
#include <pulp/view/value_channel_set.hpp>
#include <pulp/view/window_host.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <utility>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#elif defined(__APPLE__)
#include <mach-o/dyld.h>
#else
#include <unistd.h>
#endif

namespace pulp::format::detail {
namespace {

#if defined(PULP_STANDALONE_INSPECTOR_TEST_HOOKS)
std::mutex rpc_post_hook_mutex;
std::shared_ptr<const StandaloneInspectorRpcPostOverride> rpc_post_override;
std::mutex telemetry_clock_hook_mutex;
std::shared_ptr<const StandaloneInspectorTelemetryClock> telemetry_clock_override;

std::shared_ptr<const StandaloneInspectorRpcPostOverride> current_rpc_post_override() {
    std::lock_guard lock(rpc_post_hook_mutex);
    return rpc_post_override;
}

StandaloneInspectorTelemetryClock current_telemetry_clock_override() {
    std::lock_guard lock(telemetry_clock_hook_mutex);
    if (!telemetry_clock_override)
        return {};
    auto clock = telemetry_clock_override;
    return [clock = std::move(clock)] { return (*clock)(); };
}
#endif

std::optional<std::string> random_id(std::string_view prefix) {
    const auto bytes = runtime::secure_random_bytes(16);
    if (!bytes)
        return std::nullopt;
    return std::string(prefix) + runtime::hex_encode(*bytes);
}

std::filesystem::path executable_path() {
#if defined(_WIN32)
    std::wstring buffer(32768, L'\0');
    const auto length =
        GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (length == 0 || length >= buffer.size())
        return {};
    buffer.resize(length);
    return std::filesystem::path(buffer);
#elif defined(__APPLE__)
    std::uint32_t size = 0;
    _NSGetExecutablePath(nullptr, &size);
    std::string buffer(size, '\0');
    if (_NSGetExecutablePath(buffer.data(), &size) != 0)
        return {};
    buffer.resize(std::char_traits<char>::length(buffer.c_str()));
    std::error_code error;
    auto canonical = std::filesystem::weakly_canonical(buffer, error);
    return error ? std::filesystem::path(buffer) : canonical;
#else
    std::string buffer(4096, '\0');
    const auto length = ::readlink("/proc/self/exe", buffer.data(), buffer.size());
    if (length <= 0)
        return {};
    buffer.resize(static_cast<std::size_t>(length));
    return std::filesystem::path(buffer);
#endif
}

std::int64_t mtime_unix_ms(const std::filesystem::path& path) {
    std::error_code error;
    const auto mtime = std::filesystem::last_write_time(path, error);
    if (error)
        return 0;
    const auto system_time = std::chrono::time_point_cast<std::chrono::milliseconds>(
        mtime - std::filesystem::file_time_type::clock::now() + std::chrono::system_clock::now());
    return system_time.time_since_epoch().count();
}

std::optional<inspect::InspectorProfile> parse_profile(std::string_view profile) {
    if (profile.empty() || profile == "off")
        return inspect::InspectorProfile::Off;
    return inspect::profile_from_id(profile);
}

std::vector<inspect::InspectorCapability>
standalone_capabilities(bool back_buffer_capture, bool runtime_eval_enabled) {
    using C = inspect::InspectorCapability;
    std::vector<inspect::InspectorCapability> result{
        C::SessionDescribe, C::SessionControl, C::StateRead, C::UiRead,
        C::DiagnosticsRead, C::LogsRead, C::StateWrite, C::TestInput,
        C::AuthoringTweaks, C::TelemetryStream};
    if (back_buffer_capture)
        result.push_back(C::CaptureImage);
    if (runtime_eval_enabled)
        result.push_back(C::RuntimeEval);
    return result;
}

bool standalone_capability_available(inspect::InspectorCapability capability,
                                     bool back_buffer_capture,
                                     bool runtime_eval_enabled) {
    const auto available = standalone_capabilities(back_buffer_capture,
                                                   runtime_eval_enabled);
    return std::find(available.begin(), available.end(), capability) != available.end();
}



} // namespace

class StandaloneInspectorRuntime::Impl final : public inspect::InspectorAgentContextSource,
                                               public inspect::InspectorCaptureSource,
                                               public inspect::InspectorTestInputSource {
  public:
    struct IndicatorState {
        bool active = false;
        bool pending = false;
        std::string label;
    };

    Impl(StandaloneApp& app, Processor& processor, ViewBridge& bridge, view::View& root,
         view::WindowHost& window, inspect::InspectorOverlay* overlay,
         inspect::InspectorProfile profile, std::vector<inspect::InspectorCapability> custom,
         bool runtime_eval_enabled, std::string session_id, std::string instance_id)
        : app_(app), processor_(processor), bridge_(bridge), root_(root), window_(window),
          overlay_(overlay), profile_(profile),
          telemetry_enabled_(
              profile == inspect::InspectorProfile::Develop ||
              (profile == inspect::InspectorProfile::Custom &&
               std::find(custom.begin(), custom.end(),
                         inspect::InspectorCapability::TelemetryStream) !=
                   custom.end())),
          session_id_(std::move(session_id)),
          instance_id_(std::move(instance_id)), executable_(executable_path()), state_(app.state()),
          telemetry_(inspect::ValueChannelTelemetryBroker::Config{},
#if defined(PULP_STANDALONE_INSPECTOR_TEST_HOOKS)
                     current_telemetry_clock_override()),
#else
                     {}),
#endif
          rpc_(std::make_shared<inspect::InspectorMainThreadRpc>(
              inspect::InspectorMainThreadRpc::Config{},
              [](std::function<void()> task) {
#if defined(PULP_STANDALONE_INSPECTOR_TEST_HOOKS)
                  if (const auto post_override = current_rpc_post_override()) {
                      if (const auto result = (*post_override)(task))
                          return *result;
                  }
#endif
                  return events::MainThreadDispatcher::call_async(std::move(task));
              },
              [] { return events::MainThreadDispatcher::is_main_thread(); })),
          session_(inspect::InspectorSessionInfo{session_id_, instance_id_,
                                                 processor.descriptor().bundle_id, "1"},
                   make_policy(profile, std::move(custom),
                               standalone_capture_available(root, window),
                               runtime_eval_enabled),
                   [this](const inspect::InspectorRequestContext& context,
                          const inspect::InspectorMessage& request) {
                       refresh_live_state();
                       if (request.method.rfind("Telemetry.", 0) == 0)
                           return telemetry_.handle(context, request);
                       if (request.method.rfind("Runtime.", 0) == 0)
                           return handle_runtime_request(request);
                       return domains_.handle(request);
                   }) {
        session_.set_audit_log(audit_log_);
        telemetry_.set_event_sink(
            [this](std::string_view client_id,
                   const inspect::InspectorMessage& event,
                   std::string_view loss_owner) {
                if (!server_)
                    return inspect::InspectorTargetedEventResult::EventUnavailable;
                return server_->send_to_client(client_id, event, loss_owner);
            });
        telemetry_.set_event_retirement_sink(
            [this](std::string_view client_id, std::string_view loss_owner) {
                if (server_)
                    server_->cancel_client_events(client_id, loss_owner);
            });
        refresh_value_channel_sources(true);
        audio_.set_config(inspect::AudioConfig{app_.config().sample_rate, app_.config().buffer_size,
                                               app_.config().input_channels,
                                               app_.config().output_channels,
                                               std::max(0, processor_.latency_samples())});
        domains_.set_root_view(&root_);
        domains_.set_agent_context_source(this);
        if (standalone_capture_producer_available(window_))
            domains_.set_capture_source(this);
        domains_.set_state_inspector(&state_);
        domains_.set_console_capture(console_.get());
        domains_.set_audio_inspector(&audio_);
        domains_.set_tweak_store(&tweaks_);
        domains_.set_test_input_source(this);
        domains_.set_overlay(overlay_);
        session_.set_controller_scope_end_handler(
            [this](const inspect::InspectorControllerScopeEnd& event) {
                domains_.release_test_input(event.reason);
            });
        session_.set_client_disconnect_handler(
            [this](std::string_view client_id) {
                std::lock_guard lock(pending_disconnects_mutex_);
                pending_disconnects_.emplace_back(client_id);
            });
        if (overlay_)
            overlay_->set_tweak_store(&tweaks_);
    }

    ~Impl() { stop(); }

    bool start(std::vector<std::uint8_t> token) {
        inspect::InspectorDiscoveryRecord record;
        record.session_id = session_id_;
        record.instance_id = instance_id_;
        record.plugin_id = processor_.descriptor().bundle_id;
        record.profile = profile_;
        inspect::InspectorServerConfig config;
        config.session = &session_;
        config.discovery = &publisher_;
        config.record = std::move(record);
        config.token = std::move(token);
        config.authentication_timeout = std::chrono::seconds(3);
        config.frame_read_timeout = std::chrono::seconds(3);
        config.heartbeat_interval = std::chrono::seconds(10);
        config.max_message_bytes = inspect::kInspectorExtendedMessageBytes;
        config.max_clients = 16;
        config.domain_bindings = &domains_;
        config.main_thread_rpc = rpc_;
        if (!server_->start_authenticated(std::move(config))) {
            return false;
        }
        active_ = true;
        indicator_->active = true;
        indicator_->label =
            "INSPECT " + std::string(inspect::profile_id(profile_));
        runtime::log_info(
            "Standalone: Development Inspector active (profile={}, session={}, port={})",
            inspect::profile_id(profile_), session_id_, server_->port());
        return true;
    }

    void stop() {
        begin_stop();
        if (shutdown_fence_.ready())
            detach_borrowed_sources();
    }

    inspect::InspectorServerShutdownFence begin_stop() {
        if (stopped_)
            return shutdown_fence_;
        stopped_ = true;
        active_ = false;
        indicator_->active = false;
        app_.test_input_host().release_test_input();
        session_.set_client_disconnect_handler({});
        if (server_) {
            shutdown_fence_ = server_->shutdown_fence();
            server_->stop();
            server_.reset();
        }
        return shutdown_fence_;
    }

    StandaloneInspectorLifecycleState lifecycle_state() const {
        return {
            .rpc_accepting = rpc_ && rpc_->active(),
            .dispatch_accepting = session_.dispatches_accepting(),
            .borrowed_sources_attached = !sources_detached_,
        };
    }

#if defined(PULP_STANDALONE_INSPECTOR_TEST_HOOKS)
    std::vector<StandaloneInspectorAuditEntry> audit_snapshot_for_testing() const {
        const auto audit = audit_log_->snapshot();
        std::vector<StandaloneInspectorAuditEntry> result;
        result.reserve(audit.size());
        for (const auto& entry : audit) {
            auto outcome = StandaloneInspectorAuditOutcome::Rejected;
            if (entry.outcome == inspect::InspectorAuditOutcome::Denied)
                outcome = StandaloneInspectorAuditOutcome::Denied;
            else if (entry.outcome == inspect::InspectorAuditOutcome::Applied)
                outcome = StandaloneInspectorAuditOutcome::Applied;
            result.push_back({entry.session_id, entry.instance_id, entry.client_id,
                              entry.method, outcome, entry.error_code});
        }
        return result;
    }

    StandaloneInspectorTelemetryState telemetry_state_for_testing() const {
        std::lock_guard lock(pending_disconnects_mutex_);
        return {
            .pending_disconnects = pending_disconnects_.size(),
            .active_subscriptions = telemetry_.subscription_count(),
            .source_generation = telemetry_.source_generation(),
            .source_transition_count = value_channel_source_transitions_,
            .attachment_attempt_count = value_channel_attachment_attempts_,
        };
    }
#endif

    void detach_borrowed_sources() {
        if (sources_detached_)
            return;
        sources_detached_ = true;
        telemetry_.clear_attachment();
        telemetry_.set_event_sink({});
        telemetry_.set_event_retirement_sink({});
        state_.set_value_channels(
            std::span<const view::ValueChannelInfo>{});
        domains_.set_test_input_source(nullptr);
        domains_.set_script_inspector(nullptr);
        log_callback_epoch_->fetch_add(1, std::memory_order_acq_rel);
        bridge_.visit_scripted_ui([this](view::ScriptedUiSession* current) {
            if (current && log_subscription_ != 0 && log_subscription_session_identity_
                && *log_subscription_session_identity_ == current->identity()) {
                current->remove_log_callback(log_subscription_);
            }
        });
        log_subscription_ = 0;
        log_subscription_generation_.reset();
        log_subscription_session_identity_.reset();
        if (overlay_)
            overlay_->set_tweak_store(nullptr);
    }

    inspect::InspectorAgentContext snapshot() const override {
        inspect::InspectorAgentContext result;
        result.binary_path = executable_.string();
        result.binary_mtime_unix_ms = mtime_unix_ms(executable_);
        result.binary_build_id = std::string(runtime::kStampLabel);
        result.plugin_id = processor_.descriptor().bundle_id;
        result.session_id = session_id_;
        result.instance_id = instance_id_;
        result.editor_open = bridge_.view() != nullptr;
        result.window_visible = window_.is_visible();
        result.processing = app_.is_running();
        result.xrun_count = app_.audio_xrun_count();
        bridge_.visit_scripted_ui([&result](const view::ScriptedUiSession* scripted) {
            if (!scripted) return;
            result.hot_reload_available = true;
            result.hot_reload_enabled = scripted->hot_reload_enabled();
            result.hot_reload_pending = scripted->hot_reload_pending();
        });
        result.unsaved_tweak_count = tweaks_.count();
        if (result.binary_path.empty())
            result.actionable_issues.push_back("Executable path is unavailable");
        if (result.xrun_count > 0)
            result.actionable_issues.push_back("Audio device reported " +
                                               std::to_string(result.xrun_count) + " xruns");
        return result;
    }

    inspect::InspectorCapture capture_png() override {
        return capture_standalone_png(root_, window_, processor_);
    }

    inspect::TestInputApplyResult inject_midi(
        const inspect::MidiTestInput& input) override {
        const auto kind = input.kind == inspect::MidiTestInputKind::NoteOn
            ? StandaloneTestMidiKind::NoteOn
            : StandaloneTestMidiKind::NoteOff;
        const auto result = app_.test_input_host().inject_note({
            .kind = kind,
            .channel = static_cast<std::uint8_t>(input.channel + 1),
            .note = input.note,
            .velocity = input.velocity,
        });
        switch (result) {
        case StandaloneTestInputResult::Applied:
            return inspect::TestInputApplyResult::success();
        case StandaloneTestInputResult::QueueFull:
            return inspect::TestInputApplyResult::failure(
                "test_input_queue_full",
                "Standalone test MIDI queue is full");
        case StandaloneTestInputResult::InvalidArgument:
            return inspect::TestInputApplyResult::failure(
                "invalid_params",
                "Standalone rejected invalid MIDI test input");
        }
        return inspect::TestInputApplyResult::failure(
            "test_input_rejected",
            "Standalone rejected MIDI test input");
    }

    inspect::TestInputApplyResult set_transport(
        const inspect::StandaloneTransportTestInput& input) override {
        const auto result = app_.test_input_host().update_transport({
            .playing = input.playing,
            .position_samples = input.position_samples,
            .tempo_bpm = input.tempo_bpm,
        });
        if (result == StandaloneTestInputResult::Applied)
            return inspect::TestInputApplyResult::success();
        return inspect::TestInputApplyResult::failure(
            "invalid_params",
            "Standalone rejected invalid transport test input");
    }

    void release_test_input(
        inspect::TestInputReleaseReason) noexcept override {
        app_.test_input_host().release_test_input();
    }

    void enqueue_indicator() {
        if (!active_ || indicator_->pending)
            return;
        refresh_live_state();
        indicator_->pending = true;
        auto indicator = indicator_;
        root_.interaction().overlay_queue.push_back(
            {[indicator = std::move(indicator)](canvas::Canvas& canvas) {
                 indicator->pending = false;
                 if (!indicator->active)
                     return;
                 canvas.save();
                 canvas.set_fill_color(pulp::canvas::Color::rgba(0.08f, 0.10f, 0.14f, 0.92f));
                 canvas.fill_rect(8.0f, 8.0f, 126.0f, 24.0f);
                 canvas.set_fill_color(pulp::canvas::Color::rgba(0.35f, 0.85f, 1.0f, 1.0f));
                 canvas.set_font("monospace", 11.0f);
                 canvas.fill_text(indicator->label, 15.0f, 24.0f);
                 canvas.restore();
             },
             &root_});
        root_.request_repaint();
    }

    void pump() {
        drain_client_disconnects();
        refresh_value_channel_sources(false);
        if (telemetry_enabled_)
            telemetry_.poll();
        enqueue_indicator();
    }

  private:
    inspect::InspectorMessage handle_runtime_request(
        const inspect::InspectorMessage& request) {
        inspect::InspectorMessage response;
        bool visited = false;
        bridge_.visit_scripted_ui([this, &request, &response, &visited](
                                      view::ScriptedUiSession* scripted) {
            visited = true;
            domains_.set_script_inspector(
                scripted ? scripted->script_inspector() : nullptr);
            struct ResetScriptInspector {
                inspect::DomainHandler& domains;
                ~ResetScriptInspector() { domains.set_script_inspector(nullptr); }
            } reset{domains_};
            response = domains_.handle(request);
        });
        if (!visited) {
            domains_.set_script_inspector(nullptr);
            response = domains_.handle(request);
        }
        return response;
    }

    void drain_client_disconnects() {
        std::vector<std::string> disconnected;
        {
            std::lock_guard lock(pending_disconnects_mutex_);
            disconnected.swap(pending_disconnects_);
        }
        for (const auto& client_id : disconnected)
            telemetry_.disconnect(client_id);
    }

    void refresh_value_channel_sources(bool force) {
        bool attachment_ready = !telemetry_enabled_;
        std::uint64_t source_identity = 0;
        processor_.visit_value_channels([this, &attachment_ready, &source_identity,
                                         force](view::ValueChannelSet* channels) {
            source_identity = channels ? channels->generation_identity() : 0;
            if (!force && value_channel_source_identity_
                && *value_channel_source_identity_ == source_identity) {
                attachment_ready = true;
                return;
            }
            if (!channels) {
                state_.set_value_channels(
                    std::span<const view::ValueChannelInfo>{});
                telemetry_.replace_with_empty_source();
                attachment_ready = true;
                return;
            }
            state_.set_value_channels(channels->infos());
            if (channels->infos().empty()) {
                telemetry_.replace_with_empty_source();
                attachment_ready = true;
                return;
            }
            if (!telemetry_enabled_) {
                telemetry_.clear_attachment();
                return;
            }
            // Release the prior exclusive claim before asking the current set
            // for its attachment. A reload generation may legitimately retain
            // the same ValueChannelSet object; constructing the replacement
            // argument first would then fail against our own old claim.
            const bool first_attempt_for_source =
                !failed_value_channel_source_identity_
                || *failed_value_channel_source_identity_ != source_identity;
            if (first_attempt_for_source) {
                telemetry_.clear_attachment();
                ++value_channel_source_transitions_;
            }
            ++value_channel_attachment_attempts_;
            auto candidate = channels->attach_telemetry();
            attachment_ready = candidate.valid()
                && telemetry_.replace_attachment(std::move(candidate));
            if (!attachment_ready) {
                if (!failed_value_channel_source_identity_ ||
                    *failed_value_channel_source_identity_ != source_identity) {
                    runtime::log_error(
                        "Standalone: could not claim value-channel telemetry reader");
                }
            }
        });
        refresh_scripted_sources();
        if (attachment_ready) {
            value_channel_source_identity_ = source_identity;
            failed_value_channel_source_identity_.reset();
        } else {
            failed_value_channel_source_identity_ = source_identity;
        }
    }

    void refresh_scripted_sources() {
        const auto generation = processor_.supports_editor_reload()
            ? processor_.editor_reload_generation()
            : 0;
        if (log_subscription_generation_
            && *log_subscription_generation_ == generation) {
            return;
        }

        const auto callback_epoch =
            log_callback_epoch_->fetch_add(1, std::memory_order_acq_rel) + 1;
        bridge_.visit_scripted_ui([this, callback_epoch, generation](
                                      view::ScriptedUiSession* current) {
            if (!current) {
                log_subscription_ = 0;
                log_subscription_session_identity_.reset();
                return;
            }

            // A generation may reload the same session in place. Retire its old
            // token before replacing it. A different or temporarily unavailable
            // session is covered by the epoch guard, so any retained observer is
            // inert even when its session outlives this runtime.
            if (log_subscription_ != 0 && log_subscription_session_identity_
                && *log_subscription_session_identity_ == current->identity()) {
                current->remove_log_callback(log_subscription_);
            }

            auto callback = console_->callback();
            std::weak_ptr<inspect::ConsoleCapture> capture = console_;
            auto epoch = log_callback_epoch_;
            log_subscription_ = current->add_log_callback(
                [capture = std::move(capture), epoch = std::move(epoch),
                 callback_epoch, callback = std::move(callback)](
                    std::string_view level, std::string_view message) {
                    // A retained but no-longer-active session may outlive the
                    // inspector runtime. Never let its observer dereference the
                    // destroyed composition root.
                    if (epoch->load(std::memory_order_acquire) == callback_epoch
                        && capture.lock()) {
                        callback(level, message);
                    }
                });
            log_subscription_session_identity_ = current->identity();
            log_subscription_generation_ = generation;
        });
    }

    void refresh_live_state() {
        const auto& config = app_.config();
        audio_.set_config(inspect::AudioConfig{config.sample_rate, config.buffer_size,
                                               config.input_channels, config.output_channels,
                                               std::max(0, processor_.latency_samples())});
        audio_.set_xrun_count(app_.audio_xrun_count());
    }

    static inspect::InspectorPolicyConfig
    make_policy(inspect::InspectorProfile profile,
                std::vector<inspect::InspectorCapability> custom,
                bool back_buffer_capture,
                bool runtime_eval_enabled) {
        inspect::InspectorPolicyConfig policy;
        policy.profile = profile;
        policy.custom_capabilities = std::move(custom);
        policy.available_capabilities = standalone_capabilities(
            back_buffer_capture, runtime_eval_enabled);
        policy.runtime_eval_enabled = runtime_eval_enabled;
        return policy;
    }

    StandaloneApp& app_;
    Processor& processor_;
    ViewBridge& bridge_;
    view::View& root_;
    view::WindowHost& window_;
    inspect::InspectorOverlay* overlay_ = nullptr;
    inspect::InspectorProfile profile_ = inspect::InspectorProfile::Off;
    bool telemetry_enabled_ = false;
    std::string session_id_;
    std::string instance_id_;
    std::filesystem::path executable_;
    inspect::InspectorDiscoveryPublisher publisher_;
    inspect::DomainHandler domains_;
    inspect::StateInspector state_;
    std::shared_ptr<inspect::ConsoleCapture> console_ =
        std::make_shared<inspect::ConsoleCapture>();
    std::shared_ptr<std::atomic<std::uint64_t>> log_callback_epoch_ =
        std::make_shared<std::atomic<std::uint64_t>>(0);
    inspect::AudioInspector audio_;
    inspect::TweakStore tweaks_;
    inspect::ValueChannelTelemetryBroker telemetry_;
    mutable std::mutex pending_disconnects_mutex_;
    std::vector<std::string> pending_disconnects_;
    std::shared_ptr<inspect::InspectorMainThreadRpc> rpc_;
    inspect::InspectorSession session_;
    std::shared_ptr<inspect::InspectorAuditLog> audit_log_ =
        std::make_shared<inspect::InspectorAuditLog>();
    std::unique_ptr<inspect::InspectorServer> server_ =
        std::make_unique<inspect::InspectorServer>();
    inspect::InspectorServerShutdownFence shutdown_fence_;
    std::shared_ptr<IndicatorState> indicator_ = std::make_shared<IndicatorState>();
    std::uint64_t log_subscription_ = 0;
    std::optional<std::uint64_t> log_subscription_generation_;
    std::optional<std::uint64_t> log_subscription_session_identity_;
    std::optional<std::uint64_t> value_channel_source_identity_;
    std::optional<std::uint64_t> failed_value_channel_source_identity_;
    std::uint64_t value_channel_source_transitions_ = 0;
    std::uint64_t value_channel_attachment_attempts_ = 0;
    bool active_ = false;
    bool stopped_ = false;
    bool sources_detached_ = false;
};

#if defined(PULP_STANDALONE_INSPECTOR_TEST_HOOKS)
void set_standalone_inspector_rpc_post_override_for_testing(
    StandaloneInspectorRpcPostOverride post_override) {
    std::lock_guard lock(rpc_post_hook_mutex);
    rpc_post_override =
        post_override
            ? std::make_shared<StandaloneInspectorRpcPostOverride>(std::move(post_override))
            : nullptr;
}

void set_standalone_inspector_telemetry_clock_for_testing(
    StandaloneInspectorTelemetryClock clock) {
    std::lock_guard lock(telemetry_clock_hook_mutex);
    telemetry_clock_override =
        clock ? std::make_shared<StandaloneInspectorTelemetryClock>(std::move(clock))
              : nullptr;
}
#endif

class StandaloneInspectorRuntime::RetirementCoordinator final
    : public std::enable_shared_from_this<RetirementCoordinator> {
  public:
    RetirementCoordinator(std::shared_ptr<inspect::InspectorOverlay> overlay,
                          std::shared_ptr<Impl> impl)
        : overlay_(std::move(overlay)), impl_(std::move(impl)) {}

    void set_close_editor(std::function<void()> close_editor) {
        std::lock_guard lock(mutex_);
        if (!close_editor_)
            close_editor_ = std::move(close_editor);
    }

    void begin() {
        {
            std::lock_guard lock(mutex_);
            if (!begun_) {
                begun_ = true;
                if (impl_)
                    fence_ = impl_->begin_stop();
            }
        }
        if (!try_finalize())
            schedule_poll();
    }

    bool try_finalize() {
        std::shared_ptr<Impl> impl;
        std::shared_ptr<inspect::InspectorOverlay> overlay;
        std::function<void()> close_editor;
        {
            std::lock_guard lock(mutex_);
            if (finalized_)
                return true;
            if (!begun_ || !fence_.ready())
                return false;
            finalized_ = true;
            impl = std::move(impl_);
            overlay = std::move(overlay_);
            close_editor = std::move(close_editor_);
        }

        if (impl)
            impl->detach_borrowed_sources();
        if (close_editor)
            close_editor();
        // The overlay's process-wide hooks borrow the root view. Destroy it only
        // after the editor close has retired that view, while the local shared
        // owners above still keep every inspector source alive.
        overlay.reset();
        impl.reset();
        return true;
    }

    bool pending() const {
        std::lock_guard lock(mutex_);
        return begun_ && !finalized_;
    }

    bool begun() const {
        std::lock_guard lock(mutex_);
        return begun_;
    }

  private:
    void schedule_poll() {
        {
            std::lock_guard lock(mutex_);
            if (finalized_ || poll_posted_)
                return;
            poll_posted_ = true;
        }
        const std::weak_ptr<RetirementCoordinator> weak = shared_from_this();
        const bool posted = events::MainThreadDispatcher::call_async_after(
            [weak] {
                if (const auto self = weak.lock()) {
                    {
                        std::lock_guard lock(self->mutex_);
                        self->poll_posted_ = false;
                    }
                    if (!self->try_finalize())
                        self->schedule_poll();
                }
            },
            1);
        if (!posted) {
            std::lock_guard lock(mutex_);
            poll_posted_ = false;
        }
    }

    mutable std::mutex mutex_;
    std::shared_ptr<inspect::InspectorOverlay> overlay_;
    std::shared_ptr<Impl> impl_;
    inspect::InspectorServerShutdownFence fence_;
    std::function<void()> close_editor_;
    bool begun_ = false;
    bool finalized_ = false;
    bool poll_posted_ = false;
};

StandaloneInspectorRuntime::StandaloneInspectorRuntime(
    std::shared_ptr<inspect::InspectorOverlay> overlay, std::shared_ptr<Impl> impl,
    std::vector<std::uint8_t> token, view::View& root, view::WindowHost& window)
    : overlay_(std::move(overlay)), impl_(std::move(impl)),
      retirement_(std::make_shared<RetirementCoordinator>(overlay_, impl_)),
      token_(std::move(token)), root_(root), window_(window) {}

StandaloneInspectorRuntime::~StandaloneInspectorRuntime() {
    stop();
}

bool StandaloneInspectorRuntime::profile_is_off(std::string_view profile) {
    const auto parsed_profile = parse_profile(profile);
    return parsed_profile && *parsed_profile == inspect::InspectorProfile::Off;
}

std::unique_ptr<StandaloneInspectorRuntime>
StandaloneInspectorRuntime::create(StandaloneApp& app, Processor& processor, ViewBridge& bridge,
                                   view::View& root, view::WindowHost& window,
                                   std::string profile,
                                   std::vector<std::string> custom_capabilities,
                                   bool runtime_eval_enabled) {
    const bool local_only = profile == "local";
    const auto parsed_profile = parse_profile(profile);
    if (!local_only && !parsed_profile)
        return nullptr;
    if (!local_only && *parsed_profile == inspect::InspectorProfile::Off)
        return nullptr;
    if (runtime_eval_enabled &&
        (local_only || (*parsed_profile != inspect::InspectorProfile::Develop &&
                        *parsed_profile != inspect::InspectorProfile::Custom))) {
        runtime::log_error(
            "Standalone: runtime evaluation requires the develop or custom inspector profile");
        return nullptr;
    }
    auto overlay = std::make_shared<inspect::InspectorOverlay>(root);
    inspect::install_inspector_hooks(*overlay);
    if (local_only) {
        overlay->set_active(true);
        return std::unique_ptr<StandaloneInspectorRuntime>(
            new StandaloneInspectorRuntime(std::move(overlay), nullptr, {}, root, window));
    }
    // This composition root is stack-owned by StandaloneApp::run() and is
    // stopped after run_event_loop() returns. A page-owned/non-blocking loop
    // would otherwise return before the first idle pump, falsely report a
    // successful start, and immediately destroy the inspector runtime.
    if (!window.event_loop_blocks_until_close()) {
        runtime::log_error(
            "Standalone: Development Inspector requires a blocking window event loop");
        return nullptr;
    }
    if (!window.event_loop_supports_exit_drain()) {
        runtime::log_error("Standalone: Development Inspector requires an event-loop exit drain");
        return nullptr;
    }
    if (!window.supports_deferred_close()) {
        runtime::log_error(
            "Standalone: Development Inspector requires deferred window close support");
        return nullptr;
    }
    std::vector<inspect::InspectorCapability> custom;
    if (*parsed_profile == inspect::InspectorProfile::Custom) {
        bool has_session_control = false;
        bool needs_session_control = false;
        for (const auto& id : custom_capabilities) {
            const auto capability = inspect::capability_from_id(id);
            if (!capability || !inspect::capability_is_grantable(*capability) ||
                !standalone_capability_available(
                    *capability, standalone_capture_available(root, window),
                    runtime_eval_enabled)) {
                runtime::log_error("Standalone: invalid custom inspector capability '{}'", id);
                return nullptr;
            }
            custom.push_back(*capability);
            has_session_control |=
                *capability == inspect::InspectorCapability::SessionControl;
            needs_session_control |=
                *capability != inspect::InspectorCapability::SessionControl
                && inspect::capability_requires_controller_lease(*capability);
        }
        if (custom.empty()) {
            runtime::log_error("Standalone: custom inspector profile requires capabilities");
            return nullptr;
        }
        if (needs_session_control && !has_session_control) {
            runtime::log_error(
                "Standalone: custom inspector mutation capabilities require session.control");
            return nullptr;
        }
    }

    auto token = inspect::generate_inspector_secret();
    if (!token) {
        runtime::log_error("Standalone: could not generate inspector credential");
        return nullptr;
    }
    auto session_id = random_id("session-");
    auto instance_id = random_id("instance-");
    if (!session_id || !instance_id) {
        runtime::log_error("Standalone: could not generate inspector identity");
        return nullptr;
    }
    auto impl =
        std::make_shared<Impl>(app, processor, bridge, root, window, overlay.get(), *parsed_profile,
                               std::move(custom), runtime_eval_enabled,
                               std::move(*session_id), std::move(*instance_id));
    return std::unique_ptr<StandaloneInspectorRuntime>(
        new StandaloneInspectorRuntime(std::move(overlay), std::move(impl),
                                       std::move(*token), root, window));
}

void StandaloneInspectorRuntime::pump() {
    if (stopped_ || retirement_->begun())
        return;
    if (impl_ && !startup_attempted_) {
        startup_attempted_ = true;
        if (!impl_->start(std::move(token_))) {
            startup_failed_ = true;
            runtime::log_error(
                "Standalone: could not start authenticated inspector session");
            window_.request_close_deferred();
            return;
        }
    }
    if (impl_)
        impl_->pump();
    if (overlay_->is_active()) {
        auto* overlay = overlay_.get();
        root_.interaction().overlay_queue.push_back(
            {[overlay](canvas::Canvas& canvas) { overlay->paint(canvas); },
             &root_});
    }
}

void StandaloneInspectorRuntime::stop() {
    if (stopped_)
        return;
    stopped_ = true;
    retirement_->begin();
}

bool StandaloneInspectorRuntime::try_finish_retirement() {
    return retirement_->try_finalize();
}

bool StandaloneInspectorRuntime::retirement_pending() const {
    return retirement_->pending();
}

StandaloneInspectorLifecycleState StandaloneInspectorRuntime::lifecycle_state() const {
    return impl_ ? impl_->lifecycle_state() : StandaloneInspectorLifecycleState{};
}

#if defined(PULP_STANDALONE_INSPECTOR_TEST_HOOKS)
std::vector<StandaloneInspectorAuditEntry>
StandaloneInspectorRuntime::audit_snapshot_for_testing() const {
    return impl_ ? impl_->audit_snapshot_for_testing()
                 : std::vector<StandaloneInspectorAuditEntry>{};
}

StandaloneInspectorTelemetryState
StandaloneInspectorRuntime::telemetry_state_for_testing() const {
    return impl_ ? impl_->telemetry_state_for_testing()
                 : StandaloneInspectorTelemetryState{};
}
#endif

void StandaloneInspectorRuntime::set_overlay_active(bool active) {
    if (retirement_->begun())
        return;
    overlay_->set_active(active);
}

std::function<void()> StandaloneInspectorRuntime::wrap_close(
    std::function<void()> close_editor) {
    retirement_->set_close_editor(std::move(close_editor));
    const auto retirement = retirement_;
    return [retirement] { retirement->begin(); };
}

} // namespace pulp::format::detail
