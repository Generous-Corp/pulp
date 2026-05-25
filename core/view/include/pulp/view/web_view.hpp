#pragma once

// WebView embedding for Pulp
// Wraps CHOC's WebView (ISC license) to provide embedded web content
// inside plugin/app UIs — for Monaco editors, docs panels, custom UIs, etc.
//
// Usage:
//   auto wv = WebViewPanel::create(options);
//   wv->navigate("https://example.com");
//   wv->set_html("<h1>Hello</h1>");
//   wv->evaluate_js("document.title");
//   wv->bind("myFunc", [](const std::string& args) { ... });

#include <string>
#include <functional>
#include <filesystem>
#include <memory>
#include <optional>
#include <vector>

namespace pulp::view {

// Platform-native handle (NSView*, HWND, etc.)
using NativeViewHandle = void*;

struct WebViewOptions {
    struct Resource {
        std::vector<uint8_t> data;
        std::string mime_type;
    };
    using FetchResource = std::function<std::optional<Resource>(const std::string& path)>;

    bool enable_debug = false;         ///< Enable DevTools / Web Inspector
    bool enable_debug_inspector = false; ///< Request a separate inspector window when supported
    bool transparent_background = false;
    bool accept_first_click = true;    ///< macOS: first click on unfocused view is input
    std::string initial_html;          ///< Optional placeholder HTML shown before navigate()/set_html()
    std::string custom_user_agent;     ///< Override User-Agent string
    FetchResource fetch_resource;      ///< Optional custom resource server
    std::string custom_scheme_uri;     ///< Optional home URI for fetch_resource, e.g. pulp://app
};

// Simple structured message envelope for native <-> WebView communication.
// payload_json must contain serialized JSON (object, array, string, number, bool, or null).
struct WebViewMessage {
    std::string type;
    std::string payload_json = "null";
    std::string id;
};

// Offline resource description for embedded WebView content. reference_path is
// the URL used inside HTML (for example "app.js" or "./styles.css"), while
// embedded_name is the key previously registered with AssetManager.
struct WebViewEmbeddedAsset {
    std::string reference_path;
    std::string embedded_name;
    std::string mime_type;
};

/// Encode a structured WebView message to canonical JSON.
std::string encode_webview_message_json(const WebViewMessage& message);

/// Decode a structured WebView message from JSON. Accepts either an object
/// directly or a single-element argument array produced by some JS bridges.
bool decode_webview_message_json(const std::string& json, WebViewMessage& message);

/// JavaScript helper that installs a small `window.pulp` bridge on a page.
/// Pages can append this script to their bundled HTML to use
/// `window.pulp.postMessage(type, payload, id)` and receive
/// `window.pulp.on(type, callback)` events from native code.
std::string make_webview_bridge_bootstrap_script();

/// JavaScript wrapper that evaluates user code asynchronously and reports the
/// result back through the internal `__pulpEvalResult` bridge.
std::string make_webview_eval_script(const std::string& request_id,
                                     const std::string& js_source);

/// Best-effort MIME type guess for an HTML-linked asset path.
std::string guess_webview_mime_type(const std::string& path);

/// Create a `data:` URI for an embedded WebView asset.
std::string make_webview_data_uri(const std::string& mime_type,
                                  const std::vector<uint8_t>& bytes);

/// Load a UTF-8 text asset previously registered with AssetManager.
std::string load_webview_embedded_text(const std::string& embedded_name);

/// Rewrite simple src/href references in HTML to offline `data:` URIs backed
/// by embedded assets.
std::string make_webview_offline_html(const std::string& html,
                                      const std::vector<WebViewEmbeddedAsset>& assets);

/// Load embedded HTML and rewrite its referenced assets for offline use.
std::string make_webview_offline_html_from_embedded(
    const std::string& html_embedded_name,
    const std::vector<WebViewEmbeddedAsset>& assets);

/// Fetch a single embedded resource for a WebView custom resource server.
std::optional<WebViewOptions::Resource> fetch_webview_embedded_resource(
    const std::string& path,
    const std::string& html_embedded_name,
    const std::vector<WebViewEmbeddedAsset>& assets);

/// Create a resource callback that serves embedded HTML/assets through CHOC's
/// fetchResource/customSchemeURI path.
WebViewOptions::FetchResource make_webview_embedded_resource_fetcher(
    std::string html_embedded_name,
    std::vector<WebViewEmbeddedAsset> assets);

/// Fetch a single file-system resource for a WebView custom resource server.
/// The root directory is treated as the web origin root, and requests are
/// normalized so callers cannot escape it with `..` path segments.
std::optional<WebViewOptions::Resource> fetch_webview_directory_resource(
    const std::string& path,
    const std::filesystem::path& root_directory,
    std::string index_filename = "index.html");

/// Create a resource callback that serves files from a directory through
/// CHOC's fetchResource/customSchemeURI path. Useful for local development
/// flows such as a separately-built Monaco distribution.
WebViewOptions::FetchResource make_webview_directory_resource_fetcher(
    std::filesystem::path root_directory,
    std::string index_filename = "index.html");

// Embedded web view component.
// Create with WebViewPanel::create(), then use native_handle() to embed the
// platform WebView in a WindowHost/PluginViewHost that actually exposes and
// accepts native child views. WebView backend availability and host embedding
// availability are separate capabilities; callers must check the host handle
// and attach_native_child_view() result.
class WebViewPanel {
public:
    using JsCallback = std::function<std::string(const std::string& args_json)>;
    using EvalCallback = std::function<void(const std::string& result_json,
                                            const std::string& error)>;
    using MessageHandler = std::function<std::string(const WebViewMessage& message)>;
    using ReadyHandler = std::function<void()>;

    static std::unique_ptr<WebViewPanel> create(const WebViewOptions& options = {});

    virtual ~WebViewPanel() = default;

    // Whether the underlying native WebView is ready for navigation / JS
    // evaluation. Some platforms become ready asynchronously after creation.
    virtual bool is_ready() const = 0;

    // Set a callback fired once the underlying native WebView becomes ready.
    // If the backend is already ready when this is called, the handler runs
    // immediately.
    virtual void set_ready_handler(ReadyHandler handler) = 0;

    // Get the native view handle for embedding (NSView*, HWND, etc.). This is
    // the child surface handle only; successful embedding also requires a host
    // whose native child-view attach contract returns true.
    virtual NativeViewHandle native_handle() = 0;

    // Navigate to a URL
    virtual void navigate(const std::string& url) = 0;

    // Load HTML content directly
    virtual void set_html(const std::string& html) = 0;

    // Execute JavaScript in the webview context
    virtual void evaluate_js(const std::string& js) = 0;

    // Execute JavaScript and receive its eventual JSON-serializable result.
    // Errors are surfaced via the error string.
    virtual void evaluate_js(const std::string& js, EvalCallback callback) = 0;

    // Bind a C++ function callable from JavaScript as window.<name>(args)
    // The callback receives JSON args string and returns JSON result string
    virtual void bind(const std::string& name, JsCallback callback) = 0;

    // Register a structured JSON message handler for JavaScript callers using
    // the built-in `__pulpPostMessage(...)` bridge.
    virtual void set_message_handler(MessageHandler handler) = 0;

    // Deliver a structured message to the current page. If the page has
    // installed `make_webview_bridge_bootstrap_script()`, listeners registered
    // via `window.pulp.on(...)` will receive it. A DOM CustomEvent named
    // `pulp-message` is also dispatched.
    virtual void post_message(const WebViewMessage& message) = 0;

    // Resize the webview
    virtual void set_size(uint32_t width, uint32_t height) = 0;
};

} // namespace pulp::view
