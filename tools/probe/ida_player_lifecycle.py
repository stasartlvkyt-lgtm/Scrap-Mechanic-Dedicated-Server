"""Export PlayerManager source references and selected BlobData handlers."""
from pathlib import Path
import json
import ida_auto, ida_funcs, ida_hexrays, ida_nalt, ida_pro, idautils, idc

out = Path(idc.ARGV[1])
out.mkdir(parents=True, exist_ok=True)
try:
    ida_auto.auto_wait()
    ida_hexrays.init_hexrays_plugin()
    base = ida_nalt.get_imagebase()
    targets = {0x7A89A0, 0x7A6690, 0x7A6D00, 0x7FC2D0, 0x735A50, 0xD9C7B0, 0xB56B20, 0xD9DA35, 0x729A05}
    references = []
    for s in idautils.Strings():
        value = str(s)
        if 'PlayerManager.cpp' not in value and 'NetObjReceiver.cpp' not in value:
            continue
        for x in idautils.XrefsTo(s.ea):
            f = ida_funcs.get_func(x.frm)
            if f:
                targets.add(f.start_ea-base)
                references.append({'string': value, 'reference': hex(x.frm-base), 'function': hex(f.start_ea-base)})
    (out / 'references.json').write_text(json.dumps(references, indent=2), encoding='utf-8')
    extra = []
    for target in [0x7FC350, 0x7A6D00, 0x7A89A0, 0x7A8AF0, 0xB56B20, 0xB57500, 0x730A00]:
        for x in idautils.XrefsTo(base+target):
            f=ida_funcs.get_func(x.frm)
            extra.append({'target':hex(target),'reference':hex(x.frm-base),'function':hex(f.start_ea-base) if f else None})
            if f: targets.add(f.start_ea-base)
    (out / 'xrefs.json').write_text(json.dumps(extra, indent=2), encoding='utf-8')
    for rva in sorted(targets):
        try:
            (out / f'{rva:x}.c.txt').write_text(str(ida_hexrays.decompile(base+rva)), encoding='utf-8')
        except Exception as e:
            (out / f'{rva:x}.error.txt').write_text(str(e), encoding='utf-8')
finally:
    ida_pro.qexit(0)
