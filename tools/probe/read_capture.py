#!/usr/bin/env python3
"""Inspect SDSCAP01 capture files without executing or decoding game payloads."""
from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import struct

FILE_HEADER = struct.Struct("<8sIIQQ64s")
RECORD_HEADER = struct.Struct("<IIQQQIIQQiIII")
EXPECTED_SHA256 = "cf403cf9d3e465e05298c8e6082f4d664c32140ebcc4696a64200e367abb530d"
KINDS = {1: "packet-compress", 2: "packet-decompress", 3: "server-raw-decompress-without-message-id", 4: "physics-step"}
MAX_PAYLOAD = 64 * 1024 * 1024


def inspect(path: Path, prefix_size: int = 32) -> dict:
    if not 0 <= prefix_size <= 4096:
        raise ValueError("prefix size must be between 0 and 4096")
    with path.open("rb") as stream:
        header = stream.read(FILE_HEADER.size)
        if len(header) != FILE_HEADER.size:
            raise ValueError("truncated file header")
        magic, version, header_size, frequency, process_id, sha = FILE_HEADER.unpack(header)
        if magic != b"SDSCAP01" or version != 1 or header_size != FILE_HEADER.size:
            raise ValueError("unsupported capture header")
        if not frequency:
            raise ValueError("invalid QPC frequency")
        try:
            source_sha256 = sha.decode("ascii")
            if len(source_sha256) != 64 or any(c not in "0123456789abcdef" for c in source_sha256):
                raise ValueError("invalid source SHA-256")
        except UnicodeDecodeError as error:
            raise ValueError("invalid source SHA-256") from error
        records = []
        counts = {name: 0 for name in KINDS.values()}
        last_sequence = None
        while True:
            offset = stream.tell()
            header = stream.read(RECORD_HEADER.size)
            if not header:
                break
            if len(header) != RECORD_HEADER.size:
                raise ValueError(f"truncated record header at offset {offset}")
            magic, size, qpc, sequence, caller, kind, thread, source_size, capacity, result, input_size, output_size, flags = RECORD_HEADER.unpack(header)
            if magic != 0x31434552 or size != RECORD_HEADER.size or kind not in KINDS or flags & ~3:
                raise ValueError(f"unsupported record at offset {offset}")
            if input_size > MAX_PAYLOAD or output_size > MAX_PAYLOAD or input_size > source_size or output_size > capacity:
                raise ValueError(f"invalid payload sizes at offset {offset}")
            if result <= 0 and output_size or result > 0 and output_size not in (0, result):
                raise ValueError(f"inconsistent codec result at offset {offset}")
            if last_sequence is not None and sequence <= last_sequence:
                raise ValueError(f"non-increasing sequence at offset {offset}")
            last_sequence = sequence
            payloads = {}
            first_bytes = {}
            step_prefix = bytearray()
            for direction, length in (("input", input_size), ("output", output_size)):
                digest = hashlib.sha256()
                prefix = bytearray()
                remaining = length
                while remaining:
                    data = stream.read(min(remaining, 65536))
                    if not data:
                        raise ValueError(f"truncated {direction} payload at offset {offset}")
                    digest.update(data)
                    if direction == "input" and kind == 4 and len(step_prefix) < 20:
                        step_prefix.extend(data[:20 - len(step_prefix)])
                    if direction not in first_bytes:
                        first_bytes[direction] = data[0]
                    if len(prefix) < prefix_size:
                        prefix.extend(data[:prefix_size - len(prefix)])
                    remaining -= len(data)
                payloads[direction] = {"size": length, "sha256": digest.hexdigest(), "prefix_hex": prefix.hex()}
            message_id = first_bytes.get("input" if kind == 1 else "output") if kind in (1, 2) else None
            record = {"file_offset": offset, "sequence": sequence, "qpc": qpc,
                            "seconds": qpc / frequency, "caller_rva": hex(caller), "kind": KINDS[kind],
                            "thread_id": thread, "source_size": source_size, "destination_capacity": capacity,
                            "result": result, "flags": flags, "message_id": message_id, **payloads}
            if kind == 4:
                if source_size != 20 or capacity != 0 or output_size != 0 or (input_size != 20 and not flags & 1):
                    raise ValueError(f"invalid physics record at offset {offset}")
                if input_size == 20:
                    world, time_step, maximum, fixed_time_step = struct.unpack("<Qfif", step_prefix)
                    record["physics"] = {"world_address": hex(world), "time_step": time_step,
                                         "max_substeps": maximum, "fixed_time_step": fixed_time_step,
                                         "returned_substep_count": result}
            records.append(record)
            counts[KINDS[kind]] += 1
    return {"format": "SDSCAP01", "version": version, "process_id": process_id,
            "source_sha256": source_sha256, "expected_build": source_sha256 == EXPECTED_SHA256,
            "qpc_frequency": frequency, "record_count": len(records), "counts": counts,
            "records": records, "limitations": ["Payload schemas and handshake are not decoded.",
            "Kind 3 contains the compressed body only; the uncompressed message ID byte is absent.",
            "QPC seconds are monotonic host-clock values, not wall-clock timestamps.",
            "Hooks cover the listed codec paths only; one-byte packets and bypass paths may be absent."]}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("capture", type=Path)
    parser.add_argument("--output", type=Path)
    parser.add_argument("--prefix-bytes", type=int, default=32)
    args = parser.parse_args()
    try:
        report = inspect(args.capture, args.prefix_bytes)
    except (OSError, ValueError) as error:
        parser.exit(1, f"capture error: {error}\n")
    encoded = json.dumps(report, indent=2) + "\n"
    if args.output:
        args.output.write_text(encoded, encoding="utf-8")
        print(json.dumps({"output": str(args.output.resolve()), "record_count": report["record_count"]}))
    else:
        print(encoded, end="")


if __name__ == "__main__":
    main()
