# Save inspection evidence — 2026-11-02

Source: local licensed Scrap Mechanic installation and eight user saves (four
top-level worlds, one Survival world, and three backups). These are direct
Python `sqlite3` read-only observations, not REA Evidence records or runtime
observations. No game process was running when this investigation began.
Source SHA-256 values and per-file coverage are retained in
`save-layout-evidence.json`. Original saves were not modified or copied into
the distributable.

## Confirmed database observations

- SQLite files contain Game, RigidBody, ChildShape, Joint, Controller,
  Harvestable, VoxelTerrain, Portal, PathNode, Unit, Container, Tool,
  ShapeGroup, ScriptData, GenericData, ScriptDataUuid, ScriptableObject.
- RigidBodyBounds is `CREATE VIRTUAL TABLE ... USING rtree(id,minX,maxX,minY,maxY)`.
  Its `_node`, `_rowid`, and `_parent` tables are SQLite RTree internals;
  they are not separate game entity classes. SQLite must be compiled with
  `SQLITE_ENABLE_RTREE` to query RigidBodyBounds.
- Observed `savegameversion` values: 27, 28, 29, 30. Only the version-30
  inspected Game schema has `gameversion TEXT, buildnumber INTEGER` after
  `uniqueIds BLOB`; those optional columns must not be required.
- `11312312.db`: version 30, gameversion `1.0.7`, build 894, seed
  1064798589, tick 2475375, 25 RigidBody and 448 ChildShape rows.
- `123123123.db`: version 28, 237 RigidBody, 1880 ChildShape, 230 Joint,
  and 280 Controller rows.
- `Survival/123.db`: version 29, 9256 RigidBody, 640319 ChildShape,
  491 Joint, 11149 Controller, 100417 Harvestable, 7653 Unit, and
  31463 ScriptData rows. File length: 65,282,048 bytes.

## RigidBody partial byte map

Offsets are zero-based. Fields below were compared across **every RigidBody
row in all eight saves**; this is empirical format evidence, not serializer
decompilation. All numbers listed below use big-endian encoding.

| Offset | Size | Observation | Coverage/interpretation |
|---|---:|---|---|
| 0 | 3 | `00 01 01` on 60-byte records, `00 02 01` on 80-byte records | All observed rows; semantic header fields unknown |
| 3 | 4 | uint32 equals SQLite RigidBody.id | Exact match on every observed row |
| 7 | 2 | uint16 equals SQLite RigidBody.worldId | Exact match on every observed row |
| 9 | 4 | float equals RTree maxY | Exact match on every observed row |
| 13 | 4 | float equals RTree minY | Exact match on every observed row |
| 17 | 4 | float equals RTree maxX | Exact match on every observed row |
| 21 | 4 | float equals RTree minX | Exact match on every observed row |
| 25 | 2 | Unknown flags/field | No semantic claim |
| 27 | 16 | Four float32 values whose squared norm is within ~4e-7 of 1 | Likely quaternion; component order requires runtime/serializer proof |
| 43 | 12 | Three plausible float32 coordinates | Likely body position; not independently verified against live poses |
| 55 | remaining | Variant-dependent data | Static/dynamic interpretation and velocity mapping remain unknown |

Example body 696 in `11312312.db`: bounds minX=-944.4959716796875,
maxX=-924.2540283203125, minY=819.7540283203125,
maxY=826.9959716796875; four values at 27 are `[1,0,0,0]`;
three values at 43 are `[-944,824,0]`.

Bounds can be offset substantially from the candidate position because shapes
have local offsets. A coordinate falling outside the bounds does not alone
invalidate the candidate position. No mass, collision geometry, velocity,
or solver configuration has been recovered from this map.

## ChildShape partial byte map

All comparisons below cover every ChildShape in the eight saves. Payload
lengths vary (including 42, 43, and 48 bytes); length must not be assumed fixed.

| Offset | Size | Observation |
|---|---:|---|
| 0 | 3 | Examples start `01 1f 01`, `01 20 01`, or `01 28 01`; semantic tags not proven |
| 3 | 4 | Big-endian uint32 equals ChildShape.id in every observed row |
| 7 | 4 | Big-endian uint32 equals ChildShape.bodyId in every observed row |
| 11 | 16 | Reversing the entire 16-byte sequence gives a canonical shape UUID in two independently checked examples |
| 27 | 4 | Big-endian uint32 also equals ChildShape.bodyId in every observed row |
| 31 | remaining | Local placement, orientation, color, size, or other fields remain to be proven |

UUID cross-checks against installed asset definitions:

- Raw `10884e44c1b26e88864c43239c5981e2` reversed gives
  `e281599c-2343-4c86-886e-b2c1444e8810`, present in
  `Survival/Objects/Database/ShapeSets/blocks.shapeset` line 173.
- Raw `32ac9c232d45c5a7ed4daf9afd3f5c4c` reversed gives
  `4c5c3ffd-9aaf-4ded-a7c5-452d239cac32`, present in
  `Data/Objects/Database/ShapeSets/characterobject.shapeset` line 423.

These examples establish the UUID ordering for the checked records, not full
asset resolution for every save (some worlds contain mods).

## Reader contract

`scrapds::inspect_world_json(path, sample_limit)` opens the existing file with
`SQLITE_OPEN_READONLY`, enables query-only mode, and uses a read transaction
for a consistent SQLite view. A live WAL database still needs its original
sidecar files; use SQLite's backup API for a portable snapshot, never copy
only the main file while the game writes it.

Output contains table counts, schema SQL, column definitions, Game metadata,
actual BLOB counts and byte-length statistics, and at most 100 samples per
table (default 3). BLOB previews are limited to 64 bytes in SQL and clearly
marked truncated; large text previews are limited to 4096 characters. Ordered
primary keys give stable samples where available. Virtual tables without
declared primary keys retain SQLite's unspecified sample order.

The reader reports bounded partial RigidBody/ChildShape prefixes with explicit
confidence labels, and all other payloads as opaque. It rejects missing files,
invalid SQLite, missing required Game columns, noninteger numeric metadata,
and zero/multiple metadata rows. It accepts historical optional-column
differences. It performs no schema migration or save writes.
