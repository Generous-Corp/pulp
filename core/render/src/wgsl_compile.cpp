#include "wgsl_compile.hpp"

#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace pulp::render::detail {

namespace {

WgslDiagnostic::Severity to_severity(wgpu::CompilationMessageType type) {
    switch (type) {
    case wgpu::CompilationMessageType::Error:
        return WgslDiagnostic::Severity::error;
    case wgpu::CompilationMessageType::Warning:
        return WgslDiagnostic::Severity::warning;
    case wgpu::CompilationMessageType::Info:
        return WgslDiagnostic::Severity::info;
    }
    return WgslDiagnostic::Severity::error;
}

std::string to_string(wgpu::StringView view) {
    if (view.data == nullptr)
        return {};
    if (view.length == wgpu::kStrlen)
        return std::string(view.data);
    return std::string(view.data, view.length);
}

} // namespace

WgslCompileOutput compile_wgsl(const wgpu::Device& device, const wgpu::Instance& instance,
                               std::string_view code, std::string_view label,
                               std::chrono::milliseconds timeout) {
    // Callbacks may run on any thread that processes this instance's events,
    // and may run after a timed-out caller returned, so they write into
    // shared, locked state.
    struct State {
        std::mutex mutex;
        bool info_done = false;
        bool scope_done = false;
        std::vector<WgslDiagnostic> diagnostics;
        std::string scope_error;
    };
    auto state = std::make_shared<State>();

    WgslCompileOutput out;
    const std::string label_copy(label);
    wgpu::ShaderSourceWGSL wgsl{};
    wgsl.code = wgpu::StringView(code.data(), code.size());
    wgpu::ShaderModuleDescriptor desc{};
    desc.nextInChain = &wgsl;
    desc.label = wgpu::StringView(label_copy.data(), label_copy.size());

    device.PushErrorScope(wgpu::ErrorFilter::Validation);
    wgpu::ShaderModule module = device.CreateShaderModule(&desc);
    if (module) {
        module.GetCompilationInfo(
            wgpu::CallbackMode::AllowProcessEvents,
            [state](wgpu::CompilationInfoRequestStatus status, const wgpu::CompilationInfo* info) {
                std::lock_guard<std::mutex> lock(state->mutex);
                if (status == wgpu::CompilationInfoRequestStatus::Success && info != nullptr) {
                    for (size_t i = 0; i < info->messageCount; ++i) {
                        const auto& message = info->messages[i];
                        WgslDiagnostic diagnostic;
                        diagnostic.severity = to_severity(message.type);
                        diagnostic.line = static_cast<std::uint32_t>(message.lineNum);
                        diagnostic.column =
                            message.lineNum > 0 ? static_cast<std::uint32_t>(message.linePos) : 0u;
                        diagnostic.message = to_string(message.message);
                        state->diagnostics.push_back(std::move(diagnostic));
                    }
                }
                state->info_done = true;
            });
    } else {
        state->info_done = true;
    }
    device.PopErrorScope(
        wgpu::CallbackMode::AllowProcessEvents,
        [state](wgpu::PopErrorScopeStatus status, wgpu::ErrorType type, wgpu::StringView message) {
            std::lock_guard<std::mutex> lock(state->mutex);
            if (status == wgpu::PopErrorScopeStatus::Success && type != wgpu::ErrorType::NoError) {
                state->scope_error = to_string(message);
                if (state->scope_error.empty())
                    state->scope_error = "shader module rejected";
            }
            state->scope_done = true;
        });

    const auto deadline = std::chrono::steady_clock::now() + timeout;
    for (;;) {
        instance.ProcessEvents();
        {
            std::lock_guard<std::mutex> lock(state->mutex);
            if (state->info_done && state->scope_done)
                break;
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            out.timed_out = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    std::lock_guard<std::mutex> lock(state->mutex);
    out.diagnostics = state->diagnostics;
    // A scope error that the compiler already explained with positioned
    // messages adds nothing; keep it only when no error message carries it.
    if (!state->scope_error.empty() && !has_wgsl_errors(out.diagnostics))
        out.scope_error = state->scope_error;
    if (!out.timed_out && !has_wgsl_errors(out.diagnostics) && out.scope_error.empty())
        out.module = std::move(module);
    return out;
}

} // namespace pulp::render::detail
