# Passive packet probe

`scrapds_probe.dll` is a Windows x64 research instrument for the executable with
SHA-256 `cf403cf9d3e465e05298c8e6082f4d664c32140ebcc4696a64200e367abb530d`.
It captures calls without changing input bytes, output bytes, arguments, or the
original return value. It is not a server or a handshake implementation.

The DLL verifies the executable file hash and the four mapped executable
prologues before creating any hooks. Both checks must pass. ASLR is supported:
the documented RVAs are added to the actual module base.

## Lifecycle ABI

Both exported functions use the Windows x64 ABI and return a `DWORD` Win32 error
code. `0` means success.

```cpp
DWORD WINAPI ScrapdsProbeStart(void* nullable_utf16_log_path);
DWORD WINAPI ScrapdsProbeStop(void* ignored);
DWORD WINAPI ScrapdsProbeCodecTest(void* utf16_new_fixture_path);
```

Start accepts a NUL-terminated UTF-16 path located inside the target process, or
NULL. NULL selects `SCRAPDS_PROBE_LOG`, falling back to
`%TEMP%/scrapds-probe-PID.cap`. Parent directories must already exist. Start does
not infer the path from the injector's environment. The optional path argument
allows an injector to configure an already-running process.

`SCRAPDS_PROBE_MAX_MB` limits total file size; default 256 MiB, valid range
1..65536 MiB. File and record headers count toward the limit. At the limit the
probe stops recording and continues forwarding game calls. Each input/output
snapshot is bounded to 64 MiB; unavailable or oversized snapshots are explicitly
flagged. Output is captured only for successful positive return values within
the destination capacity.

Stop disables the four owned hooks, flushes and closes the log. Start may be
called again after Stop. An existing log must have the same format, executable
hash, QPC frequency and process ID; otherwise Start rejects it. Sequence numbers
continue within the loaded module and are recovered from validated complete
records on append. A partial or malformed tail is rejected. A generation counter
drops in-flight records from an earlier Stop/Start cycle instead of writing them
to the new capture.

The DLL pins itself on the first hook installation. **Do not unload it.** Stop
leaves executable trampolines allocated because another game thread may still
be returning through a hook or trampoline. No hooks, file operations, or hashing
run inside DllMain. Normal process exit releases the pinned module and resources.
Creating hooks requires thread suspension by MinHook and can perturb timing.
Snapshot and log work also add latency; use captures for protocol investigation,
not for performance measurement or final physics timing.

`ScrapdsProbeCodecTest` is a separate synthetic codec self-test, not a network
operation. It checks executable/prologue identity, constructs 4097 deterministic
bytes (message byte `0x7f` followed by a fixed pattern), compresses and decompresses
with the original game wrappers, and verifies byte equality. It writes a new-only
fixture with magic `SDSCOD01`, little-endian uint32 source/compressed lengths,
then source bytes and compressed bytes. Existing paths are rejected. It calls
original trampolines directly and does not add synthetic records to the real
packet capture. A successful result proves a codec round-trip only; it does not
prove a real client join or a compatible server handshake.

## Captured paths

| Kind | Hook RVA | Contents |
| --- | --- | --- |
| 1 | `0xB59510` | Full plaintext packet input and compressed output, retaining message ID byte 0 |
| 2 | `0x4658D0` | Full compressed packet input and plaintext output, retaining message ID byte 0 |
| 3 | `0x4EEA30`, only caller return RVA `0xB4DDE5` | Inline server receive decompression; compressed/plaintext body **excludes message ID** |
| 4 | `0x1118410` | Physics step arguments and original returned substep count |

Kind 1/2 signatures reconstructed from register usage:

```cpp
int compress(const uint8_t* source, uint32_t input_size,
             uint8_t* destination, uint32_t destination_capacity);
int decompress(const uint8_t* source, uintptr_t input_size,
               uint8_t* destination, uint32_t destination_capacity);
```

Kind 3 has raw codec ordering `int(source, destination, int size, int capacity)`.
Only the exact server callsite is recorded to avoid misclassifying persistence
or asset decompression as network traffic. Nested wrapper/raw calls are recorded
once. The probe never reads `source[-1]` to guess an absent message ID.

Kind 4 hooks the RTTI-resolved `TickDynamicsWorld` step entry. Start checks both
its prologue and slot 13 of the mapped vtable at RVA `0x1605900`. Its ABI is
`int(world*, float timeStep, int maxSubSteps, float fixedTimeStep)`. The record's
input is exactly 20 packed bytes (`<Qfif`): world address, timeStep, maxSubSteps,
fixedTimeStep. The header result is the original returned substep count;
destination capacity and output length are zero. No world memory is read or
modified. In particular, this does not record gravity, solver configuration,
broadphase, body poses or collision events. Calls emitted during a physics step
can still produce independent codec records.

These paths do not establish complete packet coverage: one-byte packet paths,
inline compression elsewhere, transport control messages, and other receive
paths can bypass these hooks. Capturing a client join on both client and host is
still required. Steam transport framing, identity, authentication and ordering
remain separate work.

## Log format

All integers use little-endian byte order; structures are packed with no padding.

File header (`96` bytes, Python struct `<8sIIQQ64s`):

| Field | Type |
| --- | --- |
| Magic `SDSCAP01` | 8 ASCII bytes |
| Version `1`, header size `96` | 2 × uint32 |
| QPC frequency, process ID | 2 × uint64 |
| Executable SHA-256, lowercase hexadecimal | 64 ASCII bytes |

Record header (`72` bytes, Python struct `<IIQQQIIQQiIII`):

| Field | Type |
| --- | --- |
| Magic `0x31434552`, header size `72` | 2 × uint32 |
| QPC timestamp, sequence number, caller RVA | 3 × uint64 |
| Kind, thread ID | 2 × uint32 |
| Original source size, destination capacity | 2 × uint64 |
| Original codec result | int32 |
| Captured input bytes, captured output bytes, flags | 3 × uint32 |

Exactly `input bytes` then `output bytes` follow each header. Flags bit 0 means
input snapshot unavailable; bit 1 means output snapshot unavailable. A failed
codec call retains its original nonpositive result and has no output snapshot.
Timestamps are QPC values taken on entry, while records are written on return;
sequence numbers describe log order. Concurrent threads can finish out of entry
order. Do not interpret QPC values as wall-clock time.

Writes are serialized. A failed record write attempts to truncate back to the
last complete record, then disables recording. An abrupt process crash can still
leave a partial tail; the reader rejects truncation explicitly.

Inspect a capture:

```powershell
python tools/probe/read_capture.py path/to/session.cap --output session.json
```

The report contains payload sizes, SHA-256, a bounded byte prefix, callsites and
message IDs where present. It does not claim to decode handshake or NetObj
schemas. Captures may contain player/session information; keep local artifacts
out of source control.

## Verification status

The static addresses and prologues come from `research/binary-report.json` and
`research/network-disassembly.txt`. Parser tests cover directions, missing raw
message IDs, truncated files, malicious lengths and sequence order. The DLL
should also be loaded in an unrelated test process: Start must return
`ERROR_BAD_EXE_FORMAT` (`193`) without creating a log. A successful game capture
must be recorded separately; a compiled DLL or rejected foreign process does
not demonstrate successful live interception.
