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
    std::string expected_sha256;           ///< Hex digest the finished file must match.
    std::vector<HttpHeader> headers;       ///< Extra request headers (e.g. Authorization).
    bool resume = true;                    ///< Resume from a prior partial <dest>.part via Range.
    int timeout_seconds = 0;               ///< 0 ⇒ a sensible library default.
    /// An empty `expected_sha256` means nothing can vouch for the bytes, so the
    /// download is refused (`DownloadResult::unverified`) unless the caller opts
    /// in here. Opting in still records the digest the transfer produced.
    bool allow_unverified = false;
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
    /// The request carried no expected digest. With `ok == false` nothing was
    /// fetched or written; with `ok == true` the caller opted in and `sha256` is
    /// the only record of what arrived.
    bool unverified = false;
};

/// A redirect `Location` resolved against the request it answered.
struct ResolvedRedirect {
    std::string scheme_host_port; ///< e.g. "https://cdn.example.com:8443"
    std::string path;             ///< Absolute path on that origin.
    bool ok = false;              ///< False when unresolvable or refused; see `error`.
    std::string error;
};

/// Resolve an absolute, host-absolute, or path-relative redirect `location`
/// against the current origin + path. Refuses a hop from https to http so a
/// transfer that started encrypted cannot be downgraded mid-flight (an http
/// origin may upgrade to https).
ResolvedRedirect resolve_redirect(const std::string& current_scheme_host_port,
                                  const std::string& current_path, const std::string& location);

/// Progress callback. Return false to cancel the in-flight download.
using DownloadProgressFn = std::function<bool(const DownloadProgress&)>;

/// Blocking streaming download. Writes to `<dest>.part`, verifies sha256 against
/// `expected_sha256`, then atomically renames to `dest`. A request with no expected
/// digest is refused before any network or disk activity unless it sets
/// `allow_unverified`. On cancel/error the `.part` file is left in place so a later
/// call with `resume = true` continues it.
DownloadResult download_file(const DownloadRequest& request,
                             const DownloadProgressFn& on_progress = {},
                             const CancellationToken* cancel = nullptr);

}  // namespace pulp::runtime
