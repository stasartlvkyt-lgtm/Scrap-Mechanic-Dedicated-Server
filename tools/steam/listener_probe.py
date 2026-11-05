#!/usr/bin/env python3
"""Hold an anonymous GameServer SNS012 listener and capture allowed peer messages.

Never launches a game or fabricates game-protocol replies. Only one explicitly
allowed Steam identity can be accepted. DLL is loaded from the installed game.
"""
from __future__ import annotations

import argparse
import ctypes as C
import hashlib
import json
from pathlib import Path
import struct
import sys
import time

import identity_probe as identity

SDK_REVISION = "28ab7e1d4d73ffd2afef76a5c90f9551ae033bd7"


class NetworkIdentity(C.Structure):
    _pack_ = 1
    _fields_ = [("kind", C.c_int32), ("size", C.c_int32), ("data", C.c_uint8 * 128)]

    def steam_id(self) -> int:
        return struct.unpack_from("<Q", bytes(self.data))[0] if self.kind == 16 and self.size == 8 else 0


class IPAddress(C.Structure):
    _pack_ = 1
    _fields_ = [("ipv6", C.c_uint8 * 16), ("port", C.c_uint16)]


class ConnectionInfo(C.Structure):
    _pack_ = 8
    _fields_ = [("remote", NetworkIdentity), ("user_data", C.c_int64), ("listen_socket", C.c_uint32),
                ("remote_address", IPAddress), ("pad1", C.c_uint16), ("remote_pop", C.c_uint32),
                ("relay_pop", C.c_uint32), ("state", C.c_int32), ("end_reason", C.c_int32),
                ("end_debug", C.c_char * 128), ("description", C.c_char * 128),
                ("flags", C.c_int32), ("reserved", C.c_uint32 * 63)]


class ConnectionStatus(C.Structure):
    _pack_ = 8
    _fields_ = [("connection", C.c_uint32), ("info", ConnectionInfo), ("old_state", C.c_int32)]


class Message(C.Structure):
    _pack_ = 8
    _fields_ = [("data", C.c_void_p), ("size", C.c_int32), ("connection", C.c_uint32),
                ("peer", NetworkIdentity), ("connection_user_data", C.c_int64), ("received_usec", C.c_int64),
                ("number", C.c_int64), ("free_data", C.c_void_p), ("release", C.c_void_p),
                ("channel", C.c_int32), ("flags", C.c_int32), ("user_data", C.c_int64),
                ("lane", C.c_uint16), ("pad1", C.c_uint16)]


class AuthenticationStatus(C.Structure):
    _pack_ = 8
    _fields_ = [("availability", C.c_int32), ("debug", C.c_char * 256)]


SIGNATURES = {
    "SteamGameServer_GetHSteamUser": (C.c_int32, []),
    "SteamInternal_FindOrCreateGameServerInterface": (C.c_void_p, [C.c_int32, C.c_char_p]),
    "SteamAPI_ISteamNetworkingSockets_CreateListenSocketP2P": (C.c_uint32, [C.c_void_p, C.c_int, C.c_int, C.c_void_p]),
    "SteamAPI_ISteamNetworkingSockets_CloseListenSocket": (C.c_bool, [C.c_void_p, C.c_uint32]),
    "SteamAPI_ISteamNetworkingSockets_CreatePollGroup": (C.c_uint32, [C.c_void_p]),
    "SteamAPI_ISteamNetworkingSockets_DestroyPollGroup": (C.c_bool, [C.c_void_p, C.c_uint32]),
    "SteamAPI_ISteamNetworkingSockets_SetConnectionPollGroup": (C.c_bool, [C.c_void_p, C.c_uint32, C.c_uint32]),
    "SteamAPI_ISteamNetworkingSockets_AcceptConnection": (C.c_int, [C.c_void_p, C.c_uint32]),
    "SteamAPI_ISteamNetworkingSockets_CloseConnection": (C.c_bool, [C.c_void_p, C.c_uint32, C.c_int, C.c_char_p, C.c_bool]),
    "SteamAPI_ISteamNetworkingSockets_GetConnectionInfo": (C.c_bool, [C.c_void_p, C.c_uint32, C.POINTER(ConnectionInfo)]),
    "SteamAPI_ISteamNetworkingSockets_GetIdentity": (C.c_bool, [C.c_void_p, C.POINTER(NetworkIdentity)]),
    "SteamAPI_ISteamNetworkingSockets_InitAuthentication": (C.c_int, [C.c_void_p]),
    "SteamAPI_ISteamNetworkingSockets_GetAuthenticationStatus": (C.c_int, [C.c_void_p, C.POINTER(AuthenticationStatus)]),
    "SteamAPI_ISteamNetworkingSockets_ReceiveMessagesOnPollGroup": (C.c_int, [C.c_void_p, C.c_uint32, C.POINTER(C.POINTER(Message)), C.c_int]),
    "SteamAPI_SteamNetworkingMessage_t_Release": (None, [C.POINTER(Message)]),
}


def parse_status(payload: bytes) -> dict:
    if len(payload) != C.sizeof(ConnectionStatus):
        raise ValueError(f"Unexpected status callback size {len(payload)}")
    status = ConnectionStatus.from_buffer_copy(payload)
    return {"connection": status.connection, "state": status.info.state, "old_state": status.old_state,
            "steam_id": str(status.info.remote.steam_id()), "listen_socket": status.info.listen_socket,
            "end_reason": status.info.end_reason, "end_debug": bytes(status.info.end_debug).decode("utf8", "replace"),
            "flags": status.info.flags}


def validate_layouts() -> None:
    expected = [(NetworkIdentity, 136), (IPAddress, 18), (ConnectionInfo, 696), (ConnectionStatus, 712), (Message, 216), (AuthenticationStatus, 260)]
    for kind, size in expected:
        if C.sizeof(kind) != size:
            raise ValueError(f"{kind.__name__} ABI layout mismatch")
    if ConnectionStatus.info.offset != 8 or ConnectionInfo.state.offset != 176 or Message.peer.offset != 16 or Message.flags.offset != 196:
        raise ValueError("SNS012 member offset mismatch")


def listen(library, login_functions, server, pipe, login_record, output: Path, ready: Path, seconds: int, allowed_peer: int):
    validate_layouts()
    functions = {}
    for name, (returns, arguments) in SIGNATURES.items():
        function = getattr(library, name)
        function.restype, function.argtypes = returns, arguments
        functions[name] = function
    user = functions["SteamGameServer_GetHSteamUser"]()
    sockets = functions["SteamInternal_FindOrCreateGameServerInterface"](user, b"SteamNetworkingSockets012")
    if not sockets:
        raise RuntimeError("No SteamNetworkingSockets012 gameserver interface")
    get = lambda method: functions["SteamAPI_ISteamNetworkingSockets_" + method]
    own_identity = NetworkIdentity()
    if not get("GetIdentity")(sockets, C.byref(own_identity)) or own_identity.steam_id() != int(login_record["steam_id"]):
        raise RuntimeError("Networking identity differs from anonymous GameServer identity")
    auth_initial = get("InitAuthentication")(sockets)
    poll_group = get("CreatePollGroup")(sockets)
    if not poll_group:
        raise RuntimeError("Could not create poll group")
    listener = 0
    connections = set()
    accepted = set()
    total_bytes = 0
    message_count = 0
    started = time.monotonic()
    record = {"format": "scrapds.sns-listener-probe.v1", "steam_id": login_record["steam_id"], "allowed_peer": str(allowed_peer),
              "interface": "SteamNetworkingSockets012", "virtual_port": 0, "advertising": False, "sends_game_messages": False,
              "seconds": seconds, "authentication_initial": auth_initial, "events": [], "messages": [],
              "sdk_revision": SDK_REVISION}
    login_record["listener"] = record

    def save_progress():
        record.update(elapsed_seconds=round(time.monotonic()-started, 3), message_count=message_count, total_message_bytes=total_bytes)
        temporary = output / ".listener-progress.tmp"
        temporary.write_text(json.dumps(record, indent=2), encoding="utf8")
        temporary.replace(output / "listener.json")

    def close(connection, reason=1000, message=b"scrapds probe complete"):
        get("CloseConnection")(sockets, connection, reason, message, False)
        connections.discard(connection)
        accepted.discard(connection)

    try:
        listener = get("CreateListenSocketP2P")(sockets, 0, 0, None)
        if not listener:
            raise RuntimeError("Could not create P2P virtual-port-0 listener")
        record["listen_socket"] = listener
        record["status"] = "listening"
        save_progress()
        ready.parent.mkdir(parents=True, exist_ok=True)
        with ready.open("x", encoding="utf8") as stream:
            json.dump({"steam_id": login_record["steam_id"], "virtual_port": 0, "output": str(output), "seconds": seconds, "allowed_peer": str(allowed_peer)}, stream, indent=2)
        print(json.dumps({"ready": str(ready), "steam_id": login_record["steam_id"]}), flush=True)
        deadline = time.monotonic()+seconds
        next_save = 0
        while time.monotonic() < deadline and not (output / "stop.request").exists():
            login_functions["SteamAPI_ManualDispatch_RunFrame"](pipe)
            callback = identity.CallbackMsg()
            for _ in range(1000):
                if not login_functions["SteamAPI_ManualDispatch_GetNextCallback"](pipe, C.byref(callback)):
                    break
                try:
                    if callback.callback == 1221 and callback.parameter:
                        if callback.size != C.sizeof(ConnectionStatus):
                            raise RuntimeError(f"Status ABI mismatch: callback {callback.size} bytes")
                        event = parse_status(C.string_at(callback.parameter, callback.size))
                        event["elapsed_seconds"] = round(time.monotonic()-started, 3)
                        connection = event["connection"]
                        connections.add(connection)
                        if event["state"] == 1:
                            # Get current info independently, rather than trusting
                            # a potentially obsolete queued callback.
                            current = ConnectionInfo()
                            valid = get("GetConnectionInfo")(sockets, connection, C.byref(current))
                            if not valid or current.remote.steam_id() != allowed_peer or current.listen_socket != listener:
                                event["action"] = "rejected-unexpected-peer"
                                close(connection, 1001, b"probe peer not allowed")
                            else:
                                grouped = get("SetConnectionPollGroup")(sockets, connection, poll_group)
                                result = get("AcceptConnection")(sockets, connection) if grouped else 0
                                event.update(action="accept", accept_eresult=result, poll_group_set=bool(grouped))
                                if result == 1:
                                    accepted.add(connection)
                                else:
                                    close(connection)
                        elif event["state"] in {4, 5}:
                            close(connection)
                        record["events"].append(event)
                    elif callback.callback == 1222 and callback.parameter and callback.size == C.sizeof(AuthenticationStatus):
                        auth = AuthenticationStatus.from_buffer_copy(C.string_at(callback.parameter, callback.size))
                        record["events"].append({"authentication_availability": auth.availability, "debug": bytes(auth.debug).decode("utf8", "replace"), "elapsed_seconds": round(time.monotonic()-started, 3)})
                finally:
                    login_functions["SteamAPI_ManualDispatch_FreeLastCallback"](pipe)
            messages = (C.POINTER(Message) * 32)()
            received = get("ReceiveMessagesOnPollGroup")(sockets, poll_group, messages, len(messages))
            if received < 0 or received > len(messages):
                raise RuntimeError("Invalid poll-group receive result")
            for index in range(received):
                pointer = messages[index]
                try:
                    message = pointer.contents
                    if message.connection not in accepted or message.peer.steam_id() != allowed_peer:
                        raise RuntimeError("Received unapproved peer message")
                    if not 0 <= message.size <= 16*1024*1024 or total_bytes+message.size > 128*1024*1024:
                        raise RuntimeError("Capture size budget exceeded")
                    payload = C.string_at(message.data, message.size) if message.size else b""
                    filename = f"{message_count:06d}-rx.bin"
                    (output / filename).write_bytes(payload)
                    record["messages"].append({"file": filename, "bytes": len(payload), "sha256": hashlib.sha256(payload).hexdigest(),
                                               "connection": message.connection, "number": message.number, "flags": message.flags,
                                               "reliable": bool(message.flags & 8), "lane": message.lane, "received_usec": message.received_usec,
                                               "elapsed_seconds": round(time.monotonic()-started, 3)})
                    message_count += 1
                    total_bytes += len(payload)
                finally:
                    functions["SteamAPI_SteamNetworkingMessage_t_Release"](pointer)
            if time.monotonic() >= next_save:
                auth = AuthenticationStatus()
                get("GetAuthenticationStatus")(sockets, C.byref(auth))
                record["authentication_current"] = {"availability": auth.availability, "debug": bytes(auth.debug).decode("utf8", "replace")}
                save_progress()
                next_save = time.monotonic()+1
            time.sleep(0.01)
        record["status"] = "capture_finished"
    finally:
        for connection in list(connections):
            close(connection)
        if listener:
            get("CloseListenSocket")(sockets, listener)
        get("DestroyPollGroup")(sockets, poll_group)
        record["closed"] = True
        save_progress()


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--game", type=Path, required=True, help="Game directory, Release directory, or ScrapMechanic.exe")
    parser.add_argument("--output", type=Path, required=True, help="New capture directory")
    parser.add_argument("--ready-file", type=Path, required=True)
    parser.add_argument("--seconds", type=int, default=180)
    parser.add_argument("--allow-steam-id", type=int, required=True)
    args = parser.parse_args()
    if not 1 <= args.seconds <= 300:
        parser.error("--seconds must be between 1 and 300")
    game = args.game.resolve(strict=True)
    if game.is_file():
        dll = game.parent / "steam_api64.dll"
    else:
        dll = game / "steam_api64.dll" if (game / "steam_api64.dll").exists() else game / "Release/steam_api64.dll"
    output = args.output.absolute()
    output.mkdir(parents=True, exist_ok=False)
    if args.ready_file.exists():
        parser.error("Ready file already exists")
    try:
        result = identity.run_probe(dll, 30, 27565, 27566, lambda *session: listen(*session, output, args.ready_file.absolute(), args.seconds, args.allow_steam_id))
    except Exception as error:
        result = {"status": "probe_error", "error": str(error)}
    (output / "session.json").write_text(json.dumps(result, indent=2), encoding="utf8")
    print(json.dumps({"status": result["status"], "output": str(output)}), flush=True)
    return 0 if result["status"] == "logged_on" else 1


if __name__ == "__main__":
    sys.exit(main())
