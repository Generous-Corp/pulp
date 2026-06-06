#include <pulp/runtime/model_download.hpp>

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
    return true;
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
    if (req.resume && fs::exists(part, ec)) resume_from = static_cast<std::uint64_t>(fs::file_size(part, ec));

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

    httplib::Client cli(parsed.scheme_host_port);
    cli.set_follow_location(true);
    cli.set_keep_alive(true);
    if (req.timeout_seconds > 0) {
        cli.set_read_timeout(req.timeout_seconds, 0);
        cli.set_connection_timeout(req.timeout_seconds, 0);
    }
    if (const char* ca = system_ca_path()) cli.set_ca_cert_path(ca);
    cli.enable_server_certificate_verification(true);

    httplib::Headers headers;
    for (const auto& h : req.headers) headers.emplace(h.name, h.value);
    const bool attempted_resume = resume_from > 0;
    if (attempted_resume) headers.emplace("Range", "bytes=" + std::to_string(resume_from) + "-");

    std::uint64_t total = 0;
    bool reset_due_to_200 = false;
    bool user_cancel = false;
    bool write_error = false;

    auto response_handler = [&](const httplib::Response& resp) -> bool {
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

    auto result = cli.Get(parsed.path, headers, response_handler, content_receiver);
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
