#include <pulp/inspect/control_standalone_host.hpp>

#include <pulp/inspect/control_host_preflight.hpp>
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
#include <pulp/format/view_bridge.hpp>
#include <pulp/view/frame_clock.hpp>
#include <pulp/view/motion.hpp>
#include <pulp/view/scripted_ui.hpp>
#include <pulp/view/screenshot.hpp>
#include <pulp/view/value_channel_set.hpp>
#include <pulp/view/window_host.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <fstream>
#include <functional>
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
        "PULP_INSPECT_CAPABILITY_CAPTURE_IMAGE_V1\0"
        "PULP_INSPECT_CAPABILITY_UI_INPUT_V1\0"
        "PULP_INSPECT_CAPABILITY_TRACE_CONTROL_V1\0"
        "PULP_INSPECT_CAPABILITY_TRACE_SESSION_CONTROL_V1\0"
        "PULP_INSPECT_CAPABILITY_STATE_WRITE_V1\0"
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
    bool start(format::Processor& processor, state::StateStore& store) override {
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
    std::unique_ptr<view::View> root_;
    std::unique_ptr<format::ViewBridge> view_bridge_;
    std::unique_ptr<HeadlessViewWindow> window_;
    std::unique_ptr<MotionInspector> motion_;
    MotionScrubber scrubber_;
    view::FrameClock frame_clock_;
    std::optional<std::chrono::steady_clock::time_point> last_frame_tick_;
    format::Processor* processor_ = nullptr;
    state::StateStore* store_ = nullptr;
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
