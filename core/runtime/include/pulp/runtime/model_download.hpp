#pragma once

// Streaming HTTPS file downloader for the model manager. Unlike
// runtime/http.hpp's http_download (whole-body, in-memory — fine for small JSON,
// fatal for GB-scale weights), this streams the body incrementally to disk with a
// progress callback, HTTP Range **resume**, request **headers** (e.g. HuggingFace
// auth), **cancellation**, **sha256** verification, and a temp-stage → atomic-rename
// so a partial/aborted transfer never appears as a finished file. HTTPS rides on
// cpp-httplib's mbedTLS backend (mbedTLS is already linked by core/runtime).

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace pulp::runtime {

class CancellationToken;  // async_stream.hpp

struct DownloadProgress {
    std::uint64_t downloaded = 0;  ///< Bytes written so far (includes resumed bytes).
    std::uint64_t total = 0;       ///< Total expected bytes, or 0 if the server didn't say.
};

struct HttpHeader {
    std::string name;
    std::string value;
};

struct DownloadRequest {
    std::string url;                       ///< http(s)://… (resolve hf:// first).
    std::filesystem::path dest;            ///< Final destination path.
    std::string expected_sha256;           ///< Optional hex digest; verified when non-empty.
    std::vector<HttpHeader> headers;       ///< Extra request headers (e.g. Authorization).
    bool resume = true;                    ///< Resume from a prior partial <dest>.part via Range.
    int timeout_seconds = 0;               ///< 0 ⇒ a sensible library default.
};

/// Connect-phase timeout (seconds) the downloader applies for a given request
/// `timeout_seconds`. Always bounded (never 0), so an unreachable or blocked
/// endpoint fails fast instead of hanging on the HTTP library's multi-minute
/// connection-timeout default — the failure mode seen when a plug-in host
/// sandboxes the audio-unit process's network (download appears stuck at 0%
/// with an unresponsive Cancel, because the cancel/progress callback only runs
/// once body streaming begins). The read/stream phase stays governed by
/// `timeout_seconds` (0 ⇒ library default) so large bodies aren't capped here.
inline int download_connect_timeout_seconds(int request_timeout_seconds) {
    constexpr int kConnectTimeoutSeconds = 15;
    return request_timeout_seconds > 0
               ? (request_timeout_seconds < kConnectTimeoutSeconds
                      ? request_timeout_seconds
                      : kConnectTimeoutSeconds)
               : kConnectTimeoutSeconds;
}

struct DownloadResult {
    bool ok = false;
    std::string error;
    std::filesystem::path path;            ///< The finished file (== request.dest) on success.
    std::uint64_t bytes = 0;               ///< Total bytes of the finished file.
    std::string sha256;                    ///< Hex digest of the finished file.
    bool resumed = false;                  ///< A prior partial was continued.
    bool cancelled = false;                ///< Stopped via callback / token (partial kept for resume).
};

/// Progress callback. Return false to cancel the in-flight download.
using DownloadProgressFn = std::function<bool(const DownloadProgress&)>;

/// Blocking streaming download. Writes to `<dest>.part`, verifies sha256 (when an
/// expected digest is given), then atomically renames to `dest`. On cancel/error the
/// `.part` file is left in place so a later call with `resume = true` continues it.
DownloadResult download_file(const DownloadRequest& request,
                             const DownloadProgressFn& on_progress = {},
                             const CancellationToken* cancel = nullptr);

}  // namespace pulp::runtime
