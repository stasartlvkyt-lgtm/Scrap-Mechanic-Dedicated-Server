# Saved geometry evidence — 2026-11-02

Target SHA-256: `cf403cf9d3e465e05298c8e6082f4d664c32140ebcc4696a64200e367abb530d`.
Evidence below comes from ordinary IDA/PE inspection and eight read-only SQLite
saves. It is not REA Evidence. Exports used only the private database
`artifacts/ida-work/ScrapMechanic.exe.i64`; the owner's original DB is untouched.

## ChildShape

Writer RVA6B55F0 (`artifacts/ida-shapes/6b55f0.c`, ChildShape.cpp628–670)
confirms the following complete observed saved record layouts:

| Offset | Field |
|---|---|
| 0–2 | NetObj type1, subtype31/32/40, schema1 |
| 3–6 | BE shape ID |
| 7–10 | BE parent body ID in DB envelope |
| 11–26 | UUID with the entire 128-bit scalar byte-reversed |
| 27–30 | Repeated BE parent body ID in virtual payload |
| 31–36 | Signed BEint16 local construction-grid X,Y,Z |
| 37–40 | Packed BEuint32 color, numeric0xAABBGGRR |
| 41–46 | Signed BEint16 dimensions X,Y,Z, block31/scaled-part40 |
| 41 | Packed orientation for ordinary part32 |
| 47 | Packed orientation for scaled-part40 |
| Last byte | Raw ChildShape+193 state; omitted by legacy42-byte part32 |

The final block byte47 is **not orientation**. Blocks are canonicalized by
6B3780: origin/dimensions absorb rotation, then axes are reset to X=(1,0,0),
Z=(0,0,1). Ordinary part collision meshes and dimensions come from assets,
not this BLOB. Scaled-part dimensions are present but do not prove box geometry.

Helper2DBD00 (`artifacts/ida-shape-followup/2dbd00.c`) maps signed axes
X=±1,Y=±2,Z=±3. The packed byte is
`((zAxisCode+4)<<4)|(xAxisCode+4)`. The decoder requires perpendicular axes.
All24 orientations occur in observed part rows.

Color order has an independent asset cross-check: shape428's
UUIDe281599c-2343-4c86-886e-b2c1444e8810 has saved colorFFDCD7C9. The matching
Survival blocks.shapeset entry `blk_concrete3` defines color`c9d7dc`: low byte
redC9, then greenD7, blueDC, alphaFF. This agrees with the paint protocol.

The decoder passed all650,059 shapes across eight saves. Four Survival blocks
have dimensions(1,0,1); they are preserved as degenerate geometry and skipped
with explicit coverage counts by the collision scene. No part mesh is invented.

## RigidBody poses

RTTI resolves StaticBody vtable1736540 and DynamicBody vtable17363C0. Writers
B8B8B0/B87CC0 are exported under `artifacts/ida-shape-body`; helper24D6A0
under `artifacts/ida-body-pose` produces native quaternionX,Y,Z,W.

| Field | Static subtype1,60 bytes | Dynamic subtype2,80 bytes |
|---|---|---|
| World ID | BEuint16@7, repeated@25 | Same |
| Bounds | BEfloat maxY,minY,maxX,minX@9 | Same |
| Quaternion@27 | BEfloat **W,Z,Y,X** | BEfloat **X,Y,Z,W** |
| Position@43 | BEfloatX,Y,Z | Same |
| Tail | state byte55, attachment referenceBEuint32@56 | linear velocityBEfloat3@55, angular velocityBEfloat3@67, state byte79 |

StaticBody writes quaternion using128-bit helper2DB100, whose endian conversion
reverses all16 bytes. Dynamic helper60FA50 writes four separate32-bit floats.
The decoder normalizes representation to`rotation_xyzw` without changing values.
Applying one stored component order to both body types is incorrect.

The field@25 is **repeated world ID**, not flags: all11,639 bodies match SQL
worldId, including Survival worlds1–13. It must be populated for nonzero worlds.

Static tail writerB8BA30 calls6DC240 for body+132 state byte, then writes+464.
ReaderB8BA70 resolves+464 through Character registry1A69638; it is an attachment
reference, not a proven creation-group ID. Constructor default-1 and zero state
support new unattached bodies. Other state-byte meanings remain unclaimed.

Velocity order is independently confirmed by the Lua getter table at145ADF8:
`velocity`→A64460 reads BodyPhysicsProxy+330/+334/+338;
`angularVelocity`→A64620 reads+340/+344/+348. Dynamic writerB87D31/B87D38
passes those vectors in that order to60FA50. Saved offsets55/67 are therefore
linear/angular XYZ respectively. The C++ compatibility fields dynamic_vector0/1
also expose named accessors linear_velocity()/angular_velocity().

## Block collision queries

`src/physics/world_collision.*` builds one separate Bullet collision world per
game world ID, preserving shape/body/world IDs. Supported blocks use
`halfExtents=dimensions*0.125` and
`localCenter=(gridPosition+dimensions/2)*0.25`, transformed by the body pose.

Tests cover transformed/rotated ray hits, world isolation and malformed inputs.
In actual11312312.db, all448 boxes loaded and all nine static all-block AABBs
matched SQLite RTree bounds within0.011 units (typical inset≈0.004/face).
For123123123.db, six joint-free static all-block bodies match tightly. Body162
has eight attached joints and saved bounds extend≈0.5 beyond its block union;
joint-bearing tests use containment and explicitly report unsupported joints.

This module provides a collision snapshot, not game physics parity. Terrain,
part meshes, joints, dynamics, mass/inertia and character controllers are absent.
Rebuild the snapshot after changing WorldState.

## Reproduce

- `tools/probe/ida_shape_export.py`, `ida_shape_followup.py`,
  `ida_shape_body.py`, `ida_body_pose.py`: private DB export scripts.
- `shape_geometry_tests <save-or-directory>`: every supported saved row.
- `world_collision_tests <Creative-save>`: geometry queries and stored bounds.

No script patches, injects or executes the game. Export files and licensed game
data stay local and are excluded from distribution.
