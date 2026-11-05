#!/usr/bin/env python3
"""Launch a suspended Scrap Mechanic client, inject the probe, force a
-connect_steam_id value into the game's parser, then resume.

The launcher is intended to run inside the Sandboxie box for the second Steam
account. It does not launch Steam and does not modify game files.
"""
from __future__ import annotations

import argparse
import ctypes as C
from ctypes import wintypes as W
import json
import os
from pathlib import Path
import sys

import pefile

CREATE_SUSPENDED = 0x00000004
PROCESS_ALL = 0x0002 | 0x0008 | 0x0020 | 0x0010 | 0x1000
MEM_COMMIT_RESERVE = 0x3000
PAGE_READWRITE = 0x04
INFINITE = 0xFFFFFFFF
WAIT_OBJECT_0 = 0


class STARTUPINFOW(C.Structure):
    _fields_ = [("cb", W.DWORD), ("lpReserved", W.LPWSTR), ("lpDesktop", W.LPWSTR),
                ("lpTitle", W.LPWSTR), ("dwX", W.DWORD), ("dwY", W.DWORD),
                ("dwXSize", W.DWORD), ("dwYSize", W.DWORD), ("dwXCountChars", W.DWORD),
                ("dwYCountChars", W.DWORD), ("dwFillAttribute", W.DWORD), ("dwFlags", W.DWORD),
                ("wShowWindow", W.WORD), ("cbReserved2", W.WORD),
                ("lpReserved2", C.POINTER(C.c_byte)), ("hStdInput", W.HANDLE),
                ("hStdOutput", W.HANDLE), ("hStdError", W.HANDLE)]


class PROCESS_INFORMATION(C.Structure):
    _fields_ = [("hProcess", W.HANDLE), ("hThread", W.HANDLE),
                ("dwProcessId", W.DWORD), ("dwThreadId", W.DWORD)]


class MODULEENTRY32W(C.Structure):
    _fields_ = [("dwSize", W.DWORD), ("th32ModuleID", W.DWORD), ("th32ProcessID", W.DWORD),
                ("GlblcntUsage", W.DWORD), ("ProccntUsage", W.DWORD),
                ("modBaseAddr", C.c_void_p), ("modBaseSize", W.DWORD),
                ("hModule", W.HMODULE), ("szModule", W.WCHAR * 256),
                ("szExePath", W.WCHAR * 260)]


def bind(dll, name, restype, argtypes):
    function = getattr(dll, name)
    function.restype, function.argtypes = restype, argtypes
    return function


def export_rva(dll: Path, name: str) -> int:
    pe = pefile.PE(str(dll))
    try:
        matches = [entry for entry in pe.DIRECTORY_ENTRY_EXPORT.symbols
                   if entry.name == name.encode() and not entry.forwarder]
        if len(matches) != 1:
            raise RuntimeError(f"export {name} missing or forwarded")
        return matches[0].address
    finally:
        pe.close()


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--exe", type=Path, required=True)
    parser.add_argument("--dll", type=Path, required=True)
    parser.add_argument("--connect-id", type=int, required=True)
    parser.add_argument("--log", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    exe = args.exe.resolve(strict=True)
    dll = args.dll.resolve(strict=True)
    if not args.log.parent.is_dir():
        raise RuntimeError("log parent directory must exist")
    if args.log.exists():
        raise RuntimeError("log path must be new")
    os.environ["SteamAppId"] = "387990"
    os.environ["SteamGameId"] = "387990"

    k32 = C.WinDLL("kernel32", use_last_error=True)
    create_process = bind(k32, "CreateProcessW", W.BOOL,
                          [W.LPCWSTR, W.LPWSTR, C.c_void_p, C.c_void_p, W.BOOL, W.DWORD,
                           C.c_void_p, W.LPCWSTR, C.POINTER(STARTUPINFOW), C.POINTER(PROCESS_INFORMATION)])
    resume = bind(k32, "ResumeThread", W.DWORD, [W.HANDLE])
    wait = bind(k32, "WaitForSingleObject", W.DWORD, [W.HANDLE, W.DWORD])
    exit_code = bind(k32, "GetExitCodeThread", W.BOOL, [W.HANDLE, C.POINTER(W.DWORD)])
    close = bind(k32, "CloseHandle", W.BOOL, [W.HANDLE])
    alloc = bind(k32, "VirtualAllocEx", C.c_void_p,
                 [W.HANDLE, C.c_void_p, C.c_size_t, W.DWORD, W.DWORD])
    free = bind(k32, "VirtualFreeEx", W.BOOL, [W.HANDLE, C.c_void_p, C.c_size_t, W.DWORD])
    write = bind(k32, "WriteProcessMemory", W.BOOL,
                 [W.HANDLE, C.c_void_p, C.c_void_p, C.c_size_t, C.POINTER(C.c_size_t)])
    create_thread = bind(k32, "CreateRemoteThread", W.HANDLE,
                         [W.HANDLE, C.c_void_p, C.c_size_t, C.c_void_p, C.c_void_p, W.DWORD, C.POINTER(W.DWORD)])
    get_module = bind(k32, "GetModuleHandleW", W.HMODULE, [W.LPCWSTR])
    get_proc = bind(k32, "GetProcAddress", C.c_void_p, [W.HMODULE, C.c_char_p])
    snapshot = bind(k32, "CreateToolhelp32Snapshot", W.HANDLE, [W.DWORD, W.DWORD])
    first = bind(k32, "Module32FirstW", W.BOOL, [W.HANDLE, C.POINTER(MODULEENTRY32W)])
    next_ = bind(k32, "Module32NextW", W.BOOL, [W.HANDLE, C.POINTER(MODULEENTRY32W)])

    def check(value, what):
        if not value:
            raise OSError(C.get_last_error(), what)
        return value

    si = STARTUPINFOW()
    si.cb = C.sizeof(si)
    pi = PROCESS_INFORMATION()
    command = f'"{exe}" -window -connect_steam_id {args.connect_id}'
    if not create_process(str(exe), command, None, None, False, CREATE_SUSPENDED, None,
                          str(exe.parent), C.byref(si), C.byref(pi)):
        raise OSError(C.get_last_error(), "CreateProcessW")
    process = pi.hProcess
    result = {"pid": pi.dwProcessId, "exe": str(exe), "dll": str(dll),
              "connect_id": str(args.connect_id), "log": str(args.log), "status": "suspended"}
    try:
        def remote_run(address, parameter=None):
            mem = None
            if parameter is not None:
                encoded = parameter if isinstance(parameter, bytes) else (str(parameter) + "\0").encode("utf-16-le")
                mem = check(alloc(process, None, len(encoded), MEM_COMMIT_RESERVE, PAGE_READWRITE), "VirtualAllocEx")
                written = C.c_size_t()
                check(write(process, mem, C.create_string_buffer(encoded), len(encoded), C.byref(written)), "WriteProcessMemory")
            thread = check(create_thread(process, None, 0, address, mem, 0, None), "CreateRemoteThread")
            try:
                if wait(thread, 20000) != WAIT_OBJECT_0:
                    raise RuntimeError("remote thread timeout")
                code = W.DWORD()
                check(exit_code(thread, C.byref(code)), "GetExitCodeThread")
                return code.value
            finally:
                close(thread)
                if mem:
                    free(process, mem, 0, 0x8000)

        loader = get_proc(get_module("kernel32.dll"), b"LoadLibraryW")
        remote_run(loader, str(dll))
        handle = snapshot(0x8 | 0x10, pi.dwProcessId)
        if handle == C.c_void_p(-1).value:
            raise OSError(C.get_last_error(), "CreateToolhelp32Snapshot")
        base = None
        try:
            entry = MODULEENTRY32W()
            entry.dwSize = C.sizeof(entry)
            if not first(handle, C.byref(entry)):
                raise OSError(C.get_last_error(), "Module32FirstW")
            while True:
                if Path(entry.szExePath).name.lower() == dll.name.lower():
                    base = int(entry.modBaseAddr or 0)
                    break
                if not next_(handle, C.byref(entry)):
                    break
        finally:
            close(handle)
        if not base:
            raise RuntimeError("probe DLL not found in target modules")
        result["dll_base"] = hex(base)
        start = remote_run(base + export_rva(dll, "ScrapdsProbeStart"), str(args.log))
        result["start_status"] = start
        if start != 0:
            raise RuntimeError(f"ScrapdsProbeStart failed: {start}")
        forced = remote_run(base + export_rva(dll, "ScrapdsForceConnectId"), str(args.connect_id))
        result["force_status"] = forced
        if forced != 0:
            raise RuntimeError(f"ScrapdsForceConnectId failed: {forced}")
        resume(pi.hThread)
        result["status"] = "resumed"
        args.output.write_text(json.dumps(result, indent=2), encoding="utf8")
        return 0
    except Exception as error:
        result["status"] = "error"
        result["error"] = str(error)
        args.output.write_text(json.dumps(result, indent=2), encoding="utf8")
        raise
    finally:
        close(pi.hThread)
        close(process)


if __name__ == "__main__":
    sys.exit(main())