"""Run with IDA's -S on a PRIVATE copy of the game executable/database.

Never point this at the user's open analysis database. This script does not
patch the binary. It exports selected functions then closes its private session.
"""
import json
from pathlib import Path
import traceback
import struct
import ida_auto
import ida_funcs
import ida_hexrays
import ida_idaapi
import ida_nalt
import ida_pro
import ida_ua
import idautils
import idc

output = Path(idc.ARGV[1])
output.mkdir(parents=True, exist_ok=True)
try:
    targeted = '--targeted' in idc.ARGV
    if not targeted:
        ida_auto.auto_wait()
    base = ida_nalt.get_imagebase()
    available = ida_hexrays.init_hexrays_plugin()
    rvas = [0xB59510, 0x4658D0, 0x4EE990, 0x4EEA30, 0xB597F0, 0xB5A150,
            0xB4D240, 0xB382E0, 0x474BC0, 0x2E3200, 0x2F1C90, 0x2F1960, 0x466A00, 0x1118410]
    records = []
    native_ranges = {}
    if targeted:
        raw = Path(ida_nalt.get_input_file_path()).read_bytes()
        pe = struct.unpack_from('<I', raw, 0x3c)[0]
        sections = struct.unpack_from('<H', raw, pe + 6)[0]
        optional_size = struct.unpack_from('<H', raw, pe + 20)[0]
        for i in range(sections):
            section = pe + 24 + optional_size + 40 * i
            if raw[section:section+8].rstrip(b'\0') != b'.pdata': continue
            size, offset = struct.unpack_from('<II', raw, section + 16)
            for j in range(0, size - 11, 12):
                begin, end, unwind = struct.unpack_from('<III', raw, offset + j)
                native_ranges[begin] = end
    for rva in rvas:
        address = base + rva
        if targeted and rva in native_ranges:
            end = base + native_ranges[rva]
            ida_funcs.del_func(address)
            current = address
            while current < end:
                instruction = ida_ua.insn_t()
                size = ida_ua.decode_insn(instruction, current)
                if size <= 0: break
                idc.create_insn(current)
                current += size
            ida_funcs.add_func(address, end)
        function = ida_funcs.get_func(address)
        entry = {'rva': hex(rva), 'name': idc.get_func_name(address), 'hexrays': bool(available)}
        if function:
            entry['start_rva'] = hex(function.start_ea - base)
            entry['end_rva'] = hex(function.end_ea - base)
            lines = []
            for ea in idautils.FuncItems(function.start_ea):
                lines.append(f'{ea-base:08x}: {idc.generate_disasm_line(ea, 0)}')
            (output / f'{rva:x}.asm.txt').write_text('\n'.join(lines), encoding='utf-8')
        if available:
            try:
                code = str(ida_hexrays.decompile(address))
                (output / f'{rva:x}.c.txt').write_text(code, encoding='utf-8')
                entry['decompiled'] = True
            except Exception as exc:
                entry['error'] = str(exc)
        records.append(entry)
    (output / 'index.json').write_text(json.dumps({'input': ida_nalt.get_input_file_path(), 'image_base': hex(base), 'targeted_no_full_autoanalysis': targeted, 'functions': records}, indent=2), encoding='utf-8')
except Exception:
    (output / 'error.txt').write_text(traceback.format_exc(), encoding='utf-8')
finally:
    ida_pro.qexit(0)
