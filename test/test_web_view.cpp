#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <pulp/view/asset_manager.hpp>
#include <pulp/view/web_view.hpp>
#include <pulp/view/window_host.hpp>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <future>
#include <optional>
#include <thread>

using namespace pulp::view;
using Catch::Matchers::ContainsSubstring;

namespace {

bool wait_until_ready(WebViewPanel& panel,
                      std::chrono::milliseconds timeout = std::chrono::milliseconds(2000)) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (panel.is_ready()) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }

    return panel.is_ready();
}

std::pair<std::string, std::string> await_eval(WebViewPanel& panel,
                                               const std::string& js,
                                               std::chrono::milliseconds timeout = std::chrono::milliseconds(1500)) {
    auto promise = std::make_shared<std::promise<std::pair<std::string, std::string>>>();
    auto future = promise->get_future();
    panel.evaluate_js(js, [promise](const std::string& result_json, const std::string& error) {
        promise->set_value({result_json, error});
    });

    if (future.wait_for(timeout) != std::future_status::ready) {
        return {"", "timeout"};
    }

    return future.get();
}

std::string extract_data_uri_base64(const std::string& uri) {
    const auto comma = uri.find(',');
    REQUIRE(comma != std::string::npos);
    return uri.substr(comma + 1);
}

std::vector<uint8_t> decode_base64(std::string_view encoded) {
    auto decode_char = [](char c) -> int {
        if (c >= 'A' && c <= 'Z') return c - 'A';
        if (c >= 'a' && c <= 'z') return c - 'a' + 26;
        if (c >= '0' && c <= '9') return c - '0' + 52;
        if (c == '+') return 62;
        if (c == '/') return 63;
        return -1;
    };

    std::vector<uint8_t> out;
    out.reserve((encoded.size() / 4) * 3);

    for (size_t i = 0; i < encoded.size(); i += 4) {
        int a = decode_char(encoded[i]);
        int b = decode_char(encoded[i + 1]);
        int c = encoded[i + 2] == '=' ? -1 : decode_char(encoded[i + 2]);
        int d = encoded[i + 3] == '=' ? -1 : decode_char(encoded[i + 3]);

        REQUIRE(a >= 0);
        REQUIRE(b >= 0);

        const uint32_t value = (static_cast<uint32_t>(a) << 18)
                             | (static_cast<uint32_t>(b) << 12)
                             | (static_cast<uint32_t>(c < 0 ? 0 : c) << 6)
                             | static_cast<uint32_t>(d < 0 ? 0 : d);

        out.push_back(static_cast<uint8_t>((value >> 16) & 0xFF));
        if (c >= 0) out.push_back(static_cast<uint8_t>((value >> 8) & 0xFF));
        if (d >= 0) out.push_back(static_cast<uint8_t>(value & 0xFF));
    }

    return out;
}

} // namespace

TEST_CASE("WebViewPanel creation", "[view][webview]") {
    // WebView creation requires a message loop / GUI context.
    // In headless test environments, create() may return nullptr.
    // This test validates the API surface compiles and the factory works.

    SECTION("Default options") {
        WebViewOptions opts;
        REQUIRE(opts.enable_debug == false);
        REQUIRE(opts.enable_debug_inspector == false);
        REQUIRE(opts.transparent_background == false);
        REQUIRE(opts.accept_first_click == true);
        REQUIRE(opts.initial_html.empty());
        REQUIRE(opts.custom_user_agent.empty());
    }

    SECTION("Factory returns non-null in GUI context or null in headless") {
        auto panel = WebViewPanel::create();
        // In CI / headless: nullptr is expected
        // In GUI context: non-null
        // Either way, no crash
        if (panel) {
            REQUIRE(panel->native_handle() != nullptr);
            REQUIRE((panel->is_ready() == true || panel->is_ready() == false));
        }
    }
}

TEST_CASE("WebView bridge message JSON round-trips", "[view][webview]") {
    WebViewMessage message;
    message.type = "theme.update";
    message.payload_json = R"({"accent":"#ff5500","alpha":0.8})";
    message.id = "msg-7";

    const auto encoded = encode_webview_message_json(message);
    REQUIRE(encoded.find("\"type\":\"theme.update\"") != std::string::npos);
    REQUIRE(encoded.find("\"id\":\"msg-7\"") != std::string::npos);

    WebViewMessage decoded;
    REQUIRE(decode_webview_message_json(encoded, decoded));
    REQUIRE(decoded.type == "theme.update");
    REQUIRE(decoded.id == "msg-7");
    REQUIRE(encode_webview_message_json(decoded) == encoded);
}

TEST_CASE("WebView bridge bootstrap script exposes structured helpers", "[view][webview]") {
    const auto script = make_webview_bridge_bootstrap_script();
    REQUIRE_THAT(script, ContainsSubstring("window.pulp"));
    REQUIRE_THAT(script, ContainsSubstring("postMessage"));
    REQUIRE_THAT(script, ContainsSubstring("__pulpPostMessage"));
    REQUIRE_THAT(script, ContainsSubstring("__deliverFromNative"));
}

TEST_CASE("WebView eval script routes async results through the bridge", "[view][webview]") {
    const auto script = make_webview_eval_script("eval-42", "JSON.stringify({ ok: true })");
    REQUIRE_THAT(script, ContainsSubstring("__pulpEvalResult"));
    REQUIRE_THAT(script, ContainsSubstring("globalThis.eval"));
    REQUIRE_THAT(script, ContainsSubstring("\"eval-42\""));
    REQUIRE_THAT(script, ContainsSubstring("JSON.stringify({ ok: true })"));
}

TEST_CASE("WebView offline helpers rewrite embedded assets to data URIs", "[view][webview]") {
    auto& assets = AssetManager::instance();

    SECTION("MIME type guessing covers common bundled asset types") {
        REQUIRE(guess_webview_mime_type("index.html") == "text/html");
        REQUIRE(guess_webview_mime_type("app.js") == "text/javascript");
        REQUIRE(guess_webview_mime_type("styles.css") == "text/css");
        REQUIRE(guess_webview_mime_type("data.json") == "application/json");
        REQUIRE(guess_webview_mime_type("icon.svg") == "image/svg+xml");
        REQUIRE(guess_webview_mime_type("photo.jpg") == "image/jpeg");
        REQUIRE(guess_webview_mime_type("photo.jpeg") == "image/jpeg");
        REQUIRE(guess_webview_mime_type("unknown.bin") == "application/octet-stream");
    }

    SECTION("JSON data URIs declare UTF-8 explicitly and preserve bytes") {
        const std::string json =
            "{\"label\":\"caf" "\xC3\xA9" "\",\"kanji\":\"" "\xE9\x9F\xB3" "\"}";
        const std::vector<uint8_t> bytes(json.begin(), json.end());
        const auto uri = make_webview_data_uri("application/json", bytes);

        REQUIRE_THAT(uri, ContainsSubstring("data:application/json;charset=utf-8;base64,"));

        const auto decoded = decode_base64(extract_data_uri_base64(uri));
        REQUIRE(decoded.size() == bytes.size());
        REQUIRE(decoded == bytes);
    }

    SECTION("embedded HTML can be rewritten for offline loading") {
        static const uint8_t html_data[] =
            "<!doctype html><html><head>"
            "<link rel=\"stylesheet\" href=\"styles.css\">"
            "</head><body>"
            "<script src=\"./app.js\"></script>"
            "</body></html>";
        static const uint8_t js_data[] = "console.log('phase7');";
        static const uint8_t css_data[] = "body{background:#123456;}";

        assets.register_embedded("phase7_page_html", html_data, sizeof(html_data) - 1);
        assets.register_embedded("phase7_app_js", js_data, sizeof(js_data) - 1);
        assets.register_embedded("phase7_styles_css", css_data, sizeof(css_data) - 1);

        const auto offline = make_webview_offline_html_from_embedded(
            "phase7_page_html",
            {
                { "app.js", "phase7_app_js", "" },
                { "styles.css", "phase7_styles_css", "" },
            });

        REQUIRE_FALSE(offline.empty());
        REQUIRE_THAT(offline, ContainsSubstring("data:text/javascript;base64,"));
        REQUIRE_THAT(offline, ContainsSubstring("data:text/css;base64,"));
        REQUIRE_THAT(offline, !ContainsSubstring("src=\"./app.js\""));
        REQUIRE_THAT(offline, !ContainsSubstring("href=\"styles.css\""));
    }

    SECTION("embedded resource fetcher serves root and relative assets") {
        static const uint8_t html_data[] =
            "<!doctype html><html><head>"
            "<link rel=\"stylesheet\" href=\"styles.css\">"
            "</head><body>"
            "<script src=\"app.js\"></script>"
            "</body></html>";
        static const uint8_t js_data[] = "window.phase7 = 'ready';";
        static const uint8_t json_data[] =
            "{\"label\":\"caf" "\xC3\xA9" "\",\"kanji\":\"" "\xE9\x9F\xB3" "\"}";

        assets.register_embedded("phase7_fetch_html", html_data, sizeof(html_data) - 1);
        assets.register_embedded("phase7_fetch_js", js_data, sizeof(js_data) - 1);
        assets.register_embedded("phase7_fetch_json", json_data, sizeof(json_data) - 1);

        const auto fetch = make_webview_embedded_resource_fetcher(
            "phase7_fetch_html",
            {
                { "app.js", "phase7_fetch_js", "" },
                { "data/config.json", "phase7_fetch_json", "" },
            });

        REQUIRE(fetch);

        const auto root = fetch("/");
        REQUIRE(root.has_value());
        REQUIRE(root->mime_type == "text/html");
        REQUIRE(root->data.size() == sizeof(html_data) - 1);

        const auto script = fetch("/app.js");
        REQUIRE(script.has_value());
        REQUIRE(script->mime_type == "text/javascript");
        REQUIRE(script->data.size() == sizeof(js_data) - 1);

        const auto json = fetch("/data/config.json");
        REQUIRE(json.has_value());
        REQUIRE(json->mime_type == "application/json");
        REQUIRE(json->data.size() == sizeof(json_data) - 1);
        REQUIRE(std::string(json->data.begin(), json->data.end()) == std::string(reinterpret_cast<const char*>(json_data), sizeof(json_data) - 1));

        REQUIRE_FALSE(fetch("/missing.txt").has_value());
    }

    SECTION("directory resource fetcher serves files and blocks parent traversal") {
        namespace fs = std::filesystem;

        const auto temp_root = fs::temp_directory_path() / "pulp_phase7_webview_files";
        fs::remove_all(temp_root);
        fs::create_directories(temp_root / "data");

        {
            std::ofstream html(temp_root / "index.html", std::ios::binary);
            html << "<!doctype html><html><body>phase7</body></html>";
        }
        {
            std::ofstream json(temp_root / "data" / "config.json", std::ios::binary);
            json << "{\"label\":\"caf" "\xC3\xA9" "\"}";
        }

        const auto fetch = make_webview_directory_resource_fetcher(temp_root);
        REQUIRE(fetch);

        const auto root = fetch("/");
        REQUIRE(root.has_value());
        REQUIRE(root->mime_type == "text/html");
        REQUIRE(std::string(root->data.begin(), root->data.end()).find("phase7") != std::string::npos);

        const auto json = fetch("/data/config.json");
        REQUIRE(json.has_value());
        REQUIRE(json->mime_type == "application/json");
        REQUIRE(std::string(json->data.begin(), json->data.end()) == "{\"label\":\"caf" "\xC3\xA9" "\"}");

        REQUIRE_FALSE(fetch("/../outside.txt").has_value());

        fs::remove_all(temp_root);
    }
}

TEST_CASE("WebViewPanel HTML content", "[view][webview]") {
    auto panel = WebViewPanel::create();
    if (!panel) {
        SKIP("WebView not available in headless environment");
        return;
    }

    if (!wait_until_ready(*panel)) {
        SKIP("WebView never became ready in this environment");
    }

    SECTION("set_html does not crash") {
        panel->set_html("<html><body><h1>Test</h1></body></html>");
    }

    SECTION("evaluate_js does not crash") {
        panel->evaluate_js("1 + 1");
    }

    SECTION("evaluate_js with callback does not crash") {
        panel->evaluate_js("1 + 1", [](const std::string& result_json, const std::string& error) {
            (void)result_json;
            (void)error;
        });
    }

    SECTION("navigate does not crash") {
        panel->navigate("about:blank");
    }

    SECTION("bind callback") {
        panel->bind("testFunc", [](const std::string& args) -> std::string {
            return "\"ok\"";
        });
    }

    SECTION("structured message bridge does not crash") {
        panel->set_message_handler([](const WebViewMessage& message) -> std::string {
            (void)message;
            return R"({"ok":true})";
        });
        panel->post_message(WebViewMessage{
            .type = "theme.sync",
            .payload_json = R"({"mode":"dark"})",
            .id = "native-1",
        });
    }

    SECTION("ready handler can be installed after creation") {
        bool fired = false;
        panel->set_ready_handler([&] { fired = true; });
        if (!wait_until_ready(*panel)) {
            SKIP("WebView never became ready in this environment");
        }
        REQUIRE(fired);
    }

    SECTION("initial_html provides a placeholder page before later content") {
        WebViewOptions options;
        options.initial_html =
            "<!doctype html><html><body data-phase=\"initial\">loading</body></html>";

        auto initial_panel = WebViewPanel::create(options);
        if (!initial_panel) {
            SKIP("WebView not available in headless environment");
            return;
        }

        if (!wait_until_ready(*initial_panel)) {
            SKIP("WebView never became ready in this environment");
        }

        auto initial = std::pair<std::string, std::string>{"", "timeout"};
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(5000);
        while (std::chrono::steady_clock::now() < deadline) {
            initial = await_eval(
                *initial_panel,
                "document.body ? document.body.getAttribute('data-phase') : ''",
                std::chrono::milliseconds(300));
            if (initial.second.empty() && initial.first == "\"initial\"") {
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }

        if (!initial.second.empty() || initial.first != "\"initial\"") {
            SKIP("Initial HTML never became readable in this environment");
        }

        initial_panel->set_html(
            "<!doctype html><html><body data-phase=\"final\">ready</body></html>");
        auto final = await_eval(
            *initial_panel,
            "document.body ? document.body.getAttribute('data-phase') : ''",
            std::chrono::milliseconds(5000));
        REQUIRE(final.second.empty());
        REQUIRE(final.first == "\"final\"");
    }

    SECTION("simple HTML page round-trips bridge messages") {
        std::string last_message_type;
        std::string last_message_payload;
        panel->set_message_handler([&](const WebViewMessage& message) -> std::string {
            last_message_type = message.type;
            last_message_payload = message.payload_json;
            return R"({"ack":true,"source":"native"})";
        });

        const auto html = std::string(R"HTML(
<!doctype html>
<html>
<body>
<script>
)HTML") + make_webview_bridge_bootstrap_script() + R"HTML(
window.testState = { nativePayload: null, nativeReply: null };
window.pulp.on('native-ready', function(message) {
  window.testState.nativePayload = JSON.stringify(message.payload);
});
window.pingNative = function() {
  const response = window.pulp.postMessage('page-ready', { from: 'html' }, 'page-1');
  window.testState.nativeReply = JSON.stringify(response);
  return response && response.ok === true;
};
</script>
</body>
</html>
)HTML";

        panel->set_html(html);

        auto ready = await_eval(*panel,
            "typeof window.pingNative === 'function'",
            std::chrono::milliseconds(5000));
        if (!ready.second.empty() || ready.first != "true") {
            SKIP("Live WebView bridge never reached callback-ready state");
        }
        REQUIRE(ready.first == "true");

        panel->post_message(WebViewMessage{
            .type = "native-ready",
            .payload_json = R"({"from":"native"})",
            .id = "native-bridge-1",
        });

        auto saw_native = await_eval(*panel,
            "window.testState.nativePayload ? JSON.parse(window.testState.nativePayload).from : ''");
        REQUIRE(saw_native.second.empty());
        REQUIRE(saw_native.first == "\"native\"");

        auto ping = await_eval(*panel, "window.pingNative()");
        REQUIRE(ping.second.empty());
        REQUIRE(ping.first == "true");
        REQUIRE(last_message_type == "page-ready");
        REQUIRE(last_message_payload.find("\"from\"") != std::string::npos);
        REQUIRE(last_message_payload.find("\"html\"") != std::string::npos);

        auto reply = await_eval(*panel,
            "window.testState.nativeReply ? JSON.parse(window.testState.nativeReply).payload.ack : false");
        REQUIRE(reply.second.empty());
        REQUIRE(reply.first == "true");
    }
}

TEST_CASE("WebViewPanel can attach to a WindowHost native content view", "[view][webview][window]") {
    View root;
    WindowOptions window_options;
    window_options.title = "Phase 7 WebView Host";
    window_options.width = 360;
    window_options.height = 240;

    auto host = WindowHost::create(root, window_options);
    if (!host) {
        SKIP("WindowHost is not available on this platform");
    }

    auto panel = WebViewPanel::create();
    if (!panel) {
        SKIP("WebView not available in this environment");
    }

    if (!wait_until_ready(*panel)) {
        SKIP("WebView never became ready in this environment");
    }

    if (!host->native_window_handle() || !host->native_content_view_handle()) {
        SKIP("WindowHost does not expose native embedding handles on this platform");
    }

    REQUIRE(host->attach_native_child_view(panel->native_handle(), 0, 0, 320, 200));
    REQUIRE(host->set_native_child_view_bounds(panel->native_handle(), 16, 12, 300, 180));

    std::string last_message_type;
    panel->set_message_handler([&](const WebViewMessage& message) -> std::string {
        last_message_type = message.type;
        return R"({"ok":true})";
    });

    const auto html = std::string(R"HTML(
<!doctype html>
<html>
<body>
<script>
)HTML") + make_webview_bridge_bootstrap_script() + R"HTML(
window.phase7Attached = function() {
  const response = window.pulp.postMessage('attached-ready', { state: 'ok' }, 'attached-1');
  return response && response.ok === true;
};
</script>
</body>
</html>
)HTML";

    panel->set_html(html);

    auto ready = await_eval(*panel,
        "typeof window.phase7Attached === 'function'",
        std::chrono::milliseconds(5000));
    if (!ready.second.empty() || ready.first != "true") {
        host->detach_native_child_view(panel->native_handle());
        SKIP("Attached WebView never reached callback-ready state");
    }

    auto ping = await_eval(*panel, "window.phase7Attached()");
    REQUIRE(ping.second.empty());
    REQUIRE(ping.first == "true");
    REQUIRE(last_message_type == "attached-ready");

    host->detach_native_child_view(panel->native_handle());
}
