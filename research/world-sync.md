# Native initial-world framing and entity payloads — 2026-11-02

These observations use the fixed game executable SHA-256
`cf403cf9d3e465e05298c8e6082f4d664c32140ebcc4696a64200e367abb530d`,
an owned copy of its IDA database, a previously captured real initialization,
and the owner's SQLite save. This investigation did not run/inject a game or
write an original save. Results are direct local analysis, not REA Evidence.

## Aggregate framing

ID29 is an aggregate, not one opaque world blob:

```
u8 message_id = 29
repeat until end:
    u32 big-endian child_size
    u8 child_packet[child_size]
```

NetworkClient RVA `0x450FC0` handles id29 before its normal dispatcher,
reads each size, recursively dispatches the child, and checks exact consumed
bytes and nonnegative remaining bytes. Saved source/assembly:
`artifacts/ida-init/450fc0.c.txt` and `.asm.txt`.

The 86,905-byte real aggregate has SHA-256
`d6e12d2c8b75d5b7348ca52ea647d30e4eb746eb45b06d60d0a9fd3b4f46fbe2`.

| Child offset | Bytes | ID | Decoded role |
| --- | ---: | ---: | --- |
| 5 | 72,928 | 21 | Initial NetObj records; tick 558439 |
| 72937 | 13,968 | 25 | Script BlobData; 27 framed entries |

## NetObj framing and previous-record byte deltas

IDs21/22 contain a big-endian u32 tick followed by byte-aligned records.
NetObjReceiver RVA `0xB55930` and writer NetObjUser `0xE1ADB0` agree on:

```
u16 big-endian size_including_own_two_bytes  // <= 0x7fff
u8 operation_and_type  // operation = byte >> 5; type = byte & 31
if operation == 1:
    u8 subtype
u32 big-endian object_id
u8 entity_specific_payload[...]
```

Operations are 1=create, 2=remove, 3=state, 5=reliable update. Type names come
from the original receiver's pointer table RVA `0x1479510`:

| Type | Class | Type | Class |
| ---: | --- | ---: | --- |
| 0 | RigidBody | 8 | Tool |
| 1 | ChildShape | 9 | Portal |
| 2 | Joint | 10 | PathNode |
| 3 | Controller | 11 | Unit |
| 4 | Container | 12 | VoxelTerrainCell |
| 5 | Harvestable | 13 | ScriptableObject |
| 6 | Character | 14 | MultiShape |
| 7 | Lift | | |

If the next stream byte has bit7 set, it begins a byte-delta record rather
than a u16 length. A delta requires previous decoded record data <=63 bytes.
Its mask length is `(previous_size + 8) / 8`. Bit7 in the first byte is the
marker. Data index `i` is copied from the previous record if bit `i%8` in
mask byte `mask_size-1-i/8` is set; otherwise consume a replacement stream
byte. Decoded size stays equal to the previous size. An ordinary data block
larger than 63 bytes resets the delta base. The writer uses a delta only
when its mask and replacement bytes are smaller than an ordinary frame.

All **5,636 records**, including **4,809 deltas**, parse to the exact end of
the 72,928-byte real initial packet. Reader/writer exports are retained in
`artifacts/world-protocol-decompiled/`. The native test's optional real
packet mode compares re-encoded bytes with the original packet exactly.

## Entity identity parity

The captured entity population matches `Save/123123123.db`. Every create
ID and type/subtype pair equals the corresponding SQLite BLOB header:

| Type | SQLite table | Create/state counts | IDs/subtypes matched |
| ---: | --- | ---: | ---: |
| 0 | RigidBody | 237 / 237 | 237 / 237 |
| 1 | ChildShape | 1880 / 1880 | 1880 / 1880 |
| 2 | Joint | 230 / 230 | 230 / 230 |
| 3 | Controller | 280 / 280 | 280 / 280 |

Reproduce with `tools/probe/inspect_world_sync.py --save`. Bounded JSON is
in `artifacts/world-sync/structure.json`; decoded per-record payloads and
offsets are in `artifacts/world-sync/decoded-netobj/`.

## Proven save-to-network payload transformations

Offsets are zero-based. BLOBs begin with type, subtype, schema1, then object
ID at offset3 in big-endian order.

- RigidBody create subtype1: world bytes `[7:9]` followed by quaternion and
  position `[27:55]`, total30 bytes. For subtype2, append velocity vectors
  `[55:79]`, total54 bytes. Static body values match capture exactly;
  dynamic poses changed under the original simulation. The body repeats
  worldId at offset25; it is not an unknown flags field.
- Static-body state: BLOB `[55:60]`, exact for all15 static bodies. The
  dynamic-body state uses runtime data not yet recovered from persistence.
- ChildShape creation has no payload. State is a big-endian u16 asset
  catalog index plus BLOB `[27:]`, exact for all **1880** shape states.
- Joint creation has no payload. State is a big-endian u16 asset index plus
  BLOB `[31:]`, exact for all **230** joint states.
- UUIDs in the saved BLOB are replaced by these u16 indices. All **96 shape
  UUIDs** and **7 joint UUIDs** map consistently in this capture. The mapping
  is asset-catalog metadata, not an invented UUID hash.
- Controller formats vary. A direct BLOB `[17:]` copy works for some
  variants but not others. They remain explicitly preserved bootstrap data.

`make_body_creation`, `make_child_shape_state`, and `make_joint_state` implement
these supported transformations with schema/subtype/length checks.

## BlobData framing

Packet IDs11/13/25/27 contain a big-endian u32 tick then repeated entries:

```
u8 uid[16]
u16 big-endian key_length
u8 key[key_length]
u16 big-endian world_id
u8 flags
u32 big-endian payload_length
u8 payload[payload_length]
```

BlobDataManager handler RVA `0x7A8AF0` supplies the UUID/key, world, flags,
size and payload reads. Real init11 contains1 entry with980 payload bytes;
init13 contains3 entries with55/74/231 bytes; id25 in the initial aggregate
contains27 entries and consumes all13,968 bytes. Payloads have additional
codecs/serialization that this framing layer does not interpret.

SQLite ScriptData/GenericData `data` contains the same entry structure. Its
runtime UUIDs/flags can differ from persisted values, so table rows are not
transferred wholesale. The listener now refreshes only mappings supported by
the local capture: `terrainData` UUID
`61aa13d7e7154153a2694d338c0c5bd4` maps to itself; GenericData runtime UUID
`3ff36c8b93f74428ae4d429a6f0cf77d` maps to saved UUID
`5297769df4514e5e9a388b0f95e2edad`; and
`44ac020caec74f8bb23034d2e3bd23eb` maps to itself. The generic mapping is
based on the captured init13 entry and the exact key/world/payload match in the
oracle save. Runtime UUIDs and flags come from the capture, while saved keys,
world IDs and payloads come from the supplied save. Unknown managers remain
from the capture. This limited rebuild compiles but has not been validated by a
live client joining a Survival save. Native parse/build functions retain exact
raw payload bytes.

ID25 also has two locally mapped ScriptData runtime groups:
`ea4a3501c8d55d13b074498f392d5d7e` and
`c5b804f568305ff7947367630e1c5984` both map to saved UUID
`5130c47b0a1953fca5a1205c19264574`. Exact captured keys take priority. For
world-specific object IDs that differ from the capture, the builder reads the
root keys from the bounded, LZ4-decoded Lua table: `responseSlider` selects the
first group and known controller fields (`gearIdx`, `fuelPoints`, `batteryPoints`,
and related fields) select the second. Refresh activates only after three
distinct saved keys route to a group, and ambiguous/unknown schemas stay from
the bootstrap. The mapping remains capture-specific and needs a live-client
Survival check.

## Hybrid native bootstrap

`rebuild_initial_world(captured, WorldState, tick)` replaces body creations,
shape states, joint states and static-body states from the current in-memory
save. It rejects different body/shape/joint identity sets. Unsupported scripts,
containers, tools, terrain, controllers and dynamic-body runtime states stay
explicitly preserved. This is not a complete standalone world serializer.

For a mutable world, infer the asset catalog once from the unchanged source
using `infer_initial_asset_catalog`, then pass it to the four-argument builder.
This prevents a changed UUID from silently inheriting the old object's index;
an unseen UUID is rejected until its client asset index is established.

`rebuild_live_initial_world` additionally supports new native StaticBody/block
records while retaining original IDs. It inserts body creates, shape creates,
shape states and body states in their dependency order. New dynamic bodies,
new joints and unmapped block assets are rejected; no runtime state is guessed.
Captured Container create/state records are retained only when the selected
SQLite save has the same Container row ID. Their payloads are still copied from
the capture. For the observed schema-1 save BLOB, the loader recognizes the
BE16 entry count at offset7 and the following 22-byte records. Save samples
cross-check bytes2..17 as the serialized item UUID and bytes18..21 as BE32
quantity; `0xffffffff` is the observed empty-slot marker. The first two bytes
and variable suffix remain opaque. `WorldState` can query a matching slot and
spend a quantity transactionally without changing any opaque bytes. Player
snapshots now map to persistent per-player containers; new players receive a
30-slot container, and type-4 create/state records encode it for clients.
Survival BuildShape validates the player's slot and UUID, checks block volume
against command quantity, and creates the block while spending the stack in
one save transaction. Changed slots are broadcast to same-world peers. This
code compiles but still needs live-client validation; crafting and transfers
remain unsupported.

## Reproduction and validation

```powershell
python tools/probe/inspect_world_sync.py artifacts/world-sync/replay/world29.bin --samples 1 --save 'C:/path/123123123.db'
build/world_sync_tests.exe artifacts/world-sync/replay/world29.bin 'C:/path/123123123.db'
```

Unit cases cover malformed lengths, truncated deltas, invalid operations,
delta-base resets, BlobData field bounds, native fixture-world paint, catalog
conflicts, strict identity mismatches, live added bodies/blocks, dependency
ordering and rejection of unsupported new dynamic bodies/unmapped assets.
