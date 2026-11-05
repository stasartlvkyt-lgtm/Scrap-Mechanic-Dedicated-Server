"""Export GameInfo/VersionMismatch functions from a PRIVATE IDA database.

Run with IDA's -S on artifacts/ida-work/ScrapMechanic.exe.i64. Read-only except
for IDA's own database bookkeeping; the owner's open game database is not used.
"""
import json
import traceback
from pathlib import Path

import ida_auto
import ida_bytes
import ida_funcs
import ida_hexrays
import ida_nalt
import ida_pro
import ida_xref
import idautils
import idc

output = Path(idc.ARGV[1])
output.mkdir(parents=True, exist_ok=True)

TARGETS = {
    "version_rtti": 0x19DD700,
    "version_message": 0x1733EF0,
    "gameinfo_cpp": 0x17336E0,
    "seed_assert": 0x14D8110,
    "gamemode_assert": 0x14D80D8,
    "ugc_assert": 0x14D8090,
    "gametick_assert": 0x14D8050,
    "checksums_assert": 0x14D8DD0,
}

try:
    ida_auto.auto_wait()
    base = ida_nalt.get_imagebase()
    available = ida_hexrays.init_hexrays_plugin()
    report = {
        "input": ida_nalt.get_input_file_path(),
        "image_base": hex(base),
        "hexrays": bool(available),
        "targets": {},
        "named": [],
    }
    functions = {}
    for name, rva in TARGETS.items():
        ea = base + rva
        entry = {"rva": hex(rva), "xrefs": []}
        for ref in idautils.XrefsTo(ea, ida_xref.XREF_ALL):
            func = ida_funcs.get_func(ref.frm)
            record = {
                "from": hex(ref.frm - base),
                "type": int(ref.type),
                "func_start": hex(func.start_ea - base) if func else None,
                "func_end": hex(func.end_ea - base) if func else None,
            }
            entry["xrefs"].append(record)
            if func:
                functions[func.start_ea] = func
        report["targets"][name] = entry

    for ea in idautils.Functions():
        name = idc.get_func_name(ea)
        low = name.lower()
        if "gameinfo" in low or "versionmismatch" in low or "serversgameinfo" in low:
            report["named"].append({"rva": hex(ea - base), "name": name})

    for start, func in sorted(functions.items()):
        rva = start - base
        size = func.end_ea - start
        entry = {
            "rva": hex(rva),
            "end_rva": hex(func.end_ea - base),
            "size": size,
            "decompiled": False,
        }
        if size > 200000:
            entry["skipped"] = "function too large"
            report["targets"].setdefault("_functions", []).append(entry)
            continue
        lines = []
        try:
            for item in idautils.FuncItems(start):
                lines.append(f"{item-base:08x}: {idc.generate_disasm_line(item, 0)}")
            (output / f"{rva:x}.asm.txt").write_text("\n".join(lines), encoding="utf-8")
        except Exception as exc:
            entry["asm_error"] = str(exc)
        if available:
            try:
                code = str(ida_hexrays.decompile(start))
                (output / f"{rva:x}.c.txt").write_text(code, encoding="utf-8")
                entry["decompiled"] = True
            except Exception as exc:
                entry["decompile_error"] = str(exc)
        report["targets"].setdefault("_functions", []).append(entry)

    (output / "index.json").write_text(json.dumps(report, indent=2), encoding="utf-8")
except Exception:
    (output / "error.txt").write_text(traceback.format_exc(), encoding="utf-8")
finally:
    ida_pro.qexit(0)