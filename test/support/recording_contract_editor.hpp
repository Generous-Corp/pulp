#pragma once

// An editor-host contract editor that records every call, for format adapter
// tests: each test drives the adapter's own editor API and asserts what it
// became in the contract. One global log and one global config, reset by the
// test, so a factory a processor owns needs no wiring to the test.

#if defined(__APPLE__)
#include <CoreFoundation/CoreFoundation.h>
#include <pthread.h>
#endif

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
    /// Attach, detach, invalidate and destruction calls made off the main
    /// thread (always 0 where the main thread cannot be told apart).
    int off_main_thread_calls = 0;

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
inline void note_editor_thread() {
#if defined(__APPLE__)
    if (pthread_main_np() == 0)
        ++contract_log().off_main_thread_calls;
#endif
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
        note_editor_thread();
        --contract_log().live;
        contract_log().calls.push_back("destroyed");
    }

    bool supports_parent(format::NativeParentKind kind) const override {
        return kind != format::NativeParentKind::none;
    }
    bool attach(const format::NativeParent& parent) override {
        note_editor_thread();
        contract_log().calls.push_back(parent.kind == format::NativeParentKind::cocoa_view
                                           ? "attach:cocoa_view"
                                           : "attach:other");
        contract_log().parent_handle = parent.handle;
        return contract_config().attach_succeeds;
    }
    void detach() override {
        note_editor_thread();
        contract_log().calls.push_back("detach");
    }
    void invalidate() override {
        note_editor_thread();
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

/// The main thread's event loop, for an editor's own idle tick. On Apple
/// platforms that tick is a dispatch source on the main queue, so the loop
/// runs the real main run loop; elsewhere the tick posts through
/// MainThreadDispatcher, and the loop runs those posts from a queued backend,
/// registered after the adapter's own backend so it is the active one.
class MainLoop {
  public:
#if !defined(__APPLE__)
    MainLoop() {
        events::MainThreadDispatcher::Backend backend;
        backend.post = [this](events::Task task) {
            tasks_.push_back(std::move(task));
            return true;
        };
        backend.is_main_thread = [] { return true; };
        backend.post_after = [this](events::Task task, int) {
            tasks_.push_back(std::move(task));
            return true;
        };
        token_ = events::MainThreadDispatcher::register_backend(std::move(backend));
    }
    ~MainLoop() {
        events::MainThreadDispatcher::unregister_backend(token_);
    }
#else
    MainLoop() = default;
#endif
    MainLoop(const MainLoop&) = delete;
    MainLoop& operator=(const MainLoop&) = delete;

    /// Run the loop for about `seconds`.
    void run_for(double seconds) {
#if defined(__APPLE__)
        CFRunLoopRunInMode(kCFRunLoopDefaultMode, seconds, false);
#else
        for (int i = 0; i < static_cast<int>(seconds * 100.0) + 1; ++i) {
            auto batch = std::move(tasks_);
            tasks_.clear();
            for (auto& task : batch)
                task();
        }
#endif
    }
    /// Run the loop until `done()` holds or about `seconds` pass.
    template <typename Done> bool run_until(Done done, double seconds = 2.0) {
        for (double waited = 0.0; waited < seconds; waited += 0.02) {
            if (done())
                return true;
            run_for(0.02);
        }
        return done();
    }

  private:
#if !defined(__APPLE__)
    std::vector<events::Task> tasks_;
    events::MainThreadDispatcher::Token token_ = 0;
#endif
};

} // namespace pulp::test_support
