#include <pulp/runtime/model_download.hpp>

#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif

#if defined(TARGET_OS_IPHONE) && TARGET_OS_IPHONE

namespace pulp::runtime {

DownloadResult download_file(const DownloadRequest& req, const DownloadProgressFn&,
                             const CancellationToken*) {
    DownloadResult r;
    r.path = req.dest;
    r.error = "model downloads are not supported on iOS";
    return r;
}

}  // namespace pulp::runtime

#else

#include <pulp/runtime/async_stream.hpp>

#include <mbedtls/sha256.h>

// HTTPS via cpp-httplib's mbedTLS backend. The CMake target defines
// CPPHTTPLIB_MBEDTLS_SUPPORT when mbedTLS is available.
#include <httplib.h>

#include <array>
#include <cstdint>
#include <fstream>
#include <regex>
#include <string>

namespace fs = std::filesystem;

namespace pulp::runtime {

namespace {

struct ParsedUrl {
    std::string scheme_host_port;  // e.g. "https://huggingface.co"
    std::string path;              // e.g. "/user/repo/resolve/main/file"
    bool ok = false;
};

ParsedUrl parse_url(const std::string& url) {
    static const std::regex re(R"(^(https?)://([^/:]+)(?::(\d+))?(/.*)?$)", std::regex::icase);
    std::smatch m;
    ParsedUrl out;
    if (!std::regex_match(url, m, re)) return out;
    const std::string scheme = m[1].str();
    const std::string host = m[2].str();
    const std::string port = m[3].str();
    const std::string path = m[4].str();
    out.scheme_host_port = scheme + "://" + host + (port.empty() ? "" : ":" + port);
    out.path = path.empty() ? "/" : path;
    out.ok = true;
    return out;
}

// Resolve a redirect Location (absolute, host-absolute, or relative) against the
// current scheme/host/port + path. Returns ok=false if it can't be resolved.
ParsedUrl resolve_redirect(const std::string& cur_scheme_host_port, const std::string& cur_path,
                           const std::string& location) {
    ParsedUrl out;
    if (location.empty()) return out;
    if (location.starts_with("http://") || location.starts_with("https://"))
        return parse_url(location);
    if (location.front() == '/') {  // host-absolute
        out.scheme_host_port = cur_scheme_host_port;
        out.path = location;
        out.ok = true;
        return out;
    }
    // Path-relative: replace the last segment of the current path.
    out.scheme_host_port = cur_scheme_host_port;
    auto slash = cur_path.find_last_of('/');
    out.path = (slash == std::string::npos ? std::string("/") : cur_path.substr(0, slash + 1)) + location;
    out.ok = true;
    return out;
}

std::string to_hex(const unsigned char* data, size_t n) {
    static const char* digits = "0123456789abcdef";
    std::string s;
    s.reserve(n * 2);
    for (size_t i = 0; i < n; ++i) {
        s += digits[data[i] >> 4];
        s += digits[data[i] & 0x0F];
    }
    return s;
}

// Feed a file's existing bytes into the running SHA (resume re-hash).
bool hash_existing(mbedtls_sha256_context& ctx, const fs::path& path, std::uint64_t& counted) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    std::array<char, 1 << 16> buf{};
    while (in) {
        in.read(buf.data(), static_cast<std::streamsize>(buf.size()));
        const auto got = in.gcount();
        if (got > 0) {
            mbedtls_sha256_update(&ctx, reinterpret_cast<const unsigned char*>(buf.data()),
                                  static_cast<size_t>(got));
            counted += static_cast<std::uint64_t>(got);
        }
    }
    // Distinguish a clean EOF from a genuine read error: badbit means the
    // partial could not be fully re-hashed, so the caller must restart rather
    // than trust a truncated hash for a resumed transfer.
    if (in.bad()) return false;
    return true;
}

// Verify a 206 (Partial Content) response actually continues from the byte
// offset we asked for. A server that returns 206 but a different range (or no
// Content-Range at all) would corrupt the appended .part + rehash, so treat
// any mismatch as untrusted.
bool content_range_starts_at(const httplib::Response& resp, std::uint64_t expected_start) {
    auto it = resp.headers.find("Content-Range");
    if (it == resp.headers.end()) return false;  // 206 must carry Content-Range
    static const std::regex re(R"(bytes\s+(\d+)-(\d+)/(\d+|\*))", std::regex::icase);
    std::smatch m;
    if (!std::regex_search(it->second, m, re)) return false;
    try {
        return std::stoull(m[1].str()) == expected_start;
    } catch (...) {
        return false;
    }
}


// Best-effort system CA bundle for mbedTLS verification (macOS / common Linux).
const char* system_ca_path() {
    static const char* candidates[] = {
        "/etc/ssl/cert.pem",                      // macOS, some BSDs
        "/etc/ssl/certs/ca-certificates.crt",     // Debian/Ubuntu
        "/etc/pki/tls/certs/ca-bundle.crt",       // RHEL/Fedora
    };
    for (const char* c : candidates) {
        std::error_code ec;
        if (fs::exists(c, ec)) return c;
    }
    return nullptr;
}

}  // namespace

DownloadResult download_file(const DownloadRequest& req, const DownloadProgressFn& on_progress,
                             const CancellationToken* cancel) {
    DownloadResult r;
    r.path = req.dest;

    const auto parsed = parse_url(req.url);
    if (!parsed.ok) {
        r.error = "invalid url: " + req.url;
        return r;
    }

    std::error_code ec;
    if (!req.dest.parent_path().empty()) fs::create_directories(req.dest.parent_path(), ec);
    fs::path part = req.dest;
    part += ".part";

    std::uint64_t resume_from = 0;
    if (req.resume && fs::exists(part, ec)) {
        const auto sz = fs::file_size(part, ec);
        // file_size returns static_cast<uintmax_t>(-1) and sets `ec` on error;
        // guard against that turning into a bogus huge Range offset.
        if (!ec && sz != static_cast<std::uintmax_t>(-1))
            resume_from = static_cast<std::uint64_t>(sz);
    }

    mbedtls_sha256_context sha;
    mbedtls_sha256_init(&sha);
    mbedtls_sha256_starts(&sha, 0);  // 0 = SHA-256 (not SHA-224)

    std::uint64_t downloaded = 0;
    if (resume_from > 0 && !hash_existing(sha, part, downloaded)) resume_from = 0;  // unreadable → restart

    std::ofstream out;
    auto open_out = [&](bool append) {
        out.close();
        out.clear();
        out.open(part, std::ios::binary | (append ? std::ios::app : std::ios::trunc));
    };
    open_out(resume_from > 0);
    if (!out) {
        mbedtls_sha256_free(&sha);
        r.error = "cannot open " + part.string() + " for writing";
        return r;
    }

    httplib::Headers headers;
    for (const auto& h : req.headers) headers.emplace(h.name, h.value);
    const bool attempted_resume = resume_from > 0;
    if (attempted_resume) headers.emplace("Range", "bytes=" + std::to_string(resume_from) + "-");

    std::uint64_t total = 0;
    bool reset_due_to_200 = false;
    bool user_cancel = false;
    bool write_error = false;
    bool http_status_error = false;
    bool range_mismatch = false;
    bool is_redirect = false;
    int response_status = 0;
    std::string redirect_location;

    auto response_handler = [&](const httplib::Response& resp) -> bool {
        response_status = resp.status;
        // Redirect: capture Location and abort before any body is streamed into
        // .part. We follow redirects manually (below) so we can strip sensitive
        // headers on a cross-origin hop.
        if (resp.status >= 300 && resp.status < 400) {
            if (auto it = resp.headers.find("Location"); it != resp.headers.end()) {
                redirect_location = it->second;
                is_redirect = true;
                return false;
            }
            http_status_error = true;  // 3xx without Location — give up
            return false;
        }
        // Reject other non-success responses before any content is received so an
        // error body is never written into the resumable .part file.
        if (resp.status != 200 && resp.status != 206) {
            http_status_error = true;
            return false;
        }
        // A 206 must continue from exactly the offset we requested; otherwise
        // appending its bytes to our partial would corrupt the file + hash.
        if (attempted_resume && resp.status == 206 &&
            !content_range_starts_at(resp, resume_from)) {
            range_mismatch = true;
            return false;
        }
        if (attempted_resume && resp.status == 200) {
            // Server ignored our Range request — discard the partial and restart.
            reset_due_to_200 = true;
            mbedtls_sha256_free(&sha);
            mbedtls_sha256_init(&sha);
            mbedtls_sha256_starts(&sha, 0);
            downloaded = 0;
            open_out(false);
        }
        if (auto it = resp.headers.find("Content-Length"); it != resp.headers.end()) {
            try {
                total = downloaded + std::stoull(it->second);
            } catch (...) {
                total = 0;
            }
        }
        return true;
    };

    auto content_receiver = [&](const char* data, size_t len) -> bool {
        out.write(data, static_cast<std::streamsize>(len));
        if (!out) {
            write_error = true;
            return false;
        }
        mbedtls_sha256_update(&sha, reinterpret_cast<const unsigned char*>(data), len);
        downloaded += len;
        if (cancel && cancel->is_cancelled()) {
            user_cancel = true;
            return false;
        }
        if (on_progress && !on_progress(DownloadProgress{downloaded, total})) {
            user_cancel = true;
            return false;
        }
        return true;
    };

    // Manual redirect loop. set_follow_location is OFF so cpp-httplib can't replay
    // the (possibly sensitive) request headers to a redirect target on another
    // host — HF gated downloads 302 to a pre-signed S3 URL, and the Authorization
    // token must not travel there (httplib::Headers compares keys case-insensitively).
    std::string cur_shp = parsed.scheme_host_port;
    std::string cur_path = parsed.path;
    int redirects_left = 8;
    httplib::Result result;
    while (true) {
        is_redirect = false;
        redirect_location.clear();

        httplib::Client cli(cur_shp);
        cli.set_follow_location(false);
        cli.set_keep_alive(true);
        // Always bound the CONNECT phase (see download_connect_timeout_seconds):
        // without it, a blocked/unreachable endpoint falls back to cpp-httplib's
        // 300s default and the download wedges at 0% with an unresponsive Cancel
        // (the cancel/progress callback only runs once body streaming starts).
        // The read/stream phase stays governed by timeout_seconds so large bodies
        // aren't capped by this connect bound.
        cli.set_connection_timeout(
            download_connect_timeout_seconds(req.timeout_seconds), 0);
        if (req.timeout_seconds > 0) {
            cli.set_read_timeout(req.timeout_seconds, 0);
        }
        if (const char* ca = system_ca_path()) cli.set_ca_cert_path(ca);
        cli.enable_server_certificate_verification(true);

        result = cli.Get(cur_path, headers, response_handler, content_receiver);

        if (!is_redirect) break;
        if (redirects_left-- <= 0) {
            mbedtls_sha256_free(&sha);
            out.close();
            r.error = "too many redirects";
            return r;
        }
        const auto next = resolve_redirect(cur_shp, cur_path, redirect_location);
        if (!next.ok) {
            mbedtls_sha256_free(&sha);
            out.close();
            r.error = "invalid redirect location: " + redirect_location;
            return r;
        }
        if (next.scheme_host_port != cur_shp) {
            // Cross-origin hop: drop credentials so they don't leak to the new host.
            headers.erase("Authorization");
            headers.erase("Proxy-Authorization");
            headers.erase("Cookie");
        }
        cur_shp = next.scheme_host_port;
        cur_path = next.path;
    }

    out.flush();
    out.close();

    if (write_error) {
        mbedtls_sha256_free(&sha);
        r.error = "write failed to " + part.string();
        return r;  // keep .part
    }
    if (user_cancel) {
        mbedtls_sha256_free(&sha);
        r.cancelled = true;
        r.error = "cancelled";
        return r;  // keep .part for resume
    }
    // Status / range checks happen in the response handler (which aborts the
    // transfer via `return false`) so no error/short body is ever appended to
    // .part. Surface those before the generic transport-error path, since an
    // aborted handler makes `result` falsy with Error::Canceled.
    if (http_status_error) {
        mbedtls_sha256_free(&sha);
        r.error = "http status " + std::to_string(response_status);
        return r;  // .part untouched — safe to resume against later
    }
    if (range_mismatch) {
        mbedtls_sha256_free(&sha);
        fs::remove(part, ec);  // partial no longer trustworthy
        r.error = "server returned unexpected content-range for resume";
        return r;
    }
    if (!result) {
        mbedtls_sha256_free(&sha);
        r.error = "http error: " + httplib::to_string(result.error());
        return r;  // keep .part
    }
    if (result->status != 200 && result->status != 206) {
        mbedtls_sha256_free(&sha);
        r.error = "http status " + std::to_string(result->status);
        return r;
    }

    unsigned char digest[32];
    mbedtls_sha256_finish(&sha, digest);
    mbedtls_sha256_free(&sha);
    r.sha256 = to_hex(digest, sizeof digest);
    r.resumed = attempted_resume && !reset_due_to_200;

    if (!req.expected_sha256.empty() && req.expected_sha256 != r.sha256) {
        fs::remove(part, ec);  // corrupt — don't leave a resumable bad file
        r.error = "sha256 mismatch: expected " + req.expected_sha256 + " got " + r.sha256;
        return r;
    }

    fs::rename(part, req.dest, ec);
    if (ec) {
        r.error = "rename failed: " + ec.message();
        return r;
    }
    r.bytes = static_cast<std::uint64_t>(fs::file_size(req.dest, ec));
    r.ok = true;
    return r;
}

}  // namespace pulp::runtime

#endif
