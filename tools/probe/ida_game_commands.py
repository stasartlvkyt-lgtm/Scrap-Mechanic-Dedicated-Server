"""Export gameplay command handlers from a PRIVATE game analysis database."""
import ida_auto, ida_funcs, ida_hexrays, ida_nalt, ida_pro, idautils, idc
import json
from pathlib import Path

out = Path(idc.ARGV[1]); out.mkdir(parents=True, exist_ok=True)
try:
    ida_auto.auto_wait()
    base = ida_nalt.get_imagebase()
    initialized = ida_hexrays.init_hexrays_plugin()
    rvas = [int(x,16) for x in idc.ARGV[2:]] or [0xB3FCB0,0xB42B60,0xB44B70,0xB451C0,0xB45640,0xB45A70,0xB465B0,0xB46960,0xB3F580]
    records=[]
    for rva in rvas:
        func=ida_funcs.get_func(base+rva)
        record={'requested_rva':hex(rva),'hexrays':bool(initialized)}
        if func:
            record.update(start_rva=hex(func.start_ea-base),end_rva=hex(func.end_ea-base))
            asm=[f'{ea-base:08x}: {idc.generate_disasm_line(ea,0)}' for ea in idautils.FuncItems(func.start_ea)]
            (out/f'{rva:x}.asm.txt').write_text('\n'.join(asm),encoding='utf-8')
            if initialized:
                try:
                    code=ida_hexrays.decompile(func.start_ea)
                    (out/f'{rva:x}.c.txt').write_text(str(code),encoding='utf-8')
                    record['decompiled']=code is not None
                except Exception as exc: record['error']=str(exc)
        records.append(record)
    (out/'index.json').write_text(json.dumps({'input':ida_nalt.get_input_file_path(),'functions':records},indent=2),encoding='utf-8')
finally:
    ida_pro.qexit(0)
