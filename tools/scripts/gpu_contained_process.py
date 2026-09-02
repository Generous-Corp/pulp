#!/usr/bin/env python3
"""Bounded process-tree ownership for GPU validation adapters."""

from __future__ import annotations

import ctypes
import os
import signal
import subprocess
import time
from pathlib import Path
from typing import Any

TERMINATION_GRACE_SECONDS = 2.0
TERMINATION_FINAL_SECONDS = 2.0
WINDOWS_JOB_ATTRIBUTE = "_pulp_gpu_adapter_job"


class ProcessTreeTerminationError(RuntimeError):
    """An owned process tree could not be contained, terminated, or reaped."""

    code = "adapter-termination-failed"


def _windows_kernel32() -> Any:
    kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
    kernel32.CreateJobObjectW.argtypes = [ctypes.c_void_p, ctypes.c_wchar_p]
    kernel32.CreateJobObjectW.restype = ctypes.c_void_p
    kernel32.SetInformationJobObject.argtypes = [
        ctypes.c_void_p, ctypes.c_int, ctypes.c_void_p, ctypes.c_uint32,
    ]
    kernel32.SetInformationJobObject.restype = ctypes.c_int
    kernel32.AssignProcessToJobObject.argtypes = [ctypes.c_void_p, ctypes.c_void_p]
    kernel32.AssignProcessToJobObject.restype = ctypes.c_int
    kernel32.TerminateJobObject.argtypes = [ctypes.c_void_p, ctypes.c_uint32]
    kernel32.TerminateJobObject.restype = ctypes.c_int
    kernel32.QueryInformationJobObject.argtypes = [
        ctypes.c_void_p, ctypes.c_int, ctypes.c_void_p, ctypes.c_uint32,
        ctypes.c_void_p,
    ]
    kernel32.QueryInformationJobObject.restype = ctypes.c_int
    kernel32.CreateToolhelp32Snapshot.argtypes = [ctypes.c_uint32, ctypes.c_uint32]
    kernel32.CreateToolhelp32Snapshot.restype = ctypes.c_void_p
    kernel32.Thread32First.argtypes = [ctypes.c_void_p, ctypes.c_void_p]
    kernel32.Thread32First.restype = ctypes.c_int
    kernel32.Thread32Next.argtypes = [ctypes.c_void_p, ctypes.c_void_p]
    kernel32.Thread32Next.restype = ctypes.c_int
    kernel32.OpenThread.argtypes = [ctypes.c_uint32, ctypes.c_int, ctypes.c_uint32]
    kernel32.OpenThread.restype = ctypes.c_void_p
    kernel32.ResumeThread.argtypes = [ctypes.c_void_p]
    kernel32.ResumeThread.restype = ctypes.c_uint32
    kernel32.CloseHandle.argtypes = [ctypes.c_void_p]
    kernel32.CloseHandle.restype = ctypes.c_int
    return kernel32


def _assign_windows_job(process: subprocess.Popen[bytes]) -> None:
    """Put a suspended process in a kill-on-close Job Object."""
    class BasicLimitInformation(ctypes.Structure):
        _fields_ = [
            ("per_process_user_time_limit", ctypes.c_int64),
            ("per_job_user_time_limit", ctypes.c_int64),
            ("limit_flags", ctypes.c_uint32),
            ("minimum_working_set_size", ctypes.c_size_t),
            ("maximum_working_set_size", ctypes.c_size_t),
            ("active_process_limit", ctypes.c_uint32),
            ("affinity", ctypes.c_size_t),
            ("priority_class", ctypes.c_uint32),
            ("scheduling_class", ctypes.c_uint32),
        ]

    class IoCounters(ctypes.Structure):
        _fields_ = [
            ("read_operation_count", ctypes.c_uint64),
            ("write_operation_count", ctypes.c_uint64),
            ("other_operation_count", ctypes.c_uint64),
            ("read_transfer_count", ctypes.c_uint64),
            ("write_transfer_count", ctypes.c_uint64),
            ("other_transfer_count", ctypes.c_uint64),
        ]

    class ExtendedLimitInformation(ctypes.Structure):
        _fields_ = [
            ("basic_limit_information", BasicLimitInformation),
            ("io_info", IoCounters),
            ("process_memory_limit", ctypes.c_size_t),
            ("job_memory_limit", ctypes.c_size_t),
            ("peak_process_memory_used", ctypes.c_size_t),
            ("peak_job_memory_used", ctypes.c_size_t),
        ]

    kernel32 = _windows_kernel32()
    handle = kernel32.CreateJobObjectW(None, None)
    if not handle:
        raise ProcessTreeTerminationError(
            f"{ProcessTreeTerminationError.code}: CreateJobObjectW failed "
            f"with Windows error {ctypes.get_last_error()}"
        )
    information = ExtendedLimitInformation()
    information.basic_limit_information.limit_flags = 0x00002000
    if not kernel32.SetInformationJobObject(
        handle, 9, ctypes.byref(information), ctypes.sizeof(information)
    ):
        error = ctypes.get_last_error()
        kernel32.CloseHandle(handle)
        raise ProcessTreeTerminationError(
            f"{ProcessTreeTerminationError.code}: configuring the adapter Job Object "
            f"failed with Windows error {error}"
        )
    process_handle = ctypes.c_void_p(int(getattr(process, "_handle")))
    if not kernel32.AssignProcessToJobObject(handle, process_handle):
        error = ctypes.get_last_error()
        kernel32.CloseHandle(handle)
        raise ProcessTreeTerminationError(
            f"{ProcessTreeTerminationError.code}: assigning the adapter to its Job "
            f"Object failed with Windows error {error}"
        )
    setattr(process, WINDOWS_JOB_ATTRIBUTE, int(handle))


def _resume_windows_process(process: subprocess.Popen[bytes]) -> None:
    """Resume the sole thread only after its process belongs to the Job Object."""
    class ThreadEntry32(ctypes.Structure):
        _fields_ = [
            ("size", ctypes.c_uint32),
            ("usage", ctypes.c_uint32),
            ("thread_id", ctypes.c_uint32),
            ("owner_process_id", ctypes.c_uint32),
            ("base_priority", ctypes.c_long),
            ("priority_delta", ctypes.c_long),
            ("flags", ctypes.c_uint32),
        ]

    kernel32 = _windows_kernel32()
    snapshot = kernel32.CreateToolhelp32Snapshot(0x00000004, 0)
    if not snapshot or snapshot == ctypes.c_void_p(-1).value:
        raise ProcessTreeTerminationError(
            f"{ProcessTreeTerminationError.code}: enumerating the suspended adapter "
            f"thread failed with Windows error {ctypes.get_last_error()}"
        )
    entry = ThreadEntry32()
    entry.size = ctypes.sizeof(entry)
    thread_id: int | None = None
    try:
        found = kernel32.Thread32First(snapshot, ctypes.byref(entry))
        while found:
            if entry.owner_process_id == process.pid:
                thread_id = int(entry.thread_id)
                break
            found = kernel32.Thread32Next(snapshot, ctypes.byref(entry))
    finally:
        kernel32.CloseHandle(snapshot)
    if thread_id is None:
        raise ProcessTreeTerminationError(
            f"{ProcessTreeTerminationError.code}: suspended adapter thread not found"
        )
    thread = kernel32.OpenThread(0x0002, False, thread_id)
    if not thread:
        raise ProcessTreeTerminationError(
            f"{ProcessTreeTerminationError.code}: opening the suspended adapter thread "
            f"failed with Windows error {ctypes.get_last_error()}"
        )
    try:
        previous_count = kernel32.ResumeThread(thread)
    finally:
        kernel32.CloseHandle(thread)
    if previous_count != 1:
        raise ProcessTreeTerminationError(
            f"{ProcessTreeTerminationError.code}: resuming the contained adapter thread "
            f"returned unexpected suspend count {previous_count}"
        )


def _close_windows_job(process: subprocess.Popen[bytes]) -> None:
    handle = getattr(process, WINDOWS_JOB_ATTRIBUTE, None)
    if not isinstance(handle, int) or handle <= 0:
        return
    setattr(process, WINDOWS_JOB_ATTRIBUTE, None)
    if not _windows_kernel32().CloseHandle(ctypes.c_void_p(handle)):
        raise ProcessTreeTerminationError(
            f"{ProcessTreeTerminationError.code}: closing the adapter Job Object "
            f"failed with Windows error {ctypes.get_last_error()}"
        )


def _terminate_windows_job(process: subprocess.Popen[bytes]) -> None:
    handle = getattr(process, WINDOWS_JOB_ATTRIBUTE, None)
    if not isinstance(handle, int) or handle <= 0:
        if process.poll() is None:
            process.kill()
        return
    if not _windows_kernel32().TerminateJobObject(ctypes.c_void_p(handle), 1):
        raise ProcessTreeTerminationError(
            f"{ProcessTreeTerminationError.code}: terminating the adapter Job Object "
            f"failed with Windows error {ctypes.get_last_error()}"
        )


def _windows_job_active_processes(process: subprocess.Popen[bytes]) -> int:
    class BasicAccountingInformation(ctypes.Structure):
        _fields_ = [
            ("total_user_time", ctypes.c_int64),
            ("total_kernel_time", ctypes.c_int64),
            ("period_user_time", ctypes.c_int64),
            ("period_kernel_time", ctypes.c_int64),
            ("total_page_fault_count", ctypes.c_uint32),
            ("total_processes", ctypes.c_uint32),
            ("active_processes", ctypes.c_uint32),
            ("total_terminated_processes", ctypes.c_uint32),
        ]

    handle = getattr(process, WINDOWS_JOB_ATTRIBUTE, None)
    if not isinstance(handle, int) or handle <= 0:
        return 0
    information = BasicAccountingInformation()
    if not _windows_kernel32().QueryInformationJobObject(
        ctypes.c_void_p(handle), 1, ctypes.byref(information),
        ctypes.sizeof(information), None,
    ):
        raise ProcessTreeTerminationError(
            f"{ProcessTreeTerminationError.code}: querying adapter Job Object "
            f"liveness failed with Windows error {ctypes.get_last_error()}"
        )
    return int(information.active_processes)


def _wait_windows_job_empty(
    process: subprocess.Popen[bytes], timeout_seconds: float,
) -> None:
    deadline = time.monotonic() + timeout_seconds
    while _windows_job_active_processes(process) != 0:
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            raise ProcessTreeTerminationError(
                f"{ProcessTreeTerminationError.code}: adapter Job Object retained "
                "live descendants past the final termination bound"
            )
        time.sleep(min(0.02, remaining))


def spawn_contained(
    command: list[str], *, cwd: Path, stdin: Any = None,
) -> subprocess.Popen[bytes]:
    """Spawn in a POSIX session or a race-free Windows Job Object."""
    creationflags = (
        (
            getattr(subprocess, "CREATE_NEW_PROCESS_GROUP", 0x00000200)
            | getattr(subprocess, "CREATE_SUSPENDED", 0x00000004)
        )
        if os.name == "nt" else 0
    )
    process = subprocess.Popen(
        command, cwd=cwd, stdin=stdin, stdout=subprocess.PIPE,
        stderr=subprocess.PIPE, start_new_session=os.name == "posix",
        creationflags=creationflags,
    )
    if os.name != "nt":
        return process
    try:
        _assign_windows_job(process)
        _resume_windows_process(process)
    except Exception as error:
        cleanup_error: Exception | None = None
        try:
            handle = getattr(process, WINDOWS_JOB_ATTRIBUTE, None)
            if isinstance(handle, int) and handle > 0:
                try:
                    _terminate_windows_job(process)
                except ProcessTreeTerminationError as terminate_error:
                    cleanup_error = terminate_error
                    process.kill()
            else:
                process.kill()
            try:
                process.wait(timeout=TERMINATION_FINAL_SECONDS)
            except (OSError, subprocess.TimeoutExpired) as wait_error:
                cleanup_error = wait_error
            if isinstance(handle, int) and handle > 0:
                try:
                    _wait_windows_job_empty(process, TERMINATION_FINAL_SECONDS)
                except ProcessTreeTerminationError as job_wait_error:
                    cleanup_error = cleanup_error or job_wait_error
        except OSError as terminate_error:
            cleanup_error = terminate_error
        finally:
            try:
                _close_windows_job(process)
            except ProcessTreeTerminationError as close_error:
                cleanup_error = cleanup_error or close_error
        if cleanup_error is not None:
            raise ProcessTreeTerminationError(
                f"{ProcessTreeTerminationError.code}: suspended adapter cleanup "
                "did not complete safely within its bounded spawn failure path"
            ) from cleanup_error
        if isinstance(error, ProcessTreeTerminationError):
            raise
        raise ProcessTreeTerminationError(
            f"{ProcessTreeTerminationError.code}: adapter Job Object setup failed"
        ) from error
    return process


def terminate_contained(process: subprocess.Popen[bytes]) -> None:
    """Terminate an owned tree with bounded graceful and force waits."""
    try:
        if os.name == "posix":
            os.killpg(process.pid, signal.SIGTERM)
        elif process.poll() is None:
            process.terminate()
    except ProcessLookupError:
        pass
    except OSError:
        pass
    if process.poll() is None:
        try:
            process.wait(timeout=TERMINATION_GRACE_SECONDS)
        except subprocess.TimeoutExpired:
            pass

    force_error: Exception | None = None
    try:
        if os.name == "posix":
            os.killpg(process.pid, signal.SIGKILL)
        elif os.name == "nt":
            _terminate_windows_job(process)
        elif process.poll() is None:
            process.kill()
    except ProcessLookupError:
        pass
    except (OSError, ProcessTreeTerminationError) as error:
        force_error = error

    final_error: subprocess.TimeoutExpired | None = None
    try:
        if process.poll() is None:
            process.wait(timeout=TERMINATION_FINAL_SECONDS)
    except subprocess.TimeoutExpired as error:
        final_error = error
    except OSError as error:
        force_error = force_error or error
    finally:
        if os.name == "nt":
            try:
                _wait_windows_job_empty(process, TERMINATION_FINAL_SECONDS)
            except ProcessTreeTerminationError as error:
                force_error = force_error or error
            try:
                _close_windows_job(process)
            except ProcessTreeTerminationError as error:
                force_error = force_error or error

    if final_error is not None:
        raise ProcessTreeTerminationError(
            f"{ProcessTreeTerminationError.code}: adapter pid {process.pid} did not "
            "exit within the final termination bound"
        ) from final_error
    if force_error is not None:
        raise ProcessTreeTerminationError(
            f"{ProcessTreeTerminationError.code}: adapter process-tree termination "
            f"failed: {force_error}"
        ) from force_error
