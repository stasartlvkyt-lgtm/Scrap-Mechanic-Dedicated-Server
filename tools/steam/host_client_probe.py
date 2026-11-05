#!/usr/bin/env python3
"""Synthetic remote client: anonymous GameServer identity -> user host.

Used only to make a real host send its initial world synchronization so the
passive host probe can capture it. Replays the two client wire packets observed
from the real game (ids 6 and 9); it does not claim to be a game client.
"""
from __future__ import annotations

import argparse
import ctypes as C
import json
import struct
import time
from pathlib import Path

import identity_probe as identity
import listener_probe as network

SNS = {
    "SteamAPI_ISteamNetworkingSockets_ConnectP2P": (
        C.c_uint32, [C.c_void_p, C.POINTER(network.NetworkIdentity), C.c_int, C.c_int, C.c_void_p]),
    "SteamAPI_ISteamNetworkingSockets_SendMessageToConnection": (
        C.c_int, [C.c_void_p, C.c_uint32, C.c_void_p, C.c_uint32, C.c_int, C.POINTER(C.c_int64)]),
    "SteamAPI_ISteamNetworkingSockets_FlushMessagesOnConnection": (
        C.c_int, [C.c_void_p, C.c_uint32]),
    "SteamAPI_ISteamNetworkingSockets_CreatePollGroup": (C.c_uint32, [C.c_void_p]),
    "SteamAPI_ISteamNetworkingSockets_SetConnectionPollGroup": (
        C.c_bool, [C.c_void_p, C.c_uint32, C.c_uint32]),
    "SteamAPI_ISteamNetworkingSockets_ReceiveMessagesOnPollGroup": (
        C.c_int, [C.c_void_p, C.c_uint32, C.POINTER(C.POINTER(network.Message)), C.c_int]),
    "SteamAPI_ISteamNetworkingSockets_GetConnectionInfo": (
        C.c_bool, [C.c_void_p, C.c_uint32, C.POINTER(network.ConnectionInfo)]),
    "SteamAPI_ISteamNetworkingSockets_InitAuthentication": (C.c_int, [C.c_void_p]),
}


def run(remote_id: int, client6: bytes, client9: bytes, timeout: float = 30.0) -> dict:
    result = {
        "format": "scrapds.synthetic-remote-client.v1",
        "remote_steam_id": str(remote_id),
        "messages": [],
        "sent": [],
        "status": "initializing",
    }

    def callback(library, functions, server, pipe, record):
        for name, (returns, arguments) in {**network.SIGNATURES, **SNS}.items():
            function = getattr(library, name)
            function.restype, function.argtypes = returns, arguments
            functions[name] = function
        user = functions["SteamGameServer_GetHSteamUser"]()
        sockets = functions["SteamInternal_FindOrCreateGameServerInterface"](
            user, b"SteamNetworkingSockets012")
        if not sockets:
            result["status"] = "no_gameserver_sns"
            return
        group = functions["SteamAPI_ISteamNetworkingSockets_CreatePollGroup"](sockets)
        if not group:
            result["status"] = "no_poll_group"
            return
        functions["SteamAPI_ISteamNetworkingSockets_InitAuthentication"](sockets)
        peer = network.NetworkIdentity()
        peer.kind, peer.size = 16, 8
        for index, byte in enumerate(struct.pack("<Q", remote_id)):
            peer.data[index] = byte
        connection = functions["SteamAPI_ISteamNetworkingSockets_ConnectP2P"](
            sockets, C.byref(peer), 0, 0, None)
        result["connection"] = connection
        if not connection:
            result["status"] = "connection_creation_failed"
            return
        started = time.monotonic()
        deadline = started + timeout
        last_state = None
        sent_ready = sent6 = sent9 = False

        def send(payload: bytes, label: str):
            number = C.c_int64(-1)
            status = functions["SteamAPI_ISteamNetworkingSockets_SendMessageToConnection"](
                sockets, connection, payload, len(payload), 8, C.byref(number))
            functions["SteamAPI_ISteamNetworkingSockets_FlushMessagesOnConnection"](
                sockets, connection)
            result["sent"].append({"label": label, "bytes": len(payload),
                                   "hex": payload[:64].hex(), "eresult": status,
                                   "number": number.value,
                                   "elapsed_seconds": round(time.monotonic() - started, 3)})

        while time.monotonic() < deadline:
            functions["SteamAPI_ManualDispatch_RunFrame"](pipe)
            callback_msg = identity.CallbackMsg()
            for _ in range(1000):
                if not functions["SteamAPI_ManualDispatch_GetNextCallback"](pipe, C.byref(callback_msg)):
                    break
                try:
                    if callback_msg.callback == 1221 and callback_msg.parameter:
                        info = network.parse_status(C.string_at(callback_msg.parameter, callback_msg.size))
                        info["elapsed_seconds"] = round(time.monotonic() - started, 3)
                        result.setdefault("events", []).append(info)
                finally:
                    functions["SteamAPI_ManualDispatch_FreeLastCallback"](pipe)
            info = network.ConnectionInfo()
            if functions["SteamAPI_ISteamNetworkingSockets_GetConnectionInfo"](
                    sockets, connection, C.byref(info)):
                result["state"] = info.state
                result["end_reason"] = info.end_reason
                result["end_debug"] = bytes(info.end_debug).decode("utf8", "replace")
                if info.state in (2, 3):
                    functions["SteamAPI_ISteamNetworkingSockets_SetConnectionPollGroup"](
                        sockets, connection, group)
                if info.state in (4, 5):
                    result["status"] = "connection_closed"
                    break
                if info.state != last_state:
                    last_state = info.state
                    result.setdefault("states", []).append({
                        "state": info.state,
                        "elapsed_seconds": round(time.monotonic() - started, 3)})
            pointers = (C.POINTER(network.Message) * 16)()
            count = functions["SteamAPI_ISteamNetworkingSockets_ReceiveMessagesOnPollGroup"](
                sockets, group, pointers, len(pointers))
            if count < 0:
                result["status"] = "receive_failed"
                break
            for index in range(count):
                pointer = pointers[index]
                message = pointer.contents
                payload = C.string_at(message.data, message.size)
                result["messages"].append({
                    "bytes": len(payload), "prefix": payload[:96].hex(),
                    "number": message.number, "flags": message.flags,
                    "elapsed_seconds": round(time.monotonic() - started, 3)})
                functions["SteamAPI_SteamNetworkingMessage_t_Release"](pointer)
                if payload[:1] == b"\x03":
                    send(bytes.fromhex("04000000000000000000"), "authentication")
                elif payload[:1] == b"\x05" and not sent_ready:
                    send(b"\x01", "ready")
                    sent_ready = True
                elif payload[:1] == b"\x02" and not sent6:
                    send(client6, "client-id6")
                    sent6 = True
                elif payload[:1] == b"\x07" and not sent9:
                    send(client9, "client-id9")
                    sent9 = True
            if sent9 and result.get("messages") and len(result["messages"]) > 8:
                break
            time.sleep(0.01)
        if "status" not in result or result["status"] in ("initializing",):
            result["status"] = "completed"
        result["elapsed_seconds"] = round(time.monotonic() - started, 3)
        record["remote_client"] = result

    identity.run_probe(
        Path(r"D:/Steam/steamapps/common/Scrap Mechanic/Release/steam_api64.dll"),
        20, 27665, 27666, callback)
    return result


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--host-steam-id", type=int, required=True)
    parser.add_argument("--client6", type=Path, required=True)
    parser.add_argument("--client9", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    result = run(args.host_steam_id, args.client6.read_bytes(), args.client9.read_bytes())
    args.output.write_text(json.dumps(result, indent=2), encoding="utf8")
    print(json.dumps({"status": result["status"], "output": str(args.output)}))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())