from pathlib import Path
import ida_auto,ida_funcs,ida_hexrays,ida_nalt,ida_pro,idautils,idc
out=Path(idc.ARGV[1]);out.mkdir(parents=True,exist_ok=True)
try:
 ida_auto.auto_wait();base=ida_nalt.get_imagebase();ida_hexrays.init_hexrays_plugin();lines=[];found=set()
 for s in idautils.Strings():
  t=str(s)
  if any(k in t for k in ['ChildShape','Block.cpp','BlockData','Part.cpp','RigidBody.cpp','blockSize','m_BlockSize']):
   refs=[]
   for x in idautils.XrefsTo(s.ea):
    f=ida_funcs.get_func(x.frm)
    refs.append(hex(x.frm-base))
    if f:found.add(f.start_ea)
   lines.append(f'{s.ea-base:x} {t[:500]} xrefs={refs}')
 (out/'strings.txt').write_text('\n'.join(lines),encoding='utf8')
 for ea in sorted(found):
  try:(out/f'{ea-base:x}.c').write_text(str(ida_hexrays.decompile(ea)),encoding='utf8')
  except Exception as e:(out/f'{ea-base:x}.error').write_text(str(e))
finally:ida_pro.qexit(0)
