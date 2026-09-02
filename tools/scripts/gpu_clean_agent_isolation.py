#!/usr/bin/env python3
"""Bounded process and network isolation helpers for the A5 recorder.

The recorder is the trusted side of the two-party journey.  These helpers keep
untrusted command output out of unbounded memory and give the sandboxed Codex
process one narrow HTTPS egress path: a loopback CONNECT proxy whose upstream
host set is closed before the process starts.
"""

from __future__ import annotations

import os
import queue
import selectors
import signal
import socket
import subprocess
import threading
import time
from typing import Callable, Iterable


class IsolationError(RuntimeError):
    """A bounded process or proxy invariant failed."""


def _terminate_group(process: subprocess.Popen[bytes]) -> None:
    if process.poll() is not None:
        return
    try:
        os.killpg(process.pid, signal.SIGTERM)
    except ProcessLookupError:
        return
    try:
        process.wait(timeout=0.5)
        return
    except subprocess.TimeoutExpired:
        pass
    try:
        os.killpg(process.pid, signal.SIGKILL)
    except ProcessLookupError:
        pass
    try:
        process.wait(timeout=1.0)
    except subprocess.TimeoutExpired as exc:
        raise IsolationError("bounded command process group did not terminate") from exc


def bounded_run(
    argv: list[str], *, cwd: str | None = None, input_bytes: bytes | None = None,
    timeout: float = 30.0, output_limit: int = 4 * 1024 * 1024,
    env: dict[str, str] | None = None,
) -> subprocess.CompletedProcess[bytes]:
    """Run one process group while enforcing each output cap during reads."""

    if not argv or output_limit < 1 or timeout <= 0:
        raise IsolationError("bounded command arguments are invalid")
    try:
        process = subprocess.Popen(
            argv,
            cwd=cwd,
            stdin=subprocess.PIPE if input_bytes is not None else subprocess.DEVNULL,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            env=env,
            start_new_session=True,
        )
    except OSError as exc:
        raise IsolationError(f"bounded command could not start: {argv[0]}") from exc
    assert process.stdout is not None and process.stderr is not None
    messages: queue.Queue[tuple[str, bytes | BaseException | None]] = queue.Queue()

    def read_stream(name: str, stream: object) -> None:
        try:
            while True:
                chunk = stream.read(64 * 1024)  # type: ignore[attr-defined]
                if not chunk:
                    break
                messages.put((name, chunk))
        except BaseException as exc:  # surfaced on the recorder thread
            messages.put((name, exc))
        finally:
            messages.put((name, None))

    readers = [
        threading.Thread(target=read_stream, args=("stdout", process.stdout), daemon=True),
        threading.Thread(target=read_stream, args=("stderr", process.stderr), daemon=True),
    ]
    for reader in readers:
        reader.start()

    writer_error: list[BaseException] = []

    def write_input() -> None:
        assert process.stdin is not None
        try:
            process.stdin.write(input_bytes or b"")
            process.stdin.close()
        except (BrokenPipeError, OSError) as exc:
            if process.poll() is None:
                writer_error.append(exc)

    writer: threading.Thread | None = None
    if input_bytes is not None:
        writer = threading.Thread(target=write_input, daemon=True)
        writer.start()

    output = {"stdout": bytearray(), "stderr": bytearray()}
    finished: set[str] = set()
    deadline = time.monotonic() + timeout
    failure: IsolationError | None = None
    try:
        while len(finished) != 2:
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                failure = IsolationError(f"bounded command timed out: {argv[0]}")
                break
            try:
                name, item = messages.get(timeout=min(0.05, remaining))
            except queue.Empty:
                continue
            if item is None:
                finished.add(name)
            elif isinstance(item, BaseException):
                failure = IsolationError(f"bounded command {name} reader failed: {argv[0]}")
                break
            else:
                output[name].extend(item)
                if len(output[name]) > output_limit:
                    failure = IsolationError(
                        f"bounded command exceeded its {name} cap: {argv[0]}"
                    )
                    break
        if failure is not None:
            _terminate_group(process)
            raise failure
        remaining = max(0.01, deadline - time.monotonic())
        try:
            returncode = process.wait(timeout=remaining)
        except subprocess.TimeoutExpired as exc:
            _terminate_group(process)
            raise IsolationError(f"bounded command timed out: {argv[0]}") from exc
        if writer_error:
            raise IsolationError(f"bounded command stdin writer failed: {argv[0]}")
        return subprocess.CompletedProcess(
            argv, returncode, bytes(output["stdout"]), bytes(output["stderr"])
        )
    finally:
        if process.poll() is None:
            _terminate_group(process)
        for reader in readers:
            reader.join(timeout=1.0)
        if writer is not None:
            writer.join(timeout=1.0)
        process.stdout.close()
        process.stderr.close()
        if process.stdin is not None and not process.stdin.closed:
            process.stdin.close()


class ExactHostConnectProxy:
    """Recorder-owned HTTP CONNECT proxy with a closed HTTPS host allowlist."""

    def __init__(
        self, allowed_hosts: Iterable[str], *, max_connections: int = 32,
        max_bytes_each_way: int = 64 * 1024 * 1024, idle_timeout: float = 120.0,
        connector: Callable[[tuple[str, int], float], socket.socket] | None = None,
    ) -> None:
        hosts = frozenset(host.lower() for host in allowed_hosts)
        if (
            not hosts or any(not host.isascii() or not host or ":" in host for host in hosts)
            or max_connections < 1 or max_bytes_each_way < 1 or idle_timeout <= 0
        ):
            raise IsolationError("CONNECT proxy configuration is invalid")
        self.allowed_hosts = hosts
        self.max_connections = max_connections
        self.max_bytes_each_way = max_bytes_each_way
        self.idle_timeout = idle_timeout
        self._connector = connector or (
            lambda address, timeout: socket.create_connection(address, timeout=timeout)
        )
        self._listener: socket.socket | None = None
        self._port: int | None = None
        self._thread: threading.Thread | None = None
        self._clients: list[threading.Thread] = []
        self._records: list[dict[str, object]] = []
        self._errors: list[str] = []
        self._active_sockets: set[socket.socket] = set()
        self._lock = threading.Lock()
        self._stop = threading.Event()
        self._accepted = 0

    @property
    def port(self) -> int:
        if self._port is None:
            raise IsolationError("CONNECT proxy is not running")
        return self._port

    def __enter__(self) -> "ExactHostConnectProxy":
        listener = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        listener.bind(("127.0.0.1", 0))
        listener.listen(min(self.max_connections, 128))
        listener.settimeout(0.2)
        self._listener = listener
        self._port = int(listener.getsockname()[1])
        self._thread = threading.Thread(target=self._serve, daemon=True)
        self._thread.start()
        return self

    def __exit__(self, exc_type: object, exc: object, traceback: object) -> None:
        self.close()
        if exc is None:
            self.assert_healthy()

    def close(self) -> None:
        self._stop.set()
        if self._listener is not None:
            self._listener.close()
        if self._thread is not None:
            self._thread.join(timeout=2.0)
        with self._lock:
            active = list(self._active_sockets)
        for active_socket in active:
            try:
                active_socket.shutdown(socket.SHUT_RDWR)
            except OSError:
                pass
            active_socket.close()
        for client in list(self._clients):
            client.join(timeout=2.0)
            if client.is_alive():
                with self._lock:
                    self._errors.append("CONNECT client did not stop")

    def assert_healthy(self) -> None:
        with self._lock:
            errors = list(self._errors)
        if errors:
            raise IsolationError(f"CONNECT proxy failed closed: {errors[0]}")

    def audit(self) -> dict[str, object]:
        with self._lock:
            records = [dict(record) for record in self._records]
        return {
            "schema": "pulp.gpu-clean-agent-connect-proxy.v1",
            "listen_host": "127.0.0.1",
            "listen_port": self.port,
            "allowed_hosts": sorted(self.allowed_hosts),
            "max_connections": self.max_connections,
            "max_bytes_each_way": self.max_bytes_each_way,
            "connections": records,
        }

    def _serve(self) -> None:
        assert self._listener is not None
        while not self._stop.is_set():
            try:
                client, _ = self._listener.accept()
            except socket.timeout:
                continue
            except OSError:
                if not self._stop.is_set():
                    with self._lock:
                        self._errors.append("listener failed")
                return
            with self._lock:
                self._accepted += 1
                accepted = self._accepted
                self._active_sockets.add(client)
            if accepted > self.max_connections:
                client.close()
                with self._lock:
                    self._active_sockets.discard(client)
                    self._errors.append("connection-count cap exceeded")
                continue
            thread = threading.Thread(target=self._handle, args=(client,), daemon=True)
            self._clients.append(thread)
            thread.start()

    def _request(self, client: socket.socket) -> tuple[str, bytes]:
        client.settimeout(10.0)
        payload = bytearray()
        while b"\r\n\r\n" not in payload:
            chunk = client.recv(1024)
            if not chunk:
                raise IsolationError("CONNECT client closed before its request")
            payload.extend(chunk)
            if len(payload) > 8192:
                raise IsolationError("CONNECT request exceeded its header cap")
        header, remainder = bytes(payload).split(b"\r\n\r\n", 1)
        try:
            lines = header.decode("ascii").split("\r\n")
        except UnicodeDecodeError as exc:
            raise IsolationError("CONNECT request is not ASCII") from exc
        fields = lines[0].split(" ")
        if len(fields) != 3 or fields[0] != "CONNECT" or fields[2] not in {"HTTP/1.0", "HTTP/1.1"}:
            raise IsolationError("proxy accepts only HTTP CONNECT")
        target = fields[1].rsplit(":", 1)
        if len(target) != 2 or target[1] != "443":
            raise IsolationError("CONNECT target must use HTTPS port 443")
        host = target[0].lower()
        if host not in self.allowed_hosts:
            raise IsolationError("CONNECT target host is not allowlisted")
        return host, remainder

    def _handle(self, client: socket.socket) -> None:
        host = "rejected"
        record: dict[str, object] = {
            "host": host, "port": 443, "outcome": "rejected",
            "bytes_to_upstream": 0, "bytes_to_client": 0,
        }
        upstream: socket.socket | None = None
        try:
            host, remainder = self._request(client)
            record["host"] = host
            upstream = self._connector((host, 443), 10.0)
            with self._lock:
                self._active_sockets.add(upstream)
            upstream.settimeout(10.0)
            client.sendall(b"HTTP/1.1 200 Connection Established\r\n\r\n")
            client.settimeout(10.0)
            if remainder:
                upstream.sendall(remainder)
                record["bytes_to_upstream"] = len(remainder)
            self._relay(client, upstream, record)
            record["outcome"] = "completed"
        except (IsolationError, OSError) as exc:
            if str(exc) == "CONNECT client closed before its request":
                record["outcome"] = "transport-preflight"
                return
            record["outcome"] = "rejected" if host == "rejected" else "failed"
            record["error"] = type(exc).__name__
            try:
                client.sendall(b"HTTP/1.1 403 Forbidden\r\nContent-Length: 0\r\n\r\n")
            except OSError:
                pass
            with self._lock:
                self._errors.append(str(exc))
        finally:
            client.close()
            if upstream is not None:
                upstream.close()
            with self._lock:
                self._active_sockets.discard(client)
                if upstream is not None:
                    self._active_sockets.discard(upstream)
                self._records.append(record)

    def _relay(
        self, client: socket.socket, upstream: socket.socket,
        record: dict[str, object],
    ) -> None:
        selector = selectors.DefaultSelector()
        selector.register(client, selectors.EVENT_READ, (upstream, "bytes_to_upstream"))
        selector.register(upstream, selectors.EVENT_READ, (client, "bytes_to_client"))
        deadline = time.monotonic() + self.idle_timeout
        try:
            while True:
                events = selector.select(timeout=min(1.0, max(0.0, deadline - time.monotonic())))
                if not events:
                    if time.monotonic() >= deadline:
                        raise IsolationError("CONNECT tunnel exceeded its idle timeout")
                    continue
                deadline = time.monotonic() + self.idle_timeout
                for key, _ in events:
                    target, counter = key.data
                    chunk = key.fileobj.recv(64 * 1024)
                    if not chunk:
                        return
                    target.sendall(chunk)
                    record[counter] = int(record[counter]) + len(chunk)
                    if int(record[counter]) > self.max_bytes_each_way:
                        raise IsolationError("CONNECT tunnel exceeded its byte cap")
        finally:
            selector.close()
