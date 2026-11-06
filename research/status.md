# Implementation status — 2026-11-02

## Runtime milestone — real client control handshake (2026-11-02)

- `scrapds listen-probe ... --handshake` now sends the pre-game control
  challenge `03`, accepts client authentication `04`, and replies with `05`.
  A real vanilla client completed that sequence twice: `live-handshake-02`
  (GameServer `90296327154011157`) and `live-handshake-03`
  (GameServer `90296327481055251`).
- Observed client authentication: `04 00 00 00 00 00 00 00 00 00`
  (empty password, zero secret), reliable. After `05` the client sends one
  reliable byte `01` and waits. That `01` is the next protocol gate.
- Client launch path: `steam://run/387990//-connect_steam_id%20<GameServerID>/`
  fills Steam's `GetLaunchCommandLine`; a plain OS `-connect_steam_id` does
  not. Verified in `game-20261102-005359.log`
  (`Contraption::SteamCommandLine -connect_steam_id ...`).
- Host oracle: `-dedicated_server -open <world.db>` starts ListenServer and a
  local client on a compatible save. A version 30 Creative save is rejected as
  newer than game 1.0.6.889; version 28/29 works and is upgraded. The passive
  probe captured only repeated small packets (message IDs 30 and 50) from the
  local loopback; it did not capture initial world data. The hooked codec
  wrappers are not on the local initial-sync path.
- Host probe capture: `artifacts/probe-host-initial-3.cap` / `.json`
  (2,597 records, max source 23 bytes). This is evidence of the limitation,
  not of the wire initial-sync format.
- Build/CTest: 11/11 tests pass, including the new `control` test.
- Follow-up game-info probe (`real-gameinfo-02`, GameServer
  `90296327825982482`): the listener sent a minimal 32-byte plaintext
  game-info (14 wire bytes, message id 2). The client responded
  `VersionMismatchException` and closed. The version/build/UGC field layout
  still needs correction.

### Correction: GameInfo accepted by the real client (2026-11-02)

- The client parser applies network byte order to multi-byte fields on this
  little-endian build; the first probe wrote them little-endian. After
  switching the minimal GameInfo serializer to big-endian, the real client
  accepted message id 2 in `artifacts/steam/real-gameinfo-03` (GameServer
  `90296328266458133`, client log `game-20261102-010413.log`).
- Observed client log after the reply: `Version: 889`,
  `Game mode: AlphaTerrain`, `Seed: 0`, `Game tick: 0`,
  `Script key count: 0`, `Generic key count: 0`,
  `Mod developer mode 0`, then `Connected`. No exception and no disconnect.
- The accepted wire packet is 16 bytes; the 32-byte plaintext is
  `02 00000379 00000000 00000000 00000000 00000000 0000 00000000 00000000 00`
  (id, version, game mode, seed, game tick, UGC count, name length, two
  empty lists, final flag). Multi-byte values are big-endian.
- Immediately after GameInfo the real client sent a reliable 41,255-byte
  game packet with id 6 (`000002-rx.bin`, SHA-256
  `0dd4a4c13a7a5682f089e02b4b360e2d81b889d44c5059531ce33834fdfcfeaa`).
  Its LZ4 body decompresses to 50,764 bytes. This is the next server gate.
- A self-connection capture in the original host
  (`artifacts/host-self-initial-02.cap`, 718 records, 62,833 bytes) shows
  the loopback join sequence: client `01`, server GameInfo `02`, client
  player-info `09` (265 bytes beginning with the UTF-8 player name),
  client `0A`, then periodic 8-byte `1E` messages. The loopback initial
  world sync is not carried by the hooked codec path, so it is absent from
  this capture. The capture supplies the exact GameInfo wire format and the
  real post-join message ids.
- The game's own `-dedicated_server -open <world.db>` host works with a
  version 29 Survival save, reaches `Start state: Game`, and accepts its
  local client (`Connecting to self`, `Join request accepted`). The real
  GameInfo values there were game mode 14, seed 834223556 and game tick
  7408790, matching the save's `Game` row. A second client instance cannot
  be started through Steam while this host is running; a remote join still
  needs a separate Steam identity or an early-injected launch hook.

### Join-state probe: client reaches "Join request accepted" (2026-11-02)

- Added an experimental `--game-ready` listener mode. After observing the
  real client's id 6 join packet it sends the one-byte state transition id 7;
  after the client's 265-byte player-info packet id 9 it sends id 10.
- With `artifacts/steam/join-probe-01`, the real client (GameServer
  `90296329214978068`) logged `Receiving initialization data...`,
  `Received 0 chunks of script initialization data`,
  `Received 0 chunks of generic initialization data`, then
  `Joining game...`. It sent the same 209-byte compressed player-info packet
  (`000003-rx.bin`) seen in the host loopback capture.
- With `artifacts/steam/join-probe-02` (GameServer `90296329733594132`),
  adding id 10 after id 9 produced `Joining game...` followed by
  `Join request accepted` in `game-20261102-011715.log`.
- The client remains connected. Missing world data produces
  `DynamicDataManager.cpp:1593 Failed to load Cached Const Data for '' invalid`
  and a JSON parse error, but no disconnect. The next required packets are
  the initial world/blob data and NetObj/player spawn data; ids 7 and 10 only
  advance the client state machine.

### Native replay: real client enters the world (2026-11-02)

- A second Steam account in Sandboxie (`<STEAMID64>`) connected to the
  native listener through
  `steam://rungameid/387990//-connect_steam_id%20<GameServerID>/`; the
  `rungameid` form populated Steam's `GetLaunchCommandLine`, unlike passing
  the URI to `steam.exe` directly.
- The real host's initial sync was captured from `probe-split-01.cap`:
  GameInfo 120 bytes, script init `id 11` 1014 bytes, generic init `id 13`
  452 bytes, world `id 29` 86,905 bytes, plus post-join `id 13`/`id 29`/
  `id 24` packets. Replay files: `artifacts/world-sync/replay/`.
- `scrapds listen-probe ... --replay artifacts/world-sync/replay` sends
  GameInfo + init 11/13 + world 29, then id 7 after client id 6, and id 10
  plus post-join packets after client id 9. The client logged
  `Received 1 chunks of script initialization data`,
  `Received 3 chunks of generic initialization data`, `Creating world 0`,
  `Loaded player 1/3`, `Join request accepted`, and **`Start state: Game`**
  (`game-20261102-014657.log`). Screenshot:
  `artifacts/native-ingame.png`; it shows the FlatTerrain world, player name
  and tool hotbar.
- This is a **static replay** of the captured host world, not native
  simulation. The listener accepts one peer; there is no physics/gameplay
  simulation, no persistence of building changes, and no multi-client relay
  yet. It proves the native client can load the real world and enter gameplay.

## Observed locally

- Windows/MSVC C++20 executable builds and runs. CTest suites for physics,
  world inspection, BLOB prefixes, CLI, importer, snapshots, capture parsing,
  and Steam identity parsing pass. Build log: local `artifacts/build-final.log`.
- Independent Bullet falling-box/stack scenarios settle against the plane.
  `artifacts/falling-stack.csv` records 800 ticks with three boxes. This is
  an implementation bring-up check, not a comparison against game physics.
- C++ reader inspected all eight available saves without changes to the files.
  Prefix invariants match all 11,639 RigidBody and 650,059 ChildShape rows.
  See `save-layout-evidence.json` for source hashes and per-file counts.
- SQLite backup snapshots of Creative and Survival pass integrity and schema/
  row-count checks. Original hashes and mtimes are preserved. Evidence:
  local `artifacts/snapshots/verification.json`.
- Importer copied and hash-verified 11,653 candidate server assets into a local
  pack of 455,342,738 bytes including the manifest. Dependency completeness
  remains unverified. Mesh/export/rend files are included as uncertain.
- Anonymous Steam GameServer login for AppID387990 succeeds from an independent
  process on this PC. Observed BLoggedOn=true and callback101; the test session
  logged off cleanly. This establishes identity feasibility, not game join.
- Passive probe Start/Stop succeeded in original game process9764. Its menu
  capture contains a valid file header and zero records. No handshake can be
  inferred from an idle menu.
- Synthetic codec test ran inside the original game: 4,097 source bytes,
  2,074 compressed bytes, original native round trip succeeds. Independent
  `lz4.block` decoder reproduces the 4,096-byte body exactly, and packet byte0
  is preserved. Fixture hash:
  `3877eea012d6e59f56fb65e394e26118f998590a9666e132f67abf28b9801afb`.
  This confirms the tested LZ4 framing; it is not a real packet capture.

## New static findings

- `lua51.dll` contains LuaJIT2.1.1772619647. Lua5.1 describes the API family,
  not the exact runtime. The reference Lua VM in the bring-up executable is
  intentionally marked as such.
- Network packet compressor/decompressor wrappers preserve byte0 and operate
  on the remaining payload. Server receive inlines a wrapper, requiring a
  separate raw-decompressor callsite hook. See `protocol.md`.
- RTTI `TickDynamicsWorld` resolves a vtable whose slot13 points at RVA1118410.
  Its accumulator/substep code matches the stepSimulation signature. The
  passive hook records arguments and returned substep count only.

Static results were produced with ordinary PE/disassembly tools. REA's doctor
reported no connected configuration/provider. No REA configuration was changed;
these reports are not REA Evidence records. A private executable copy is being
analyzed through installed IDA; the owner's existing locked database is untouched.

## Remaining work

- Replace the static replay with native world/NetObj serialization; add
  multiple peers and relay of character/creation updates so two clients play
  together on the native server. The control handshake, GameInfo, init data
  and one-client world load are now confirmed against a real client.
- Complete packet framing, compression conditions, IDs and object codecs.
- Full BLOB decoders, asset collision import and real-world simulation.
- Exact Bullet fork/configuration, materials, joints, terrain and characters.
- Game Lua runtime behavior and server `sm.*` API.
- Gameplay, saves written semantically, server authority, administration.
- Actual Linux build/run and cross-platform parity.

The complete dedicated server requested in server.md is **not finished**.
The current executable and evidence tools form a working foundation for it.

## Native tick/multi-peer foundation (2026-11-02)

## Character update decoder (2026-11-02)

- Added strict parsing for captured client `0x1E` packets: 21-bit-group tick,
  ack list, state type 3, signed fixed-point position and velocity, and character ID.
- Type 3 velocity uses three signed 13-bit magnitudes at scale `10/8192`,
  checked against the captured movement fixture.
- The captured player-info message is 199 transport bytes and 256 decoded
  bytes. Its fields are not yet mapped to the server Player/Character ID, so
  the experimental state-6 transition is not yet player authorization.
- IDA decompilation of the server join path (`b3f580`, `821d20`, `825440`,
  `b2cb60`) shows a CharacterManager runtime object is created with the remote
  SteamID in its owner field (`+56`) and an ID returned by
  `UniqueIdProvider::getNext(4)`. The manager inserts the object keyed by its
  NetObj ID. The constructor resolves to a `Lift` vtable, so this evidence does
  not yet prove that the ID matches the client movement Character ID. The
  native listener has no equivalent runtime manager or allocator and still
  cannot validate the first client state against a server-created Character.
- IDA decompilation of `b45640` confirms PaintCharacter resolves the target by
  Character ID, checks that it is active, then applies RGBA color through the
  server-only Character setter. Color zero resolves the Character asset's
  default color by its saved asset UUID; the setter marks the NetObj dirty and
  issues the game's Character update path. This command requires live runtime
  Character objects and is not a world-save BLOB edit.
- Reproducible IDA output is in `artifacts/ida-work/character-construct-query/`
  and `artifacts/ida-work/join-followup-query/` (analysis copy of the game IDB;
  the existing GUI IDA database was not modified by these queries).
- The Character movement lease is now keyed by the full authenticated SteamID
  from the SNS message, not the transient connection handle. Per-account
  connection counts keep the lease across a same-account reconnect/parallel
  connection and release it when the final connection closes. Tests cover
  SteamIDs above 32 bits and values with colliding low 32 bits. This still
  validates only first-claim consistency; the claim is not server-authorized.
- Confirmed against the actual 31-byte SNS message `000664-rx.bin` after
  decompression to 29 bytes, plus captured tick-only heartbeat bytes.
- Added type 4 tumble decoding for quaternion (smallest-three encoding),
  position and six compact state scalars. This path has a generated synthetic
  fixture, not a captured live tumble packet yet.
- The listener validates these packets, retains the most recent decoded state
  per connection, binds the first character ID claimed by each peer, rejects
  peer ID changes and duplicate ownership, and releases the binding on
  disconnect. It rejects stale ticks with wrap-aware uint32 serial arithmetic.
  Accepted updates are relayed to other joined peers. Malformed, stale, or
  ownership-conflicting updates are dropped and counted.
- The listener now rejects horizontal movement above 6 m/s plus 0.3 m tolerance
  per client tick delta, then sweeps the full capsule path against saved block
  boxes when native physics is disabled. In native-physics mode it instead
  checks the current Bullet scene, including mapped moving bodies and resident
  terrain. Corrected coordinates are written into the server ID24 packet sent
  to the owner and observing peers. The first update is measured from the
  server-assigned spawn position and tick, closing the initial teleport gap.
  Capsule dimensions come from the installed default character definition
  (radius 0.3 m, total height 1.4 m). The server still does not simulate
  CharacterController movement, gravity or jumping, and live behavior is
  unverified.
- `character_tests` and the full CTest suite pass (20/20 in the current build).

- Added a 30 Hz server tick stream (message `0x1E`, tick +2 per packet) to
  clients that reached `game_started`, a second allowed Steam peer, and relay
  of post-join client game packets between peers. This makes the listener a
  multi-client prototype.
- The world is still the captured **static replay**. There is no native
  physics, server-side Lua game mode, creation/inventory/container
  interaction, character authority, persistence or Linux Steam transport.
  Building, inventory, seating and held-tool state therefore remain
  unsupported; client prediction has no authoritative server simulation.
- A full 100% native server requires porting the game's Bullet/custom
  physics layer, world/NetObj serialization, server Lua and gameplay systems,
  plus a Linux Steam transport. This is a multi-month project, not a
  one-night task.
- The real-game host and null-driver host paths were tested as oracles and
  are explicitly **not** part of the product.

## Two-client native session (2026-11-02)

- Added `tools/probe/launch_connect.py`: launches the game suspended, injects
  the probe, installs the parser hook at RVA `0x2F1C90`, forces
  `-connect_steam_id`, then resumes. This bypasses Steam's launch-command-line
  and confirmation dialog entirely.
- A 24-hour listener (`serve-03`, GameServer `90296332603514901`) accepted both
  Steam accounts (`<STEAMID64>`, `<STEAMID64>`). Both clients
  logged `Received 1/3 chunks`, `Loaded player 1/3`, `Join request accepted`
  and `Start state: Game`.
- The listener reports `game_started_count=2`, a 30 Hz tick stream and relay
  of post-join client packets. The two clients are in the same replayed
  FlatTerrain world and exchange character updates.
- This is still a **static replay + client relay**, not native simulation:
  building, inventory, containers, seating and held-tool server logic are not
  implemented. The two clients can be in-world together, but the world is not
  server-authoritative and changes are not persisted.
- Long-run check: the same listener stayed up for 18,015 seconds (~5 hours)
  with `status=listening`, 1,800,358 messages and 39,553,593 bytes, while both
  clients had already exited. Counts: `challenge_count=2`,
  `authenticated_count=2`, `game_info_sent_count=2`,
  `join_accepted_count=2`, `game_started_count=2`. Evidence:
  `artifacts/steam/serve-03/listener.json`.
- The listener later died after ~10.3 hours: `listener.json` had grown to
  398 MB and the final write failed with `Cannot finish probe output`.
  Cause: the whole report (all captured messages) was rewritten every second
  with no array cap. Fixed by writing a compact summary every 30 seconds,
  keeping full arrays for the final report, capping capture at 10,000 messages
  and 64 MiB, and making periodic write failures non-fatal.

## Native world and build/paint (2026-11-02)

- Added `WorldState` (SQLite world in memory, `create_block`,
  `replace_shape_blobs`, `save_as`, ID counters, RTree bounds),
  `AssetDefinitions`, `shape_geometry`, `world_collision`, `world_sync`
  (initial NetObj rebuild and asset catalog), `game_commands` and
  `game/commands`.
- `listen-probe --world <save.db>` loads a save copy, rebuilds the initial
  world packets from that state instead of replaying the captured `world29`,
  applies client commands, writes an initial `<output>/world.db` snapshot,
  checkpoints it atomically every30 seconds and on clean stop.
- `BuildShapeCommand` kinds 2/4 (new body and existing body) and
  `PaintShapeCommand` (single shape, whole block, complete block region) are
  applied with world, asset, orientation, volume and overlap checks. Rejected
  commands are atomic. New bodies/shapes and their ID counters survive
  save/load. CTest `gameplay` covers the real 110-byte build fixture.
- Replication of created bodies/shapes is implemented in the probe: after a
  build it sends an ID22 packet with body creation, shape creation and
  `make_child_shape_state` records to all started peers. It is not yet
  live-verified with an actual client build (`applied_commands=0` in the
  15-minute session).
- This gameplay path has not yet been verified with an actual client command
  (`applied_commands=0` in the earlier15-minute session).
- Not implemented: PaintCharacter, inventory, containers, seating, held tools,
  authoritative physics, server
  Lua and Survival progression. Complete native save snapshots preserve
  existing opaque tables and persist supported build/paint/remove mutations.

## Follow-up native creative operations

The listener now applies complete `RemoveShape` requests for block shapes,
deletes an empty StaticBody, and broadcasts NetObj remove records. Shapes
with joint/controller references are rejected to avoid orphaning runtime
state. Partial region removal uses the same split operation: the selected
intersection is removed and untouched cuboids remain under fresh IDs.

Partial block painting follows the recovered half-open `B48310` geometry:
the selected intersection keeps its existing shape ID, and the original
block remainder is represented by up to six non-overlapping cuboids with
fresh shape IDs. Blob updates, inserted fragment rows and the saved shape-ID
counter commit in one SQLite transaction. Live replication emits create
records for fragments and state records for all resulting shapes. The
gameplay fixture covers geometry, color, IDs, save/reload and subsequent
removal. All17 CTest targets pass. A real vanilla-client gameplay command
has not yet been captured, so protocol acceptance and visual results remain
unverified.

Current remaining work includes Character paint and authoritative character
movement, inventory/containers/seating/tools, authoritative game
physics, Lua/game modes, full native initial-world serialization and the
production packaging/service supervision. The `serve` CLI now starts the
experimental listener when explicit replay bootstrap metadata and a native
world save are provided; it is not yet a production-ready server command.

The server output now receives an initial native SQLite snapshot before
accepting clients, an atomically replaced checkpoint every30 seconds, and a
final checkpoint on clean stop. Snapshot publication refuses to replace the
source save and preserves the last good output if writing the next snapshot
fails. A short `serve` invocation logged on an anonymous Steam GameServer and
produced a loadable 421,888-byte world checkpoint.

`serve` now defaults to `--seconds 0`, an indefinite process lifetime. It
keeps the existing 30-second checkpoint/status publication and exits cleanly
when `<output>/stop.request` appears, then writes a final world checkpoint.
Use `--seconds N` for bounded validation runs. This is graceful lifecycle
control for the experimental listener; process supervision, restart policy,
and public discovery are still outstanding.

## Native PaintJoint persistence

- Save inspection showed RGBA32 bytes at offsets65..68 in the 69-byte Joint
  BLOB; the same values match ChildShape colors saved in the same world.
- Added identity-preserving, transactional Joint BLOB replacement and the
  explicit-color `PaintJoint` path. Updates are sent as NetObj state type2
  using the original Joint asset-catalog index.
- `color_rgba==0` resolves through the Joint asset's ShapeSet default color.
  Runtime visual confirmation against a vanilla client is still pending.
- Gameplay tests cover the target BLOB layout, NetObj update, and save/reload.

## Character creation path and default Joint paint (2026-11-02)

- IDA call-chain tracing now distinguishes the join-created Lift from the
  movement Character. `CharacterManager::create` at RVA `0x821220` allocates
  the real Character through `UniqueIdProvider::getNext(3)`, then registers
  it with CharacterManager. The owner ID comes from the Player descriptor;
  PlayerManager's spawn path fills that field from its Player object. This is
  the first confirmed server-side source for movement Character IDs. It still
  needs the exact spawn NetObj wire record before the native listener can
  reproduce it safely. Provider index 3 starts at 1 and is not in the saved
  counter mapping; saved slot 3 instead restores provider index 5.
- IDA artifacts: `artifacts/ida-work/character-spawn-calls/821220.c`,
  `artifacts/ida-work/character-spawn-entry/7fb060.c`, and
  `artifacts/ida-work/character-ctor-calls/824f40.c`.
- Character create serialization is now available as a NetObj create record
  of type 6. Client factory `D99ED0` case 6 constructs Character and calls its
  virtual create reader at `72FE60`; the matching writer `72FDB0` emits owner
  ID, a reversed 96-bit position vector, world ID, two 32-bit fields, a 128-bit
  asset UUID and a one-bit flag. `make_character_creation` builds the framing;
  a hand-written byte oracle exercises the field order and endianness.
  References: `character-client-factory/d99ed0.c`,
  `character-vtable-slots/72fdb0.c`, and `character-spawn-codec/`.
  The payload is not yet wired into join: player descriptor, character asset,
  orientation semantics and a vanilla-client spawn confirmation are pending.
- Client message ID 9 is now parsed before the join transition: BE16 name
  length, appearance version 2, appearance type and part count (at most 9),
  16-byte UUIDs, BE32 colors, then a kind and length followed by a serialized
  Steam inventory result. The game's ID 9 handler invokes the Steam Inventory
  deserialize method with those bytes; this is not an authentication ticket.
  Two previously captured messages match this layout with no trailing bytes.
  The listener retains the parsed descriptor per connection and rejects
  malformed packets. The listener now starts Steam GameServer Inventory
  deserialization for nonempty results, polls completion, verifies the result
  owner against the transport SteamID and releases the handle on completion,
  timeout or disconnect. Expired results are marked separately. Verification
  is not used as player authentication or item authorization; applying the
  validated inventory and Player/Character spawn remain outstanding. A
  one-second anonymous listener start passed with the new exports, while the
  inventory validation path itself has only fake-API tests so far.
- PlayerManager `7FD1E0` publishes Player state as BlobData UID
  `51868883d2d2495391351ab0bdc2a47e`, a little-endian 32-bit Player ID key,
  world `0xfffe`, flags 13, and a raw LZ4-compressed payload from `803C00`.
  `make_player_blob_entry` generates this record; a local captured host
  fixture reconstructs and compares the complete decompressed payload without
  exposing the player name or inventory. Player IDs use UniqueIdProvider
  index 10, stored in Game.uniqueIds slot 8; `reserve_player_id` commits this
  counter and survives save/reload. The experimental native-player join path
  now uses these records.
- The captured `post-003-id29-67185.bin` aggregate contains a type-6
  Character create record followed by a type-6 state record. The creation
  serializer and the new full-state writer reproduce both payloads. Tests use
  the captured state bytes as an oracle and compare the complete captured
  create payload when the local artifact is present. The sampled spawn is in
  world 0 at `(16, 16, ~0.72)` with a zero UUID; this is one captured world,
  not a general spawn-position rule. The experimental join path currently uses
  the recorded spawn descriptor, so other worlds need an appropriate template.
- `prepare_player_spawn` now reserves the saved Player ID, allocates the
  runtime Character ID, and constructs the observed Player-before,
  Player-linked and Character-create/state sequence using an explicitly
  supplied spawn descriptor. It overwrites both owner fields with the
  authenticated SteamID. If packet construction fails, the Player counter
  rolls back and the new Character is removed; a consumed runtime Character ID
  is not reused. Tests cover packet order, counter persistence and rollback.
  The listener calls this function in opt-in `--native-players` mode. A real
  client reached Game and loaded its generated Player in live-02. Session
  `native-players-live-04` accepted 920 movement/heartbeat updates and rejected
  none after correcting the Character ID decoder. Authoritative simulation and
  complete player lifecycle remain outstanding.
- `strip_replay_players` removes Player BlobData and Character NetObj records
  from replay packets and nested aggregates while retaining other entries;
  it has synthetic coverage and checks against the captured Player/Character
  packets. It is enabled with `--native-players`, together with generated
  Player/Character spawn. An experimental disconnect path now unlinks the
  departing Player and sends a Character NetObj removal to peers that received
  its complete spawn. A later join receives the offline Player snapshot; a
  reconnect in the same listener session reuses that Player ID and allocates a
  fresh Character ID. Failed reliable sends still force the affected peer to
  reconnect. The unlinked snapshot and SteamID-to-Player ID mapping are stored
  in server-owned SQLite table `scrapds_player_v1` in the same transaction as
  Player ID allocation. Checkpoint/reload restores and validates the snapshot;
  reconnect reuses the saved Player ID. Packet construction, rollback and ID
  reuse have unit coverage, but the new lifecycle has not yet been verified
  with two real clients. Vanilla compatibility of this extra table has not been
  verified.
- The latest accepted movement position and world are retained in memory,
  used for late-joining observers and reconnecting players, and flushed to
  `scrapds_player_location_v1` with the world checkpoint. Checkpoint/reload
  coverage includes the location and Player identity. These positions are
  client supplied; portals and authoritative motion remain unimplemented.
- Added a vertical capsule sweep and bounded sweep-and-slide movement over
  imported Bullet block collision shapes, with rotated-body, wall-sliding and
  separate-world checks. The world importer still omits terrain, parts,
  dynamic motion and joints; initial-overlap recovery is also missing, so this
  is not enabled as a complete Character controller or anti-cheat rule.
- Character movement bit fields use sequential byte chunks stored low byte
  first, including non-byte-aligned 15-bit IDs and 27-/13-bit magnitudes.
  The former interpretation misread captured ID 1 as 128 and position
  `(16, 16, 0.72003)` as approximately `(0.002, 0.002, 677.75)`.
  Magnitude scaling follows the decompiled game readers. Decoding movement
  does not implement server-authoritative character physics.
- Tumble quaternion magnitudes use the native reader's `0.00008632728227`
  scale (RVA `735A50`), including its little-endian bit chunks. A nonzero
  quaternion test now covers this alongside overflow rejection for tick values.
- Game reader `7A89A0` removes a BlobData entry when its size is zero, but
  PlayerManager's update loop `8006D0` does not call the normal Player update
  handler for this case. `7FE0F0` instead unlinks a Player by assigning
  Character ID `-1`; therefore the experimental disconnect path retains the
  Player and sends that snapshot before the Character NetObj removal.
  A reconnect test on 2026-11-03 could not start because the sandbox game
  reported `SteamAPI_Init() failed`; no live reconnect is claimed.
- Native `PaintJoint` now resolves color `0` from the Joint BLOB's asset UUID
  and ShapeSet default color, then persists and replicates the resolved RGBA.
  Unknown assets or missing defaults remain rejected. This path has not yet
  been exercised against a real client.

## Live world bootstrap follows the current save

- The captured NetObj bootstrap is now treated as a packet/catalog template:
  create and state records for removed bodies, shapes, and joints are pruned
  before a joining client receives it. This prevents the replay template from
  resurrecting entities removed during the server session.
- Newly inserted supported 69-byte Joint blobs now produce both NetObj create
  and catalog-indexed state records. Joints with unsupported layouts, missing
  shape references, or UUIDs absent from the established asset catalog fail
  closed. New native block shapes and static bodies continue to use the same
  live reconstruction path.
- The `scrapds` target compiles with these changes. A vanilla client join after
  deleting or adding a joint has not yet been run.

## Atomic gameplay commands

- Each native gameplay command now wraps nested SQLite changes in a savepoint
  and snapshots the in-memory entity caches. A rejected command or exception
  rolls back all nested writes and restores the caches, including partial block
  splits performed earlier in the same command.
- Gameplay checks exercise rollback after an actual nested shape mutation for
  both a rejected operation and an exception, then reload the saved database.
  The current implementation copies world caches for each command; reducing
  this cost without weakening rollback remains future work.
- Listener gameplay processing now prepares and encodes the NetObj response
  inside an outer transaction before committing the world. A missing catalog
  entry or encoding error rolls back both cache and SQLite state; the gameplay
  fixture checks this through save/reload. A peer whose reliable send is
  rejected is disconnected and must bootstrap again; retry is not implemented.
- Catalog inference skips replay entities deleted from the loaded checkpoint.
  Server output now includes `asset-catalog.bin`, bound to the world29 SHA-256;
  restarting with the checkpoint restores its UUID/index mappings even if all
  original entities of an asset were removed. Conflicting mappings, malformed
  snapshots and mismatched bootstrap hashes are rejected. Older checkpoints
  without a sidecar can only infer mappings from their surviving replay entities.
  The recorded 1,880-shape/230-joint fixture independently yielded 96 shape and
  7 joint indices with no duplicate-index conflicts.
- `catalog-smoke-01` started the real anonymous Steam listener and wrote a
  checkpoint plus catalog; `catalog-smoke-02` restarted from that checkpoint
  and wrote an identical catalog. Both ran for one second without clients.
  The indefinite listener duration guard now accepts `0`, consistent with the
  documented `serve` default; a regression check reaches the executable hash
  guard with that duration.

## Optional Steam listing and open admission

- `serve --public` now asks Steam GameServer to advertise after anonymous
  login succeeds and permits authenticated individual Steam accounts in the
  public universe. Private mode still requires `--allow-peer` and remains the
  default. `listener.json` and `ready.json` report both requested public mode
  and the active advertising state.
- Shutdown disables advertisement before logging off. The `scrapds` target
  compiles with the CLI and GameServer API wiring.

## Fixed server tick and 15-minute native session (2026-11-02)

**Later correction (2026-11-03):** client dispatcher `450FC0` does not handle
message ID30; the session below had repeated `Unprocessed network message` log
entries despite successful decompression. The experimental listener now sends
an empty ID22 NetObj update with a big-endian tick after replay initialization,
whose receiver path is supported by `research/heartbeat.md`. It also no longer
relays client ID30 input directly to other clients, which cannot dispatch it.
The ID22 change has unit coverage and static client-reader evidence but has not
yet been verified in a new real-client session.
The server-side ID24 Character frame has now been reconstructed using actual
client reader `47BCC0`/`D9E190`/`D9D740`: 21-bit-group server tick, signed
zigzag delta in 9-bit groups to client tick, six-bit sequence, Character state
and ID, then a zero four-bit terminator. Two independently captured host packets
(`post-021-id24-67332.bin` and `post-114-id24-67447.bin`) are reconstructed
byte for byte from their state bits. The native-player listener now converts
accepted client ID30 state into unreliable server ID24 frames for other peers
that received the corresponding Character spawn. The delta uses each receiving
peer's latest accepted input/heartbeat tick (or zero before its first packet),
following the receiver's acknowledgement bookkeeping; this mapping is inferred
from `47BCC0` and still requires a live multi-client check. Horizontal speed is
bounded; saved block geometry clips the path without native physics, while the
current native Bullet scene clips it when native physics is enabled. Vertical
movement and the full character controller remain client supplied, and
multi-client gameplay confirmation remains outstanding.
Client loop `47BCC0` advances its runtime tick once per 25 ms. The listener now
advances its clock once per 25 ms instead of twice per 33 ms; the old cadence
diverged from the client. The revised cadence needs live-session confirmation.

## Experimental live block-body physics

- `WorldPhysics` builds separate Bullet worlds from complete supported bodies
  containing blocks or explicitly box-shaped parts with valid saved axes,
  preserving saved pose and linear/angular velocity. Compound block masses
  determine the Bullet centre of mass and principal inertia; conversion back
  preserves the saved construction origin. Force and impulse APIs are available
  for future gameplay integration. Bodies containing unknown geometry and
  dynamic bodies associated with joints/controllers are conservatively excluded.
  Dynamic bodies without a captured runtime flag are excluded from live mode.
  Material density is a configurable approximation (1000 kg/m³ by default),
  not the game's material model. Voxel terrain and unsupported joint types remain absent.
- Box parts use explicit asset dimensions, saved grid origin and saved X/Z axes.
  Five one-part static bodies (1011, 1179, 1180, 1182 and 1183), including three
  non-identity orientations, match independent saved RTree XY bounds within
  0.01 units. Static bodies with joints/controllers may act as immovable
  colliders; dynamic connected bodies remain excluded.
- Runtime body ID24 type-1/type-2 records were recovered from reader `D9D740`
  and writer `B893D0`/`736A00`/`736BA0`/`736D60`. The codec roundtrips every
  bit of a captured DynamicBody record, including quantized pose, velocities,
  three runtime flag bits, ID, tick delta and sequence.
- `serve --native-players --native-physics` advances supported bodies, updates
  the private in-memory SQLite world and broadcasts their ID24 poses. Recorded
  post-bootstrap ID24 packets are suppressed in this mode; runtime body flags
  come from the matching replay. A gameplay mutation rebuilds the physics scene.
  Physics is disabled on simulation/rebuild failure. `physics.json` and
  `physics-final.json` show coverage and tick count.
- Smoke run `native-physics-smoke-01` ran 120 steps with 5 static and 2 dynamic
  bodies out of 237, saved the result and kept the source read-only. It did not
  connect a client. Because terrain and most colliders are missing, bodies can
  fall through the world; this opt-in experiment is not production physics.
- After adding static anchors and identity box parts, `native-physics-smoke-04`
  ran 80 steps with 9 static and 2 dynamic bodies, including 2 box parts, while
  explicitly reporting 86 unsupported terrain chunks. No client joined.
- After adding rotated box parts, `native-physics-smoke-05` ran 80 steps with
  11 static and 2 dynamic bodies, including 12 box parts. No client joined.
- Cylinders use their saved axis and asset dimensions; two static examples match
  saved XY bounds to 0.01 units, and a falling block contacts a test cylinder.
  Assets with a hull are excluded even when a box is also declared. Run
  `native-physics-smoke-06` completed 80 ticks with 13 static and 2 dynamic
  bodies, including 12 box parts and 2 cylinders, with 86 unsupported terrain
  chunks. No client joined; collision margins and materials remain approximate.

## Voxel terrain deltas

- `inspect-terrain WORLD.db` opens the save read-only and validates its compressed
  VoxelTerrain records against their SQLite IDs, world and region coordinates.
  The record stores a reversed XYZ chunk coordinate, type 4–9 and a bounded
  payload; each outer record is raw LZ4. Type 4 and 5 decoders extract low-five-bit
  voxel values from an octant mask and Morton-ordered occupancy bitmaps, using
  the original executable's 512-entry coordinate table and octant offsets; type
  4 also contains high-three-bit values. The decoded edits can be applied to a
  17×17×17 base chunk, preserving omitted high bits and unchanged voxels.
- The test world contains 86 type-5 chunks and 1,621 changes. These are deltas:
  unchanged values and material bits come from the generated world, whose
  placement has not been reconstructed. This reader does not enable voxel
  collision in the live server.
- Shared 17³ chunk boundaries now accept edits from the corresponding positive
  neighbour, including edges and corners; invalid or duplicate relevant records
  leave the target chunk unchanged. WorldState can also extract the compressed
  `terrainData` ScriptData entry (UUID `61aa13d7-e715-4153-a269-4d338c0c5bd4`)
  as bounded LUA data. Serializer `246140` establishes the bit-packed table
  format, including sequential integer keys and UUID native class 10001. The
  implemented bounded reader decodes tile IDs, offsets and rotations from the
  637,572-byte saved generator table. Its 16,128 cells match installed TILE
  headers by UUID (stored in reverse byte order). Complete tile voxel chunks can
  now be meshed, rotated using the game's quarter-turn convention and translated
  into world coordinates. Surface heights, voxel edits and automatic loading of
  the resulting mesh into live Survival collision remain unimplemented.
- The sample's 16,128 overworld cells contain no voxel chunks; their principal
  surface comes from TILE height data. `E09520` reads a 33×33 mip-0 height grid
  from the first part of a compressed per-cell mip block, each sample stored as
  a float with four additional bytes. The reader loads those samples and checks
  a real tile. The saved elevation, cliff and flattening contributions are now
  decoded and the resulting surface is streamed into experimental live
  collisions; see “Saved Survival overworld collision streaming” below.
- Terrain tile versions 11–15 contain an LZ4 voxel section per cell: each entry
  contains three little-endian coordinates and 4096 voxel bytes. The reader
  validates offsets and sizes, and reconstructs the 17³ chunk only when all
  eight boundary neighbours are available. One shipped underground tile has
  148 voxel chunks in its first cell, 71 with all boundary neighbours present;
  those 71 chunks yielded 4,096 triangles. Underground voxel collisions are
  still not streamed into the live server.
- Original mesher `BC69D0` subtracts 16 from the low five bits, interpolates
  zero crossings, uses tables at RVAs `14711C0`, `14763E0`, `14764E0`, and
  offsets vertices by `16*chunkPosition+0.5` (`BB0AC0`). The table reader
  validates those tables in the installed 889 executable; another version needs
  verification. `WorldPhysics` can accept supplied static terrain triangles;
  saved Survival heightfield cells now use the active-area streamer documented
  below. Underground voxel meshes remain offline-only.
- `CreativeFlatWorld` metadata is detected from saved GenericData and its
  `terrain_flat.lua` specifies `GetHeightAt` of zero. The live physics scene
  adds a bounded plane for this world (cell limits -16..15, cell size 64).
  Four Bullet substeps per 25 ms tick prevent a captured thin block, body 387,
  from passing through the plane in the 400-tick fixture; this is not a general
  proof against tunnelling. `native-physics-flat-02` ran 79 ticks with the plane
  and 2 dynamic bodies, with no client connected. Voxel chunks remain unsupported.

## Experimental free bearings

- Joint serializer `9562C0` stores two 96-bit reversed XYZ integer positions at
  offsets 39 and 51, and axis/reference directions in packed bytes 63 and 64.
  The game uses anchor `grid*0.25+0.125` (`954EC0`, `955080`), and the two
  directions in byte 63 are the bearing axes. Across 146 captured bearings,
  the absolute world-axis dot product was at least 0.999; saved anchor errors
  were at most 0.092 world units. The decoder checks both shape IDs and axes.
- Bullet free hinge constraints are enabled only for complete connected groups
  of known vanilla bearings whose bodies, geometry, saved frames and runtime
  mapping are all supported; a controlled or unknown joint disables its entire
  group. Known controller host kinds 1 and 2 refer to shapes and joints rather
  than coincident body IDs. Motors, suspensions and pistons remain unsupported.
- The copied 237-body world simulates 13 dynamic bodies and 6 free bearings;
  after 400 ticks the six simulated anchor separations were at most 0.0025.
  Tests exercise translational coupling, free rotation, and rejection of
  partially mapped and controller-driven groups. The Steam smoke run
  `native-bearing-smoke-01` timed out during server login; retry
  `native-bearing-smoke-02` ran 78 ticks with 13 dynamic bodies and 6 bearings.
  Neither run had a client, so live bearing replication remains unverified;
  captured runtime flags limit live simulation to mapped bodies.

## Convex part geometry

- For pure `hull` definitions with an OBJ collision file, the asset importer
  loads bounded, finite vertex sets from the installed game and
  creates a Bullet convex hull. Local vertices are in grid units and scaled by
  0.25; shape placement uses the saved part axes and half-dimensions. The hull's
  cached broadphase bounds include its margin once, as observed on static
  Survival objects. Explicit margins are in grid units and are multiplied by
  0.25; rounded lines/planes are accepted only with a positive explicit margin.
  Mixed shape definitions and missing or unusable files are excluded. Hull mass
  still uses a bounding-volume approximation.
- The `native-hull-smoke-01` listener completed 119 ticks with 15 dynamic and
  14 static bodies, 6 bearings, and 4 hull parts; no client connected. The
  local 400-tick run without replay mapping included 17 dynamic bodies and 7
  hull parts. Synthetic tests exercise asset loading, placement and collision;
  two independent saved Survival object bounds match within 0.015 units.

## Experimental survival suspension

- For an unlinked, compact SurvivalSpringController (type 36, 20-byte record),
  the final byte is the saved stiffness index (`C6BC00`/`C6BC40`). The asset
  supplies spring length; fallback stiffness values come from `21B4D0`. Spring
  joint creation `C7BA00` uses a 6-DOF spring along frame Z, linear limits
  `[0.25, length*0.25]`, zero angular limits, damping 2500 and equilibrium
  `(length+1)*0.25` (`C7A910`).
- The scene accepts only fully supported, world-consistent groups, one verified
  controller per survival spring, aligned axes and unlinked controller state.
  Tests cover lateral and travel limits and show different motion for stiffness
  indices 0 and 3. Real joint 4 and controller 33 were copied read-only into an
  isolated two-body fixture; after 100 ticks the anchors were 0.50003 apart.
- Recognizing strictly validated, unlinked vanilla headlight controllers as
  passive allows one saved assembly to run with five springs and ten bearings.
  `native-spring-group-01` ran 193 ticks in five seconds with 24 dynamic bodies
  and no client. In a 400-tick offline run, one spring exceeded its 0.5-unit
  travel limit by approximately 0.028 units under load. The result remains an
  approximation; controller changes and live behavior have not been verified.

## Piston investigation

- The vanilla piston controller writer `BF4960` stores two bytes at controller
  offsets 360 and 361 after ControllerBase. The reader `BF49B0` treats 0xff
  as unchanged and clamps speed to its configured speed-level list. Compact
  unlinked type-35 records have been decoded, including saved controller 57.
- Survival initialization `C70ED0` sets the joint length parameter to 16 and
  its target to length index plus two. Joint creation `C7E380` offsets frame A
  along its axis by half the joint length in grid units and sets linear limits
  to `0.25 - length*0.125` and `length*0.125`. Powered movement, input links,
  motor force and speed values remain unresolved, so piston assemblies are not
  yet enabled in the physics scene. The motor update `C7D2F0` compares target
  and current positions in grid units, sets zero velocity within 0.01 units,
  otherwise clamps the error to ±0.66666597 and multiplies it by the saved
  runtime speed and 0.37500039; this still needs the game's force, activation,
  and input state before it can be used safely.

- Host capture shows the real tick: 8 plaintext bytes
  `1E <tick LE32> 00 00 00`, compressed to 9 wire bytes
  `1E 70 <tick LE32> 00 00 00`, sent unreliable.
- The old implementation sent 5 raw bytes, so the client tried to LZ4-decode
  them and produced 3,485 `Failed to decompress network packet! Error code: -2`
  errors in about two minutes.
- The server now sends the exact compressed 8-byte tick with a flush at 30 Hz.
- Verified session `native-tick-04`: GameServer `90296370461787156`, client
  `<STEAMID64>`, 900 seconds, 41,814 messages, all join counters 1,
  `Start state: Game` at 13:25:50, clean stop at 13:37:37, zero decompression
  errors, 412 KiB `world.db` saved. Evidence:
  `artifacts/steam/native-tick-04/` and the game log
  `game-20261102-132527.log` inside the Sandboxie box.
- Client launch on this machine must run inside Sandboxie `DefaultBox`
  (Steam has `SbieDll.dll`) and `Start.exe` must be started de-elevated
  (`runas /trustlevel:0x20000`). Launching outside the box gives
  `SteamAPI_Init() failed`. The sandbox Steam account is
  `<STEAMID64>`.

## Saved Survival overworld collision streaming (2026-11-05)

- `WorldState::terrain_generator_worlds()` locates saved terrain-generator
  ScriptData records without guessing world IDs. The experimental streamer
  decodes supported Overworld tables and indexes installed `.tile` assets.
- The server builds heightfield collision meshes around active players and
  simulated bodies. Player cells take priority; up to 4 new cells are meshed per
  25 ms tick. Cells outside active areas are removed, and the scene is capped at
  512 resident cells. It reattaches the resident set after native world updates
  rebuild the physics scene.
- Status is written to `overworld-terrain.json`: supported worlds, source and
  resident cells, deferred targets, and unavailable cells.
- Initial ID11/ID13 packets now refresh only mapped managers: terrainData UUID
  maps to itself, GenericData runtime UUID `3ff36c8b93f74428ae4d429a6f0cf77d`
  maps to saved UUID `5297769df4514e5e9a388b0f95e2edad`, and the known `44ac...`
  manager maps to itself. Runtime UUIDs/flags come from the capture; keys,
  world IDs and payloads come from the save. Unknown BlobData managers remain
  from the capture. Survival client compatibility is not verified.
- `WorldState::terrain_deltas()` now decodes and cross-checks saved
  `VoxelTerrain` rows. The active-area streamer adds complete voxel chunks from
  supported overworld `.tile` cells and applies nearby deltas symmetrically to
  shared chunk edges/corners. Telemetry reports source delta count, unmapped
  region IDs and resident cells that contain voxel meshes. This collision path
  compiles but has not been exercised in a live Survival session.
- ID25 now refreshes two captured ScriptData groups from the selected save.
  Exact captured keys take priority; changed world-specific keys route through
  known root fields in the bounded Lua serialization. Each group needs at least
  three distinct matching keys. Other groups remain capture-sourced, and live
  Survival compatibility remains unverified.
- `--native-players` now handles explicit-color `PaintCharacter` commands for
  active server characters in the sender's world. It verifies the target
  against the live server character registry, changes the cached spawn state
  and broadcasts a type-6 NetObj state. Explicit colors are stored in the
  server output world and restored after restart. Color `0` resets to the
  captured player Character default (`0xffffffff`); other Character asset
  defaults and complete inventory/tool authorization remain unimplemented.
  This wire path has not been exercised by a live client.
- Player-info Steam Inventory results are no longer copied into Player records
  before validation. After the GameServer Inventory API confirms the blob's
  owner SteamID, the server updates the linked Player entry, persists the
  unlinked snapshot, and broadcasts the verified result. This covers the Steam
  inventory blob only; Survival item stacks, crafting and containers are still
  unimplemented, and the asynchronous update has not been live-client checked.
- The live bootstrap now removes captured Container create/state records when
  their IDs are absent from the selected save. This prevents stale captured
  containers from appearing in a different save. Container contents still
  come from the capture; Survival item transactions and persistence remain
  unimplemented.
- Schema-1 Container BLOBs with the observed 22-byte entry layout are indexed
  in `WorldState`, including their opaque suffix. A save cross-check maps bytes
  2..17 of each entry to its serialized item UUID; bytes 18..21 are the BE32
  stack quantity, with `0xffffffff` observed for empty slots. `WorldState` can
  now read a matching slot and spend an exact amount transactionally while
  preserving the two opaque entry bytes and all other container data. Player
  snapshots map to persistent containers; new containers use 30 slots and are
  sent as type-4 creates. Survival BuildShape validates the player's slot,
  UUID and volume cost, atomically creates the block and spends inventory,
  then broadcasts the changed slot to same-world peers. This compiles but has
  not been checked with a live client. Survival block removal now refunds the
  selected volume using the asset's explicit stackSize. Full containers reject
  the entire edit; changed slots and geometry replicate together. The behavior
  has fixture coverage but remains unverified against the live game's refund
  rules. Existing stack subtype bytes are preserved; if no matching stack
  exists, new entries use subtype 1 as observed for starter tools, which is not
  yet confirmed for block items. Supported joints without attached controllers
  can also be removed with a one-item refund; paint remains Creative-only.
- Native GameInfo now uses the selected save's `Game.flags` value for game
  mode, alongside its saved seed and live server tick. Local Survival evidence
  matches mode14; the Creative save has flags1. Custom-mode mappings are not
  independently verified.
- The mesh uses the saved elevation/cliff data and FLATTEN nodes. Boundary
  samples match across the checked neighboring cells. The game’s exact height
  triangulation, terrain edits/underground voxels, character collision, and
  physics parity remain unverified. The server still relies on captured
  bootstrap data and is not a complete Survival server.
- Client command ID27 removes supported 69-byte Joint rows transactionally and
  broadcasts NetObj operation2/type2. Survival removal refunds one item using
  the saved joint UUID and asset stack size, rejecting a full inventory and
  joints with attached controllers. Custom modes remain unsupported; the
  Survival path has not yet been verified against a live client.


## Peer liveness cleanup (2026-11-06)

- The native listener tracks accepted connections from acceptance, allowing 120 seconds before joining and 45 seconds of silence in game. It drains queued packets before checking the timeout and releases player, character, inventory request and visibility state through the normal close path. Real-client timeout behavior has not yet been confirmed.

## Physics writeback and player restoration (2026-11-06)

- Physics writeback updates all simulated bodies and RTree bounds in one SQLite transaction without copying the entire world. A per-world geometry version detects intervening body, shape and joint changes, and rolls back with an unsuccessful world transaction. This avoids repeated full-world comparisons each tick; performance on a live Survival server is not yet measured.
- Player snapshot parsing consumes the appearance count before reading its entries, fixing a field offset that prevented previously saved players from being restored. The gameplay fixture now completes its spawn and restore path.
- With `--native-physics`, accepted player displacement is swept and slid as a vertical capsule against the current Bullet scene, including simulated rigid bodies and resident terrain. The static collision snapshot remains the fallback when native physics is off. This corrects client positions but does not simulate a CharacterController, gravity, jumping, falling, damage or world transitions; live-client behavior is unverified.
- Live Character create/state and inventory-container visibility is now restricted to peers in the same saved world. Offline Player records still replicate for the player list. Multi-world visibility has fixture/build coverage only and has not been checked with multiple live clients.

