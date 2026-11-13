# Compiles the shaders in shaders.py and dumps how D3DX9_43's ID3DXConstantTable reads each
# constant table: every constant, member and array element, with its registers and default value.
# usage: python d3dx_probe.py <out dir>
import ctypes, json, os, struct, sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from shaders import SHADERS

out = sys.argv[1]
os.makedirs(out, exist_ok=True)

d3dc = ctypes.WinDLL('d3dcompiler_47')
d3dx = ctypes.WinDLL('d3dx9_43')

PV = ctypes.c_void_p


def vtbl(obj, idx, restype, *argtypes):
    vt = ctypes.cast(ctypes.cast(obj, ctypes.POINTER(PV))[0], ctypes.POINTER(PV))
    return ctypes.WINFUNCTYPE(restype, PV, *argtypes)(vt[idx])


def blob(p):
    if not p:
        return b''
    ptr = vtbl(p, 3, PV)(p)
    size = vtbl(p, 4, ctypes.c_size_t)(p)
    return ctypes.string_at(ptr, size)


def compile_new(src, prof):
    code = PV()
    err = PV()
    f = d3dc.D3DCompile
    f.argtypes = [ctypes.c_char_p, ctypes.c_size_t, ctypes.c_char_p, PV, PV, ctypes.c_char_p,
                  ctypes.c_char_p, ctypes.c_uint, ctypes.c_uint, ctypes.POINTER(PV), ctypes.POINTER(PV)]
    hr = f(src, len(src), b'x.hlsl', None, None, b'main', prof, 0, 0, ctypes.byref(code), ctypes.byref(err))
    return hr & 0xffffffff, blob(code.value), blob(err.value)


def compile_legacy(src, prof):
    # D3DXCompileShader(src, len, defines, include, entry, profile, flags, shader, errors, table)
    code = PV()
    err = PV()
    f = d3dx.D3DXCompileShader
    f.argtypes = [ctypes.c_char_p, ctypes.c_uint, PV, PV, ctypes.c_char_p, ctypes.c_char_p,
                  ctypes.c_uint, ctypes.POINTER(PV), ctypes.POINTER(PV), PV]
    D3DXSHADER_USE_LEGACY_D3DX9_31_DLL = 0x10000
    hr = f(src, len(src), None, None, b'main', prof, D3DXSHADER_USE_LEGACY_D3DX9_31_DLL,
           ctypes.byref(code), ctypes.byref(err), None)
    return hr & 0xffffffff, blob(code.value), blob(err.value)


class CONSTANT_DESC(ctypes.Structure):
    _fields_ = [('Name', ctypes.c_char_p), ('RegisterSet', ctypes.c_uint), ('RegisterIndex', ctypes.c_uint),
                ('RegisterCount', ctypes.c_uint), ('Class', ctypes.c_uint), ('Type', ctypes.c_uint),
                ('Rows', ctypes.c_uint), ('Columns', ctypes.c_uint), ('Elements', ctypes.c_uint),
                ('StructMembers', ctypes.c_uint), ('Bytes', ctypes.c_uint), ('DefaultValue', PV)]


class TABLE_DESC(ctypes.Structure):
    _fields_ = [('Creator', ctypes.c_char_p), ('Version', ctypes.c_uint), ('Constants', ctypes.c_uint)]


def ctab_offset(code):
    # the CTAB comment's data, after the fourcc, as an offset into the bytecode
    words = struct.unpack('<%dI' % (len(code) // 4), code)
    i = 1
    while i < len(words):
        tok = words[i]
        if tok & 0xffff == 0xfffe:
            n = (tok >> 16) & 0x7fff
            if words[i + 1] == 0x42415443:
                return (i + 2) * 4, n - 1
            i += 1 + n
        else:
            break
    return None, 0


def dump(code):
    table = PV()
    f = d3dx.D3DXGetShaderConstantTableEx
    f.argtypes = [ctypes.c_char_p, ctypes.c_uint, ctypes.POINTER(PV)]
    hr = f(code, 0, ctypes.byref(table)) & 0xffffffff
    if hr != 0:
        return {'error': hex(hr)}
    t = table.value
    buf = vtbl(t, 3, PV)(t)
    bufsize = vtbl(t, 4, ctypes.c_uint)(t)
    raw = ctypes.string_at(buf, bufsize)
    td = TABLE_DESC()
    vtbl(t, 5, ctypes.c_long, ctypes.POINTER(TABLE_DESC))(t, ctypes.byref(td))
    getdesc = vtbl(t, 6, ctypes.c_long, PV, ctypes.POINTER(CONSTANT_DESC), ctypes.POINTER(ctypes.c_uint))
    getconst = vtbl(t, 8, PV, PV, ctypes.c_uint)
    getelem = vtbl(t, 10, PV, PV, ctypes.c_uint)

    nodes = []

    def walk(h, path):
        desc = CONSTANT_DESC()
        n = ctypes.c_uint(1)
        getdesc(t, h, ctypes.byref(desc), ctypes.byref(n))
        node = {
            'path': path, 'set': desc.RegisterSet, 'reg': desc.RegisterIndex, 'count': desc.RegisterCount,
            'class': desc.Class, 'type': desc.Type, 'rows': desc.Rows, 'cols': desc.Columns,
            'elements': desc.Elements, 'members': desc.StructMembers, 'bytes': desc.Bytes,
        }
        if desc.DefaultValue:
            off = desc.DefaultValue - buf
            node['default_offset'] = off
            if 0 <= off <= bufsize:
                vals = raw[off:off + desc.Bytes]
                node['default'] = ['%08x' % v for v in struct.unpack('<%dI' % (len(vals) // 4), vals)]
            else:
                node['default_outside'] = True
        nodes.append(node)
        if desc.Elements > 1:
            for e in range(desc.Elements):
                walk(getelem(t, h, e), '%s[%d]' % (path, e))
        elif desc.StructMembers:
            for m in range(desc.StructMembers):
                mh = getconst(t, h, m)
                md = CONSTANT_DESC()
                n2 = ctypes.c_uint(1)
                getdesc(t, mh, ctypes.byref(md), ctypes.byref(n2))
                walk(mh, '%s.%s' % (path, md.Name.decode()))

    for c in range(td.Constants):
        h = getconst(t, None, c)
        desc = CONSTANT_DESC()
        n = ctypes.c_uint(1)
        getdesc(t, h, ctypes.byref(desc), ctypes.byref(n))
        walk(h, desc.Name.decode())

    vtbl(t, 2, ctypes.c_ulong)(t)
    return {'creator': td.Creator.decode(), 'version': hex(td.Version), 'nodes': nodes,
            'table_size': bufsize}


summary = {}
for name, src, prof, legacy in SHADERS:
    hr, code, err = (compile_legacy if legacy else compile_new)(src.encode(), prof.encode())
    if hr != 0 or not code:
        print(name, 'compile failed', hex(hr), err.decode(errors='replace')[:2000])
        continue
    open(os.path.join(out, name + '.bin'), 'wb').write(code)
    off, n = ctab_offset(code)
    info = dump(code)
    info['ctab_offset_in_bytecode'] = off
    info['ctab_dwords'] = n
    summary[name] = info
    print(name, prof, len(code), 'bytes,', len(info.get('nodes', [])), 'nodes', info.get('error', ''))

json.dump(summary, open(os.path.join(out, 'd3dx.json'), 'w'), indent=1)
