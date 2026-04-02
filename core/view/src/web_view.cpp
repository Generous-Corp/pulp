// WebView embedding implementation
// Wraps CHOC's native WebView layer (WKWebView / WebView2 / WebKitGTK) and
// adds a small structured bridge on top for Pulp's Phase 7 interop needs.

#include <pulp/view/web_view.hpp>
#include <pulp/view/asset_manager.hpp>

#include <choc/gui/choc_WebView.h>
#include <choc/text/choc_JSON.h>

#include <atomic>
#include <cctype>
#include <filesystem>
#include <mutex>
#include <optional>
#include <sstream>
#include <unordered_map>

namespace pulp::view {
namespace {

constexpr const char* kPostMessageBinding = "__pulpPostMessage";
constexpr const char* kEvalResultBinding = "__pulpEvalResult";

std::string json_string_literal(const std::string& text) {
    return choc::json::toString(choc::value::createString(text), false);
}

std::string base64_encode(const std::vector<uint8_t>& data) {
    static const char* table = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve((data.size() + 2) / 3 * 4);

    for (size_t i = 0; i < data.size(); i += 3) {
        uint32_t n = static_cast<uint32_t>(data[i]) << 16;
        if (i + 1 < data.size()) n |= static_cast<uint32_t>(data[i + 1]) << 8;
        if (i + 2 < data.size()) n |= static_cast<uint32_t>(data[i + 2]);

        out += table[(n >> 18) & 0x3F];
        out += table[(n >> 12) & 0x3F];
        out += (i + 1 < data.size()) ? table[(n >> 6) & 0x3F] : '=';
        out += (i + 2 < data.size()) ? table[n & 0x3F] : '=';
    }

    return out;
}

choc::value::ValueView unwrap_bridge_argument(const choc::value::ValueView& args) {
    if (args.isArray() && args.size() > 0) {
        return args[0];
    }
    return args;
}

std::string value_to_json(const choc::value::ValueView& value) {
    return choc::json::toString(value, false);
}

std::string normalize_payload_json(const std::string& payload_json) {
    if (payload_json.empty()) {
        return "null";
    }

    try {
        auto parsed = choc::json::parse(payload_json);
        return choc::json::toString(parsed, false);
    } catch (...) {
        return json_string_literal(payload_json);
    }
}

void replace_all(std::string& text, const std::string& from, const std::string& to) {
    if (from.empty()) {
        return;
    }

    size_t pos = 0;
    while ((pos = text.find(from, pos)) != std::string::npos) {
        text.replace(pos, from.size(), to);
        pos += to.size();
    }
}

std::string with_dot_slash(const std::string& path) {
    if (path.rfind("./", 0) == 0 || path.rfind("../", 0) == 0 || path.empty()) {
        return path;
    }
    return "./" + path;
}

std::string strip_leading_slashes(std::string path) {
    while (!path.empty() && path.front() == '/') {
        path.erase(path.begin());
    }
    return path;
}

std::string normalize_resource_path(const std::string& path) {
    auto normalized = strip_leading_slashes(path);
    if (normalized == "." || normalized == "./") {
        return {};
    }
    if (normalized.rfind("./", 0) == 0) {
        normalized.erase(0, 2);
    }
    return normalized;
}

bool path_is_within_root(const std::filesystem::path& root,
                         const std::filesystem::path& candidate) {
    auto root_it = root.begin();
    auto candidate_it = candidate.begin();

    for (; root_it != root.end(); ++root_it, ++candidate_it) {
        if (candidate_it == candidate.end() || *root_it != *candidate_it) {
            return false;
        }
    }

    return true;
}

std::string canonical_data_uri_mime_type(std::string mime_type) {
    if (mime_type == "application/json" || mime_type == "text/json") {
        return "application/json;charset=utf-8";
    }
    return mime_type;
}

void rewrite_html_reference(std::string& html,
                            const std::string& reference_path,
                            const std::string& replacement) {
    if (reference_path.empty()) {
        return;
    }

    const auto dot_slash = with_dot_slash(reference_path);
    replace_all(html, "src=\"" + reference_path + "\"", "src=\"" + replacement + "\"");
    replace_all(html, "src='" + reference_path + "'", "src='" + replacement + "'");
    replace_all(html, "href=\"" + reference_path + "\"", "href=\"" + replacement + "\"");
    replace_all(html, "href='" + reference_path + "'", "href='" + replacement + "'");

    if (dot_slash != reference_path) {
        replace_all(html, "src=\"" + dot_slash + "\"", "src=\"" + replacement + "\"");
        replace_all(html, "src='" + dot_slash + "'", "src='" + replacement + "'");
        replace_all(html, "href=\"" + dot_slash + "\"", "href=\"" + replacement + "\"");
        replace_all(html, "href='" + dot_slash + "'", "href='" + replacement + "'");
    }
}

bool decode_message_value(const choc::value::ValueView& raw_value, WebViewMessage& message) {
    const auto value = unwrap_bridge_argument(raw_value);
    if (!value.isObject() || !value.hasObjectMember("type")) {
        return false;
    }

    message.type = std::string(value["type"].toString());
    message.id = value.hasObjectMember("id") ? std::string(value["id"].toString()) : std::string{};
    message.payload_json = value.hasObjectMember("payload")
        ? value_to_json(value["payload"])
        : "null";
    return true;
}

std::string make_bridge_delivery_script(const WebViewMessage& message) {
    const auto envelope_json = encode_webview_message_json(message);
    return "(function(){"
           "const __pulpMessage=" + envelope_json + ";"
           "if (typeof window !== 'undefined') {"
             "if (typeof window.dispatchEvent === 'function' && typeof CustomEvent === 'function') {"
               "window.dispatchEvent(new CustomEvent('pulp-message', { detail: __pulpMessage }));"
             "}"
             "if (window.pulp && typeof window.pulp.__deliverFromNative === 'function') {"
               "window.pulp.__deliverFromNative(__pulpMessage);"
             "}"
           "}"
           "})();";
}

std::string make_bridge_response_json(const std::string& id,
                                      const std::string& payload_json,
                                      const std::string& error) {
    std::ostringstream out;
    out << "{";
    out << "\"id\":" << json_string_literal(id) << ",";
    out << "\"ok\":" << (error.empty() ? "true" : "false") << ",";
    if (error.empty()) {
        out << "\"payload\":" << normalize_payload_json(payload_json);
    } else {
        out << "\"error\":" << json_string_literal(error);
    }
    out << "}";
    return out.str();
}

} // namespace

std::string encode_webview_message_json(const WebViewMessage& message) {
    std::ostringstream out;
    out << "{";
    out << "\"type\":" << json_string_literal(message.type) << ",";
    out << "\"payload\":" << normalize_payload_json(message.payload_json);
    if (!message.id.empty()) {
        out << ",\"id\":" << json_string_literal(message.id);
    }
    out << "}";
    return out.str();
}

bool decode_webview_message_json(const std::string& json, WebViewMessage& message) {
    try {
        auto parsed = choc::json::parse(json);
        return decode_message_value(parsed, message);
    } catch (...) {
        return false;
    }
}

std::string make_webview_bridge_bootstrap_script() {
    return R"JS((function(){
  if (typeof window === 'undefined') return;
  const listeners = new Map();
  const deliver = function(message) {
    if (!message || typeof message.type !== 'string') return;
    const callbacks = listeners.get(message.type);
    if (!callbacks) return;
    for (const cb of callbacks) cb(message);
  };

  const api = window.pulp || {};
  api.__bridgeVersion = 1;
  api.on = function(type, callback) {
    if (!listeners.has(type)) listeners.set(type, []);
    listeners.get(type).push(callback);
    return function unsubscribe() {
      const callbacks = listeners.get(type) || [];
      listeners.set(type, callbacks.filter(cb => cb !== callback));
    };
  };
  api.postMessage = function(type, payload, id) {
    if (typeof window.__pulpPostMessage !== 'function') {
      throw new Error('Pulp native postMessage bridge is not available');
    }
    return window.__pulpPostMessage({
      type: type,
      payload: payload === undefined ? null : payload,
      id: id || ''
    });
  };
  api.__deliverFromNative = deliver;
  window.pulp = api;
})();)JS";
}

std::string make_webview_eval_script(const std::string& request_id,
                                     const std::string& js_source) {
    return "(function(){"
           "const __pulpEvalId=" + json_string_literal(request_id) + ";"
           "const __pulpEvalSource=" + json_string_literal(js_source) + ";"
           "Promise.resolve()"
             ".then(function(){ return globalThis.eval(__pulpEvalSource); })"
             ".then(function(result){"
               "if (typeof window !== 'undefined' && typeof window." + std::string(kEvalResultBinding) + " === 'function') {"
                 "window." + std::string(kEvalResultBinding) + "({ id: __pulpEvalId, ok: true, result: result === undefined ? null : result });"
               "}"
             "})"
             ".catch(function(error){"
               "if (typeof window !== 'undefined' && typeof window." + std::string(kEvalResultBinding) + " === 'function') {"
                 "window." + std::string(kEvalResultBinding) + "({ id: __pulpEvalId, ok: false, error: String(error) });"
               "}"
             "});"
           "})();";
}

std::string guess_webview_mime_type(const std::string& path) {
    auto dot = path.find_last_of('.');
    std::string ext = dot == std::string::npos ? std::string{} : path.substr(dot);
    for (auto& c : ext) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }

    if (ext == ".html" || ext == ".htm") return "text/html";
    if (ext == ".js" || ext == ".mjs") return "text/javascript";
    if (ext == ".css") return "text/css";
    if (ext == ".json") return "application/json";
    if (ext == ".svg") return "image/svg+xml";
    if (ext == ".png") return "image/png";
    if (ext == ".jpg" || ext == ".jpeg") return "image/jpeg";
    if (ext == ".gif") return "image/gif";
    if (ext == ".webp") return "image/webp";
    if (ext == ".woff2") return "font/woff2";
    if (ext == ".woff") return "font/woff";
    if (ext == ".ttf") return "font/ttf";
    return "application/octet-stream";
}

std::string make_webview_data_uri(const std::string& mime_type,
                                  const std::vector<uint8_t>& bytes) {
    return "data:" + canonical_data_uri_mime_type(
                mime_type.empty() ? "application/octet-stream" : mime_type)
         + ";base64," + base64_encode(bytes);
}

std::string load_webview_embedded_text(const std::string& embedded_name) {
    auto blob = AssetManager::instance().load_blob_embedded(embedded_name);
    if (!blob.valid()) {
        return {};
    }

    return std::string(blob.data.begin(), blob.data.end());
}

std::string make_webview_offline_html(const std::string& html,
                                      const std::vector<WebViewEmbeddedAsset>& assets) {
    std::string result = html;

    for (const auto& asset : assets) {
        if (asset.reference_path.empty() || asset.embedded_name.empty()) {
            continue;
        }

        auto blob = AssetManager::instance().load_blob_embedded(asset.embedded_name);
        if (!blob.valid()) {
            continue;
        }

        const auto mime = asset.mime_type.empty()
            ? guess_webview_mime_type(asset.reference_path)
            : asset.mime_type;
        const auto uri = make_webview_data_uri(mime, blob.data);
        rewrite_html_reference(result, asset.reference_path, uri);
    }

    return result;
}

std::string make_webview_offline_html_from_embedded(
    const std::string& html_embedded_name,
    const std::vector<WebViewEmbeddedAsset>& assets) {
    auto html = load_webview_embedded_text(html_embedded_name);
    if (html.empty()) {
        return {};
    }

    return make_webview_offline_html(html, assets);
}

std::optional<WebViewOptions::Resource> fetch_webview_embedded_resource(
    const std::string& path,
    const std::string& html_embedded_name,
    const std::vector<WebViewEmbeddedAsset>& assets) {
    const auto normalized_path = normalize_resource_path(path);

    if (normalized_path.empty() || normalized_path == "index.html") {
        auto blob = AssetManager::instance().load_blob_embedded(html_embedded_name);
        if (!blob.valid()) {
            return std::nullopt;
        }

        return WebViewOptions::Resource{
            .data = std::move(blob.data),
            .mime_type = "text/html",
        };
    }

    for (const auto& asset : assets) {
        const auto reference_path = normalize_resource_path(asset.reference_path);
        if (reference_path != normalized_path || asset.embedded_name.empty()) {
            continue;
        }

        auto blob = AssetManager::instance().load_blob_embedded(asset.embedded_name);
        if (!blob.valid()) {
            return std::nullopt;
        }

        return WebViewOptions::Resource{
            .data = std::move(blob.data),
            .mime_type = asset.mime_type.empty()
                ? guess_webview_mime_type(reference_path)
                : asset.mime_type,
        };
    }

    return std::nullopt;
}

WebViewOptions::FetchResource make_webview_embedded_resource_fetcher(
    std::string html_embedded_name,
    std::vector<WebViewEmbeddedAsset> assets) {
    return [html_embedded_name = std::move(html_embedded_name),
            assets = std::move(assets)](const std::string& path)
            -> std::optional<WebViewOptions::Resource> {
        return fetch_webview_embedded_resource(path, html_embedded_name, assets);
    };
}

std::optional<WebViewOptions::Resource> fetch_webview_directory_resource(
    const std::string& path,
    const std::filesystem::path& root_directory,
    std::string index_filename) {
    namespace fs = std::filesystem;

    std::error_code ec;
    auto canonical_root = fs::weakly_canonical(root_directory, ec);
    if (ec || canonical_root.empty() || !fs::is_directory(canonical_root)) {
        return std::nullopt;
    }

    auto normalized_path = normalize_resource_path(path);
    if (normalized_path.empty() || normalized_path == "index.html") {
        normalized_path = index_filename.empty() ? "index.html" : std::move(index_filename);
    }

    auto resolved = fs::weakly_canonical(canonical_root / fs::path(normalized_path), ec);
    if (ec || resolved.empty() || !path_is_within_root(canonical_root, resolved)) {
        return std::nullopt;
    }

    auto blob = AssetManager::instance().load_blob(resolved.string());
    if (!blob.valid()) {
        return std::nullopt;
    }

    return WebViewOptions::Resource{
        .data = std::move(blob.data),
        .mime_type = guess_webview_mime_type(normalized_path),
    };
}

WebViewOptions::FetchResource make_webview_directory_resource_fetcher(
    std::filesystem::path root_directory,
    std::string index_filename) {
    return [root_directory = std::move(root_directory),
            index_filename = std::move(index_filename)](const std::string& path)
            -> std::optional<WebViewOptions::Resource> {
        return fetch_webview_directory_resource(path, root_directory, index_filename);
    };
}

class ChocWebViewPanel : public WebViewPanel {
public:
    explicit ChocWebViewPanel(const WebViewOptions& options) {
        choc::ui::WebView::Options choc_opts;
        choc_opts.enableDebugMode = options.enable_debug;
        choc_opts.enableDebugInspector = options.enable_debug_inspector;
        choc_opts.transparentBackground = options.transparent_background;
        choc_opts.acceptsFirstMouseClick = options.accept_first_click;
        choc_opts.customUserAgent = options.custom_user_agent;
        choc_opts.webviewIsReady = [this](choc::ui::WebView&) {
            ready_seen_.store(true, std::memory_order_release);

            ReadyHandler handler;
            {
                std::lock_guard lock(mutex_);
                handler = ready_handler_;
            }

            if (handler) {
                handler();
            }
        };
        if (options.fetch_resource) {
            choc_opts.fetchResource = [fetch = options.fetch_resource](const std::string& path)
                -> std::optional<choc::ui::WebView::Options::Resource> {
                if (auto resource = fetch(path)) {
                    return choc::ui::WebView::Options::Resource{
                        std::string_view(reinterpret_cast<const char*>(resource->data.data()),
                                         resource->data.size()),
                        resource->mime_type,
                    };
                }
                return std::nullopt;
            };
            choc_opts.customSchemeURI = options.custom_scheme_uri.empty()
                ? "pulp://app"
                : options.custom_scheme_uri;
        }

        webview_ = std::make_unique<choc::ui::WebView>(choc_opts);
        if (webview_) {
            webview_->addInitScript(make_webview_bridge_bootstrap_script());
        }
        install_internal_bindings();
    }

    bool is_ready() const override {
        return ready_seen_.load(std::memory_order_acquire) || (webview_ && webview_->isReady());
    }

    void set_ready_handler(ReadyHandler handler) override {
        bool should_call_now = false;
        {
            std::lock_guard lock(mutex_);
            ready_handler_ = std::move(handler);
            should_call_now = static_cast<bool>(ready_handler_) && is_ready();
        }

        if (should_call_now) {
            ReadyHandler callback;
            {
                std::lock_guard lock(mutex_);
                callback = ready_handler_;
            }

            if (callback) {
                callback();
            }
        }
    }

    NativeViewHandle native_handle() override {
        if (!webview_ || !webview_->loadedOK()) return nullptr;
        return webview_->getViewHandle();
    }

    void navigate(const std::string& url) override {
        if (webview_) webview_->navigate(url);
    }

    void set_html(const std::string& html) override {
        if (!webview_) return;
        webview_->setHTML(html);
    }

    void evaluate_js(const std::string& js) override {
        if (webview_) webview_->evaluateJavascript(js);
    }

    void evaluate_js(const std::string& js, EvalCallback callback) override {
        if (!webview_ || !callback) {
            return;
        }

        const auto request_id = next_eval_request_id();
        {
            std::lock_guard lock(mutex_);
            pending_eval_[request_id] = std::move(callback);
        }

        webview_->evaluateJavascript(make_webview_eval_script(request_id, js));
    }

    void bind(const std::string& name, JsCallback callback) override {
        if (!webview_) return;
        webview_->bind(name, [cb = std::move(callback)](const choc::value::ValueView& args) {
            auto json = value_to_json(args);
            auto result_str = cb(json);
            if (result_str.empty()) return choc::value::Value();
            try {
                return choc::json::parse(result_str);
            } catch (...) {
                return choc::value::createString(result_str);
            }
        });
    }

    void set_message_handler(MessageHandler handler) override {
        std::lock_guard lock(mutex_);
        message_handler_ = std::move(handler);
    }

    void post_message(const WebViewMessage& message) override {
        if (!webview_) return;
        webview_->evaluateJavascript(make_bridge_delivery_script(message));
    }

    void set_size(uint32_t /*width*/, uint32_t /*height*/) override {
        // CHOC WebView sizing is handled by the parent view/window.
        // The native handle is added as a child view and resized by the parent.
    }

private:
    std::string next_eval_request_id() {
        auto value = eval_counter_.fetch_add(1, std::memory_order_relaxed);
        return "eval-" + std::to_string(value);
    }

    void install_internal_bindings() {
        if (!webview_) return;

        webview_->bind(kPostMessageBinding, [this](const choc::value::ValueView& args) {
            WebViewMessage message;
            if (!decode_message_value(args, message)) {
                return choc::json::parse(R"({"ok":false,"error":"Invalid Pulp WebView message"})");
            }

            MessageHandler handler;
            {
                std::lock_guard lock(mutex_);
                handler = message_handler_;
            }

            if (!handler) {
                return choc::json::parse(make_bridge_response_json(message.id, "null", ""));
            }

            try {
                return choc::json::parse(make_bridge_response_json(message.id, handler(message), ""));
            } catch (const std::exception& e) {
                return choc::json::parse(make_bridge_response_json(message.id, "", e.what()));
            } catch (...) {
                return choc::json::parse(make_bridge_response_json(message.id, "", "Unknown Pulp WebView bridge error"));
            }
        });

        webview_->bind(kEvalResultBinding, [this](const choc::value::ValueView& args) {
            const auto value = unwrap_bridge_argument(args);
            if (!value.isObject() || !value.hasObjectMember("id")) {
                return choc::value::Value();
            }

            const auto id = std::string(value["id"].toString());
            EvalCallback callback;
            {
                std::lock_guard lock(mutex_);
                auto it = pending_eval_.find(id);
                if (it == pending_eval_.end()) {
                    return choc::value::Value();
                }
                callback = std::move(it->second);
                pending_eval_.erase(it);
            }

            const bool ok = value.hasObjectMember("ok") ? value["ok"].getWithDefault(false) : false;
            const auto result_json = value.hasObjectMember("result")
                ? value_to_json(value["result"])
                : std::string("null");
            const auto error = value.hasObjectMember("error")
                ? std::string(value["error"].toString())
                : std::string{};

            if (callback) {
                callback(ok ? result_json : "null", ok ? "" : error);
            }

            return choc::value::Value();
        });
    }

    std::unique_ptr<choc::ui::WebView> webview_;
    std::mutex mutex_;
    std::unordered_map<std::string, EvalCallback> pending_eval_;
    MessageHandler message_handler_;
    ReadyHandler ready_handler_;
    std::atomic<uint64_t> eval_counter_{1};
    std::atomic<bool> ready_seen_{false};
};

std::unique_ptr<WebViewPanel> WebViewPanel::create(const WebViewOptions& options) {
    auto panel = std::make_unique<ChocWebViewPanel>(options);
    if (!panel->native_handle()) return nullptr;
    return panel;
}

} // namespace pulp::view
