# Saved entity ID allocation — 2026-11-02

Target SHA-256 `cf403cf9d3e465e05298c8e6082f4d664c32140ebcc4696a64200e367abb530d`.
Evidence is static analysis of the owner's installed executable using ordinary
PE/Capstone tools and Hex-Rays exports from a private database copy, plus eight
read-only SQLite saves. No original database or save was modified.

## Blob structure and mapping

`Game.uniqueIds` contains BE32 count followed by that many BE32 next-ID counters.
Eight inspected saves contain count15,16 or17 and exact length `4*(count+1)`.
Per-save raw bytes, decoded words and entity maxima are retained in local
`artifacts/unique-id-evidence.json`.

The mapping is proven by upgrade function RVA `0x9984E0`:

- Pseudocode lines548/571/593/611/629 query `MAX(id)` from RigidBody, Joint,
  ChildShape, Controller, Container respectively.
- Lines792–823 write BE32 count15 followed by each table's maximum plus1 in the
  order below. The code is used while upgrading saves older than28; it proves
  the historical schema and initial-counter semantics.
- Current loader RVA `0x98FC80` reads the count, clamps to17, then reads each
  BE32 word and exchanges it into the provider's corresponding counter. Its
  explicit index mapping at data RVA `0x1460D00` is
  `[0,1,2,5,6,7,8,9,10,11,12,14,15,16,17,18,19]`.
  Therefore later counters are appended; the first counters are not reordered.

| Zero-based counter | BE32 word index (including count) | Manager enum | Entity |
| ---: | ---: | ---: | --- |
|0 |1 |0 |RigidBody |
|1 |2 |1 |Joint |
|2 |3 |2 |ChildShape |
|3 |4 |5 |Controller |
|4 |5 |6 |Container |
|5 |6 |7 |Harvestable |
|6 |7 |8 |Unresolved; preserve |
|7 |8 |9 |Tool |
|8 |9 |10 |Player |
|9 |10 |11 |Unit |
|10 |11 |12 |Unresolved; preserve |
|11 |12 |14 |Portal |
|12 |13 |15 |PathNode |
|13 |14 |16 |VoxelTerrain |
|14 |15 |17 |ScriptableObject |
|15 |16 |18 |Later field; preserve |
|16 |17 |19 |Later field; preserve |

All eight saves satisfy counter strictly greater than the corresponding
persisted maximum for the resolved populated tables. Some counters are larger
than MAX+1 because IDs can have been consumed and their entities deleted.
The saved counter must be retained; resetting it to MAX+1 can reuse retired IDs.

Provider indices 3 and 4 are not among the saved slots above. Constructor RVA
`0xB14510` initializes the 20 runtime counters to 1 (index 19 is overridden),
and Character creation calls `getNext(3)`. Thus Character's runtime counter is
not the fourth saved counter: saved slot 3 restores provider index 5. Character
spawn wire encoding and client binding remain separate work.
PlayerManager `0x7FD5F0` allocates Player IDs with `getNext(10)`, so the
persisted next Player ID is BE32 word 9 (offset 36). Native
`WorldState::reserve_player_id` advances this counter transactionally.

## Exact allocator behavior

MSVC RTTI chain:

`UniqueIdProvider` TypeDescriptor RVA `0x1A2AF10` → CompleteObjectLocator
`0x17BB5D0` (signature1,selfRVA verified,objectoffset0) → primary vtable
`0x1731A40` → slot0 function RVA `0x2CB680`.

The complete leaf function is:

```asm
002cb680: movsxd rax, edx
002cb683: mov edx, 1
002cb688: lock xadd dword ptr [rcx + rax*4 + 8], edx
002cb68e: mov eax, edx
002cb690: ret
```

It returns the old counter value and increments the counter by1. Body allocation
therefore uses word1, shape allocation word3. This is not a “last allocated ID”
format. `artifacts/unique-id-allocate.asm.txt` retains the complete disassembly.

## SQLite persistence path

SQL wrapper RVA `0xD91570` binds game tick as parameter1 and the BitStream's
exact byte length and buffer as parameter2 to
`UPDATE Game SET gametick = ?, uniqueIds = ?`.
Wrapper `0xD95760` reads the blob from `SELECT uniqueIds FROM Game` into a
BitStream without adding a separate envelope. Caller `0x994170` asserts the
unique-ID buffer is non-empty before writing a normal world update.

Native code should validate count/length and counter range, reserve the old
counter then increment, and persist changed counters in the same SQLite
transaction as inserted entities. Preserve every unmodified/unknown word.
The target allocator itself wraps at32bits; a new server should reject exhaustion
and collision rather than silently reuse IDs. That check is implementation
policy, separate from the observed target instruction sequence.

Full decompilation evidence is in `artifacts/unique-id-decompiled/`.
The generic reproducible export helper `tools/probe/ida_game_commands.py` accepts
explicit hexadecimal RVAs after its output-directory argument; use only a
private copy of the analysis database. Source-string xrefs are retained in
`artifacts/unique-id-string-xrefs.json`.
