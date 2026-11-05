# Client gameplay commands — 2026-11-02

Target executable SHA-256:
`cf403cf9d3e465e05298c8e6082f4d664c32140ebcc4696a64200e367abb530d`.

Evidence is ordinary shell PE/Capstone inspection and Hex-Rays output from a
private copy of the already analyzed database. The owner's original analysis
database and original saves were untouched. No process was injected or launched
by this investigation. These are not REA Evidence records.

## Dispatcher and byte order

Full dispatcher pseudocode: `artifacts/ida-init/b387c0.c.txt`, function
RVA `0xB387C0`. It reads the application ID as8 bits before switching. Client
communication resolves the authenticated sending Steam identity to a Player
before operating on that player's world. These packets are plaintext application
packets; the transport's first-byte-preserving LZ4 wrapper is a separate layer.

| Application ID | Payload read | Handler RVA | Meaning |
| --- | ---: | --- | --- |
| `22` | 872bits /109bytes | `B3FCB0` | Place/build shape |
| `23` | 264bits /33bytes | `B42B60` | Remove child shape or block region |
| `24` | 296bits /37bytes | `B44B70` | Paint child shape or block region |
| `25` | 64bits /8bytes | `B451C0` | Paint joint |
| `26` | 64bits /8bytes | `B45640` | Paint paintable character |
| `27` | 32bits /4bytes | `B45A70` | Remove joint, including inventory rules |
| `28` | 32bits /4bytes | `B465B0` | Harvestable interaction; exact action unresolved |
| `29` | 32bits /4bytes | `B46960` | Character interaction; exact action unresolved |

IDs in this table are hexadecimal. On this little-endian target the dispatcher
reverses **the complete packed object**, rather than independently swapping its
fields. Consequently vector order and field order are reversed in the packet.
Treating the109-byte BuildShape packet as a normal sequence of BE fields in
memory order is incorrect.

## Remove and paint selection

The33-byte host-memory layout used by remove and shape-paint is:

| Memory offset | Width | Field |
| --- | ---: | --- |
| 0 |4 | body ID |
| 4 |4 | child-shape ID;0 selects block region |
| 8 |1 | target kind; these two handlers require4 |
| 9 |12 | first integer region corner `(x,y,z)` |
| 21 |12 | second integer region corner `(x,y,z)` |
| 33 |4 | paint only: packed color |

Handler `B44B70` calls body lookup `75E6F0` with offset0; child lookup
`75F040` with offset4. For child ID0 it forwards both integer vectors to block
painting `B48310`. Remove uses the same region fields in block removal
`766650`; its effect position is the sum of the corners times0.125, consistent
with a midpoint in0.25-unit block coordinates. Paint corner semantics are proven below: cornerA is inclusive and cornerB is
exclusive. The parser preserves both corners without a semantic range check.

Wire order after ID `23` is cornerB.z/y/x, cornerA.z/y/x, kind, child ID, body
ID, each integer big-endian. ID `24` prepends colorBE to that same body.
All of the zero/negative/sentinel values are preserved by the codec. Policies
must validate selectors before mutating a world.

Joint and character paint have host memory `{entityIDu32, coloru32}`. Thus wire
is `{IDbyte,colorBE,entityIDBE}`. Character handler `B45640` explicitly extracts
R from the lowbyte, G from bits8–15, B16–23, A24–31 and divides by255 before
calling the color setter. Zero color asks the original shape/joint handler for
the asset's default color.

## Real BuildShape request

The existing native session captured two real construction requests in
`artifacts/steam/serve-03/1799613-rx.bin` and `1799723-rx.bin`. Each is69 wire
bytes and becomes110bytes after raw LZ4 decompression of the body. The first
plaintext packet is retained as `tests/fixtures/game-build-command.bin` with
SHA/provenance alongside it. It contains no account ID or player name.

The109-byte host-memory object after reversing the payload is:

| Offset | Width | Field/evidence |
| --- | ---: | --- |
| 0 |4 | target body ID |
| 4 |4 | target child-shape ID |
| 8 |1 | kind: observed2 for new placement; handler branches4 for body/shape and8 for joint |
| 9 |16 | shape UUID; validated by asset lookup |
| 25 |16 | inventory/item UUID, used by container transactions |
| 41 |12 | integer cell position; handler scales by0.25 |
| 53 |12 | integer x-axis vectorA |
| 65 |12 | integer z-axis vectorB |
| 77 |12 | auxiliary integer vector; semantic role unresolved |
| 89 |12 | block dimensions; product used as item count |
| 101 |4 | requested count for non-block shapes |
| 105 |4 | signed inventory slot |

The first request places UUID `a6c6ce30-dd47-4587-b475-085d55c6a3b4`, which
matches `Data/Objects/Database/ShapeSets/blocks.shapeset` line22. Its position
is `[54,63,0]`, axesA/B `[1,0,0]`/`[0,0,1]`, dimensions `[1,2,5]`, slot2,
count1, kind2 and target IDs0. The auxiliary vector also equals `[54,63,0]`
in both observed requests; that is a sample observation, not a semantic proof.

## Implemented codec

`src/net/game_commands.hpp/.cpp` parses/serializes22–27 with exact payload
lengths, safe integer decoding, whole-struct ordering, canonical UUID bytes,
and unknown-command rejection. It preserves unverified BuildShape fields under
explicit names instead of inferring values. It neither authorizes commands nor
modifies game state. `tests/game_commands_tests.cpp` passes standalone MSVC
tests: real-client BuildShape fixture, signed/boundary coordinates, hand-written
paint/remove byte oracle, every truncation, trailing bytes and unknown IDs.

ID `27` is a four-byte Joint ID in the decoded application packet. The
dispatcher reads 32 bits, reverses that packed integer into host memory, and
calls `B45A70`; therefore the wire representation is `{27, joint_id_BE32}`.
The native listener now removes supported 69-byte Joint rows transactionally
and sends NetObj operation2/type2 to connected clients. The original handler
returns the removed joint item through an inventory transaction. Since that
transaction is not implemented, the native removal path is explicitly limited
to saves whose `Game.flags` is `1` (the observed Creative mode); other modes
are rejected. This bounded path has not yet been exercised by a live client.

## Replication and remaining work

Painting does **not** simply echo the incoming client command. The shape/joint
handlers modify their object's stored color and call virtual slot+0x18 to mark
its state changed; the NetObj manager later serializes updates. Shape groups
use separate `B488D0`; block paint uses `B48310`. The exact server-to-client
NetObj update codec remains unresolved and must be implemented before claiming
that a persisted color change is visible to live clients.

Build requests enter world creation routines, inventory transactions, placement
validation, collision checks and NetObj initialization. Parsing a request is not
equivalent to implementing construction. Seat, inventory, item/tool state,
creation response and authoritative physics remain separate tasks.

Reproduction: `tools/probe/ida_game_commands.py` exports selected function RVAs
from a caller-selected private IDA database into a caller-selected directory.
`artifacts/gameplay-full/` retains pseudocode and complete function disassembly.


## New-body construction and paint bounds: focused proof

Further private-database exports are retained in `artifacts/gameplay-build-body/`,
`artifacts/gameplay-body-factory/`, `artifacts/gameplay-body-construct/`, and
`artifacts/gameplay-body-final/`.

`B47DA0` branches on the packed command kind. Kind4 resolves the existing body
using `75E6F0` and adds the new child. Kinds2,3,13 create a new body; kind6 uses
the same body creation path but also attaches it to a character. Other kinds do
not create a body through this routine. Before new-body creation, for each signed
integer cell coordinate `c`, the routine sets child position to `c % 16` using
C++ signed remainder, and sets body translation to `(c - c % 16) * 0.25`.
Negative coordinates use truncation toward zero rather than floor division.
The body basis comes from `24CB80`: its three immutable vectors at RVAs
`1776B20`, `1776C10`, `17775E0` are `[1,0,0,0]`, `[0,1,0,0]`, `[0,0,1,0]`.
Their exact float values are retained in `artifacts/gameplay-identity-constants.json`.
For the captured kind2 command at `[54,63,0]`, body translation is `[12,12,0]`
and child position is `[6,15,0]`.

The factory `75D6C0` is explicitly a **StaticBody** factory: its failure string
is `Create StaticBody on client :o` at CreationManager.cpp:772 and its resource
RTTI is `StaticBody*`. It allocates an ID through provider enum0, forwards
transform, ID and world ID to `76CA60`, whose constructor `B8A0E0` installs the
StaticBody vtable after calling the common RigidBody constructor `6D7650`.
The world ID is not part of BuildShape: `B3FCB0` obtains it from `E1A380` for the
request's ClientInstance; this resolves the player then character and returns
the uint16 at character+48, or `0xffff` on failure. Caller checks world validity
before continuing. For kind4, existing-body world/permission checks remain
necessary in a reconstructed server.

ChildShape constructor `6B0D90` stores position at object+48, command vectorB at
+60, command vectorA at+72, and explicitly zeros WORD+192 and BYTE+193.
Thus the persisted shape state byte at+193 begins at0, independently of any
particular save used as a template. Creation may subsequently merge compatible
blocks (`6DB870`) and adjacent StaticBodies (`96F5D0`), which is separate from
the basic constructor. A reconstruction that defers these merges must preserve
geometry and allocate independent IDs rather than pretending the merge occurred.

`B48310` proves the **paint** region is half-open: intersection start is
`max(cornerA, child.cell)` and intersection end is
`min(cornerB, child.cell + dimensions)`; new dimensions are `end - start`, with
no added1. It detaches the selected child, uses `6B3950` to produce untouched
fragments, changes the selected intersection dimensions/position/color, and
reattaches fragments and intersection. Therefore whole-child paint is safe when
A is at or below the child's lower cell and B is at or above cell+dimensions.
This proof is for the block paint path; arbitrary rotated/scaled parts and
other handlers must not inherit unsupported assumptions.


Build orientation is now corroborated against the recovered save serializer:
command vectorB (+65) is stored by `6B0D90` at ChildShape+60, and vectorA (+53)
at+72. The shape geometry proof identifies +60 as z-axis (packed high nibble)
and +72 as x-axis (low nibble). Thus Build `axis_a` is x and `axis_b` is z.
The public codec names are retained for compatibility; their comments now state
these proven roles. See `src/data/shape_geometry.cpp` and the geometry research
for the serializer's independent derivation.

The constructor-only flag observation is additionally supported for the basic
attach path: `6D8E30` and `6DDF90` do not change ChildShape+193. The ChildShape
vtable slot+0x18 resolves to `6B11F0` (vtable RVA `14C01C8`, reproduced in
`artifacts/gameplay-childshape-vtable.json`); it increments revision at+36 and
dirties linked objects without changing+193. Common RigidBody constructor
`6D7650` initializes revision0, flags+132=0 and supplied world/transform;
StaticBody constructor then sets its character link at+464 to-1. Detailed
pseudocode is retained in `artifacts/gameplay-shape-flags/`.


## Bounded native creative construction implementation

`WorldState::create_block(BlockCreation)` now inserts an authoritative block:

- No target body creates the proven StaticBody at native signed-remainder origin.
  Existing-body insertion requires a static body with identity rotation and zero
  body flags, in the requested world. The service accepts Build kinds2 and4
  with identity x/z axes only; other variants return an explicit reason.
- The service resolves the UUID against the loaded asset catalog, requires an
  actual block and a default color, then creates a48-byte ChildShape save BLOB
  with independently reserved ID, reversed UUID, position, dimensions and flags0.
- The state operation reads validated BE32 `Game.uniqueIds`, reserves old body
  and shape counters, increments only their words and preserves every other word.
  It rejects exhaustion or counters below/equal to existing table high water.
- Body, shape, affected RTree bounds and counters are changed in one SQLite
  transaction. Cache nodes are allocated before the transaction commits. Failed
  inserts roll back IDs and geometry together; only the private in-memory copy
  is written. Snapshot/reload retains the actual records and counter bytes.
- Exact half-open box collision tests reject overlap with known static blocks.
  Potential collision with unsupported part/dynamic/rotated geometry returns an
  explicit refusal. This is intentionally bounded construction: terrain contacts,
  arbitrary shape collision, physics, inventory cost, survival authorization and
  automatic body/block merging are not asserted to be implemented.

`apply_game_command(..., world_id=0)` returns created body/shape IDs separately
from changed shape IDs so the transport can create real replicated objects.
World0 is the current creative prototype context; callers must supply the actual
world for other player contexts. Paint region handling now uses the proven
inclusive lower/exclusive upper corners and rejects empty/inverted regions.

Meaningful `gameplay_tests`/`state_tests` pass under MSVC: the captured110-byte
BuildShape produces body100 at[12,12,0], shape200 at[6,15,0] with[1,2,5] dimensions;
existing-body insertion, default wood color/UUID, shape/world rejection, exact
counter byte order and untouched words, bounds/RTree, half-open paint, real save
reload, rejected overlap, volume/ID exhaustion and an injected SQL INSERT failure
with full rollback are covered. A negative-coordinate test proves signed native
remainder rather than floor division. Source fixture bytes remain unchanged.


StaticBody tail confirmation from geometry exports: serializer `B8BA30` invokes
base `6DC240` then writes object+464 as BE32. Base's final byte is raw body+132,
so saved byte55=0 and bytes56..59=`ffffffff` follow the proven constructors.
Reader `B8BA70` / attach routine `B8A550` resolves +464 in the character registry
and calls `B30CC0`; it is an attached-character reference, not a proven group ID.


Correction caught by nonzero-world validation: saved StaticBody uint16@25 is the
repeated world ID from object+52, not a flag field. The writer now emits world ID
at both@7 (save envelope) and@25 (virtual payload); true raw body flags are@55.
A world1 test covers both copies. Existing-body build checks true@55 flags.
