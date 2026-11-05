#!/usr/bin/env python3
"""Synthetic transport-only SNS012 test using the already logged-in Steam user.

Does not launch/restart a game or Steam, switch accounts, authenticate with
credentials, or send game-protocol payloads. Requires an independently running
anonymous listener_probe.py session and captures only connection metadata.
"""
from __future__ import annotations

import argparse
import ctypes as C
import json
import os
from pathlib import Path
import struct
import time

import identity_probe
import listener_probe as network


def run(dll_path: Path, remote_id: int, timeout: float = 30) -> dict:
    if os.name != "nt" or C.sizeof(C.c_void_p) != 8:
        raise ValueError("Windows x64 required")
    if not 0 < timeout <= 30:
        raise ValueError("Timeout must be >0 and <=30")
    network.validate_layouts()
    result = {"format": "scrapds.synthetic-sns-transport-selftest.v1", "remote_steam_id": str(remote_id),
              "game_protocol_test": False, "application_messages_sent": 0, "events": []}
    dll_path = dll_path.resolve(strict=True)
    identity_probe.inspect_exports(dll_path)
    previous = {name: os.environ.get(name) for name in ["SteamAppId", "SteamGameId"]}
    os.environ.update(SteamAppId="387990", SteamGameId="387990")
    initialized = False
    connection = 0
    sockets = None
    dll_directory = None
    functions = {}
    started = time.monotonic()
    try:
        dll_directory = os.add_dll_directory(str(dll_path.parent))
        library = C.CDLL(str(dll_path))
        signatures = dict(identity_probe.SIGNATURES)
        signatures.update(network.SIGNATURES)
        signatures.update({
            "SteamAPI_Init": (C.c_bool, []),
            "SteamAPI_Shutdown": (None, []),
            "SteamAPI_GetHSteamPipe": (C.c_int32, []),
            "SteamAPI_SteamNetworkingSockets_SteamAPI_v012": (C.c_void_p, []),
            "SteamAPI_ISteamNetworkingSockets_ConnectP2P": (C.c_uint32, [C.c_void_p, C.POINTER(network.NetworkIdentity), C.c_int, C.c_int, C.c_void_p]),
        })
        for name, (returns, arguments) in signatures.items():
            fn = getattr(library, name)
            fn.restype, fn.argtypes = returns, arguments
            functions[name] = fn
        initialized = bool(functions["SteamAPI_Init"]())
        result["initialized"] = initialized
        if not initialized:
            result["status"] = "user_api_initialization_failed"
            return result
        sockets = functions["SteamAPI_SteamNetworkingSockets_SteamAPI_v012"]()
        if not sockets:
            raise RuntimeError("No user SNS012 interface")
        own = network.NetworkIdentity()
        if not functions["SteamAPI_ISteamNetworkingSockets_GetIdentity"](sockets, C.byref(own)) or own.steam_id() == 0:
            raise RuntimeError("User SNS identity is not the explicitly authorized local Steam account")
        result["local_steam_id"] = str(own.steam_id())
        functions["SteamAPI_ManualDispatch_Init"]()
        pipe = functions["SteamAPI_GetHSteamPipe"]()
        peer = network.NetworkIdentity()
        peer.kind, peer.size = 16, 8
        for index, byte in enumerate(struct.pack("<Q", remote_id)):
            peer.data[index] = byte
        functions["SteamAPI_ISteamNetworkingSockets_InitAuthentication"](sockets)
        connection = functions["SteamAPI_ISteamNetworkingSockets_ConnectP2P"](sockets, C.byref(peer), 0, 0, None)
        result["connection"] = connection
        if not connection:
            result["status"] = "connection_creation_failed"
            return result
        deadline = time.monotonic()+timeout
        while time.monotonic() < deadline:
            functions["SteamAPI_ManualDispatch_RunFrame"](pipe)
            callback = identity_probe.CallbackMsg()
            for _ in range(1000):
                if not functions["SteamAPI_ManualDispatch_GetNextCallback"](pipe, C.byref(callback)):
                    break
                try:
                    if callback.callback == 1221 and callback.parameter:
                        event = network.parse_status(C.string_at(callback.parameter, callback.size))
                        event["elapsed_seconds"] = round(time.monotonic()-started, 3)
                        result["events"].append(event)
                finally:
                    functions["SteamAPI_ManualDispatch_FreeLastCallback"](pipe)
            current = network.ConnectionInfo()
            if functions["SteamAPI_ISteamNetworkingSockets_GetConnectionInfo"](sockets, connection, C.byref(current)):
                result.update(last_state=current.state, end_reason=current.end_reason, end_debug=bytes(current.end_debug).decode("utf8", "replace"))
                if current.state == 3:
                    result["status"] = "transport_connected"
                    time.sleep(0.2)
                    break
                if current.state in {4, 5}:
                    result["status"] = "transport_failed"
                    break
            time.sleep(0.01)
        else:
            result["status"] = "connection_timeout"
        return result
    finally:
        if connection and sockets:
            functions["SteamAPI_ISteamNetworkingSockets_CloseConnection"](sockets, connection, 1000, b"synthetic transport test finished", False)
        if initialized:
            functions["SteamAPI_Shutdown"]()
        if dll_directory:
            dll_directory.close()
        for key, value in previous.items():
            if value is None:
                os.environ.pop(key, None)
            else:
                os.environ[key] = value
        result["elapsed_seconds"] = round(time.monotonic()-started, 3)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--dll", type=Path, required=True)
    parser.add_argument("--server-id", type=int, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--timeout", type=float, default=30)
    args = parser.parse_args()
    if args.output.exists():
        parser.error("Output must be new")
    try:
        result = run(args.dll, args.server_id, args.timeout)
    except Exception as error:
        result = {"status": "probe_error", "error": str(error), "game_protocol_test": False}
    args.output.parent.mkdir(parents=True, exist_ok=True)
    with args.output.open("x", encoding="utf8") as stream:
        json.dump(result, stream, indent=2)
    print(json.dumps({"status": result["status"], "output": str(args.output)}))
    return 0 if result["status"] == "transport_connected" else 1


if __name__ == "__main__":
    raise SystemExit(main())
