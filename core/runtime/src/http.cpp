#include <pulp/runtime/http.hpp>

// Disable SSL/TLS support to avoid OpenSSL dependency.
// This must be set BEFORE including httplib.h.
#ifdef CPPHTTPLIB_OPENSSL_SUPPORT
#undef CPPHTTPLIB_OPENSSL_SUPPORT
#endif

#include <httplib.h>

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstdint>
#include <fstream>
#include <regex>

namespace pulp::runtime {
namespace {

bool parse_url(std::string_view url, std::string& scheme,
               std::string& host, int& port, std::string& path) {
    std::string url_str(url);
    std::regex url_regex(R"(^(https?)://([^/:]+)(?::(\d+))?(/.*)?)",
                         std::regex::icase);
    std::smatch match;

    if (!std::regex_match(url_str, match, url_regex))
        return false;

    scheme = match[1];
    std::transform(scheme.begin(), scheme.end(), scheme.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    host = match[2];
    port = scheme == "https" ? 443 : 80;
    if (match[3].length() > 0) {
        const auto port_str = match[3].str();
        int parsed_port = 0;
        const auto* begin = port_str.data();
        const auto* end = begin + port_str.size();
        const auto result = std::from_chars(begin, end, parsed_port);
        if (result.ec != std::errc{} || result.ptr != end || parsed_port < 1 || parsed_port > 65535)
            return false;
        port = parsed_port;
    }

    path = match[4].length() > 0 ? match[4].str() : "/";
    return true;
}

void copy_response_metadata(const httplib::Response& source,
                            HttpResponse& destination) {
    destination.status_code = source.status;
    destination.headers.clear();
    for (const auto& [key, value] : source.headers)
        destination.headers[key] = value;
}

}  // namespace

HttpResponse http_request(const HttpRequest& request) {
    HttpResponse response;
    std::string scheme, host, path;
    int port;
    if (!parse_url(request.url, scheme, host, port, path)) {
        response.error = "Invalid URL";
        return response;
    }
    if (request.timeout_seconds <= 0) {
        response.error = "Timeout must be positive";
        return response;
    }

    auto method = request.method;
    std::transform(method.begin(), method.end(), method.begin(),
                   [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    if (method != "GET" && method != "POST") {
        response.error = "Unsupported HTTP method";
        return response;
    }

    std::string base = scheme + "://" + host + ":" + std::to_string(port);
    httplib::Client client(base);
    client.set_connection_timeout(request.timeout_seconds);
    client.set_read_timeout(request.timeout_seconds);
    client.set_write_timeout(request.timeout_seconds);

    httplib::Request transport_request;
    transport_request.method = method;
    transport_request.path = path;
    transport_request.body = request.body;
    for (const auto& [key, value] : request.headers)
        transport_request.headers.emplace(key, value);
    if (method == "POST" && !request.content_type.empty() &&
        transport_request.headers.find("Content-Type") == transport_request.headers.end()) {
        transport_request.headers.emplace("Content-Type", request.content_type);
    }

    bool callback_aborted = false;
    bool callback_failed = false;
    if (request.on_chunk) {
        transport_request.response_handler = [&response](const httplib::Response& incoming) {
            copy_response_metadata(incoming, response);
            return true;
        };
        transport_request.content_receiver =
            [&request, &callback_aborted, &callback_failed](
                const char* data, size_t size, std::uint64_t, std::uint64_t) {
                try {
                    if (request.on_chunk(std::string_view(data, size)))
                        return true;
                    callback_aborted = true;
                } catch (...) {
                    callback_failed = true;
                }
                return false;
            };
    }

    httplib::Result result;
    try {
        result = client.send(transport_request);
    } catch (...) {
        response.error = "HTTP request failed";
        return response;
    }

    if (!result) {
        if (callback_failed)
            response.error = "Response callback failed";
        else if (callback_aborted)
            response.error = "Response callback aborted request";
        else
            response.error = "Connection failed: " + httplib::to_string(result.error());
        return response;
    }

    copy_response_metadata(*result, response);
    if (!request.on_chunk)
        response.body = result->body;

    return response;
}

HttpResponse http_get(std::string_view url, int timeout_seconds) {
    HttpRequest request;
    request.url = std::string(url);
    request.timeout_seconds = timeout_seconds;
    return http_request(request);
}

HttpResponse http_post(std::string_view url, std::string_view body,
                       std::string_view content_type, int timeout_seconds) {
    HttpRequest request;
    request.url = std::string(url);
    request.method = "POST";
    request.body = std::string(body);
    request.content_type = std::string(content_type);
    request.timeout_seconds = timeout_seconds;
    return http_request(request);
}

bool http_download(std::string_view url, std::string_view output_path,
                   int timeout_seconds) {
    auto response = http_get(url, timeout_seconds);
    if (!response.ok())
        return false;

    std::ofstream file(std::string(output_path), std::ios::binary);
    if (!file)
        return false;

    file.write(response.body.data(), static_cast<std::streamsize>(response.body.size()));
    return file.good();
}

}  // namespace pulp::runtime
