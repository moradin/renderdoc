# For each compiled shader in <dir>, fills the device's constants with a sentinel, lets D3DX9_43's
# ID3DXConstantTable::SetDefaults write the table's default values, and reads the registers back.
# That's the register image the defaults stand for. Writes <dir>/setdefaults.json.
# usage: python setdefaults_probe.py <dir>
import ctypes, ctypes.wintypes as wt, glob, json, os, struct, sys

d = sys.argv[1]
PV = ctypes.c_void_p
u32 = ctypes.c_uint


def vtbl(obj, idx, restype, *argtypes):
    vt = ctypes.cast(ctypes.cast(obj, ctypes.POINTER(PV))[0], ctypes.POINTER(PV))
    return ctypes.WINFUNCTYPE(restype, PV, *argtypes)(vt[idx])


class PP(ctypes.Structure):
    _fields_ = [('BackBufferWidth', u32), ('BackBufferHeight', u32), ('BackBufferFormat', u32),
                ('BackBufferCount', u32), ('MultiSampleType', u32), ('MultiSampleQuality', u32),
                ('SwapEffect', u32), ('hDeviceWindow', wt.HWND), ('Windowed', wt.BOOL),
                ('EnableAutoDepthStencil', wt.BOOL), ('AutoDepthStencilFormat', u32), ('Flags', u32),
                ('FullScreen_RefreshRateInHz', u32), ('PresentationInterval', u32)]


d3d9 = ctypes.WinDLL('d3d9')
d3d9.Direct3DCreate9.restype = PV
d3dx = ctypes.WinDLL('d3dx9_43')
user32 = ctypes.WinDLL('user32')
user32.CreateWindowExW.restype = wt.HWND
user32.CreateWindowExW.argtypes = [u32, ctypes.c_wchar_p, ctypes.c_wchar_p, u32, ctypes.c_int, ctypes.c_int,
                                   ctypes.c_int, ctypes.c_int, wt.HWND, PV, PV, PV]
hwnd = user32.CreateWindowExW(0, 'STATIC', 'ctab probe', 0, 0, 0, 64, 64, None, None, None, None)

d3d = d3d9.Direct3DCreate9(32)
pp = PP()
pp.BackBufferWidth = 64
pp.BackBufferHeight = 64
pp.BackBufferFormat = 21  # X8R8G8B8... A8R8G8B8
pp.BackBufferCount = 1
pp.SwapEffect = 1  # DISCARD
pp.hDeviceWindow = hwnd
pp.Windowed = 1
dev = PV()
D3DCREATE_HARDWARE_VERTEXPROCESSING = 0x40
hr = vtbl(d3d, 16, ctypes.c_long, u32, u32, wt.HWND, u32, ctypes.POINTER(PP), ctypes.POINTER(PV))(
    d3d, 0, 1, hwnd, D3DCREATE_HARDWARE_VERTEXPROCESSING, ctypes.byref(pp), ctypes.byref(dev))
assert hr == 0, hex(hr & 0xffffffff)
dev = dev.value

# Set/Get Vertex/Pixel ShaderConstant F/I/B
IDX = {'vs': {'F': 94, 'I': 96, 'B': 98}, 'ps': {'F': 109, 'I': 111, 'B': 113}}
COUNT = {'vs': {'F': 256, 'I': 16, 'B': 16}, 'ps': {'F': 224, 'I': 16, 'B': 16}}

SENT_F = struct.unpack('<I', struct.pack('<f', -12345.0))[0]
SENT_I = 0x5EE5EE5E
SENT_B = 0x77


def setget(stage, kind, data=None):
    n = COUNT[stage][kind]
    per = 1 if kind == 'B' else 4
    buf = (u32 * (n * per))()
    if data is not None:
        for i, v in enumerate(data):
            buf[i] = v
        hr = vtbl(dev, IDX[stage][kind], ctypes.c_long, u32, PV, u32)(dev, 0, buf, n)
        assert hr == 0, (stage, kind, hex(hr & 0xffffffff))
        return None
    hr = vtbl(dev, IDX[stage][kind] + 1, ctypes.c_long, u32, PV, u32)(dev, 0, buf, n)
    assert hr == 0, (stage, kind, hex(hr & 0xffffffff))
    return list(buf)


results = {}
for path in sorted(glob.glob(os.path.join(d, '*.bin'))):
    name = os.path.splitext(os.path.basename(path))[0]
    code = open(path, 'rb').read()
    stage = 'ps' if struct.unpack('<I', code[:4])[0] >> 16 == 0xffff else 'vs'
    sent = {'F': SENT_F, 'I': SENT_I, 'B': SENT_B}
    for kind in 'FIB':
        n = COUNT[stage][kind] * (1 if kind == 'B' else 4)
        setget(stage, kind, [sent[kind]] * n)
    table = PV()
    f = d3dx.D3DXGetShaderConstantTableEx
    f.argtypes = [ctypes.c_char_p, u32, ctypes.POINTER(PV)]
    assert f(code, 0, ctypes.byref(table)) == 0
    hr = vtbl(table.value, 11, ctypes.c_long, PV)(table.value, dev)
    vtbl(table.value, 2, ctypes.c_ulong)(table.value)
    res = {'stage': stage, 'hr': hex(hr & 0xffffffff), 'F': {}, 'I': {}, 'B': {}}
    for kind in 'FIB':
        vals = setget(stage, kind)
        per = 1 if kind == 'B' else 4
        for r in range(COUNT[stage][kind]):
            reg = vals[r * per:(r + 1) * per]
            if any(v != sent[kind] for v in reg):
                res[kind][r] = ['%08x' % v for v in reg]
    results[name] = res
    print(name, stage, res['hr'], {k: len(res[k]) for k in 'FIB'})

json.dump(results, open(os.path.join(d, 'setdefaults.json'), 'w'), indent=1)
vtbl(dev, 2, ctypes.c_ulong)(dev)
vtbl(d3d, 2, ctypes.c_ulong)(d3d)
