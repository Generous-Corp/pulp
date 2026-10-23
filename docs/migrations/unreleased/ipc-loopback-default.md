---
breaking = true
summary = "IPC socket servers with an omitted host bind 127.0.0.1; named-pipe FIFOs are owner-only"
---

## Breaking changes
- An `InterprocessConnection` / `InterprocessConnectionServer` TCP endpoint whose `host` is empty now binds `127.0.0.1`. Only the literal `0.0.0.0` binds every interface. An application that relied on the old all-interfaces default to accept LAN peers stops accepting them.
- `NamedPipe::create_server` creates POSIX FIFOs with mode 0600 (was 0666), and `ChildProcessManager` places its pipes under a per-user 0700 directory. A peer must run as the same user.

## Migration steps
- To accept remote peers deliberately, set `host = "0.0.0.0"` on the endpoint.
- Cross-account helpers that used a FIFO need a different transport (a socket with explicit host, or a shared file protocol).
