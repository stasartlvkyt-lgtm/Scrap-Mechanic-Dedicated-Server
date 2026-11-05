from pathlib import Path
import ida_auto,ida_funcs,ida_hexrays,ida_nalt,ida_pro,idautils,idc
out=Path(idc.ARGV[1]);out.mkdir(parents=True,exist_ok=True)
try:
 ida_auto.auto_wait();base=ida_nalt.get_imagebase();ida_hexrays.init_hexrays_plugin()
 for rva in [0x60FA50,0x60FB30,0x24D6A0,0x26A890,0x6D7890,0x6B55F0]:
  f=ida_funcs.get_func(base+rva)
  if not f:continue
  try:(out/f'{f.start_ea-base:x}.c').write_text(str(ida_hexrays.decompile(f.start_ea)),encoding='utf8')
  except Exception as e:(out/f'{rva:x}.error').write_text(str(e))
finally:ida_pro.qexit(0)
