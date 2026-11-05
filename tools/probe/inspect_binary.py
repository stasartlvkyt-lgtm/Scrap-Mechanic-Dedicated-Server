#!/usr/bin/env python3
"""Read-only, build-bound PE observations. This is not a wire protocol decoder."""
from __future__ import annotations

import argparse
import bisect
import hashlib
import json
from pathlib import Path
import re
import struct

import capstone
import pefile

EXPECTED_SHA256 = "cf403cf9d3e465e05298c8e6082f4d664c32140ebcc4696a64200e367abb530d"
FLAGS = ["-dedicated_server", "-use_null_driver", "-connect_steam_id", "-open", "-console", "-dev", "-builtin_mods_only"]
NETWORK_STRINGS = [
    "SteamNetworkingSockets012", "Failed to decompress network packet! Error code: ",
    "Failed to decompress network packet from ", "Failed to compress reliable packet! size: ",
    "Failed to compress unreliable packet! size: ", "Failed to send reliable network packet! Result: ",
    "Failed to send unreliable network packet! size: ", "Failed to send reliable split network packet! chunk: ",
    "Processing initial packet from server tick: ", "uDebugSize == uUncompressedSize",
    "iDecompressedSize == (int)uUncompressedSize", "Network channel ",
    "Contraption::SteamCommandLine ", "Contraption::start ", "Failed to parse -connect_steam_id <id>",
]
PHYSICS = ["btDiscreteDynamicsWorld", "btDbvtBroadphase2", "BulletLBVH", "btSlantShape", "CustomCollisionConfiguration", "VoxelTerrainPhysicsProxy", "BodyPhysicsProxy", "JointPhysicsProxy", "CharacterPhysicsProxy", "PhysicsMaterialManager"]
KNOWN_FUNCTIONS = {
    0xB59510: "packet_compress_wrapper", 0x4658D0: "packet_decompress_wrapper",
    0x4EE990: "raw_compress_candidate", 0x4EEA30: "raw_decompress_candidate",
    0xB597F0: "reliable_send_candidate", 0xB5A150: "unreliable_send_candidate",
    0xB5A480: "split_send_candidate", 0xB4D240: "server_receive_candidate",
    0x466A00: "client_receive_candidate",
    0x1118410: "bullet_step_simulation_candidate",
    0x2F1C90: "steam_connect_string_parser", 0x2E3200: "contraption_init_launch_parser_caller",
}


def inspect(path: Path) -> dict:
    data = path.read_bytes()
    pe = pefile.PE(data=data)
    base = pe.OPTIONAL_HEADER.ImageBase
    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
    md.detail = True
    functions = sorted((int(e.struct.BeginAddress), int(e.struct.EndAddress))
                       for e in getattr(pe, "DIRECTORY_ENTRY_EXCEPTION", []))
    starts = [x[0] for x in functions]
    entries = {int(e.struct.BeginAddress): e for e in getattr(pe, "DIRECTORY_ENTRY_EXCEPTION", [])}

    def unwind_root(start):
        visited = set()
        while start in entries and start not in visited:
            visited.add(start)
            unwind = entries[start].struct.UnwindData
            info = pe.get_data(unwind, 4)
            if len(info) != 4 or not ((info[0] >> 3) & 4):
                break
            chain = pe.get_data(unwind + 4 + ((info[2] + 1) & ~1) * 2, 12)
            if len(chain) != 12:
                break
            start = struct.unpack_from("<I", chain)[0]
        return start

    spans = {}
    for start, end in functions:
        root = unwind_root(start)
        spans[root] = max(spans.get(root, end), end)

    def containing(rva):
        i = bisect.bisect_right(starts, rva) - 1
        return functions[i] if i >= 0 and rva < functions[i][1] else None

    def locations(needle):
        raw = needle.encode("ascii")
        return [pe.get_rva_from_offset(m.start()) for m in re.finditer(re.escape(raw) + rb"(?=[\x00 ])", data)]

    targets = {s: locations(s) for s in FLAGS + NETWORK_STRINGS}
    by_target = {rva: text for text, rvas in targets.items() for rva in rvas}
    references = {text: [] for text in targets}
    # Scan candidate REX.W LEA/MOV RIP-relative encodings, then validate each
    # candidate with Capstone. This does not claim instruction-boundary coverage.
    for section in pe.sections:
        if not section.Characteristics & 0x20000000:
            continue
        code = section.get_data()
        for match in re.finditer(rb"[\x48-\x4f][\x8d\x8b][\x05\x0d\x15\x1d\x25\x2d\x35\x3d]....", code, re.DOTALL):
            rva = section.VirtualAddress + match.start()
            dest = rva + 7 + struct.unpack_from("<i", match.group(), 3)[0]
            if dest not in by_target:
                continue
            insn = next(md.disasm(match.group(), base + rva), None)
            if insn is None or insn.size != 7:
                continue
            fn = containing(rva)
            references[by_target[dest]].append({"rva": hex(rva), "instruction": f"{insn.mnemonic} {insn.op_str}",
                                                "function_rva": hex(fn[0]) if fn else None,
                                                "function_end_rva": hex(fn[1]) if fn else None})

    candidate_functions = {}
    for string, refs in references.items():
        for ref in refs:
            if not ref["function_rva"]:
                continue
            start, end = int(ref["function_rva"], 16), int(ref["function_end_rva"], 16)
            key = hex(start)
            if key not in candidate_functions:
                direct_calls = []
                indirect_calls = []
                for insn in md.disasm(pe.get_data(start, end - start), base + start):
                    if insn.mnemonic != "call":
                        continue
                    if insn.operands[0].type == capstone.x86_const.X86_OP_IMM:
                        direct_calls.append({"site_rva": hex(insn.address - base), "target_rva": hex(insn.operands[0].imm - base)})
                    else:
                        indirect_calls.append({"site_rva": hex(insn.address - base), "operand": insn.op_str})
                candidate_functions[key] = {"end_rva": hex(end), "strings": [], "direct_calls": direct_calls, "indirect_calls": indirect_calls}
            if string not in candidate_functions[key]["strings"]:
                candidate_functions[key]["strings"].append(string)

    caller_targets = {int(k, 16) for k in candidate_functions}
    for section in pe.sections:
        if not section.Characteristics & 0x20000000:
            continue
        code = section.get_data()
        for match in re.finditer(rb"\xe8....", code, re.DOTALL):
            rva = section.VirtualAddress + match.start()
            dest = rva + 5 + struct.unpack_from("<i", match.group(), 1)[0]
            if dest not in caller_targets:
                continue
            fn = containing(rva)
            candidate_functions[hex(dest)].setdefault("candidate_direct_callers", []).append({
                "site_rva": hex(rva), "function_rva": hex(fn[0]) if fn else None,
                "function_end_rva": hex(fn[1]) if fn else None,
                "chained_unwind_root_rva": hex(unwind_root(fn[0])) if fn else None})

    imports = []
    for dll in getattr(pe, "DIRECTORY_ENTRY_IMPORT", []):
        dll_name = dll.dll.decode("ascii", errors="replace")
        if "steam" in dll_name.lower() or "lua" in dll_name.lower():
            imports.append({"dll": dll_name, "symbols": [{"name": imp.name.decode("ascii", errors="replace") if imp.name else f"ordinal:{imp.ordinal}", "iat_rva": hex(imp.address - base)} for imp in dll.imports]})
    lua = path.with_name("lua51.dll")
    lua_observation = None
    if lua.is_file():
        lua_bytes = lua.read_bytes()
        lua_observation = {"sha256": hashlib.sha256(lua_bytes).hexdigest(), "version_literals": sorted(set(m.group().decode("ascii") for m in re.finditer(rb"Lua(?:JIT)? [0-9][ -~]{0,30}", lua_bytes)))}
    executable_ranges = [(s.VirtualAddress, s.VirtualAddress + s.Misc_VirtualSize) for s in pe.sections if s.Characteristics & 0x20000000]
    physics_vtables = []
    for match in re.finditer(re.escape(b".?AVTickDynamicsWorld@@\x00"), data):
        type_rva = pe.get_rva_from_offset(match.start()) - 16
        for reference in re.finditer(re.escape(struct.pack("<I", type_rva)), data):
            offset = reference.start() - 12
            if offset < 0 or offset + 24 > len(data):
                continue
            fields = struct.unpack_from("<IIIIII", data, offset)
            col_rva = pe.get_rva_from_offset(offset)
            if fields[0] != 1 or fields[5] != col_rva:
                continue
            for pointer in re.finditer(re.escape(struct.pack("<Q", base + col_rva)), data):
                vtable_rva = pe.get_rva_from_offset(pointer.start() + 8)
                slots = []
                for slot in range(128):
                    value = struct.unpack("<Q", pe.get_data(vtable_rva + slot * 8, 8))[0] - base
                    if not any(begin <= value < end for begin, end in executable_ranges):
                        break
                    slots.append({"slot": slot, "target_rva": hex(value), "first_16_bytes": pe.get_data(value, 16).hex()})
                physics_vtables.append({"type": "TickDynamicsWorld", "type_descriptor_rva": hex(type_rva),
                    "complete_object_locator_rva": hex(col_rva), "class_hierarchy_rva": hex(fields[4]),
                    "object_offset": fields[1], "vtable_rva": hex(vtable_rva), "slots": slots})
    return {
        "evidence_type": "offline-shell-static-pefile-capstone", "source": str(path.resolve()),
        "sha256": hashlib.sha256(data).hexdigest(), "expected_build": hashlib.sha256(data).hexdigest() == EXPECTED_SHA256,
        "image_base": hex(base), "size_of_image": hex(pe.OPTIONAL_HEADER.SizeOfImage),
        "tools": {"pefile": pefile.__version__, "capstone": capstone.__version__},
        "imports": imports, "lua_runtime": lua_observation,
        "strings": [{"text": s, "rvas": [hex(x) for x in targets[s]], "references": references[s]} for s in targets],
        "physics_class_name_occurrences": {s: [hex(pe.get_rva_from_offset(m.start())) for m in re.finditer(re.escape(s.encode()), data)] for s in PHYSICS},
        "physics_rtti_vtables": physics_vtables,
        "candidate_functions": candidate_functions,
        "known_build_function_candidates": [{"name": name, "rva": hex(start), "end_rva": hex(spans.get(start, start)),
            "first_16_bytes": pe.get_data(start, 16).hex()} for start, name in KNOWN_FUNCTIONS.items()] if hashlib.sha256(data).hexdigest() == EXPECTED_SHA256 else [],
        "limitations": ["No REA provider connected; these are ordinary shell observations, not REA Evidence.",
                        "String occurrences establish presence, not executed behavior or complete RTTI ownership.",
                        "RIP reference scan covers selected REX.W LEA/MOV encodings only; candidates inside instructions may be false positives.",
                        "Function ranges are Windows exception-directory entries and can be fragments rather than complete logical functions.",
                        "Call targets are static direct calls; virtual calls and wire schemas remain unresolved.",
                        "No game execution, network capture, injection, or asset changes were performed."]}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("binary", type=Path)
    parser.add_argument("--output", type=Path)
    parser.add_argument("--disassembly-output", type=Path, help="write focused candidate disassembly for the verified build")
    parser.add_argument("--allow-unknown-build", action="store_true")
    args = parser.parse_args()
    report = inspect(args.binary)
    if not report["expected_build"] and not args.allow_unknown_build:
        parser.error("target SHA-256 differs from server.md; use --allow-unknown-build for an explicitly unverified report")
    encoded = json.dumps(report, indent=2, ensure_ascii=False) + "\n"
    if args.disassembly_output:
        pe = pefile.PE(str(args.binary))
        md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
        chunks = [f"Offline static observations; SHA-256 {report['sha256']}; candidate names are inferred.\n"]
        for candidate in report["known_build_function_candidates"]:
            start, end = int(candidate["rva"], 16), int(candidate["end_rva"], 16)
            chunks.append(f"\nFUNCTION {candidate['name']} RVA {start:#x}..{end:#x}\n")
            chunks.extend(f"{insn.address - pe.OPTIONAL_HEADER.ImageBase:x}: {insn.mnemonic} {insn.op_str}\n"
                          for insn in md.disasm(pe.get_data(start, end - start), pe.OPTIONAL_HEADER.ImageBase + start))
        args.disassembly_output.parent.mkdir(parents=True, exist_ok=True)
        args.disassembly_output.write_text("".join(chunks), encoding="utf-8")
    if args.output:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(encoded, encoding="utf-8")
        print(json.dumps({"output": str(args.output.resolve()), "sha256": report["sha256"], "candidate_functions": len(report["candidate_functions"])}))
    else:
        print(encoded, end="")


if __name__ == "__main__":
    main()
