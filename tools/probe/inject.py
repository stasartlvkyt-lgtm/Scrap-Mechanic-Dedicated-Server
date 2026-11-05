"""Attach the build-bound passive capture DLL to an explicitly selected process.

The DLL remains resident after stop. No unload is attempted while detour return
addresses could still exist in other game threads.
"""
import argparse
import ctypes as C
from ctypes import wintypes as W
import hashlib
import os
import shutil
from pathlib import Path
import sys

EXPECTED_SHA = 'cf403cf9d3e465e05298c8e6082f4d664c32140ebcc4696a64200e367abb530d'

def main():
    if os.name != 'nt' or C.sizeof(C.c_void_p) != 8:
        raise RuntimeError('requires 64-bit Python on Windows')
    import pefile
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--pid', type=int, required=True)
    parser.add_argument('--dll', type=Path, default=Path(__file__).resolve().parents[2] / 'build' / 'probe' / 'scrapds_probe.dll')
    parser.add_argument('--log', type=Path)
    parser.add_argument('--stop', action='store_true')
    parser.add_argument('--codec-test', action='store_true', help='Write synthetic native codec fixture to --log; sends no network traffic')
    args = parser.parse_args()
    if args.pid <= 0: parser.error('--pid must be positive')
    if args.stop and args.codec_test: parser.error('--stop and --codec-test are mutually exclusive')
    if not args.stop and args.log is None: parser.error('--log is required when starting')
    dll_source = args.dll.resolve(strict=True)
    with dll_source.open('rb') as f:
        dll_sha = hashlib.file_digest(f, 'sha256').hexdigest()
    # Windows pins loaded modules. Load a content-addressed copy so recompiling
    # the development binary remains possible while an old capture is resident.
    module_dir = Path(__file__).resolve().parents[2] / 'artifacts' / 'probe-modules'
    module_dir.mkdir(parents=True, exist_ok=True)
    dll = module_dir / (dll_source.stem + '-' + dll_sha[:24] + dll_source.suffix)
    if not dll.exists(): shutil.copy2(dll_source, dll)
    with dll.open('rb') as f:
        if hashlib.file_digest(f, 'sha256').hexdigest() != dll_sha:
            raise RuntimeError('staged DLL hash mismatch')
    log = args.log.resolve() if args.log else None
    if log:
        if log.exists(): raise RuntimeError('log path already exists; choose a new filename')
        if not log.parent.is_dir(): raise RuntimeError('log parent directory does not exist')
    kernel = C.WinDLL('kernel32', use_last_error=True)
    def bind(name, restype, argtypes):
        f = getattr(kernel, name); f.restype = restype; f.argtypes = argtypes; return f
    close = bind('CloseHandle', W.BOOL, [W.HANDLE])
    open_process = bind('OpenProcess', W.HANDLE, [W.DWORD, W.BOOL, W.DWORD])
    image_name = bind('QueryFullProcessImageNameW', W.BOOL, [W.HANDLE, W.DWORD, W.LPWSTR, C.POINTER(W.DWORD)])
    allocate = bind('VirtualAllocEx', C.c_void_p, [W.HANDLE, C.c_void_p, C.c_size_t, W.DWORD, W.DWORD])
    release = bind('VirtualFreeEx', W.BOOL, [W.HANDLE, C.c_void_p, C.c_size_t, W.DWORD])
    write = bind('WriteProcessMemory', W.BOOL, [W.HANDLE, C.c_void_p, C.c_void_p, C.c_size_t, C.POINTER(C.c_size_t)])
    create_thread = bind('CreateRemoteThread', W.HANDLE, [W.HANDLE, C.c_void_p, C.c_size_t, C.c_void_p, C.c_void_p, W.DWORD, C.POINTER(W.DWORD)])
    wait = bind('WaitForSingleObject', W.DWORD, [W.HANDLE, W.DWORD])
    exit_code = bind('GetExitCodeThread', W.BOOL, [W.HANDLE, C.POINTER(W.DWORD)])
    get_module = bind('GetModuleHandleW', W.HMODULE, [W.LPCWSTR])
    get_proc = bind('GetProcAddress', C.c_void_p, [W.HMODULE, C.c_char_p])
    get_module_ex = bind('GetModuleHandleExW', W.BOOL, [W.DWORD, C.c_void_p, C.POINTER(W.HMODULE)])
    get_module_name = bind('GetModuleFileNameW', W.DWORD, [W.HMODULE, W.LPWSTR, W.DWORD])
    snapshot = bind('CreateToolhelp32Snapshot', W.HANDLE, [W.DWORD, W.DWORD])
    class MODULEENTRY32W(C.Structure):
        _fields_ = [('dwSize', W.DWORD), ('th32ModuleID', W.DWORD), ('th32ProcessID', W.DWORD),
                    ('GlblcntUsage', W.DWORD), ('ProccntUsage', W.DWORD), ('modBaseAddr', C.c_void_p),
                    ('modBaseSize', W.DWORD), ('hModule', W.HMODULE), ('szModule', W.WCHAR * 256), ('szExePath', W.WCHAR * 260)]
    first = bind('Module32FirstW', W.BOOL, [W.HANDLE, C.POINTER(MODULEENTRY32W)])
    next_ = bind('Module32NextW', W.BOOL, [W.HANDLE, C.POINTER(MODULEENTRY32W)])
    def check(value, operation):
        if not value: raise C.WinError(C.get_last_error(), operation)
        return value
    def modules():
        handle = snapshot(0x8 | 0x10, args.pid)
        if handle == C.c_void_p(-1).value: raise C.WinError(C.get_last_error())
        result = {}
        try:
            entry = MODULEENTRY32W(); entry.dwSize = C.sizeof(entry)
            check(first(handle, C.byref(entry)), 'Module32FirstW')
            while True:
                result[Path(entry.szExePath).resolve()] = int(entry.modBaseAddr)
                if not next_(handle, C.byref(entry)): break
        finally: close(handle)
        return result
    process = check(open_process(0x0002 | 0x0008 | 0x0020 | 0x0010 | 0x1000, False, args.pid), 'OpenProcess')
    try:
        image = C.create_unicode_buffer(32768); size = W.DWORD(len(image))
        check(image_name(process, 0, image, C.byref(size)), 'QueryFullProcessImageNameW')
        target = Path(image.value)
        if target.name.lower() != 'scrapmechanic.exe': raise RuntimeError('selected process is not ScrapMechanic.exe')
        with target.open('rb') as f:
            digest = hashlib.file_digest(f, 'sha256').hexdigest()
        if digest != EXPECTED_SHA: raise RuntimeError('target build hash does not match verified offsets')
        def remote(function, data=None):
            memory = None; finished = False
            try:
                if data is not None:
                    memory = check(allocate(process, None, len(data), 0x3000, 0x04), 'VirtualAllocEx')
                    buffer = C.create_string_buffer(data); written = C.c_size_t()
                    check(write(process, memory, buffer, len(data), C.byref(written)), 'WriteProcessMemory')
                    if written.value != len(data): raise RuntimeError('short WriteProcessMemory')
                thread = check(create_thread(process, None, 0, function, memory, 0, None), 'CreateRemoteThread')
                try:
                    status = wait(thread, 15000)
                    if status != 0: raise RuntimeError(f'remote thread did not finish (wait={status}); argument memory retained; do not unload DLL')
                    finished = True; result = W.DWORD()
                    check(exit_code(thread, C.byref(result)), 'GetExitCodeThread')
                    return result.value
                finally: close(thread)
            finally:
                if memory and finished: release(process, memory, 0, 0x8000)
        loaded = modules()
        base = loaded.get(dll)
        if not base:
            if args.stop: raise RuntimeError('probe DLL is not loaded')
            loader = check(get_proc(get_module('kernel32.dll'), b'LoadLibraryW'), 'GetProcAddress LoadLibraryW')
            owner = W.HMODULE()
            check(get_module_ex(0x4 | 0x2, loader, C.byref(owner)), 'GetModuleHandleExW')
            owner_name = C.create_unicode_buffer(32768)
            check(get_module_name(owner, owner_name, len(owner_name)), 'GetModuleFileNameW')
            owner_path = Path(owner_name.value).resolve()
            remote_owner = loaded.get(owner_path)
            if not remote_owner: raise RuntimeError('LoadLibraryW owner module absent in target')
            remote(remote_owner + loader - owner.value, (str(dll) + '\0').encode('utf-16-le'))
            base = modules().get(dll)
            if not base: raise RuntimeError('LoadLibraryW did not load the probe DLL')
        export = b'ScrapdsProbeStop' if args.stop else b'ScrapdsProbeCodecTest' if args.codec_test else b'ScrapdsProbeStart'
        pe = pefile.PE(str(dll))
        try:
            if pe.FILE_HEADER.Machine != 0x8664: raise RuntimeError('DLL is not x64')
            matches = [e for e in pe.DIRECTORY_ENTRY_EXPORT.symbols if e.name == export and not e.forwarder]
            if len(matches) != 1: raise RuntimeError('probe export missing or forwarded')
            rva = matches[0].address
        finally: pe.close()
        result = remote(base + rva, None if args.stop else (str(log) + '\0').encode('utf-16-le'))
        if result != 0: raise RuntimeError(f'{export.decode()} failed with Windows status {result}')
        print('Probe stopped; DLL remains resident.' if args.stop else f'Synthetic codec fixture: {log}' if args.codec_test else f'Probe active; capture: {log}')
    finally: close(process)

if __name__ == '__main__':
    try: main()
    except Exception as exc:
        print(f'inject: {exc}', file=sys.stderr)
        sys.exit(1)
