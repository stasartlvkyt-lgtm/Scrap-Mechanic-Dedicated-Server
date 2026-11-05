from pathlib import Path
import ida_auto,ida_funcs,ida_hexrays,ida_nalt,ida_pro,idautils,idc
out=Path(idc.ARGV[1]);out.mkdir(parents=True,exist_ok=True)
try:
 ida_auto.auto_wait();base=ida_nalt.get_imagebase();ida_hexrays.init_hexrays_plugin();targets=[0x2DBD00,0x2652F0,0x2654F0,0x2DB100,0x2DBB80]
 targets += [ea-base for ea in idautils.Functions(base+0x6b7000,base+0x6b8300)]
 targets += [ea-base for ea in idautils.Functions(base+0x6db900,base+0x6dc350)]
 for rva in sorted(set(targets)):
  f=ida_funcs.get_func(base+rva)
  if not f:continue
  try:(out/f'{f.start_ea-base:x}.c').write_text(str(ida_hexrays.decompile(f.start_ea)),encoding='utf8')
  except Exception as e:(out/f'{rva:x}.error').write_text(str(e))
finally:ida_pro.qexit(0)
