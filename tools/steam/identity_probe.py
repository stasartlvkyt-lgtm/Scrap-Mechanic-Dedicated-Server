#!/usr/bin/env python3
"""Test anonymous Steam GameServer identity using the installed game's DLL.

Windows x64 only. Uses the SDK1.52/v014 flat C ABI, checked against Valve-
authored headers at the pinned revision below. Does not initialize the user
API, create a gameplay/SNS listener, restart Steam, or use account credentials.
Steam GameServer initialization can bind its own local query socket.
"""
from __future__ import annotations

import argparse
import ctypes as C
import hashlib
import json
import os
from pathlib import Path
import struct
import sys
import time

SDK_COMMIT = "99411b13b18d6e7f275da62d5557fac34b52d330"
SDK_BASE = f"https://github.com/rlabrecque/SteamworksSDK/blob/{SDK_COMMIT}/public/steam/"
APP_ID = 387990


class CallbackMsg(C.Structure):
    _pack_ = 8
    _fields_ = [("user", C.c_int32), ("callback", C.c_int32), ("parameter", C.c_void_p), ("size", C.c_int32)]


SIGNATURES = {
    "SteamInternal_GameServer_Init": (C.c_bool, [C.c_uint32, C.c_uint16, C.c_uint16, C.c_uint16, C.c_int, C.c_char_p]),
    "SteamAPI_SteamGameServer_v014": (C.c_void_p, []),
    "SteamAPI_ISteamGameServer_SetProduct": (None, [C.c_void_p, C.c_char_p]),
    "SteamAPI_ISteamGameServer_SetGameDescription": (None, [C.c_void_p, C.c_char_p]),
    "SteamAPI_ISteamGameServer_SetModDir": (None, [C.c_void_p, C.c_char_p]),
    "SteamAPI_ISteamGameServer_SetDedicatedServer": (None, [C.c_void_p, C.c_bool]),
    "SteamAPI_ISteamGameServer_SetAdvertiseServerActive": (None, [C.c_void_p, C.c_bool]),
    "SteamAPI_ISteamGameServer_LogOnAnonymous": (None, [C.c_void_p]),
    "SteamAPI_ISteamGameServer_BLoggedOn": (C.c_bool, [C.c_void_p]),
    "SteamAPI_ISteamGameServer_GetSteamID": (C.c_uint64, [C.c_void_p]),
    "SteamAPI_ISteamGameServer_LogOff": (None, [C.c_void_p]),
    "SteamGameServer_Shutdown": (None, []),
    "SteamGameServer_GetHSteamPipe": (C.c_int32, []),
    "SteamAPI_ManualDispatch_Init": (None, []),
    "SteamAPI_ManualDispatch_RunFrame": (None, [C.c_int32]),
    "SteamAPI_ManualDispatch_GetNextCallback": (C.c_bool, [C.c_int32, C.POINTER(CallbackMsg)]),
    "SteamAPI_ManualDispatch_FreeLastCallback": (None, [C.c_int32]),
}


def inspect_exports(dll_path: Path) -> dict:
    import pefile
    with pefile.PE(str(dll_path)) as pe:
        exports = {entry.name.decode("ascii") for entry in pe.DIRECTORY_ENTRY_EXPORT.symbols if entry.name}
        if pe.FILE_HEADER.Machine != 0x8664:
            raise ValueError("Expected x64 PE DLL")
    missing = sorted(set(SIGNATURES) - exports)
    if missing:
        raise ValueError(f"Required SDK1.52 exports missing: {missing}")
    return {
        "path": str(dll_path),
        "sha256": hashlib.sha256(dll_path.read_bytes()).hexdigest(),
        "machine": "AMD64",
        "verified_exports": sorted(SIGNATURES),
    }


def decode_callback(identifier: int, payload: bytes) -> dict:
    result = {"id": identifier, "payload_bytes": len(payload)}
    if identifier == 101:
        result["name"] = "SteamServersConnected_t"
    elif identifier == 102 and len(payload) >= 5:
        result.update(name="SteamServerConnectFailure_t", eresult=struct.unpack_from("<i", payload)[0], still_retrying=bool(payload[4]))
    elif identifier == 103 and len(payload) >= 4:
        result.update(name="SteamServersDisconnected_t", eresult=struct.unpack_from("<i", payload)[0])
    return result


def run_probe(dll_path: Path, timeout: float, game_port: int, query_port: int, on_logged_on=None) -> dict:
    if os.name != "nt" or C.sizeof(C.c_void_p) != 8:
        raise ValueError("Probe requires Windows x64 Python")
    if not 0 < timeout <= 30:
        raise ValueError("Timeout must be >0 and <=30 seconds")
    if not all(1024 <= port < 65535 for port in [game_port, query_port]) or game_port == query_port:
        raise ValueError("Distinct nonprivileged game/query ports required")
    if C.sizeof(CallbackMsg) != 24 or CallbackMsg.parameter.offset != 8 or CallbackMsg.size.offset != 16:
        raise ValueError("Unexpected callback structure layout")
    dll_path = dll_path.resolve(strict=True)
    record = {
        "format": "scrapds.steam-identity-probe.v1",
        "app_id": APP_ID,
        "dll": inspect_exports(dll_path),
        "abi_sources": [SDK_BASE + name for name in ["steam_gameserver.h", "steam_api_flat.h", "steam_api_internal.h", "isteamuser.h", "steamtypes.h"]],
        "official_documentation": ["https://partner.steamgames.com/doc/api/steam_gameserver", "https://partner.steamgames.com/doc/api/ISteamGameServer"],
        "parameters": {"bind_ip": "127.0.0.1", "legacy_steam_port": 0, "game_port": game_port, "query_port": query_port, "server_mode": 2, "version": "1.0.7", "advertising": False, "anonymous_only": True, "gameplay_listener": False, "timeout_seconds": timeout},
        "callbacks": [],
    }
    # These variables affect this short-lived process only, never Steam's config.
    previous_env = {key: os.environ.get(key) for key in ["SteamAppId", "SteamGameId"]}
    os.environ.update(SteamAppId=str(APP_ID), SteamGameId=str(APP_ID))
    initialized = False
    server = None
    started = time.monotonic()
    dll_directory = None
    functions = {}
    try:
        dll_directory = os.add_dll_directory(str(dll_path.parent))
        library = C.CDLL(str(dll_path))  # S_CALLTYPE is __cdecl; all functions are flat C.
        for name, (returns, arguments) in SIGNATURES.items():
            function = getattr(library, name)
            function.restype, function.argtypes = returns, arguments
            functions[name] = function
        initialized = bool(functions["SteamInternal_GameServer_Init"](0x7F000001, 0, game_port, query_port, 2, b"1.0.7"))
        record["initialized"] = initialized
        if not initialized:
            record["status"] = "initialization_failed"
            return record
        server = functions["SteamAPI_SteamGameServer_v014"]()
        if not server:
            record["status"] = "null_gameserver_interface"
            return record
        for name, value in [("SetProduct", b"387990"), ("SetGameDescription", b"Scrap Mechanic"), ("SetModDir", b"Scrap Mechanic"), ("SetDedicatedServer", True), ("SetAdvertiseServerActive", False)]:
            functions["SteamAPI_ISteamGameServer_" + name](server, value)
        functions["SteamAPI_ManualDispatch_Init"]()
        pipe = functions["SteamGameServer_GetHSteamPipe"]()
        if not pipe:
            record["status"] = "null_gameserver_pipe"
            return record
        functions["SteamAPI_ISteamGameServer_LogOnAnonymous"](server)
        record["logon_anonymous_called"] = True
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            functions["SteamAPI_ManualDispatch_RunFrame"](pipe)
            callback = CallbackMsg()
            for _ in range(1000):
                if not functions["SteamAPI_ManualDispatch_GetNextCallback"](pipe, C.byref(callback)):
                    break
                try:
                    # Only known connection-status callbacks are decoded. No
                    # auth tickets, addresses or arbitrary payloads are logged.
                    payload = b""
                    if callback.callback in {101, 102, 103} and 0 <= callback.size <= 64 and callback.parameter:
                        payload = C.string_at(callback.parameter, callback.size)
                    event = decode_callback(callback.callback, payload)
                    event["elapsed_seconds"] = round(time.monotonic() - started, 3)
                    event["reported_payload_bytes"] = callback.size
                    if len(record["callbacks"]) < 100:
                        record["callbacks"].append(event)
                finally:
                    functions["SteamAPI_ManualDispatch_FreeLastCallback"](pipe)
            logged_on = bool(functions["SteamAPI_ISteamGameServer_BLoggedOn"](server))
            steam_id = int(functions["SteamAPI_ISteamGameServer_GetSteamID"](server))
            record.update(logged_on=logged_on, steam_id=str(steam_id), steam_account_type=(steam_id >> 52) & 15, universe=(steam_id >> 56) & 255)
            if logged_on and steam_id:
                record["status"] = "logged_on"
                if on_logged_on is not None:
                    on_logged_on(library, functions, server, pipe, record)
                break
            time.sleep(0.05)
        else:
            record["status"] = "login_timeout"
        return record
    finally:
        if server:
            functions["SteamAPI_ISteamGameServer_LogOff"](server)
        if initialized:
            functions["SteamGameServer_Shutdown"]()
        if dll_directory:
            dll_directory.close()
        for key, previous in previous_env.items():
            if previous is None:
                os.environ.pop(key, None)
            else:
                os.environ[key] = previous
        record["elapsed_seconds"] = round(time.monotonic() - started, 3)
        record["shutdown_called"] = initialized


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--dll", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--timeout", type=float, default=30)
    parser.add_argument("--game-port", type=int, default=27565)
    parser.add_argument("--query-port", type=int, default=27566)
    args = parser.parse_args()
    if os.path.lexists(args.output):
        parser.error("Output already exists; choose a new evidence filename")
    try:
        result = run_probe(args.dll, args.timeout, args.game_port, args.query_port)
    except Exception as error:
        result = {"format": "scrapds.steam-identity-probe.v1", "status": "probe_error", "error": str(error)}
    args.output.parent.mkdir(parents=True, exist_ok=True)
    with args.output.open("x", encoding="utf8") as output:
        json.dump(result, output, indent=2)
        output.write("\n")
    print(json.dumps({"status": result["status"], "output": str(args.output), "steam_id": result.get("steam_id")}, indent=2))
    return 0 if result["status"] == "logged_on" else 1


if __name__ == "__main__":
    sys.exit(main())
