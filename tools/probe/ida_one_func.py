"""Decompile the function containing RVA 0x7A8AF0 from the private IDA DB."""
from pathlib import Path
import ida_auto, ida_funcs, ida_hexrays, ida_nalt, ida_pro, idautils, idc

out = Path(idc.ARGV[1]); out.mkdir(parents=True, exist_ok=True)
try:
    ida_auto.auto_wait()
    base = ida_nalt.get_imagebase()
    target = base + 0x7A8AF0
    func = ida_funcs.get_func(target)
    if not func:
        (out / "error.txt").write_text("no function at target\n")
    else:
        start, end = func.start_ea, func.end_ea
        (out / "info.txt").write_text(f"start={start-base:x} end={end-base:x} size={end-start}\n")
        (out / "func.c.txt").write_text(str(ida_hexrays.decompile(start)))
        lines = [f"{ea-base:08x}: {idc.generate_disasm_line(ea, 0)}" for ea in idautils.FuncItems(start)]
        (out / "func.asm.txt").write_text("\n".join(lines))
finally:
    ida_pro.qexit(0)