from pathlib import Path
import ida_auto,ida_funcs,ida_hexrays,ida_nalt,ida_pro,idautils,idc
out=Path(idc.ARGV[1]);out.mkdir(parents=True,exist_ok=True)
try:
 ida_auto.auto_wait();base=ida_nalt.get_imagebase();ida_hexrays.init_hexrays_plugin()
 for rva in [0xB87CC0,0xB87DC0,0xB87E70,0xB87EC0,0xB8B8B0,0xB8B950,0xB8BA30,0xB8BA70,0x2DBD00,0x6B6BB0,0x6B7770,0x6B7910]:
  f=ida_funcs.get_func(base+rva)
  if not f:continue
  try:(out/f'{f.start_ea-base:x}.c').write_text(str(ida_hexrays.decompile(f.start_ea)),encoding='utf8')
  except Exception as e:(out/f'{rva:x}.error').write_text(str(e))
finally:ida_pro.qexit(0)
