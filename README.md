# scrapds

Native Scrap Mechanic dedicated-server implementation in progress. The current
build has an experimental Steam listener that has accepted vanilla clients into
the game using a captured bootstrap, created players and relayed movement. The
listener can apply a limited set of world commands and run an approximate,
opt-in Bullet simulation. It is still not a complete or production-ready game
server; see the limits below and in [research/status.md](research/status.md).

## Build

Windows, Visual Studio 2022 C++ Build Tools:

```powershell
powershell -ExecutionPolicy Bypass -File tools/build.ps1 -Test
.\build\scrapds.exe status
```

CMake and Ninja bundled with Visual Studio are detected automatically. The
first configure downloads pinned, hash-checked Bullet, SQLite, Lua and MinHook
source archives unless already present in `downloads/`.

Linux, C++20 compiler and CMake 3.24+:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel 4
ctest --test-dir build --output-on-failure
```

Windows builds and tests have run locally. A Windows/Linux GitHub Actions
workflow is provided; Linux execution has not yet been verified on this PC.

## Commands

```powershell
# Inspect an existing world without writing to it.
.\build\scrapds.exe inspect 'C:\path\world.db' --samples 3

# Run an independent Bullet scenario, with a trace for later parity comparison.
.\build\scrapds.exe simulate --scenario examples/falling-stack.lua --ticks 800 --trace artifacts/stack.csv

# Snapshot a live or stopped SQLite world, including committed WAL contents.
python tools/world_snapshot.py --source 'C:\path\world.db' --output artifacts/snapshot.db

# Inventory the server asset candidate set; --copy performs the local import.
python tools/importer/import_data.py --game 'D:\Steam\steamapps\common\Scrap Mechanic' --output artifacts/data-pack

# Verify anonymous Steam GameServer identity using the owner's installed DLL.
python tools/steam/identity_probe.py --help

# Answer the control handshake and GameInfo (03/04/05, id 2).
# --game-ready adds experimental transitions 7 and 10.
.\build\scrapds.exe listen-probe --game 'D:\Steam\steamapps\common\Scrap Mechanic' --output artifacts/steam/session --allow-peer <STEAMID64> --seconds 300 --handshake --game-ready
```

The listener's `--handshake` mode sends the challenge `03`, validates the
client authentication `04`, sends acceptance `05`, and answers the client's
first game packet `01` with a minimum `GameInfo` (id 2). The standalone
`listen-probe` command remains a handshake instrument. Use `serve` with a
captured bootstrap to send initial data and spawn native players.
See [research/protocol.md](research/protocol.md).

The simulation command uses **upstream Bullet 3.25 and reference Lua 5.1.5**.
Its default 40 Hz and physics settings are bring-up choices. It does not load
game collision geometry or implement `sm.*`, NPCs, terrain or creations. The
game actually embeds LuaJIT and custom Bullet classes. Physics parity is not
claimed.

World inspection handles schema versions observed locally (27–30), RTree
bounds, metadata, per-table row counts, bounded BLOB samples, and verified
prefix fields. Candidate position/quaternion/UUID interpretations are labeled
and do not constitute a complete save decoder.

### Experimental native `serve`

`serve` defaults to an indefinite session. Use `--seconds N` for a bounded
run, or leave it at `0` to run until `<output>/stop.request` is created. The
listener checkpoints `<output>/world.db` every 30 seconds; creating the stop
file triggers a final checkpoint and status report. Monitor `<output>/ready.json`
and `<output>/listener.json`. `ready.json` appears only after saved initial-data
packets and the native NetObj world pass startup serialization checks. The
output directory must not already exist.

```powershell
.\build\scrapds.exe serve --game 'D:\Steam\steamapps\common\Scrap Mechanic' `
  --output 'D:\scrap-server\session-001' --allow-peer <STEAMID64> `
  --replay 'D:\scrap-server\bootstrap' --world 'D:\scrap-server\world.db' `
  --handshake --game-ready --native-players
# Optional: enable experimental mapped-body physics and nearby Survival terrain.
# Add --native-physics to the command above.
# Clean shutdown:
New-Item -ItemType File 'D:\scrap-server\session-001\stop.request'
```

This command still requires captured bootstrap/replay metadata and, unless
`--public` is selected, an explicit allowed Steam peer. `--public` requests
Steam advertisement, but public discovery and public joins are not verified.
`--native-physics` enables mapped dynamic bodies, nearby saved static bodies,
and supported shapes/joints. Static bodies are selected from RTree bounds
around active characters and mapped creations, then refreshed as those anchors
cross 128 m regions; `<output>/physics.json` reports loaded and culled objects.
It uses approximate materials and streams saved Survival heightfield cells
around active players and bodies. It also meshes complete supported tile voxel
chunks and applies saved `VoxelTerrain` edits to those chunks for collision.
`--native-players` also streams nearby terrain into the static character
collision scene when native body physics is off. That scene includes saved
block geometry and supported box, cylinder and hull part assets at their saved
poses, plus the saved Creative flat-world plane. Parts with missing or
unsupported asset geometry are counted and omitted; saved part proxies do not
move unless native body physics is enabled. Saved-body collision is selected
from the RTree bounds around active characters and refreshed as they cross
128 m regions; `<output>/character-collision.json` reports loaded and culled
geometry. It does
not simulate all joints/controllers, the character controller, NPCs, or the
game's Lua gameplay. Two ID25 ScriptData groups now refresh from the supplied
save when at least three distinct keys match either the captured key set or
the recognized serialized Lua fields; the other groups still come from the
capture.
For a supplied save, the server refreshes the terrain-generator ScriptData and
two GenericData managers whose persisted-to-runtime UUID mappings are known.
Unknown BlobData managers still come from the capture; it also supplies
protocol layout and asset-catalog indices. Survival initialization has not yet
been verified with a live client. Explicit-color `PaintCharacter` updates are
relayed for active characters in the same world. Explicit character colors are
saved in the output world and restored on the next server start. CharacterSet
definitions provide per-asset reset colors, and definitions with
`paint.enable=false` are rejected. The default player UUID uses the captured
white color (`0xffffffff`). NPC targeting and full tool/ownership authorization
remain unfinished.
Steam Inventory blobs from player-info are held out of Player replication until
the GameServer Inventory API confirms the owning SteamID; verified blobs are
then saved and sent as a Player update. The asynchronous Player update has not
been checked with a live client. Native Survival players get a persistent
inventory container; block construction spends the server-calculated volume,
and supported block/joint removal refunds items transactionally. Container UI
transfers, crafting, harvestables, seating, held-tool rules, and full Survival
item behavior remain unimplemented.
The terrain mesh path is experimental and capped at 512 resident cells (at most
four new cells per simulation tick). Automatic restarts and Linux deployment
are also unfinished.

## Protocol and original-game probes

See [docs/probe.md](docs/probe.md) for build-bound capture, lifecycle, reader,
and synthetic codec-test instructions. The probe verifies the game's SHA-256
and selected instruction prologues before enabling hooks. `Stop` disables
capture and leaves its module resident safely. The injector loads a hashed
copy so the development DLL can be rebuilt independently.

The binary analyzer, IDA export helper, and captures operate on the owner's
licensed installation. Captures and imported data are local artifacts and
are excluded from version control. They are not included in a server package.

## Evidence and next work

- [research/status.md](research/status.md): achieved results and remaining gates.
- [research/world-schema.md](research/world-schema.md): empirical save layout.
- [research/protocol.md](research/protocol.md): static network paths and scope.
- [research/steam-identity.md](research/steam-identity.md): tested anonymous login.
- [docs/data-pack.md](docs/data-pack.md): importer and measured candidate pack.

Vanilla clients have reached `Start state: Game` using captured bootstrap
data, and two Steam accounts have joined the same listener and exchanged
character updates. The bootstrap still depends on a capture; several saved
world managers remain unresolved, and native movement, Survival interactions,
and physics are incomplete. See [research/status.md](research/status.md) for
the verified scope and remaining work.
