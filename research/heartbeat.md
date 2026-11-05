# Empty post-initial NetObj clock frame — static proof

A plaintext5-byte packet `[0x16, BE32tick]` is accepted by client dispatcher
`450FC0` after initial ID21. Existing `compress_packet` adds the ordinary LZ4
body envelope; raw five bytes must not bypass compression. `711C10` tests native
endianness and is false on this little-endian target, so the tick is BE32.

The tick preamble requires a nondecreasing server tick. At RVA4518A9 it writes
NetworkClient+0x120 (`m_uLastProcessedServerTick`) and at4518D2 assigns stalltype1
for ID22, then45195D calls NetObjReceiver `B55930`. If a newer tick follows an
existing type1 phase, the normal dispatcher queues the packet first; processing
is therefore conditional on the normal receive/tick pump rather than immediate
for every transport arrival. Never regress the tick or roll uint32 silently.

`B55930` rejects updates before its initialized flag (set by ID21). With the
read offset at40bits and exactly40bits used, its first loop's `read+8 > used`
condition goes directly to the final `read == used` check and exits cleanly.
No record or minimum update count is required. This establishes a valid empty
NetObj update, not independent proof that it resolves any loading stall.

Captured original post-initial aggregates corroborate ID22 and BE32tick framing:
`artifacts/world-sync/id29-67177.bin` has ID22 at offset5,380bytes,tick558525,
followed by ID25 and27 at the same tick. `id29-67185.bin` has ID22 at offset5,
97bytes,tick558528, then ID25. These original examples contain records; the
empty-frame acceptance comes from complete receiver control flow, not a claim
that an empty original frame was captured. Private Hex-Rays/assembly exports are
retained in `artifacts/gameplay-heartbeat/`.
