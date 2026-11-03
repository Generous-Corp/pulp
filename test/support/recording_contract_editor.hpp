#pragma once

// An editor-host contract editor that records every call, for format adapter
// tests: each test drives the adapter's own editor API and asserts what it
// became in the contract. One global log and one global config, reset by the
// test, so a factory a processor owns needs no wiring to the test.

#include <pulp/events/main_thread_dispatcher.hpp>
#include <pulp/format/editor_host.hpp>

#include <cstdint>
#include <cstdlib>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace pulp::test_support {

/// Everything the contract editors did, in order.
struct ContractEditorLog {
    std::vector<std::string> calls;
    void* parent_handle = nullptr;
    format::EditorContext* context = nullptr;
    int live = 0;
    int idles = 0;
    double last_scale = 0.0;

    bool saw(const std::string& call) const {
        for (const auto& c : calls)
            if (c == call)
                return true;
        return false;
    }
};

/// How the next editors and factories behave.
struct ContractEditorConfig {
    bool attach_succeeds = true;
    bool fixed_size = false;
    bool factory_builds_editor = true;
    bool factory_supports_editor = true;
    /// Whether the editor offers EditorAutomation::capture_png().
    bool captures_frames = true;
};

inline ContractEditorLog& contract_log() {
    static ContractEditorLog log;
    return log;
}
inline ContractEditorConfig& contract_config() {
    static ContractEditorConfig config;
    return config;
}
inline void reset_contract_recording() {
    contract_log() = {};
    contract_config() = {};
}

/// A one-pixel PNG: the frame RecordingContractEditor reports it captured.
inline const std::vector<std::uint8_t>& recorded_frame_png() {
    static const std::vector<std::uint8_t> png = {
        0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A, 0x00, 0x00, 0x00, 0x0D, 0x49, 0x48,
        0x44, 0x52, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x08, 0x06, 0x00, 0x00,
        0x00, 0x1F, 0x15, 0xC4, 0x89, 0x00, 0x00, 0x00, 0x0D, 0x49, 0x44, 0x41, 0x54, 0x78,
        0x9C, 0x63, 0xF8, 0xCF, 0xC0, 0xF0, 0x1F, 0x00, 0x05, 0x00, 0x01, 0xFF, 0x89, 0x99,
        0x3D, 0x1D, 0x00, 0x00, 0x00, 0x00, 0x49, 0x45, 0x4E, 0x44, 0xAE, 0x42, 0x60, 0x82};
    return png;
}

class RecordingAutomation final : public format::EditorAutomation {
  public:
    bool capture_png(std::vector<std::uint8_t>& png) override {
        contract_log().calls.push_back("capture");
        png = recorded_frame_png();
        return true;
    }
};

/// 400x300 preferred, 200x150 to 800x600, holds 4:3, refuses anything under
/// 200x150, forwards Space and consumes every other key.
class RecordingContractEditor final : public format::Editor {
  public:
    explicit RecordingContractEditor(format::EditorContext& context) {
        contract_log().context = &context;
        ++contract_log().live;
    }
    ~RecordingContractEditor() override {
        --contract_log().live;
        contract_log().calls.push_back("destroyed");
    }

    bool supports_parent(format::NativeParentKind kind) const override {
        return kind != format::NativeParentKind::none;
    }
    bool attach(const format::NativeParent& parent) override {
        contract_log().calls.push_back(parent.kind == format::NativeParentKind::cocoa_view
                                           ? "attach:cocoa_view"
                                           : "attach:other");
        contract_log().parent_handle = parent.handle;
        return contract_config().attach_succeeds;
    }
    void detach() override {
        contract_log().calls.push_back("detach");
    }
    void invalidate() override {
        contract_log().calls.push_back("invalidate");
    }
    format::EditorSize size() const override {
        return size_;
    }
    bool set_size(const format::EditorSize& size) override {
        if (size.width < 200 || size.height < 150)
            return false;
        size_ = size;
        contract_log().last_scale = size.scale;
        contract_log().calls.push_back("set_size:" + std::to_string(size.width) + "x" +
                                       std::to_string(size.height));
        return true;
    }
    format::ViewSize size_hints() const override {
        format::ViewSize hints;
        hints.preferred_width = 400;
        hints.preferred_height = 300;
        hints.min_width = 200;
        hints.min_height = 150;
        hints.max_width = 800;
        hints.max_height = 600;
        hints.aspect_ratio = 4.0 / 3.0;
        if (contract_config().fixed_size) {
            hints.min_width = hints.max_width = 400;
            hints.min_height = hints.max_height = 300;
        }
        return hints;
    }
    format::EditorSize adjust_size(const format::EditorSize& proposed) const override {
        format::EditorSize adjusted = proposed;
        adjusted.height = proposed.width * 3 / 4;
        return adjusted;
    }
    void idle() override {
        ++contract_log().idles;
    }
    void visibility_changed(bool visible) override {
        contract_log().calls.push_back(visible ? "visible:on" : "visible:off");
    }
    void focus_changed(bool focused) override {
        contract_log().calls.push_back(focused ? "focus:on" : "focus:off");
    }
    format::EditorAutomation* automation() override {
        return contract_config().captures_frames ? &automation_ : nullptr;
    }
    format::KeyDisposition offer_key(const format::EditorKeyEvent& event) override {
        if (event.key == format::EditorKey::space) {
            contract_log().calls.push_back("offer:space");
            return format::KeyDisposition::forward;
        }
        contract_log().calls.push_back("offer:char");
        return format::KeyDisposition::consumed;
    }

  private:
    format::EditorSize size_{400, 300, 1.0};
    RecordingAutomation automation_;
};

class RecordingContractFactory final : public format::EditorFactory {
  public:
    bool supports(format::EditorRole role) const override {
        return role == format::EditorRole::editor && contract_config().factory_supports_editor;
    }
    std::unique_ptr<format::Editor> create(format::Processor&, format::EditorContext& context,
                                           format::EditorRole role) override {
        if (role != format::EditorRole::editor || !contract_config().factory_builds_editor)
            return nullptr;
        return std::make_unique<RecordingContractEditor>(context);
    }
};

/// Unset an environment variable for a scope.
class ScopedEnvUnset {
  public:
    explicit ScopedEnvUnset(const char* name) : name_(name) {
        if (const char* prev = std::getenv(name))
            prev_ = std::string(prev);
#if defined(_WIN32)
        _putenv_s(name_.c_str(), "");
#else
        ::unsetenv(name_.c_str());
#endif
    }
    ~ScopedEnvUnset() {
#if defined(_WIN32)
        _putenv_s(name_.c_str(), prev_ ? prev_->c_str() : "");
#else
        if (prev_)
            ::setenv(name_.c_str(), prev_->c_str(), 1);
        else
            ::unsetenv(name_.c_str());
#endif
    }
    ScopedEnvUnset(const ScopedEnvUnset&) = delete;
    ScopedEnvUnset& operator=(const ScopedEnvUnset&) = delete;

  private:
    std::string name_;
    std::optional<std::string> prev_;
};

/// Editors are refused in CI and headless environments; a test that opens
/// one clears those variables for its scope.
struct EditorEnvironment {
    ScopedEnvUnset disable_editor{"PULP_DISABLE_PLUGIN_EDITOR"};
    ScopedEnvUnset headless{"PULP_HEADLESS"};
    ScopedEnvUnset test_mode{"PULP_TEST_MODE"};
    ScopedEnvUnset ci{"CI"};
};

/// A main-thread backend whose posts wait in a queue until the test runs
/// them, so each idle tick is observable. Register it after the adapter
/// registers its own backend, so it is the active one.
class QueuedMainThread {
  public:
    QueuedMainThread() {
        events::MainThreadDispatcher::Backend backend;
        backend.post = [this](events::Task task) {
            tasks_.push_back({std::move(task), 0});
            return true;
        };
        backend.is_main_thread = [] { return true; };
        backend.post_after = [this](events::Task task, int delay_ms) {
            tasks_.push_back({std::move(task), delay_ms});
            return true;
        };
        token_ = events::MainThreadDispatcher::register_backend(std::move(backend));
    }
    ~QueuedMainThread() {
        events::MainThreadDispatcher::unregister_backend(token_);
    }
    QueuedMainThread(const QueuedMainThread&) = delete;
    QueuedMainThread& operator=(const QueuedMainThread&) = delete;

    /// Run the tasks queued so far (not the ones they queue).
    std::size_t run_pending() {
        auto batch = std::move(tasks_);
        tasks_.clear();
        for (auto& queued : batch)
            queued.task();
        return batch.size();
    }
    std::size_t pending() const {
        return tasks_.size();
    }

  private:
    struct Queued {
        events::Task task;
        int delay_ms;
    };
    std::vector<Queued> tasks_;
    events::MainThreadDispatcher::Token token_ = 0;
};

} // namespace pulp::test_support
