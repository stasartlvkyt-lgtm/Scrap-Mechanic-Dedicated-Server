# Anonymous SNS listener experiment — 2026-11-02

`tools/steam/listener_probe.py` holds an anonymous Steam GameServer login,
obtains its **GameServer** `SteamNetworkingSockets012` interface through
`SteamInternal_FindOrCreateGameServerInterface`, and listens on P2P virtual
port 0. The user API is not initialized. The script never starts a game and
never sends fabricated game-protocol messages.

Only incoming connections whose current `GetConnectionInfo` reports SteamID
`<STEAMID64>` (default explicit allowlist) and the owned listening socket
are accepted. All other identities are rejected. Received messages are saved
as exact plaintext `.bin` payloads with SHA-256, reliable flag, lane, message
number, connection handle, and timestamps. Every received native message is
released with the exported `SteamAPI_SteamNetworkingMessage_t_Release`.

## ABI sources and checks

The identity-login helper uses GameServer014 declarations from SDK1.52.
SDK1.52 networking headers define **SNS009**, so those are deliberately not
used to infer the installed SNS012 ABI. Networking uses Valve-authored
SDK1.54 headers at pinned commit
[`28ab7e1d4d73ffd2afef76a5c90f9551ae033bd7`](https://github.com/rlabrecque/SteamworksSDK/tree/28ab7e1d4d73ffd2afef76a5c90f9551ae033bd7/public/steam),
which explicitly declares `SteamNetworkingSockets012`. Downloads and hashes:
`artifacts/steam/reference-v154/provenance.json`.

Checked Windows x64 layouts:

| Type | Pack | Size | Important offsets |
|---|---:|---:|---|
| SteamNetworkingIdentity | 1 | 136 | type0, bytes4, data8 |
| SteamNetworkingIPAddr | 1 | 18 | IPv6 bytes0, port16 |
| SteamNetConnectionInfo_t | 8 | 696 | remote0, listen144, state176, reason180, flags440 |
| SteamNetConnectionStatusChangedCallback_t | 8 | 712 | connection0, info8, oldState704 |
| SteamNetworkingMessage_t | 8 | 216 | data0, size8, conn12, peer16, release184, flags196, lane208 |
| SteamNetAuthenticationStatus_t | 8 | 260 | availability0, debug4 |

Status callback ID is `1220+1=1221`, authentication callback is 1222.
Manual dispatch uses the GameServer pipe. Native callbacks are freed after
processing; unknown callback payloads are not read or retained. Python tests
exercise synthetic callback bytes, SteamID checks, truncation rejection,
and member offsets before live execution.

## Running a bounded experiment

```powershell
python tools/steam/listener_probe.py --game 'D:\Steam\steamapps\common\Scrap Mechanic' --output artifacts/steam/new-capture --ready-file artifacts/steam/new-ready.json --seconds 180
```

Output directory and ready-file must be new. Supported duration is 1–300
seconds. The ready-file contains the fresh independent SteamID as a string;
it can be used for a separate, explicitly controlled vanilla-client attempt.
Create `stop.request` inside the capture directory to end the listener early.

The script writes progress to `listener.json` once per second, final login
evidence to `session.json`, and received messages to numbered files.
Advertising remains disabled. Shutdown closes connections, listener and poll
group, logs off, and shuts down the GameServer API.

First session `artifacts/steam/listener-01` obtained SteamID
`90296325960391701`. Native SNS authentication reached availability100,
`OK`, 0.453 seconds after listener startup. It ended cleanly after 180
seconds with **no inbound connection callbacks and no application messages**.
The game's attempted launch used OS/applaunch arguments. Follow-up tracing
by the protocol investigation established that startup connection parsing
reads Steam's `GetLaunchCommandLine` data rather than the OS command line;
therefore the first attempt does not test a correctly triggered vanilla join
and must not be interpreted as a transport rejection.

A second, briefly started `listener-selftest` was stopped without running
any synthetic client. The next controlled attempt uses `listener-uri` and
a Steam launch URI supplied by the root investigator. Results must be read
from that session's final evidence; readiness and an issued certificate alone
do not establish game compatibility.

`tools/steam/transport_selftest.py` is a separately labeled synthetic
transport-only client for fallback diagnosis. It uses the existing local
Steam user identity, makes a ConnectP2P attempt, sends zero application
messages, and closes its own API context. It does not launch or restart a
game, change Steam accounts, or supply game handshake responses. It must not
be counted as a vanilla-client compatibility test.
