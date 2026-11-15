"""Capture one warmed tower frame through an already injected RenderDoc API.

Launch Editor via renderdoccmd capture, set TG_SILPOM_TOWER_RENDERDOC_VIEW,
and run TowerBenchmarkEditorSmoke.py. This is separate from GPU timing runs.
The function indices below are the stable RENDERDOC_API_1_1_2 prefix.
"""
import ctypes
from pathlib import Path


def capture_frame(output, view, general):
    kernel = ctypes.WinDLL('kernel32', use_last_error=True)
    kernel.GetModuleHandleW.argtypes = [ctypes.c_wchar_p]
    kernel.GetModuleHandleW.restype = ctypes.c_void_p
    handle = kernel.GetModuleHandleW('renderdoc.dll')
    assert handle, 'Launch Editor through renderdoccmd capture first'
    library = ctypes.WinDLL('renderdoc.dll', handle=handle)
    library.RENDERDOC_GetAPI.argtypes = [ctypes.c_int, ctypes.POINTER(ctypes.c_void_p)]
    library.RENDERDOC_GetAPI.restype = ctypes.c_int
    pointer = ctypes.c_void_p()
    assert library.RENDERDOC_GetAPI(10102, ctypes.byref(pointer)) == 1
    functions = ctypes.cast(pointer, ctypes.POINTER(ctypes.c_void_p))
    set_path = ctypes.CFUNCTYPE(None, ctypes.c_char_p)(functions[11])
    mask_overlay = ctypes.CFUNCTYPE(None, ctypes.c_uint32, ctypes.c_uint32)(functions[8])
    count = ctypes.CFUNCTYPE(ctypes.c_uint32)(functions[13])
    trigger = ctypes.CFUNCTYPE(None)(functions[15])
    get_capture = ctypes.CFUNCTYPE(ctypes.c_uint32, ctypes.c_uint32, ctypes.c_void_p,
                                  ctypes.POINTER(ctypes.c_uint32), ctypes.c_void_p)(functions[14])
    set_path(str(Path(output) / view).encode('utf-8'))
    mask_overlay(0, 0)
    general.idle_wait_frames(120)
    previous = count()
    trigger()
    general.idle_wait_frames(30)
    assert count() == previous + 1, 'Expected one RenderDoc capture'
    length = ctypes.c_uint32()
    assert get_capture(previous, None, ctypes.byref(length), None)
    filename = ctypes.create_string_buffer(length.value + 1)
    assert get_capture(previous, filename, ctypes.byref(length), None)
    path = Path(filename.value.decode('utf-8'))
    assert path.is_file(), path
    return {'view': view, 'file': path.name, 'bytes': path.stat().st_size,
            'scope': 'Separate inspection frame; excluded from baseline statistics'}
