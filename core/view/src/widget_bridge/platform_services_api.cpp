// widget_bridge/platform_services_api.cpp - platform service registrations for WidgetBridge.

#include <pulp/view/widget_bridge.hpp>
#include "api_registry.hpp"

#include <pulp/platform/child_process.hpp>
#include <pulp/platform/clipboard.hpp>
#include <pulp/platform/file_dialog.hpp>

#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace pulp::view {

namespace {

std::string build_shell_command(const std::string& cmd) {
#if defined(_WIN32)
    return std::string(
        "set \"PATH=%USERPROFILE%\\.local\\bin;%USERPROFILE%\\.npm-global\\bin;%PATH%\" && "
    ) + cmd;
#else
    return std::string(
        "export PATH=\"$HOME/.local/bin:$HOME/.npm-global/bin:"
        "/opt/homebrew/bin:/usr/local/bin:$PATH\"; "
    ) + cmd;
#endif
}

} // namespace

void WidgetBridge::register_platform_services_ai_api() {
    BridgeApiContext api{engine_};

    // Model-agnostic AI CLI: configurable command for chat integration
    register_bridge_function(api, "setAICli", [this](choc::javascript::ArgumentList args) {
        auto cmd = args.get<std::string>(0, "");
        if (!cmd.empty()) ai_cli_command_ = cmd;
        return choc::value::Value();
    });

    register_bridge_function(api, "getAICli", [this](choc::javascript::ArgumentList) {
        return choc::value::createString(ai_cli_command_);
    });
}

void WidgetBridge::register_platform_services_exec_api() {
    BridgeApiContext api{engine_};

    // Shell exec
    // Ensures PATH includes common tool locations (homebrew, npm global, etc.)
    register_bridge_function(api, "exec", [](choc::javascript::ArgumentList args) {
        auto cmd = args.get<std::string>(0, "");
        if (cmd.empty()) return choc::value::createString("");
        auto full_cmd = build_shell_command(cmd);
#ifdef _WIN32
        auto result = pulp::platform::exec("cmd", {"/c", full_cmd}, 30000);
#else
        auto result = pulp::platform::exec("/bin/sh", {"-c", full_cmd}, 30000);
#endif
        return choc::value::createString(result.stdout_output);
    });

    // execAsync(cmd, callbackId) - non-blocking shell command
    // Runs cmd on a background thread, dispatches result to JS via
    // __dispatch__(callbackId, 'result', stdout) when poll_async_results() runs.
    register_bridge_function(api, "execAsync", [this](choc::javascript::ArgumentList args) {
        auto cmd = args.get<std::string>(0, "");
        auto cbId = args.get<std::string>(1, "");
        if (cmd.empty() || cbId.empty()) return choc::value::Value();
        auto full_cmd = build_shell_command(cmd);
        auto alive = callback_alive_;
        auto async_results = async_exec_results_;
        auto async_mutex = async_exec_mutex_;
        std::thread([alive, async_results, async_mutex, full_cmd, cbId]() {
#ifdef _WIN32
            auto result = pulp::platform::exec("cmd", {"/c", full_cmd}, 60000);
#else
            auto result = pulp::platform::exec("/bin/sh", {"-c", full_cmd}, 60000);
#endif
            if (!alive || !alive->load(std::memory_order_acquire)) return;
            std::lock_guard<std::mutex> lock(*async_mutex);
            async_results->push_back({cbId, std::move(result.stdout_output)});
        }).detach();
        return choc::value::Value();
    });
}

void WidgetBridge::register_platform_services_dialog_api() {
    BridgeApiContext api{engine_};

    // showOpenDialog(title, filterDesc, extensions) -> path or ""
    // extensions: semicolon-separated, e.g. "js;json;txt"
    register_bridge_function(api, "showOpenDialog", [](choc::javascript::ArgumentList args) {
        auto title = args.get<std::string>(0, "Open");
        auto desc = args.get<std::string>(1, "");
        auto exts = args.get<std::string>(2, "");
        std::vector<platform::FileFilter> filters;
        if (!desc.empty())
            filters.push_back({desc, exts});
        auto result = platform::FileDialog::open_file(title, filters);
        return choc::value::createString(result.value_or(""));
    });

    // showSaveDialog(title, filterDesc, extensions) -> path or ""
    register_bridge_function(api, "showSaveDialog", [](choc::javascript::ArgumentList args) {
        auto title = args.get<std::string>(0, "Save");
        auto desc = args.get<std::string>(1, "");
        auto exts = args.get<std::string>(2, "");
        std::vector<platform::FileFilter> filters;
        if (!desc.empty())
            filters.push_back({desc, exts});
        auto result = platform::FileDialog::save_file(title, filters);
        return choc::value::createString(result.value_or(""));
    });

    // chooseFolder(title) -> path or ""
    register_bridge_function(api, "chooseFolder", [](choc::javascript::ArgumentList args) {
        auto title = args.get<std::string>(0, "Choose Folder");
        auto result = platform::FileDialog::choose_folder(title);
        return choc::value::createString(result.value_or(""));
    });
}

void WidgetBridge::register_platform_services_clipboard_api() {
    BridgeApiContext api{engine_};

    // Clipboard - read/write text via platform::Clipboard
    register_bridge_function(api, "readClipboard", [](choc::javascript::ArgumentList) {
        auto text = platform::Clipboard::get_text();
        return choc::value::createString(text.value_or(""));
    });

    register_bridge_function(api, "writeClipboard", [](choc::javascript::ArgumentList args) {
        auto text = args.get<std::string>(0, "");
        platform::Clipboard::set_text(text);
        return choc::value::Value();
    });
}

} // namespace pulp::view
