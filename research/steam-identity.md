# Anonymous GameServer identity — runtime verified, 2026-11-02

**Result:** an anonymous Steam GameServer for AppID **387990** logged in
successfully on the same Windows machine as the running Scrap Mechanic client.
`BLoggedOn` returned true; `GetSteamID` returned `90296325582601236`
(universe 1, account type 4 = anonymous game server). A manually dispatched
`SteamServersConnected_t` callback (101) independently confirmed the connection.
Elapsed time including cleanup: 2.422 seconds.

Complete runtime evidence: `artifacts/steam/anonymous-identity.json`.
DLL SHA-256: `4df999c0c8cb12589f0864d52be5d4c775577aeb27fee28b49b188f9ba083eea`.
The DLL was loaded from the licensed installation; it was not copied into
this project or the server package. Export inventory is retained in
`artifacts/steam/installed-dll.json`.

## ABI provenance

The installed DLL exports `SteamAPI_SteamGameServer_v014`. Current Valve
source-sdk-2013 headers expose v015, so the probe uses Valve-authored SDK1.52
headers from the public mirror at immutable commit
[`99411b13b18d6e7f275da62d5557fac34b52d330`](https://github.com/rlabrecque/SteamworksSDK/tree/99411b13b18d6e7f275da62d5557fac34b52d330/public/steam).
Exact source URLs and downloaded header hashes are retained in
`artifacts/steam/reference-v152/provenance.json`.

Relevant declarations checked before execution:

- `steam_gameserver.h:104`: six-argument `SteamInternal_GameServer_Init`:
  uint32 IP, three uint16 ports, EServerMode enum, const char* version.
- `steam_api_flat.h:965–989`: v014 accessor and flat C functions. Steam IDs
  are returned as uint64. Each flat function receives its interface pointer
  as the first argument; no C++ vtable slots are guessed.
- `steamtypes.h:13`: `S_CALLTYPE` is `__cdecl`.
- `steam_api_internal.h:197`: CallbackMsg fields are int32 user, int32 callback,
  uint8* payload, int payload-size. Windows x64 uses pack 8: size 24, pointer
  offset 8, payload-size offset 16.
- `isteamuser.h:235–260`: callback IDs 101/102/103 and connection-result fields.

The public [GameServer initialization documentation](https://partner.steamgames.com/doc/api/steam_gameserver)
and [ISteamGameServer reference](https://partner.steamgames.com/doc/api/ISteamGameServer)
describe anonymous login, required metadata, callbacks, and advertising.

## Reproduction

```powershell
python tools/steam/identity_probe.py --dll 'D:\Steam\steamapps\common\Scrap Mechanic\Release\steam_api64.dll' --output artifacts/steam/another-run.json --timeout 30
```

Runs in its own Python process, sets SteamAppId/SteamGameId in that process
only, binds Steam's query service to 127.0.0.1, and initializes mode 2
(authentication without VAC). Sets product 387990, description Scrap Mechanic,
mod directory Scrap Mechanic, dedicated=true, advertising=false before
anonymous login. Polls the GameServer pipe using manual dispatch, then logs
off and shuts down. No user credentials, Steam configuration edits, client
restart, gameplay listener, or SteamNetworkingSockets listener are involved.

Unknown callbacks are recorded only by number/size; arbitrary callback data
and authentication tickets are not copied into evidence.

## Limits and next experiment

This proves an independent anonymous Steam identity can be obtained for this
AppID from this environment. It does **not** prove vanilla Scrap Mechanic
clients can connect to that identity, that SDR certificates/routing permit
the intended topology, or that any game handshake is implemented. The ID
in the evidence is already logged off and must not be treated as an active
server. The next experiment is a deliberately separate SNS listener in a
held anonymous session, followed by a vanilla client connection attempt.
