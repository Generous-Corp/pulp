#include <catch2/catch_test_macros.hpp>

#include <pulp/runtime/http.hpp>

#include <httplib.h>

#include <chrono>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using namespace std::chrono_literals;
using namespace pulp::runtime;

namespace {

class LoopbackServer {
public:
    httplib::Server server;

    void start() {
        port_ = server.bind_to_any_port("127.0.0.1");
        thread_ = std::thread([this] { server.listen_after_bind(); });
        server.wait_until_ready();
    }

    ~LoopbackServer() {
        server.stop();
        if (thread_.joinable())
            thread_.join();
    }

    std::string url(std::string_view path) const {
        return "http://127.0.0.1:" + std::to_string(port_) + std::string(path);
    }

private:
    int port_ = 0;
    std::thread thread_;
};

void serve_sse(httplib::Response& response) {
    static const std::vector<std::string> chunks = {
        "event: token\ndata: alpha\n\n",
        "event: token\ndata: beta\n\n",
        "event: done\ndata: [DONE]\n\n",
    };
    response.set_chunked_content_provider(
        "text/event-stream",
        [](size_t offset, httplib::DataSink& sink) {
            size_t consumed = 0;
            for (const auto& chunk : chunks) {
                if (consumed == offset) {
                    std::this_thread::sleep_for(10ms);
                    return sink.write(chunk.data(), chunk.size());
                }
                consumed += chunk.size();
            }
            sink.done();
            return true;
        });
}

}  // namespace

TEST_CASE("HTTP request sends custom headers without reflecting their values",
          "[runtime][http]") {
    LoopbackServer loopback;
    std::mutex received_mutex;
    std::string received_authorization;
    loopback.server.Get("/headers",
                        [&](const httplib::Request& request, httplib::Response& response) {
                            {
                                std::lock_guard lock(received_mutex);
                                received_authorization =
                                    request.get_header_value("Authorization");
                            }
                            response.set_content("accepted", "text/plain");
                        });
    loopback.start();

    const std::string sensitive_marker = "Bearer loopback-sensitive-marker";
    HttpRequest request;
    request.url = loopback.url("/headers");
    request.headers["Authorization"] = sensitive_marker;

    const auto response = http_request(request);

    REQUIRE(response.ok());
    REQUIRE(response.body == "accepted");
    {
        std::lock_guard lock(received_mutex);
        REQUIRE(received_authorization == sensitive_marker);
    }
    REQUIRE(response.error.find(sensitive_marker) == std::string::npos);
}

TEST_CASE("HTTP request delivers chunked SSE incrementally", "[runtime][http]") {
    LoopbackServer loopback;
    loopback.server.Get("/events",
                        [](const httplib::Request&, httplib::Response& response) {
                            serve_sse(response);
                        });
    loopback.start();

    std::string delivered;
    size_t callback_count = 0;
    HttpRequest request;
    request.url = loopback.url("/events");
    request.on_chunk = [&](std::string_view chunk) {
        ++callback_count;
        delivered.append(chunk);
        return true;
    };

    const auto response = http_request(request);

    REQUIRE(response.ok());
    REQUIRE(response.body.empty());
    REQUIRE(callback_count >= 2);
    REQUIRE(delivered ==
            "event: token\ndata: alpha\n\n"
            "event: token\ndata: beta\n\n"
            "event: done\ndata: [DONE]\n\n");
}

TEST_CASE("HTTP request callback can abort a chunked response", "[runtime][http]") {
    LoopbackServer loopback;
    loopback.server.Get("/events",
                        [](const httplib::Request&, httplib::Response& response) {
                            serve_sse(response);
                        });
    loopback.start();

    size_t callback_count = 0;
    std::string delivered;
    HttpRequest request;
    request.url = loopback.url("/events");
    request.on_chunk = [&](std::string_view chunk) {
        ++callback_count;
        delivered.append(chunk);
        return false;
    };

    const auto response = http_request(request);

    REQUIRE(response.status_code == 200);
    REQUIRE_FALSE(response.ok());
    REQUIRE(response.error == "Response callback aborted request");
    REQUIRE(callback_count == 1);
    REQUIRE_FALSE(delivered.empty());
    REQUIRE(delivered.find("[DONE]") == std::string::npos);
}

TEST_CASE("HTTP request enforces its timeout after a healthy control request",
          "[runtime][http]") {
    LoopbackServer loopback;
    loopback.server.Get("/ready",
                        [](const httplib::Request&, httplib::Response& response) {
                            response.set_content("ready", "text/plain");
                        });
    loopback.server.Get("/slow",
                        [](const httplib::Request&, httplib::Response& response) {
                            std::this_thread::sleep_for(1500ms);
                            response.set_content("late", "text/plain");
                        });
    loopback.start();

    REQUIRE(http_get(loopback.url("/ready"), 1).ok());

    HttpRequest request;
    request.url = loopback.url("/slow");
    request.timeout_seconds = 1;
    request.headers["Authorization"] = "Bearer timeout-sensitive-marker";
    const auto started = std::chrono::steady_clock::now();
    const auto response = http_request(request);
    const auto elapsed = std::chrono::steady_clock::now() - started;

    REQUIRE_FALSE(response.ok());
    REQUIRE(response.status_code == 0);
    REQUIRE_FALSE(response.error.empty());
    REQUIRE(response.error.find("timeout-sensitive-marker") == std::string::npos);
    REQUIRE(elapsed < 2500ms);
}

TEST_CASE("legacy HTTP GET and POST overloads retain buffered behavior",
          "[runtime][http]") {
    LoopbackServer loopback;
    std::mutex received_mutex;
    std::string received_content_type;
    loopback.server.Get("/legacy-get",
                        [](const httplib::Request&, httplib::Response& response) {
                            response.set_header("X-Compatibility", "get");
                            response.set_content("legacy-get", "text/plain");
                        });
    loopback.server.Post(
        "/legacy-post",
        [&](const httplib::Request& request, httplib::Response& response) {
            {
                std::lock_guard lock(received_mutex);
                received_content_type = request.get_header_value("Content-Type");
            }
            response.set_content(request.body, "text/plain");
        });
    loopback.start();

    const auto get_response = http_get(loopback.url("/legacy-get"), 2);
    const auto post_response =
        http_post(loopback.url("/legacy-post"), "legacy-post", "text/x-pulp", 2);

    REQUIRE(get_response.ok());
    REQUIRE(get_response.body == "legacy-get");
    REQUIRE(get_response.headers.at("X-Compatibility") == "get");
    REQUIRE(post_response.ok());
    REQUIRE(post_response.body == "legacy-post");
    {
        std::lock_guard lock(received_mutex);
        REQUIRE(received_content_type == "text/x-pulp");
    }
}

TEST_CASE("HTTP POST content type handling remains compatible and case insensitive",
          "[runtime][http]") {
    LoopbackServer loopback;
    std::mutex received_mutex;
    size_t custom_type_count = 0;
    std::string custom_type;
    size_t empty_legacy_type_count = 0;
    std::string empty_legacy_type;
    loopback.server.Post(
        "/custom-type",
        [&](const httplib::Request& request, httplib::Response& response) {
            {
                std::lock_guard lock(received_mutex);
                custom_type_count = request.get_header_value_count("Content-Type");
                custom_type = request.get_header_value("Content-Type");
            }
            response.set_content("custom", "text/plain");
        });
    loopback.server.Post(
        "/empty-legacy-type",
        [&](const httplib::Request& request, httplib::Response& response) {
            {
                std::lock_guard lock(received_mutex);
                empty_legacy_type_count =
                    request.get_header_value_count("Content-Type");
                empty_legacy_type = request.get_header_value("Content-Type");
            }
            response.set_content("legacy", "text/plain");
        });
    loopback.start();

    HttpRequest custom_request;
    custom_request.url = loopback.url("/custom-type");
    custom_request.method = "POST";
    custom_request.body = "payload";
    custom_request.headers["content-type"] = "application/x-custom";
    const auto custom_response = http_request(custom_request);
    const auto legacy_response =
        http_post(loopback.url("/empty-legacy-type"), "payload", "", 2);

    REQUIRE(custom_response.ok());
    REQUIRE(legacy_response.ok());
    {
        std::lock_guard lock(received_mutex);
        REQUIRE(custom_type_count == 1);
        REQUIRE(custom_type == "application/x-custom");
        REQUIRE(empty_legacy_type_count == 1);
        REQUIRE(empty_legacy_type == "text/plain");
    }
}
