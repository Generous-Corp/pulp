#include <pulp/runtime/http.hpp>

// Disable SSL/TLS support to avoid OpenSSL dependency.
// This must be set BEFORE including httplib.h.
#ifdef CPPHTTPLIB_OPENSSL_SUPPORT
#undef CPPHTTPLIB_OPENSSL_SUPPORT
#endif

#include <httplib.h>

#include <charconv>
#include <fstream>
#include <regex>
#include <algorithm>

namespace pulp::runtime {

// Parse a URL into scheme, host, port, path
static bool parse_url(std::string_view url, std::string& scheme,
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

HttpResponse http_get(std::string_view url, int timeout_seconds) {
    HttpResponse response;

    std::string scheme, host, path;
    int port;
    if (!parse_url(url, scheme, host, port, path)) {
        response.error = "Invalid URL";
        return response;
    }

    std::string base = scheme + "://" + host + ":" + std::to_string(port);
    httplib::Client client(base);
    client.set_connection_timeout(timeout_seconds);
    client.set_read_timeout(timeout_seconds);

    auto result = client.Get(path);
    if (!result) {
        response.error = "Connection failed: " + httplib::to_string(result.error());
        return response;
    }

    response.status_code = result->status;
    response.body = result->body;
    for (auto& [key, value] : result->headers)
        response.headers[key] = value;

    return response;
}

HttpResponse http_post(std::string_view url, std::string_view body,
                       std::string_view content_type, int timeout_seconds) {
    HttpResponse response;

    std::string scheme, host, path;
    int port;
    if (!parse_url(url, scheme, host, port, path)) {
        response.error = "Invalid URL";
        return response;
    }

    std::string base = scheme + "://" + host + ":" + std::to_string(port);
    httplib::Client client(base);
    client.set_connection_timeout(timeout_seconds);
    client.set_read_timeout(timeout_seconds);

    auto result = client.Post(path, std::string(body), std::string(content_type));
    if (!result) {
        response.error = "Connection failed: " + httplib::to_string(result.error());
        return response;
    }

    response.status_code = result->status;
    response.body = result->body;
    for (auto& [key, value] : result->headers)
        response.headers[key] = value;

    return response;
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
