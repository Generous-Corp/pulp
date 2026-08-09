#include <pulp/inspect/control_standalone_host.hpp>

#include <pulp/inspect/control_host_preflight.hpp>
#include <pulp/inspect/console_capture.hpp>
#include <pulp/inspect/control_installed_host.hpp>
#include <pulp/inspect/control_main_thread_executor.hpp>
#include <pulp/inspect/control_manifest.hpp>
#include <pulp/inspect/control_state_read_executor.hpp>
#include <pulp/inspect/control_state_write_executor.hpp>
#include <pulp/inspect/control_standalone_ui_adapter.hpp>
#include <pulp/inspect/motion_inspector.hpp>
#include <pulp/inspect/motion_scrubber.hpp>
#include <pulp/inspect/runtime_eval_component.hpp>
#include <pulp/format/processor.hpp>
#include <pulp/format/standalone.hpp>
#include <pulp/format/view_bridge.hpp>
#include <pulp/view/frame_clock.hpp>
#include <pulp/view/inspector.hpp>
#include <pulp/view/motion.hpp>
#include <pulp/view/scripted_ui.hpp>
#include <pulp/view/screenshot.hpp>
#include <pulp/view/value_channel_set.hpp>
#include <pulp/view/window_host.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <limits>
#include <mutex>
#include <optional>
#include <ranges>
#include <string>
#include <thread>
#include <vector>

#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif

namespace pulp::inspect {
namespace {

using namespace std::chrono_literals;

#if defined(_MSC_VER)
#define PULP_CONTROL_COMPONENT_MARKER __declspec(dllexport)
#else
#define PULP_CONTROL_COMPONENT_MARKER __attribute__((used, visibility("default")))
#endif

extern "C" PULP_CONTROL_COMPONENT_MARKER const volatile char
    pulp_control_standalone_host_markers_v1[] =
        "PULP_INSPECT_SHIPPING_MANIFEST_V1\0"
        "PULP_INSPECT_CAPABILITY_SESSION_DESCRIBE_V1\0"
        "PULP_INSPECT_CAPABILITY_SESSION_CONTROL_V1\0"
        "PULP_INSPECT_CAPABILITY_STATE_READ_V1\0"
        "PULP_INSPECT_CAPABILITY_UI_READ_V1\0"
        "PULP_INSPECT_CAPABILITY_DIAGNOSTICS_READ_V1\0"
        "PULP_INSPECT_CAPABILITY_LOGS_READ_V1\0"
        "PULP_INSPECT_CAPABILITY_CAPTURE_IMAGE_V1\0"
        "PULP_INSPECT_CAPABILITY_UI_INPUT_V1\0"
        "PULP_INSPECT_CAPABILITY_TRACE_CONTROL_V1\0"
        "PULP_INSPECT_CAPABILITY_TRACE_SESSION_CONTROL_V1\0"
        "PULP_INSPECT_CAPABILITY_STATE_WRITE_V1\0"
        "PULP_INSPECT_CAPABILITY_TEST_INPUT_V1\0"
        "PULP_INSPECT_CAPABILITY_AUTHORING_TWEAKS_V1\0"
        "PULP_INSPECT_CAPABILITY_TELEMETRY_STREAM_V1";

#undef PULP_CONTROL_COMPONENT_MARKER

bool has_capability(const ControlManifest& manifest, InspectorCapability capability) {
    return std::ranges::find(manifest.capabilities, capability) != manifest.capabilities.end();
}

std::atomic<detail::StandaloneRuntimeEvaluatorFactory>& evaluator_factory() {
    static std::atomic<detail::StandaloneRuntimeEvaluatorFactory> factory{nullptr};
    return factory;
}

std::atomic<detail::StandaloneControlAuthorHooksFactory>& author_hooks_factory() {
    static std::atomic<detail::StandaloneControlAuthorHooksFactory> factory{nullptr};
    return factory;
}

std::filesystem::path current_executable() {
#ifdef __APPLE__
    std::uint32_t size = 0;
    (void)_NSGetExecutablePath(nullptr, &size);
    std::vector<char> buffer(size);
    if (size == 0 || _NSGetExecutablePath(buffer.data(), &size) != 0)
        return {};
    std::error_code error;
    auto result = std::filesystem::weakly_canonical(buffer.data(), error);
    return error ? std::filesystem::path{} : result;
#else
    return {};
#endif
}

std::optional<ControlManifest> installed_manifest() {
    const auto executable = current_executable();
    if (executable.empty())
        return std::nullopt;
    std::ifstream input(executable.string() + ".inspector-capabilities.json", std::ios::binary);
    if (!input)
        return std::nullopt;
    const std::string bytes((std::istreambuf_iterator<char>(input)),
                            std::istreambuf_iterator<char>());
    return parse_control_manifest(bytes);
}

class MainThreadQueue {
  public:
    bool post(std::function<void()> task) {
        if (!task)
            return false;
        std::lock_guard lock(mutex_);
        if (closed_ || tasks_.size() >= 64)
            return false;
        tasks_.push_back(std::move(task));
        return true;
    }

    void pump() noexcept {
        std::function<void()> task;
        {
            std::lock_guard lock(mutex_);
            if (closed_ || tasks_.empty())
                return;
            task = std::move(tasks_.front());
            tasks_.pop_front();
        }
        try {
            task();
        } catch (...) {
        }
    }

    void close() noexcept {
        std::lock_guard lock(mutex_);
        closed_ = true;
        tasks_.clear();
    }

  private:
    std::mutex mutex_;
    std::deque<std::function<void()>> tasks_;
    bool closed_ = false;
};

class HeadlessViewWindow final : public view::WindowHost {
  public:
    explicit HeadlessViewWindow(view::View& root) : root_(root) {}

    void show() override { visible_ = true; }
    void hide() override { visible_ = false; }
    bool is_visible() const override { return visible_; }
    void repaint() override {}
    void set_close_callback(std::function<void()> callback) override {
        close_ = std::move(callback);
    }
    void run_event_loop() override {}
    ContentSize get_content_size() const override {
        const auto bounds = root_.local_bounds();
        return {static_cast<std::uint32_t>(std::max(1.0f, bounds.width)),
                static_cast<std::uint32_t>(std::max(1.0f, bounds.height))};
    }
    std::vector<std::uint8_t> capture_back_buffer_png() override {
        const auto size = get_content_size();
        auto result = view::capture_view(root_, size.width, size.height, 1.0f);
        return result.ok ? std::move(result.png) : std::vector<std::uint8_t>{};
    }
    bool supports_back_buffer_capture() const override { return true; }

  private:
    view::View& root_;
    std::function<void()> close_;
    bool visible_ = false;
};

view::View* find_unique_view(view::View& root, std::string_view id) {
    view::View* found = nullptr;
    bool duplicate = false;
    const auto visit = [&](const auto& self, view::View& node) -> void {
        if (node.id() == id) {
            duplicate = found != nullptr;
            found = &node;
        }
        for (std::size_t index = 0; index < node.child_count(); ++index)
            self(self, *node.child_at(index));
    };
    visit(visit, root);
    return duplicate ? nullptr : found;
}

choc::value::Value without_geometry(choc::value::ValueView value) {
    if (value.isObject()) {
        auto result = choc::value::createObject("");
        value.visitObjectMembers([&](std::string_view name, choc::value::ValueView child) {
            if (name != "bounds")
                result.addMember(name, without_geometry(child));
        });
        return result;
    }
    if (value.isArray()) {
        auto result = choc::value::createEmptyArray();
        for (std::uint32_t index = 0; index < value.size(); ++index)
            result.addArrayElement(without_geometry(value[index]));
        return result;
    }
    return choc::value::Value(value);
}

TestInputApplyResult test_input_result(format::detail::StandaloneTestInputResult result) {
    switch (result) {
    case format::detail::StandaloneTestInputResult::Applied:
        return TestInputApplyResult::success();
    case format::detail::StandaloneTestInputResult::InvalidArgument:
        return TestInputApplyResult::failure("invalid_argument", "test input was invalid");
    case format::detail::StandaloneTestInputResult::QueueFull:
        return TestInputApplyResult::failure("queue_full", "test input queue is full");
    }
    return TestInputApplyResult::failure("invalid_argument", "test input was rejected");
}

ControlExecutionOutcome unavailable_operation() {
    return {
        .terminal_state = ControlReceiptState::Failed,
        .result = {.result_code = ControlResultCode::NotBuilt,
                   .explanation =
                       "this Standalone host does not compose the requested typed executor"},
    };
}

class CanonicalStandaloneControlHost final : public format::StandaloneControlHost {
  public:
    bool start(format::Processor& processor, state::StateStore& store,
               format::detail::StandaloneTestInputHost* test_input,
               double sample_rate) override {
        const auto handle = inherited_control_host_bootstrap_handle();
#ifdef _WIN32
        if (handle == nullptr)
#else
        if (handle < 0)
#endif
            return true;

        ControlHostPreflightDiagnostics diagnostics;
        auto bootstrap = receive_control_host_preflight(
            handle, 10s, std::chrono::system_clock::now(), &diagnostics);
        auto manifest = installed_manifest();
        if (!bootstrap || bootstrap->enrollment_id.empty() || !manifest)
            return false;

        processor_ = &processor;
        store_ = &store;
        test_input_ = test_input;
        sample_rate_ = sample_rate;
        main_thread_ = std::this_thread::get_id();
        queue_ = std::make_shared<MainThreadQueue>();
        rpc_ = std::make_shared<InspectorMainThreadRpc>(
            InspectorMainThreadRpc::Config{2s, 64},
            [queue = queue_](std::function<void()> task) {
                return queue->post(std::move(task));
            },
            [thread = main_thread_] { return std::this_thread::get_id() == thread; });

        auto state_read = make_control_state_read_executor(
            [this](const ControlAdmissionPlan& plan)
                -> std::optional<ControlStateReadSource> {
                const auto generation = stable_generation();
                if (!generation)
                    return std::nullopt;
                return ControlStateReadSource{
                    .registration_id = plan.registration_id,
                    .host_tier = ControlHostTier::Standalone,
                    .store = store_,
                    .state_generation = *generation,
                    .catalog_generation = store_->parameter_display_revision() + 1,
                    .is_sensitive = [](state::ParamID) { return false; }};
            });
        auto state_write = make_control_state_write_executor(
            [this](const ControlAdmissionPlan& plan)
                -> std::optional<ControlStateWriteTarget> {
                const auto generation = stable_generation();
                if (!generation)
                    return std::nullopt;
                return ControlStateWriteTarget{.registration_id = plan.registration_id,
                                               .host_tier = ControlHostTier::Standalone,
                                               .store = store_,
                                               .state_generation = *generation};
            });
        ControlMainThreadExecutor main_state_write(rpc_, std::move(state_write));
        auto fenced_state_write = main_state_write.executor();
        ControlOperationExecutor state_executor =
            [state_read = std::move(state_read),
             state_write = std::move(fenced_state_write)](
                const ControlAdmissionPlan& plan, const ControlRequestEnvelope& request,
                const ControlExecutionContext& context) {
                if (request.operation_id == "dev.pulp.state/read@1")
                    return state_read(plan, request, context);
                if (request.operation_id == "dev.pulp.state/parameter-gesture@1")
                    return state_write(plan, request, context);
                return unavailable_operation();
            };

        const bool needs_ui = has_capability(*manifest, InspectorCapability::CaptureImage) ||
                              has_capability(*manifest, InspectorCapability::UiRead) ||
                              has_capability(*manifest, InspectorCapability::LogsRead) ||
                              has_capability(*manifest, InspectorCapability::AuthoringTweaks) ||
                              has_capability(*manifest, InspectorCapability::UiInput) ||
                              has_capability(*manifest, InspectorCapability::TraceControl) ||
                              has_capability(*manifest, InspectorCapability::RuntimeEval);
        if (needs_ui) {
            view_bridge_ = std::make_unique<format::ViewBridge>(processor, store);
            std::string view_error;
            if (!view_bridge_->open(&view_error))
                return false;
            root_ = view_bridge_->release_view();
            if (!root_)
                return false;
        }
        if (!root_) {
            root_ = std::make_unique<view::View>();
            root_->set_id("control-root");
            root_->set_bounds({0, 0, 1, 1});
        }

        author_hooks_ = detail::create_standalone_control_author_hooks(processor);
        if (has_capability(*manifest, InspectorCapability::AuthoringTweaks) &&
            !author_hooks_.apply_authoring)
            return false;
        if (has_capability(*manifest, InspectorCapability::TestInput) &&
            (!test_input_ || !std::isfinite(sample_rate_) || sample_rate_ <= 0.0))
            return false;
        std::optional<ControlInstalledHostUiConfig> ui;
        view::motion::Coordinator::instance().reset();
        view::motion::CostAttributor::instance().reset();
        view::motion::Coordinator::instance().bind(frame_clock_);
        last_frame_tick_ = std::chrono::steady_clock::now();
        if (author_hooks_.motion_cost_probe)
            view::motion::CostAttributor::instance().set_probe(
                author_hooks_.motion_cost_probe);
        motion_ = std::make_unique<MotionInspector>(*root_);
        if (needs_ui) {
            window_ = std::make_unique<HeadlessViewWindow>(*root_);
            root_->set_window_host(window_.get());
            view_bridge_->notify_attached();
            if (!author_hooks_.motion_fixture_path.empty() &&
                !scrubber_.load_fixture(author_hooks_.motion_fixture_path))
                return false;

            if (has_capability(*manifest, InspectorCapability::RuntimeEval)) {
                if (!manifest->unsafe_runtime_eval_acknowledged)
                    return false;
                auto evaluator =
                    detail::create_standalone_runtime_evaluator(processor, *view_bridge_);
                if (!evaluator || !evaluator->capabilities().can_evaluate ||
                    !evaluator->capabilities().can_interrupt)
                    return false;
                evaluator_ = std::move(evaluator);
            }

            ui = ControlInstalledHostUiConfig{
                .manifest = *manifest,
                .make_targets = [this](const ControlHostOpenResult& binding)
                    -> std::optional<ControlInstalledHostUiTargets> {
                    if (!root_ || !window_)
                        return std::nullopt;
                    ui_adapter_ = ControlStandaloneUiAdapter::create({
                        .root = *root_,
                        .window = *window_,
                        .instance_id = binding.instance_id,
                        .instance_generation = binding.instance_generation,
                        .view_generation = binding.publication_id,
                        .capture_scale = 1.0f,
                    });
                    if (!ui_adapter_)
                        return std::nullopt;
                    return ControlInstalledHostUiTargets{
                        .capture_source = ui_adapter_,
                        .target_adapter = ui_adapter_,
                        .view_generation = binding.publication_id,
                    };
                },
                .runtime_evaluator = evaluator_,
                .redact_runtime_eval_result = [](std::string_view) {
                    return std::optional<std::string>{R"({"redacted":true})"};
                },
            };

            console_ = std::make_shared<ConsoleCapture>();
            view_bridge_->visit_scripted_ui([this](view::ScriptedUiSession* scripted) {
                if (!scripted || !console_)
                    return;
                log_session_ = scripted;
                log_subscription_ = scripted->add_log_callback(console_->callback());
            });
        }

        const bool needs_development =
            has_capability(*manifest, InspectorCapability::UiRead) ||
            has_capability(*manifest, InspectorCapability::DiagnosticsRead) ||
            has_capability(*manifest, InspectorCapability::LogsRead) ||
            has_capability(*manifest, InspectorCapability::TestInput) ||
            has_capability(*manifest, InspectorCapability::AuthoringTweaks);
        std::optional<ControlInstalledHostDevelopmentConfig> development;
        if (needs_development) {
            development = ControlInstalledHostDevelopmentConfig{
                .manifest = *manifest,
                .observe_ui = [this](const ControlUiObservationRequest& request)
                    -> std::optional<ControlUiObservation> {
                    if (!root_)
                        return std::nullopt;
                    view::View* selected = root_.get();
                    if (!request.selector.empty()) {
                        auto selector = request.selector;
                        if (selector.starts_with('#'))
                            selector.erase(selector.begin());
                        selected = find_unique_view(*root_, selector);
                        if (!selected)
                            return std::nullopt;
                    }
                    const auto count = view::ViewInspector::count_views(*selected);
                    if (count > 10'000)
                        return std::nullopt;
                    auto json = view::ViewInspector::to_json(*selected);
                    if (!request.include_geometry) {
                        try {
                            json = choc::json::toString(
                                without_geometry(choc::json::parse(json)), false);
                        } catch (...) {
                            return std::nullopt;
                        }
                    }
                    return ControlUiObservation{
                        .tree_json = std::move(json),
                        .generation = root_->root_structure_generation(),
                        .node_count = count};
                },
                .read_diagnostics = [this] {
                    std::vector<ControlDiagnosticItem> items;
                    if (test_input_) {
                        const auto transport = test_input_->transport_snapshot();
                        items.push_back({
                            .id = "standalone.test-input",
                            .severity = ControlDiagnosticSeverity::Info,
                            .message = "transport=" +
                                       std::string(transport.playing ? "playing" : "stopped") +
                                       ", midi_overflow_count=" +
                                       std::to_string(test_input_->midi_overflow_count())});
                    }
                    if (author_hooks_.diagnostics) {
                        auto author = author_hooks_.diagnostics();
                        items.insert(items.end(), std::make_move_iterator(author.begin()),
                                     std::make_move_iterator(author.end()));
                    }
                    return items;
                },
                .read_logs = [this](std::uint64_t after, std::size_t limit) {
                    ControlLogPage page;
                    if (!console_)
                        return page;
                    std::uint64_t next = 0;
                    auto entries = console_->entries_since(after, next);
                    if (entries.size() > limit) {
                        entries.resize(limit);
                        next = entries.back().seq;
                    }
                    page.next_sequence = next;
                    for (const auto& entry : entries) {
                        page.entries.push_back({
                            .sequence = entry.seq,
                            .level = entry.level.substr(0, 32),
                            .message = entry.message.substr(
                                0, kControlDevelopmentMaximumTextBytes)});
                    }
                    return page;
                },
                .apply_test_note = [this](const ControlTestNoteInput& input) {
                    if (!test_input_)
                        return TestInputApplyResult::failure(
                            "test_input_unavailable", "standalone test input is unavailable");
                    return test_input_result(test_input_->inject_note({
                        .kind = input.note_on
                                    ? format::detail::StandaloneTestMidiKind::NoteOn
                                    : format::detail::StandaloneTestMidiKind::NoteOff,
                        .channel = static_cast<std::uint8_t>(input.channel + 1),
                        .note = input.note,
                        .velocity = static_cast<std::uint8_t>(
                            std::lround(std::clamp(input.velocity, 0.0, 1.0) * 127.0))}));
                },
                .apply_test_transport = [this](const ControlTestTransportInput& input) {
                    if (!test_input_ || !std::isfinite(sample_rate_) || sample_rate_ <= 0.0)
                        return TestInputApplyResult::failure(
                            "test_input_unavailable", "standalone transport input is unavailable");
                    const auto samples = input.position_beats * 60.0 / input.tempo_bpm * sample_rate_;
                    if (!std::isfinite(samples) || samples < 0.0 ||
                        samples > static_cast<double>(std::numeric_limits<std::int64_t>::max()))
                        return TestInputApplyResult::failure(
                            "invalid_argument", "transport position exceeds the host range");
                    return test_input_result(test_input_->update_transport({
                        .playing = input.playing,
                        .position_samples = static_cast<std::int64_t>(std::llround(samples)),
                        .tempo_bpm = input.tempo_bpm}));
                },
                .release_test_input = [this](TestInputReleaseReason) {
                    if (test_input_)
                        test_input_->release_test_input();
                },
                .apply_authoring = author_hooks_.apply_authoring,
            };
        }

        telemetry_ = std::make_shared<ControlTelemetryTap>(
            ControlTelemetryTapConfig{.enabled = true, .maximum_queued_frames = 2});
        processor.visit_value_channels([this](view::ValueChannelSet* channels) {
            if (channels)
                (void)telemetry_->attach(channels->attach_telemetry(),
                                         author_hooks_.telemetry_classifier);
        });

        installed_ = ControlInstalledHost::start({
            .bootstrap = std::move(*bootstrap),
            .main_thread_rpc = rpc_,
            .trace_inspector = std::make_shared<TraceInspector>(),
            .telemetry = telemetry_,
            .motion_inspector = motion_.get(),
            .motion_scrubber = &scrubber_,
            .ui = std::move(ui),
            .development = std::move(development),
            .host_executor = std::move(state_executor),
        });
        if (!installed_ || !installed_->ready()) {
            stop();
            return false;
        }
        return true;
    }

    void stop() noexcept override {
        if (installed_)
            installed_->stop();
        installed_.reset();
        if (rpc_)
            rpc_->cancel_and_wait();
        if (queue_)
            queue_->close();
        if (log_session_ && log_subscription_ != 0)
            log_session_->remove_log_callback(log_subscription_);
        log_session_ = nullptr;
        log_subscription_ = 0;
        console_.reset();
        ui_adapter_.reset();
        evaluator_.reset();
        motion_.reset();
        if (root_)
            root_->set_window_host(nullptr);
        window_.reset();
        if (view_bridge_)
            view_bridge_->close();
        root_.reset();
        view_bridge_.reset();
        telemetry_.reset();
        rpc_.reset();
        queue_.reset();
        view::motion::Coordinator::instance().reset();
        view::motion::CostAttributor::instance().reset();
        author_hooks_ = {};
        last_frame_tick_.reset();
        processor_ = nullptr;
        store_ = nullptr;
        test_input_ = nullptr;
        sample_rate_ = 0.0;
    }

    void poll() noexcept override {
        if (queue_)
            queue_->pump();
        if (telemetry_)
            telemetry_->poll();
        if (view_bridge_) {
            view_bridge_->visit_scripted_ui([](view::ScriptedUiSession* scripted) {
                if (!scripted)
                    return;
                std::string ignored;
                (void)scripted->poll(&ignored);
            });
            (void)view_bridge_->poll_editor_reload();
        }
        const auto now = std::chrono::steady_clock::now();
        if (last_frame_tick_) {
            const auto elapsed = std::chrono::duration<double>(now - *last_frame_tick_).count();
            if (elapsed > 0.0)
                frame_clock_.tick(std::min(elapsed, 0.25));
        }
        last_frame_tick_ = now;
    }

  private:
    std::unique_ptr<ControlInstalledHost> installed_;
    std::shared_ptr<MainThreadQueue> queue_;
    std::shared_ptr<InspectorMainThreadRpc> rpc_;
    std::shared_ptr<ControlTelemetryTap> telemetry_;
    std::shared_ptr<ControlStandaloneUiAdapter> ui_adapter_;
    std::shared_ptr<RuntimeEvaluator> evaluator_;
    std::shared_ptr<ConsoleCapture> console_;
    view::ScriptedUiSession* log_session_ = nullptr;
    std::uint64_t log_subscription_ = 0;
    std::unique_ptr<view::View> root_;
    std::unique_ptr<format::ViewBridge> view_bridge_;
    std::unique_ptr<HeadlessViewWindow> window_;
    std::unique_ptr<MotionInspector> motion_;
    MotionScrubber scrubber_;
    view::FrameClock frame_clock_;
    std::optional<std::chrono::steady_clock::time_point> last_frame_tick_;
    format::Processor* processor_ = nullptr;
    state::StateStore* store_ = nullptr;
    format::detail::StandaloneTestInputHost* test_input_ = nullptr;
    double sample_rate_ = 0.0;
    std::thread::id main_thread_;
    detail::StandaloneControlAuthorHooks author_hooks_;

    std::optional<std::uint64_t> stable_generation() const noexcept {
        if (!store_)
            return std::nullopt;
        const auto state = store_->state_generation();
        if (!store_->state_snapshot_is_current(state))
            return std::nullopt;
        return state;
    }
};

} // namespace

std::unique_ptr<format::StandaloneControlHost> make_control_standalone_host() {
    return std::make_unique<CanonicalStandaloneControlHost>();
}

namespace detail {

bool install_standalone_control_author_hooks_factory(
    StandaloneControlAuthorHooksFactory factory) noexcept {
    if (!factory)
        return false;
    auto expected = static_cast<StandaloneControlAuthorHooksFactory>(nullptr);
    return author_hooks_factory().compare_exchange_strong(expected, factory,
                                                          std::memory_order_release,
                                                          std::memory_order_relaxed);
}

StandaloneControlAuthorHooks
create_standalone_control_author_hooks(format::Processor& processor) {
    const auto factory = author_hooks_factory().load(std::memory_order_acquire);
    return factory ? factory(processor) : StandaloneControlAuthorHooks{};
}

bool install_standalone_runtime_evaluator_factory(
    StandaloneRuntimeEvaluatorFactory factory) noexcept {
    if (!factory)
        return false;
    auto expected = static_cast<StandaloneRuntimeEvaluatorFactory>(nullptr);
    return evaluator_factory().compare_exchange_strong(expected, factory,
                                                       std::memory_order_release,
                                                       std::memory_order_relaxed);
}

std::shared_ptr<RuntimeEvaluator>
create_standalone_runtime_evaluator(format::Processor& processor,
                                    format::ViewBridge& bridge) {
    const auto factory = evaluator_factory().load(std::memory_order_acquire);
    return factory ? factory(processor, bridge) : nullptr;
}

} // namespace detail

} // namespace pulp::inspect
