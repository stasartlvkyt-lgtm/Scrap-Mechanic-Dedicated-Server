#!/usr/bin/env python3
"""Synthetic client for the scrapds control handshake and GameInfo reply.

Uses the already logged-in Steam user API and the installed game DLL. It does
not launch, restart or modify the game. The only game-protocol bytes it sends
are the observed 04 authentication (empty password, zero secret) and the
post-acceptance 01 message. It verifies the server's 03 challenge, 05
acceptance, and compressed GameInfo (message 2, build 889) reply.
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
    result = {
        "format": "scrapds.synthetic-handshake-selftest.v1",
        "remote_steam_id": str(remote_id),
        "application_messages_sent": 0,
        "messages": [],
        "status": "initializing",
    }
    dll_path = dll_path.resolve(strict=True)
    identity_probe.inspect_exports(dll_path)
    previous = {name: os.environ.get(name) for name in ["SteamAppId", "SteamGameId"]}
    os.environ.update(SteamAppId="387990", SteamGameId="387990")
    initialized = False
    connection = 0
    poll_group = 0
    sockets = None
    dll_directory = None
    functions = {}
    started = time.monotonic()

    def send(payload: bytes, label: str) -> bool:
        number = C.c_int64(-1)
        status = functions["SteamAPI_ISteamNetworkingSockets_SendMessageToConnection"](
            sockets, connection, payload, len(payload), 8, C.byref(number))
        result["application_messages_sent"] += 1
        result["messages"].append({"direction": "tx", "label": label, "bytes": len(payload),
                                   "hex": payload.hex(), "eresult": status, "number": number.value,
                                   "elapsed_seconds": round(time.monotonic() - started, 3)})
        functions["SteamAPI_ISteamNetworkingSockets_FlushMessagesOnConnection"](sockets, connection)
        return status == 1

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
            "SteamAPI_ISteamNetworkingSockets_ConnectP2P": (
                C.c_uint32, [C.c_void_p, C.POINTER(network.NetworkIdentity), C.c_int, C.c_int, C.c_void_p]),
            "SteamAPI_ISteamNetworkingSockets_SendMessageToConnection": (
                C.c_int, [C.c_void_p, C.c_uint32, C.c_void_p, C.c_uint32, C.c_int, C.POINTER(C.c_int64)]),
            "SteamAPI_ISteamNetworkingSockets_FlushMessagesOnConnection": (
                C.c_int, [C.c_void_p, C.c_uint32]),
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
        connection = functions["SteamAPI_ISteamNetworkingSockets_ConnectP2P"](
            sockets, C.byref(peer), 0, 0, None)
        result["connection"] = connection
        if not connection:
            result["status"] = "connection_creation_failed"
            return result
        poll_group = functions["SteamAPI_ISteamNetworkingSockets_CreatePollGroup"](sockets)
        if not poll_group:
            raise RuntimeError("Could not create poll group")

        challenge_seen = False
        accepted_seen = False
        ready_sent = False
        game_info_seen = False
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            functions["SteamAPI_ManualDispatch_RunFrame"](pipe)
            callback = identity_probe.CallbackMsg()
            for _ in range(1000):
                if not functions["SteamAPI_ManualDispatch_GetNextCallback"](pipe, C.byref(callback)):
                    break
                try:
                    if callback.callback == 1221 and callback.parameter:
                        event = network.parse_status(C.string_at(callback.parameter, callback.size))
                        event["elapsed_seconds"] = round(time.monotonic() - started, 3)
                        result.setdefault("events", []).append(event)
                finally:
                    functions["SteamAPI_ManualDispatch_FreeLastCallback"](pipe)

            current = network.ConnectionInfo()
            if functions["SteamAPI_ISteamNetworkingSockets_GetConnectionInfo"](sockets, connection, C.byref(current)):
                result["last_state"] = current.state
                result["end_reason"] = current.end_reason
                result["end_debug"] = bytes(current.end_debug).decode("utf8", "replace")
                if current.state in {2, 3} and poll_group:
                    functions["SteamAPI_ISteamNetworkingSockets_SetConnectionPollGroup"](
                        sockets, connection, poll_group)
                if current.state == 3:
                    result["status"] = "transport_connected"
                if current.state in {4, 5}:
                    result["status"] = "transport_failed"
                    break

            pointers = (C.POINTER(network.Message) * 16)()
            count = functions["SteamAPI_ISteamNetworkingSockets_ReceiveMessagesOnPollGroup"](
                sockets, poll_group, pointers, len(pointers))
            if count < 0:
                raise RuntimeError("ReceiveMessagesOnPollGroup failed")
            for index in range(count):
                pointer = pointers[index]
                message = pointer.contents
                payload = C.string_at(message.data, message.size)
                result["messages"].append({
                    "direction": "rx", "bytes": len(payload), "hex": payload.hex(),
                    "number": message.number, "flags": message.flags, "lane": message.lane,
                    "elapsed_seconds": round(time.monotonic() - started, 3)})
                functions["SteamAPI_SteamNetworkingMessage_t_Release"](pointer)
                if len(payload) == 1 and payload[0] == 3:
                    challenge_seen = True
                    send(bytes.fromhex("04000000000000000000"), "authentication")
                elif len(payload) == 1 and payload[0] == 5:
                    accepted_seen = True
                    if not ready_sent:
                        send(b"\x01", "ready")
                        ready_sent = True
                elif payload and payload[0] == 2:
                    game_info_seen = True
                    result["game_info_wire"] = payload.hex()
                    try:
                        import lz4.block
                        body = lz4.block.decompress(payload[1:], uncompressed_size=256)
                        result["game_info_plain_hex"] = (payload[:1] + body).hex()
                        result["game_info_version"] = int.from_bytes(body[:4], "big")
                    except Exception as error:
                        result["game_info_decode_error"] = str(error)
                    result["status"] = "game_info_received"
                    break
            if game_info_seen:
                break
            time.sleep(0.01)

        result["challenge_seen"] = challenge_seen
        result["acceptance_seen"] = accepted_seen
        result["ready_sent"] = ready_sent
        result["game_info_seen"] = game_info_seen
        if game_info_seen:
            result["status"] = "game_info_received"
        elif challenge_seen and accepted_seen and ready_sent:
            result["status"] = "ready_sent_no_game_info"
        elif not challenge_seen:
            result["status"] = "no_challenge"
        return result
    finally:
        if poll_group and sockets:
            functions["SteamAPI_ISteamNetworkingSockets_DestroyPollGroup"](sockets, poll_group)
        if connection and sockets:
            functions["SteamAPI_ISteamNetworkingSockets_CloseConnection"](
                sockets, connection, 1000, b"synthetic handshake test finished", False)
        if initialized:
            functions["SteamAPI_Shutdown"]()
        if dll_directory:
            dll_directory.close()
        for key, value in previous.items():
            if value is None:
                os.environ.pop(key, None)
            else:
                os.environ[key] = value
        result["elapsed_seconds"] = round(time.monotonic() - started, 3)


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
        result = {"status": "probe_error", "error": str(error)}
    args.output.parent.mkdir(parents=True, exist_ok=True)
    with args.output.open("x", encoding="utf8") as stream:
        json.dump(result, stream, indent=2)
    print(json.dumps({"status": result["status"], "output": str(args.output)}))
    return 0 if result["status"] == "game_info_received" else 1


if __name__ == "__main__":
    raise SystemExit(main())