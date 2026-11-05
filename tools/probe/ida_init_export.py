"""Export BlobData/init handler functions from a PRIVATE IDA database."""
import json
import traceback
from pathlib import Path

import ida_auto
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
    "blob_handler": 0x7A8AF0,
    "blob_manager_a": 0x1A69528,
    "blob_manager_b": 0x1A69530,
    "init_string_a": 0x14D9E00,
}
try:
    ida_auto.auto_wait()
    base = ida_nalt.get_imagebase()
    available = ida_hexrays.init_hexrays_plugin()
    report = {"input": ida_nalt.get_input_file_path(), "image_base": hex(base), "targets": {}}
    functions = {}
    for name, rva in TARGETS.items():
        entry = {"rva": hex(rva), "xrefs": []}
        for ref in idautils.XrefsTo(base + rva, ida_xref.XREF_ALL):
            func = ida_funcs.get_func(ref.frm)
            entry["xrefs"].append({"from": hex(ref.frm - base), "type": int(ref.type),
                                   "func_start": hex(func.start_ea - base) if func else None})
            if func:
                functions[func.start_ea] = func
        report["targets"][name] = entry
    for ea in idautils.Functions():
        name = idc.get_func_name(ea)
        if any(k in name.lower() for k in ("blob", "scriptdata", "genericdata")):
            report.setdefault("named", []).append({"rva": hex(ea - base), "name": name})
    for start, func in sorted(functions.items()):
        rva = start - base
        entry = {"rva": hex(rva), "end_rva": hex(func.end_ea - base), "size": func.end_ea - start}
        if func.end_ea - start <= 250000 and available:
            try:
                lines = [f"{ea-base:08x}: {idc.generate_disasm_line(ea, 0)}" for ea in idautils.FuncItems(start)]
                (output / f"{rva:x}.asm.txt").write_text("\n".join(lines), encoding="utf-8")
                (output / f"{rva:x}.c.txt").write_text(str(ida_hexrays.decompile(start)), encoding="utf-8")
                entry["decompiled"] = True
            except Exception as exc:
                entry["error"] = str(exc)
        report.setdefault("functions", []).append(entry)
    (output / "index.json").write_text(json.dumps(report, indent=2), encoding="utf-8")
except Exception:
    (output / "error.txt").write_text(traceback.format_exc(), encoding="utf-8")
finally:
    ida_pro.qexit(0)