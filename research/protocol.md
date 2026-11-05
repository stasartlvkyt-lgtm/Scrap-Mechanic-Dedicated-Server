# Protocol evidence and hook map

Evidence scope: offline ordinary shell analysis using `pefile 2024.8.26` and
`capstone 5.0.7`, on the installed executable. REA MCP/provider tools were absent
in this session; the parent observed configuration drift in read-only doctor
output. No configuration repair, injection, game execution or network recording
was performed for these static observations. These files are not REA Evidence
records.

## Reproduce

```powershell
python tools/probe/inspect_binary.py "D:/Steam/steamapps/common/Scrap Mechanic/Release/ScrapMechanic.exe" --output research/binary-report.json --disassembly-output research/network-disassembly.txt
```

The source SHA-256 matches server.md exactly:
`cf403cf9d3e465e05298c8e6082f4d664c32140ebcc4696a64200e367abb530d`.
Image base is `0x140000000`. PE runtime-function entries and chained unwind
metadata provide bounded code ranges. String xrefs are selected RIP-relative
LEA/MOV candidates, validated as individual instructions; this scan is not a
complete proof of instruction boundaries. Direct caller candidates are scanned
from E8 encodings and must also be assessed in context.

## Observations

The exe imports both `SteamInternal_FindOrCreateUserInterface` and
`SteamInternal_FindOrCreateGameServerInterface`. `SteamNetworkingSockets012`
occurs at RVA `0x14D9E70` and is referenced from helper `0x468A10`. This establishes
interface presence, not successful GameServer login or vanilla client joining.

The shipped `lua51.dll` contains `Lua 5.1` and `LuaJIT 2.1.1772619647` literals.
DLL SHA-256 is
`165888d3535618fb3a88cb0c7f008c5c155b7d6921f95887e986da25ab23ad59`.
The exe imports `luaopen_ffi`, `luaopen_bit` and `luaL_loadbufferx`. Therefore the
document's Lua 5.1 description is an ABI description; the observed runtime is
LuaJIT. An upstream Lua 5.1 bring-up VM cannot establish compatibility with FFI,
bit libraries, JIT behavior or shipped bytecode.

Hidden flags are independently located by the script. `-dedicated_server` at
`0x14DB4B0` has a reference in parser `0x474BC0`; `-use_null_driver` at
`0x14DB6C0` references the same parser. `-connect_steam_id` has occurrences at
`0x14C1B78` and `0x14CB8E0`. Presence is not a live success test.

## Compression and transport candidates

| RVA | Evidence from disassembly | Interpretation |
| --- | --- | --- |
| `0xB59510` | Copies source byte 0 to destination byte 0; calls `0x4EE990` with pointers advanced by one | Packet compression wrapper |
| `0x4658D0` | Copies byte 0; calls `0x4EEA30` on remainder; increments successful result | Packet decompression wrapper |
| `0x4EE990` | Four-argument compressor, 0x4068 stack allocation, calls `0x4ED7A0` | Raw compressor candidate |
| `0x4EEA30` | Four-argument raw decompressor | Raw decompressor candidate |
| `0xB597F0` | Calls wrapper at `0xB59A84`, SNS virtual slot `+0x58` at `0xB59AF6`, flags `9` | Reliable send candidate |
| `0xB5A150` | Calls wrapper at `0xB5A23F`, SNS slot `+0x58` at `0xB5A341`, flags `1` | Unreliable send candidate |
| `0xB5A480` | Writes split message byte `0x14`; invokes same transport slot | Split reliable send candidate |
| `0xB4D240` | Inlines byte-0 preservation; directly calls `0x4EEA30` at `0xB4DDE0` | Server receive candidate |
| `0x466A00` | Handles byte `0x14`, rebuilds packet then calls `0x450FC0` | Client receive candidate |

Hooking only `0x4658D0` misses the inline server receive path. The implemented
probe also hooks the raw decompressor, restricted to return address
`image_base + 0xB4DDE5`; this body capture excludes the original message ID.

The size bound includes `0x7E000000` and `n + n/255 + 16`, and the raw compressor
has a roughly 16 KiB workspace. These are strong indicators of **LZ4 block
compression**, but this is an inference until captured blocks pass a reference
decoder and round-trip/conformance checks. No packet codec is advertised as
compatible based solely on these constants.

The split sender writes the following static layout: first byte `0x14`, second
byte chunk index; first chunk has total compressed size at byte 2, total
uncompressed size at byte 6, data at byte 10. Later chunks have data at byte 2.
First chunk data budget is `0x7FFF6`, later chunk budget `0x7FFFE`; message size
limit is `0x80000` bytes. These are x86 little-endian stores, observed statically.
Sequence policy, bounds, failure handling and real-session acceptance still
require captured traffic. This layout is recorded for future conformance work,
not used to fabricate initial packets.

## Remaining work

1. Build and verify the probe, then capture a real host/client join using this
   exact executable. Record application logs alongside captured callsites.
2. Correlate message IDs and channel/flags with packet bodies. Confirm which
   initial/control messages bypass compression wrappers.
3. Test raw bodies against LZ4, then reconstruct split streams and compare
   decompressed plaintext with the wrapper capture.
4. Recover handshake fields, checksums, NetObj headers and initialization order
   from matched sessions before implementing a vanilla-compatible network peer.
5. Recover Bullet version/configuration and fixed-update parameters separately.
   RTTI class-name presence does not establish solver behavior or upstream parity.

`binary-report.json` retains source digest, version literals, imports, selected
string locations, xrefs, direct callers, code ranges and first 16 prologue bytes.
`network-disassembly.txt` is regenerated from those bounded known-build ranges.

## Physics entry point

The reproducible static report now resolves the MSVC x64 RTTI chain:

`TickDynamicsWorld` TypeDescriptor RVA `0x1A1D268` → CompleteObjectLocator
`0x17AD2D0` (signature 1, object offset 0, self-RVA verified) → primary vtable
`0x1605900`. Slot 13 (`+0x68`) points to `0x1118410`.

This function's code independently corroborates a `stepSimulation` role. It
receives `this` in RCX, `timeStep` in XMM1, `maxSubSteps` in R8D and
`fixedTimeStep` in XMM3. With nonzero maximum, it adds timeStep to the float
accumulator at `this+0x190`, stores fixedTimeStep at `this+0x194`, divides the
accumulator by fixedTimeStep and converts to an integer substep count, then
subtracts consumed time. It clamps the executed loop count to maxSubSteps,
invokes a per-substep virtual function at `+0x140`, synchronizes through `+0xA0`,
and calls the final `+0xF0` method. It returns the original computed count.
The zero-max branch switches to one variable step for a non-negligible dt.

The local upstream Bullet source provides a structural comparison only:
`build/_deps/bullet-src/src/BulletDynamics/Dynamics/btDiscreteDynamicsWorld.cpp`
and the collision/dynamics world headers. Their installed project version
does not establish the game's Bullet version or configuration. The accumulator,
clamped loop and register-level argument behavior above are directly observed
in this executable; names such as synchronizeMotionStates and clearForces remain
semantic mappings. Solver settings, broadphase, sleeping, CCD and gravity have
not yet been sampled dynamically.

## Runtime control handshake evidence (2026-11-02)

This is live evidence against a real vanilla client, not a synthetic transport
test. The `listen-probe` command now accepts `--handshake` and implements the
pre-game control sequence:

| Direction | Bytes | Meaning |
| --- | --- | --- |
| server → client | `03` | challenge, reliable |
| client → server | `04 00 00 00 00 00 00 00 00 00` | authentication: id 4, password length 0, secret 0, reliable |
| server → client | `05` | acceptance, reliable |
| client → server | `01` | first post-acceptance game message, reliable |

The client authentication packet matches `make_control_authentication("", 0)`
exactly. After `05` the client sends the single byte `01` and waits for the
server's initial synchronization; no server initial packet has been sent yet.

Evidence directories:

- `artifacts/steam/live-handshake-02`: GameServer `90296327154011157`,
  messages `04...00` and `01`, challenge/acceptance control events.
- `artifacts/steam/live-handshake-03`: GameServer `90296327481055251`,
  the same sequence from a restarted client.
- `artifacts/steam/live-handshake-03/000000-rx.bin` (10 bytes,
  SHA-256 `ab0c4a5b8bd8ee364e1e638e7d81465a8f4ffe9a8da3f2494962622b72ddc2b4`).
- `artifacts/steam/live-handshake-03/000001-rx.bin` (1 byte `01`,
  SHA-256 `4bf5122f344554c53bde2ebb8cd2b7e3d1600ad631c385a5d7cce23c7785459a`).

The client launch path is also verified: a cold
`steam://run/387990//-connect_steam_id%20<GameServerID>/` launch populates
Steam's `GetLaunchCommandLine`; the game log then contains
`Contraption::SteamCommandLine -connect_steam_id <id>` and the client reaches
`Connected`. Passing `-connect_steam_id` only on the OS command line does not
populate that buffer.

Host-oracle limitation: `-dedicated_server -open <world.db>` starts a
ListenServer and a local client, but the passive probe captured only repeated
small packets (message IDs 30 and 50; max source 23 bytes) from the local
loopback. The hooked codec wrappers are not on the local initial-sync path.
`artifacts/probe-host-initial-3.cap` / `.json` retains this negative evidence.
A remote client, or additional hooks on the split sender (`0xB5A480`) and raw
compressor (`0x4EE990`), is required to capture the server's initial world
packets.

## GameInfo and post-join messages (2026-11-02)

The client's receive dispatcher at `0x450FC0` handles `GameInfo` as message id
2. Its fields are network byte order; on this little-endian build the parser
byte-swaps 32/64/128-bit values. The 32-byte plaintext is:

| Offset | Size | Field |
| --- | --- | --- |
| 0 | 1 | message id `02` |
| 1 | 4 | build/version `889` (`0x00000379`) |
| 5 | 4 | game mode (`0` AlphaTerrain, `14` Survival in the oracle save) |
| 9 | 4 | seed |
| 13 | 4 | game tick |
| 17 | 4 | UGC pair count; each pair is 8-byte id + 16-byte id |
| 21 | 2 | name length, then name bytes |
| … | 4 | first blob-list count; each entry is 16-byte id + 16-bit length + bytes |
| … | 4 | second blob-list count; same entry format |
| … | 1 bit | final flag |

The real client accepted the zero-filled packet and logged
`Version: 889`, `Game mode: AlphaTerrain`, `Seed: 0`, `Game tick: 0`,
`Script key count: 0`, `Generic key count: 0`, `Mod developer mode 0`,
`Connected`. Evidence: `artifacts/steam/real-gameinfo-03` and
`game-20261102-010413.log`. The accepted wire packet is 16 bytes (the
compressed body preserves message id 2).

Immediately after GameInfo, the real client sends a reliable 41,255-byte
message with id `06` (`000002-rx.bin`, SHA-256
`0dd4a4c13a7a5682f089e02b4b360e2d81b889d44c5059531ce33834fdfcfeaa`). Its
LZ4 body decodes to 50,764 bytes. The next server response is the initial
world synchronization (the client parser accepts ids `11`, `13`, `25`, `27`
while initializing). This packet is the immediate next gate.

The original host's loopback self-connection was captured in
`artifacts/host-self-initial-02.cap` (718 records): client `01`, server
GameInfo `02` (game mode 14, seed 834223556, game tick 7408790), client
player-info `09` (265 bytes beginning with the UTF-8 Steam persona name),
client `0A`, then periodic 8-byte `1E` messages. The loopback initial-sync
payloads are not on the hooked codec path, so the capture does not include
the server's initial world blobs.

For native-save sessions, GameInfo game mode, seed and tick are taken from the
selected SQLite save. In retained local samples, `Game.flags` is14 for the
Survival save and the original host advertised mode14; the Creative save has
`Game.flags=1`. This mapping is supported by those local samples and should be
rechecked for other custom game modes.

## Join-state experiment (2026-11-02)

The `--game-ready` listener mode adds two speculative server packets to the
validated handshake:

1. After the client's 41,255-byte id `06`, send one-byte id `07`.
2. After the client's 265-byte player-info id `09`, send one-byte id `0A`.

Observed behavior with a real vanilla client:

- After id `07` the client logs `Receiving initialization data...`,
  `Received 0 chunks of script initialization data`,
  `Received 0 chunks of generic initialization data`, then `Joining game...`.
  It sends the 209-byte compressed player-info packet (id 9).
- After id `0A` the client logs `Join request accepted` and remains
  connected. Missing world data produces `DynamicDataManager.cpp:1593 Failed
  to load Cached Const Data for '' invalid` and a JSON parse error, but no
  disconnect.
- Evidence: `artifacts/steam/join-probe-01` (GameServer
  `90296329214978068`) and `join-probe-02`
  (GameServer `90296329733594132`, client log
  `game-20261102-011715.log`).

Ids `07` and `0A` only drive the client state machine. The server still has
to send the initial world/blob data (ids `11`, `13`, `25`, `27`) and the
NetObj/player spawn data before the client enters gameplay.
