//! `pulp trace open <file.pftrace>` — hand a flushed trace to the Perfetto UI.
//!
//! Browsers block `file://` for security, and ui.perfetto.dev cannot read a
//! local path directly. So `open` serves the trace from a **loopback-only**
//! HTTP server and points the UI at it via the documented deep link
//! `https://ui.perfetto.dev/#!/?url=<served-url>`. The UI fetches the trace
//! over CORS; the only server obligation is to allow that origin.
//!
//! Headless-safe: `--no-browser` never launches a browser (it prints the URLs
//! to paste), and `--json` emits `{trace_path, serve_url, perfetto_url,
//! browser_opened, served}` for agents. The server is bounded by
//! `--keep-alive-seconds` and returns as soon as the UI has fetched the trace.
//!
//! MVP note: the `?url=` handoff needs no served JS shim — just CORS. A
//! postMessage handshake is more robust for very large traces / flaky
//! networks; revisit if that becomes a real limitation.

use std::io::{Read, Write};
use std::net::TcpListener;
use std::path::PathBuf;
use std::process::{Command, Stdio};
use std::time::{Duration, Instant};

use crate::error::{CliError, Result};

/// The Perfetto UI origin the loopback server grants CORS to.
const PERFETTO_ORIGIN: &str = "https://ui.perfetto.dev";

/// Parsed `pulp trace open` arguments.
#[derive(Debug, Clone)]
pub struct OpenArgs {
    /// The `.pftrace` to serve.
    pub file: PathBuf,
    /// `--no-browser` — print the URLs but never launch a browser.
    pub no_browser: bool,
    /// `--keep-alive-seconds N` — how long to keep serving if the UI has not
    /// fetched yet. The server still returns early on the first fetch.
    pub keep_alive_secs: u64,
}

impl OpenArgs {
    /// Default keep-alive: long enough for a browser to cold-open the UI and
    /// fetch, short enough not to wedge an unattended invocation.
    pub const DEFAULT_KEEP_ALIVE_SECS: u64 = 20;
}

/// Percent-encode a string for use as a query-parameter value (encode
/// everything outside the RFC 3986 unreserved set).
fn percent_encode(s: &str) -> String {
    let mut out = String::with_capacity(s.len() * 3);
    for b in s.bytes() {
        match b {
            b'A'..=b'Z' | b'a'..=b'z' | b'0'..=b'9' | b'-' | b'_' | b'.' | b'~' => {
                out.push(b as char);
            }
            _ => out.push_str(&format!("%{b:02X}")),
        }
    }
    out
}

/// Build the Perfetto deep link for a locally-served trace URL. Pure.
#[must_use]
pub fn build_perfetto_url(serve_url: &str) -> String {
    format!(
        "{PERFETTO_ORIGIN}/#!/?url={}",
        percent_encode(serve_url)
    )
}

fn io_err(e: std::io::Error) -> CliError {
    CliError::io(std::path::Path::new("<trace-open>"), e)
}

/// Best-effort cross-platform browser open. Spawns detached; never blocks and
/// never errors — returns whether the launcher spawned.
fn open_in_browser(url: &str) -> bool {
    let (cmd, args): (&str, Vec<&str>) = if cfg!(target_os = "macos") {
        ("open", vec![url])
    } else if cfg!(target_os = "windows") {
        // `start` is a cmd builtin; the empty "" is the window title arg.
        ("cmd", vec!["/C", "start", "", url])
    } else {
        ("xdg-open", vec![url])
    };
    Command::new(cmd)
        .args(&args)
        .stdin(Stdio::null())
        .stdout(Stdio::null())
        .stderr(Stdio::null())
        .spawn()
        .is_ok()
}

/// Read an HTTP request head (up to the blank line) with a short timeout.
/// Returns the raw head text, or an empty string on timeout / EOF.
fn read_request_head(stream: &mut std::net::TcpStream) -> String {
    stream
        .set_read_timeout(Some(Duration::from_millis(500)))
        .ok();
    let mut buf = Vec::with_capacity(512);
    let mut chunk = [0u8; 256];
    loop {
        match stream.read(&mut chunk) {
            Ok(0) => break,
            Ok(n) => {
                buf.extend_from_slice(&chunk[..n]);
                if buf.windows(4).any(|w| w == b"\r\n\r\n") || buf.len() > 8192 {
                    break;
                }
            }
            Err(_) => break,
        }
    }
    String::from_utf8_lossy(&buf).into_owned()
}

fn cors_headers() -> String {
    format!(
        "Access-Control-Allow-Origin: {PERFETTO_ORIGIN}\r\n\
         Access-Control-Allow-Methods: GET, OPTIONS\r\n\
         Access-Control-Allow-Headers: *\r\n\
         Access-Control-Expose-Headers: Content-Length\r\n"
    )
}

fn write_preflight(stream: &mut std::net::TcpStream) {
    let resp = format!(
        "HTTP/1.1 204 No Content\r\n{}Content-Length: 0\r\nConnection: close\r\n\r\n",
        cors_headers()
    );
    let _ = stream.write_all(resp.as_bytes());
    let _ = stream.flush();
}

fn write_trace(stream: &mut std::net::TcpStream, body: &[u8]) {
    let head = format!(
        "HTTP/1.1 200 OK\r\n{}Content-Type: application/octet-stream\r\n\
         Content-Length: {}\r\nConnection: close\r\n\r\n",
        cors_headers(),
        body.len()
    );
    let _ = stream.write_all(head.as_bytes());
    let _ = stream.write_all(body);
    let _ = stream.flush();
}

fn write_404(stream: &mut std::net::TcpStream) {
    let resp =
        "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
    let _ = stream.write_all(resp.as_bytes());
    let _ = stream.flush();
}

/// Serve `body` as `/trace.pftrace` on `listener` until the UI fetches it (a
/// `GET`) or `deadline` passes. `OPTIONS` preflights are answered and serving
/// continues; the first successful `GET` returns `Ok(true)`. Pure I/O over the
/// caller-owned listener so tests can drive it directly.
pub fn serve_pftrace(
    listener: &TcpListener,
    body: &[u8],
    deadline: Instant,
) -> Result<bool> {
    listener.set_nonblocking(true).map_err(io_err)?;
    while Instant::now() < deadline {
        match listener.accept() {
            Ok((mut stream, _addr)) => {
                let head = read_request_head(&mut stream);
                if head.starts_with("OPTIONS") {
                    write_preflight(&mut stream);
                } else if head.starts_with("GET") && head.contains("/trace.pftrace")
                {
                    write_trace(&mut stream, body);
                    return Ok(true);
                } else {
                    write_404(&mut stream);
                }
            }
            Err(ref e) if e.kind() == std::io::ErrorKind::WouldBlock => {
                std::thread::sleep(Duration::from_millis(50));
            }
            Err(e) => return Err(io_err(e)),
        }
    }
    // Deadline passed with no GET for /trace.pftrace.
    Ok(false)
}

fn escape_json(s: &str) -> String {
    s.replace('\\', "\\\\").replace('"', "\\\"")
}

/// Run `pulp trace open`: bind a loopback server for the trace, point the
/// Perfetto UI at it, serve until fetched (or the keep-alive elapses), and
/// report. Only writer / bind failures are errors; a browser that will not
/// open or a UI that never fetches is reported in the output.
pub fn run_open(args: &OpenArgs, json: bool, out: &mut impl Write) -> Result<()> {
    let meta = std::fs::metadata(&args.file).map_err(|_| {
        CliError::Other(format!(
            "pulp trace open: no such file: {}",
            args.file.display()
        ))
    })?;
    if !meta.is_file() {
        return Err(CliError::Other(format!(
            "pulp trace open: not a file: {}",
            args.file.display()
        )));
    }
    let body = std::fs::read(&args.file).map_err(|e| {
        CliError::Other(format!(
            "pulp trace open: cannot read {}: {e}",
            args.file.display()
        ))
    })?;

    let listener = TcpListener::bind("127.0.0.1:0").map_err(|e| {
        CliError::Other(format!(
            "pulp trace open: cannot bind loopback server: {e}"
        ))
    })?;
    let port = listener.local_addr().map_err(io_err)?.port();
    let serve_url = format!("http://127.0.0.1:{port}/trace.pftrace");
    let perfetto_url = build_perfetto_url(&serve_url);

    let browser_opened = if args.no_browser {
        false
    } else {
        open_in_browser(&perfetto_url)
    };

    let deadline = Instant::now() + Duration::from_secs(args.keep_alive_secs);
    let served = serve_pftrace(&listener, &body, deadline)?;

    if json {
        writeln!(
            out,
            "{{\"trace_path\":\"{}\",\"serve_url\":\"{}\",\
             \"perfetto_url\":\"{}\",\"browser_opened\":{},\"served\":{}}}",
            escape_json(&args.file.display().to_string()),
            escape_json(&serve_url),
            escape_json(&perfetto_url),
            browser_opened,
            served
        )
        .map_err(io_err)?;
    } else {
        writeln!(out, "opening {} in the Perfetto UI", args.file.display())
            .map_err(io_err)?;
        writeln!(out, "  perfetto: {perfetto_url}").map_err(io_err)?;
        writeln!(out, "  serving:  {serve_url}").map_err(io_err)?;
        if !browser_opened {
            writeln!(
                out,
                "  (no browser launched — paste the perfetto URL above)"
            )
            .map_err(io_err)?;
        }
        if !served {
            writeln!(
                out,
                "  note: the UI did not fetch within {}s; keep this running or \
                 re-run with a larger --keep-alive-seconds",
                args.keep_alive_secs
            )
            .map_err(io_err)?;
        }
    }
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::net::TcpStream;

    #[test]
    fn percent_encode_encodes_reserved_and_keeps_unreserved() {
        assert_eq!(percent_encode("aZ9-_.~"), "aZ9-_.~");
        assert_eq!(percent_encode("a/b:c"), "a%2Fb%3Ac");
    }

    #[test]
    fn perfetto_url_encodes_the_served_url() {
        let u = build_perfetto_url("http://127.0.0.1:5000/trace.pftrace");
        assert!(u.starts_with("https://ui.perfetto.dev/#!/?url="), "{u}");
        assert!(u.contains("http%3A%2F%2F127.0.0.1%3A5000%2Ftrace.pftrace"), "{u}");
    }

    #[test]
    fn serve_pftrace_answers_a_get_with_cors_and_body() {
        let listener = TcpListener::bind("127.0.0.1:0").unwrap();
        let port = listener.local_addr().unwrap().port();
        let body = b"PFTRACE-BYTES-123".to_vec();
        std::thread::scope(|scope| {
            let h = scope.spawn(|| {
                serve_pftrace(
                    &listener,
                    &body,
                    Instant::now() + Duration::from_secs(3),
                )
            });
            let mut c = TcpStream::connect(("127.0.0.1", port)).unwrap();
            c.write_all(b"GET /trace.pftrace HTTP/1.1\r\nHost: x\r\n\r\n")
                .unwrap();
            let mut resp = Vec::new();
            c.read_to_end(&mut resp).unwrap();
            let served = h.join().unwrap().unwrap();
            assert!(served);
            let text = String::from_utf8_lossy(&resp);
            assert!(text.contains("200 OK"), "{text}");
            assert!(
                text.contains("Access-Control-Allow-Origin: https://ui.perfetto.dev"),
                "{text}"
            );
            assert!(text.contains("PFTRACE-BYTES-123"), "{text}");
        });
    }

    #[test]
    fn serve_pftrace_answers_options_preflight() {
        let listener = TcpListener::bind("127.0.0.1:0").unwrap();
        let port = listener.local_addr().unwrap().port();
        let body = b"x".to_vec();
        std::thread::scope(|scope| {
            // Deadline is short; the preflight does not count as served, so the
            // server loops until the deadline and returns false.
            let h = scope.spawn(|| {
                serve_pftrace(
                    &listener,
                    &body,
                    Instant::now() + Duration::from_millis(400),
                )
            });
            let mut c = TcpStream::connect(("127.0.0.1", port)).unwrap();
            c.write_all(b"OPTIONS /trace.pftrace HTTP/1.1\r\nHost: x\r\n\r\n")
                .unwrap();
            let mut resp = Vec::new();
            c.read_to_end(&mut resp).unwrap();
            let served = h.join().unwrap().unwrap();
            assert!(!served);
            let text = String::from_utf8_lossy(&resp);
            assert!(text.contains("204 No Content"), "{text}");
            assert!(
                text.contains("Access-Control-Allow-Origin: https://ui.perfetto.dev"),
                "{text}"
            );
        });
    }

    #[test]
    fn run_open_missing_file_errors_before_binding() {
        let args = OpenArgs {
            file: PathBuf::from("/no/such/trace.pftrace"),
            no_browser: true,
            keep_alive_secs: 0,
        };
        let mut buf: Vec<u8> = Vec::new();
        let err = run_open(&args, false, &mut buf).unwrap_err();
        assert!(matches!(err, CliError::Other(_)));
    }

    #[test]
    fn run_open_no_browser_json_reports_urls_without_hanging() {
        // A real temp file, no browser, zero keep-alive → returns immediately
        // (served:false) with the URLs. Proves the headless/agent path.
        let mut f = std::env::temp_dir();
        f.push("pulp-trace-open-test.pftrace");
        std::fs::write(&f, b"PFTRACE").unwrap();
        let args = OpenArgs {
            file: f.clone(),
            no_browser: true,
            keep_alive_secs: 0,
        };
        let mut buf: Vec<u8> = Vec::new();
        run_open(&args, true, &mut buf).unwrap();
        let _ = std::fs::remove_file(&f);
        let out = String::from_utf8(buf).unwrap();
        let v: serde_json::Value = serde_json::from_str(out.trim()).unwrap();
        assert_eq!(v["browser_opened"], false);
        assert_eq!(v["served"], false);
        assert!(v["perfetto_url"]
            .as_str()
            .unwrap()
            .starts_with("https://ui.perfetto.dev/#!/?url="));
        assert!(v["serve_url"].as_str().unwrap().contains("/trace.pftrace"));
    }
}
