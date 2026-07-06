// PulpMoonbaseHttpTransport — a moonbase::http_transport backed by Pulp's
// bundled cpp-httplib (mbedTLS) stack, so a Moonbase integration needs no
// libcurl. Moonbase injects this via the licensing() constructor when the SDK
// is built with MOONBASE_USE_CURL=OFF (which defines MOONBASE_DISABLE_CURL_TRANSPORT).
//
// The SDK owns request construction (method, URL, headers including the
// User-Agent, body); this adapter only moves bytes over HTTPS and reports the
// status/headers/body back. Keep it that thin — the http_transport interface is
// two methods and is the stable seam upstream guarantees.

#pragma once

#include <moonbase/http.hpp>

#include <httplib.h>

#include <chrono>
#include <string>

namespace moonbase_pulp {

class PulpMoonbaseHttpTransport : public moonbase::http_transport {
public:
    [[nodiscard]] moonbase::http_response send(const moonbase::http_request& request) override
    {
        // Split "scheme://host[:port]/path?query" into an origin httplib::Client
        // can target and the path+query it routes on.
        const auto scheme_end = request.url.find("://");
        if (scheme_end == std::string::npos) {
            return error_response("Malformed URL (no scheme): " + request.url);
        }
        const auto path_start = request.url.find('/', scheme_end + 3);
        const std::string origin = path_start == std::string::npos
            ? request.url
            : request.url.substr(0, path_start);
        const std::string path = path_start == std::string::npos
            ? "/"
            : request.url.substr(path_start);

        httplib::Client client(origin);
        client.set_follow_location(true);
        // Always bound every request. The SDK may pass a zero/unset timeout, in
        // which case cpp-httplib would fall back to its multi-minute defaults — and
        // a hung connect would then block the control thread that joins the
        // background re-validation worker (editor close / plugin unload), freezing
        // the host. Honor any explicit timeout the SDK sets; otherwise clamp to a
        // sane ceiling so join_revalidate() is always bounded.
        constexpr auto kDefaultTimeout = std::chrono::milliseconds(10000);
        const auto connect = request.connect_timeout.count() > 0
            ? std::chrono::duration_cast<std::chrono::milliseconds>(request.connect_timeout)
            : kDefaultTimeout;
        const auto read = request.request_timeout.count() > 0
            ? std::chrono::duration_cast<std::chrono::milliseconds>(request.request_timeout)
            : kDefaultTimeout;
        client.set_connection_timeout(connect);
        client.set_read_timeout(read);
        client.set_write_timeout(read);

        httplib::Headers headers;
        std::string content_type = "application/octet-stream";
        for (const auto& [key, value] : request.headers) {
            if (key == "Content-Type") {
                content_type = value;
                continue;  // httplib carries the body content-type separately
            }
            headers.emplace(key, value);
        }

        httplib::Result result =
            request.method == "POST"
                ? client.Post(path, headers, request.body, content_type)
                : client.Get(path, headers);

        if (!result) {
            return error_response("HTTP transport error: " +
                                  httplib::to_string(result.error()));
        }

        moonbase::http_response response;
        response.status_code = result->status;
        response.body = result->body;
        for (const auto& [key, value] : result->headers) {
            response.headers.emplace(key, value);
        }
        return response;
    }

private:
    // 0 status tells the SDK the request never reached the server; it maps that
    // to a transport/connectivity failure (kept inside the online-validation
    // grace period rather than treated as an invalid license).
    static moonbase::http_response error_response(std::string message)
    {
        moonbase::http_response response;
        response.status_code = 0;
        response.body = std::move(message);
        return response;
    }
};

} // namespace moonbase_pulp
