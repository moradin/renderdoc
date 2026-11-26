# compiles an HLSL file with d3dcompiler_47 and prints its disassembly: fxc_disasm.py <file> <profile>
import ctypes
import sys

dll = ctypes.WinDLL(r'E:\Projects\renderdoc\util\test\d3dcompiler_47.dll')


class Blob(ctypes.Structure):
    pass


def blob_bytes(p):
    if not p:
        return b''
    vt = ctypes.cast(p, ctypes.POINTER(ctypes.POINTER(ctypes.c_void_p))).contents
    GetBufferPointer = ctypes.WINFUNCTYPE(ctypes.c_void_p, ctypes.c_void_p)(vt[3])
    GetBufferSize = ctypes.WINFUNCTYPE(ctypes.c_size_t, ctypes.c_void_p)(vt[4])
    return ctypes.string_at(GetBufferPointer(p), GetBufferSize(p))


src = open(sys.argv[1], 'rb').read()
code = ctypes.c_void_p()
errs = ctypes.c_void_p()
hr = dll.D3DCompile(src, len(src), None, None, None, b'main', sys.argv[2].encode(), 0, 0, ctypes.byref(code),
                    ctypes.byref(errs))
if hr != 0:
    print('compile failed', hex(hr & 0xffffffff), blob_bytes(errs).decode(errors='replace'))
    sys.exit(1)
bc = blob_bytes(code)
dis = ctypes.c_void_p()
hr = dll.D3DDisassemble(bc, len(bc), 0, None, ctypes.byref(dis))
print(blob_bytes(dis).decode(errors='replace'))
