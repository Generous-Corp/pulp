#include "cmd_inspect_screenshot.hpp"

#include <pulp/runtime/detail/durable_file_replacement.hpp>
#include <pulp/runtime/base64.hpp>
#include <pulp/inspect/client.hpp>
#include <pulp/inspect/protocol.hpp>
#include <pulp/view/screenshot_compare.hpp>

#include <choc/text/choc_JSON.h>

#include <chrono>
#include <iostream>
#include <limits>
#include <span>
#include <system_error>

namespace fs = std::filesystem;

namespace {

choc::value::Value publication_json(
    const pulp::inspect::InspectorDiscoveryRecord& publication) {
    auto value = choc::value::createObject("");
    value.addMember("sessionId", choc::value::createString(publication.session_id));
    value.addMember("instanceId", choc::value::createString(publication.instance_id));
    value.addMember("publicationId", choc::value::createString(publication.publication_id));
    value.addMember("pluginId", choc::value::createString(publication.plugin_id));
    return value;
}

void print_screenshot_json(
    std::string_view status,
    const pulp::inspect::InspectorDiscoveryRecord* publication,
    std::string_view code,
    std::string_view message,
    std::string_view error_data_json = {}) {
    auto root = choc::value::createObject("");
    root.addMember("schemaVersion", choc::value::createString("pulp.inspect.screenshot.v1"));
    root.addMember("ok", choc::value::createBool(false));
    root.addMember("status", choc::value::createString(status));
    if (publication)
        root.addMember("session", publication_json(*publication));
    auto error = choc::value::createObject("");
    error.addMember("code", choc::value::createString(code));
    error.addMember("message", choc::value::createString(message));
    if (!error_data_json.empty() && error_data_json != "{}") {
        try {
            error.addMember("data", choc::json::parse(error_data_json));
        } catch (...) {
            error.addMember("data", choc::value::createString(error_data_json));
        }
    }
    root.addMember("error", error);
    std::cout << choc::json::toString(root, false) << "\n";
}

} // namespace

bool decode_inspect_screenshot(std::string_view response_json,
                               InspectScreenshotPayload& payload,
                               std::string& error) {
    payload = {};
    try {
        const auto value = choc::json::parse(response_json);
        if (!value.isObject() || !value.hasObjectMember("mimeType") ||
            !value["mimeType"].isString() || value["mimeType"].getString() != "image/png" ||
            !value.hasObjectMember("width") || !value["width"].isInt64() ||
            !value.hasObjectMember("height") || !value["height"].isInt64() ||
            !value.hasObjectMember("data") || !value["data"].isString()) {
            error = "Capture.screenshot returned a malformed response";
            return false;
        }
        const auto response_width = value["width"].getInt64();
        const auto response_height = value["height"].getInt64();
        if (response_width <= 0 || response_height <= 0 ||
            response_width > std::numeric_limits<std::uint32_t>::max() ||
            response_height > std::numeric_limits<std::uint32_t>::max()) {
            error = "Capture.screenshot returned invalid dimensions";
            return false;
        }
        auto decoded = pulp::runtime::base64_decode(value["data"].getString());
        if (!decoded || decoded->empty()) {
            error = "Capture.screenshot returned invalid PNG data";
            return false;
        }
        const auto metadata = pulp::view::inspect_png_metadata(*decoded);
        if (!metadata.valid) {
            error = "Capture.screenshot returned malformed PNG data";
            return false;
        }
        if (metadata.width != static_cast<std::uint32_t>(response_width) ||
            metadata.height != static_cast<std::uint32_t>(response_height)) {
            error = "Capture.screenshot PNG dimensions do not match its metadata";
            return false;
        }
        payload.png = std::move(*decoded);
        payload.width = metadata.width;
        payload.height = metadata.height;
        return true;
    } catch (...) {
        error = "Capture.screenshot returned invalid JSON";
        return false;
    }
}

int run_inspect_screenshot(
    const pulp::inspect::InspectorDiscoveryRecord& publication,
    const pulp::inspect::InspectorDiscoveryReader& discovery,
    const fs::path& destination,
    bool json_output) {
    using namespace pulp::inspect;
    const InspectorClientTarget target{publication.session_id, publication.instance_id,
                                       publication.publication_id};
    const auto result = request_inspector(std::string(methods::kCaptureScreenshot), "{}",
                                          target, std::chrono::seconds(5), discovery);
    if (!result.succeeded()) {
        const auto unsupported = result.response.error_code == "capability_unavailable" ||
                                 result.response.error_code == "capture_unavailable";
        if (json_output) {
            print_screenshot_json(unsupported ? "unsupported" : "failed", &publication,
                                  result.response.error_code.empty() ? "command_failed"
                                                                     : result.response.error_code,
                                  result.response.params_json,
                                  result.response.error_data_json);
        } else if (unsupported) {
            std::cerr << "Screenshot unsupported";
            if (!result.response.error_code.empty())
                std::cerr << " [" << result.response.error_code << "]";
            std::cerr << ": " << result.response.params_json << "\n";
        } else {
            std::cerr << "Error";
            if (!result.response.error_code.empty())
                std::cerr << " [" << result.response.error_code << "]";
            std::cerr << ": " << result.response.params_json << "\n";
        }
        return unsupported ? 3 : 1;
    }

    InspectScreenshotPayload screenshot;
    std::string error;
    if (!decode_inspect_screenshot(result.response.params_json, screenshot, error)) {
        if (json_output)
            print_screenshot_json("failed", &publication, "invalid_response", error);
        else
            std::cerr << "Error [invalid_response]: " << error << "\n";
        return 1;
    }
    if (!write_inspect_screenshot(destination, screenshot, error)) {
        if (json_output)
            print_screenshot_json("failed", &publication, "write_failed", error);
        else
            std::cerr << "Error [write_failed]: " << error << "\n";
        return 1;
    }
    if (json_output) {
        auto root = choc::value::createObject("");
        root.addMember("schemaVersion",
                       choc::value::createString("pulp.inspect.screenshot.v1"));
        root.addMember("ok", choc::value::createBool(true));
        root.addMember("status", choc::value::createString("captured"));
        root.addMember("session", publication_json(publication));
        root.addMember("path", choc::value::createString(destination.string()));
        root.addMember("mimeType", choc::value::createString("image/png"));
        root.addMember("width", choc::value::createInt64(screenshot.width));
        root.addMember("height", choc::value::createInt64(screenshot.height));
        root.addMember("bytes", choc::value::createInt64(
            static_cast<std::int64_t>(screenshot.png.size())));
        std::cout << choc::json::toString(root, false) << "\n";
    } else {
        std::cout << "Screenshot saved to " << destination.string() << " ("
                  << screenshot.width << "x" << screenshot.height << ", "
                  << screenshot.png.size() << " bytes)\n";
    }
    return 0;
}

int report_inspect_screenshot_selection_failure(std::string_view message,
                                                bool json_output) {
    if (json_output)
        print_screenshot_json("failed", nullptr, "selection_failed", message);
    else
        std::cerr << "Error: " << message << "\n";
    return 1;
}

bool write_inspect_screenshot(const fs::path& destination,
                              const InspectScreenshotPayload& payload,
                              std::string& error) {
    if (destination.empty()) {
        error = "screenshot output path is empty";
        return false;
    }
    std::error_code ec;
    const auto parent = destination.parent_path();
    if (!parent.empty()) {
        fs::create_directories(parent, ec);
        if (ec) {
            error = "could not create screenshot output directory: " + ec.message();
            return false;
        }
    }
    using pulp::runtime::detail::DurableFileCommitOutcome;
    using pulp::runtime::detail::DurableFileReplacement;
    auto replacement = DurableFileReplacement::create(destination);
    if (!replacement) {
        error = "could not create a temporary screenshot beside " + destination.string();
        return false;
    }
    if (!replacement->write_all(std::span<const std::uint8_t>(payload.png))) {
        replacement->cancel();
        error = "could not write temporary screenshot file";
        return false;
    }
    switch (replacement->commit()) {
    case DurableFileCommitOutcome::ReplacedDurably:
        return true;
    case DurableFileCommitOutcome::ReplacedButDirectorySyncFailed:
        error = "replaced screenshot but could not durably sync its output directory";
        return false;
    case DurableFileCommitOutcome::NotReplaced:
        error = "could not atomically replace screenshot output file";
        return false;
    }
    error = "unknown screenshot replacement outcome";
    return false;
}
