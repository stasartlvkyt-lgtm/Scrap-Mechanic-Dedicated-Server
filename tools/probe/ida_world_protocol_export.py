"""Export initial NetObj reader/writer functions from an owned IDA database."""
from pathlib import Path
import json
import traceback
import ida_auto, ida_funcs, ida_hexrays, ida_nalt, ida_pro, idautils, idc

out = Path(idc.ARGV[1]); out.mkdir(parents=True, exist_ok=True)
targets = [0xB55930, 0xB56B20, 0xB57500, 0xD99ED0, 0xE1ADB0, 0x7A8AF0,
           0x7A7DE0, 0x7A6D00, 0x7A8590]
try:
    ida_auto.auto_wait()
    base = ida_nalt.get_imagebase()
    ida_hexrays.init_hexrays_plugin()
    report = {"input": ida_nalt.get_input_file_path(), "functions": []}
    for rva in targets:
        function = ida_funcs.get_func(base + rva)
        item = {"requested_rva": hex(rva)}
        if function:
            start = function.start_ea
            item.update(start_rva=hex(start-base), end_rva=hex(function.end_ea-base))
            try:
                pseudocode = ida_hexrays.decompile(start)
                (out / f"{rva:x}.c.txt").write_text(str(pseudocode), encoding="utf-8")
                (out / f"{rva:x}.asm.txt").write_text("\n".join(
                    f"{ea-base:08x}: {idc.generate_disasm_line(ea,0)}"
                    for ea in idautils.FuncItems(start)), encoding="utf-8")
                item["decompiled"] = pseudocode is not None
            except Exception as error: item["error"] = str(error)
        report["functions"].append(item)
    (out / "index.json").write_text(json.dumps(report, indent=2), encoding="utf-8")
except Exception:
    (out / "error.txt").write_text(traceback.format_exc(), encoding="utf-8")
finally:
    ida_pro.qexit(0)
