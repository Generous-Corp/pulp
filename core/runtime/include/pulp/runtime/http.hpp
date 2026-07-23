#pragma once

// HTTP client — wraps cpp-httplib without exposing it through the public API.

#include <functional>
#include <map>
#include <string>
#include <string_view>

namespace pulp::runtime {

using HttpHeaders = std::map<std::string, std::string>;
using HttpChunkCallback = std::function<bool(std::string_view)>;

struct HttpRequest {
    std::string url;
    std::string method = "GET";
    std::string body;
    std::string content_type = "application/json";
    HttpHeaders headers;
    int timeout_seconds = 30;

    // Returning false stops the transfer. Chunk boundaries do not imply protocol
    // record boundaries. When set, response bytes are delivered here instead of
    // being retained in HttpResponse::body.
    HttpChunkCallback on_chunk;
};

struct HttpResponse {
    int status_code = 0;
    std::string body;
    HttpHeaders headers;
    std::string error;

    bool ok() const {
        return error.empty() && status_code >= 200 && status_code < 300;
    }
};

/// Perform an HTTP request. GET and POST are supported.
HttpResponse http_request(const HttpRequest& request);

/// Perform an HTTP GET request.
HttpResponse http_get(std::string_view url,
                      int timeout_seconds = 30);

/// Perform an HTTP POST request with a body.
HttpResponse http_post(std::string_view url,
                       std::string_view body,
                       std::string_view content_type = "application/json",
                       int timeout_seconds = 30);

/// Download a file from a URL to a local path. Returns true on success.
bool http_download(std::string_view url, std::string_view output_path,
                   int timeout_seconds = 60);

}  // namespace pulp::runtime
