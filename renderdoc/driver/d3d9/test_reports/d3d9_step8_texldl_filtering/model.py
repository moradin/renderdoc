"""A model of texldl, checked against the probe's files: model.py <probe dir> <device> [names...]"""
import glob
import math
import os
import sys

WRAP, MIRROR, CLAMP, BORDER, MIRRORONCE = 1, 2, 3, 4, 5
NONE, POINT, LINEAR, ANISO = 0, 1, 2, 3
S_ADDRU, S_ADDRV, S_ADDRW, S_BORDER, S_MAG, S_MIN, S_MIP, S_BIAS, S_MAXMIP, S_MAXANISO, S_SRGB = (
    1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11)

OPTS = {
    'wbits': 8,          # weight fraction bits (None = exact)
    'round': 'nearest',  # or 'floor'
    'magmin': 'lod',     # mag when lod <= 0
    'nonelevel': 'maxmip',
    'pointmip': 'round',
}


def load(path):
    d = {'states': {}, 'levels': [], 'samples': []}
    for line in open(path):
        p = line.split()
        if not p:
            continue
        if p[0] == 'state':
            d['states'][int(p[1])] = int(p[2])
        elif p[0] == 'level':
            d['levels'].append({'w': int(p[2]), 'h': int(p[3]), 'd': int(p[4]), 't': []})
        elif p[0] == 't':
            d['levels'][-1]['t'].append([float(x) for x in p[1:5]])
        elif p[0] == 's':
            d['samples'].append(([f32(float(x)) for x in p[1:5]], [float(x) for x in p[6:10]]))
        elif p[0] in ('kind', 'format', 'name', 'device'):
            d[p[0]] = p[1]
    return d


def f32(x):
    import struct
    return struct.unpack('f', struct.pack('f', x))[0]


def border_color(dw):
    a = (dw >> 24) & 255
    r = (dw >> 16) & 255
    g = (dw >> 8) & 255
    b = dw & 255
    return [r / 255.0, g / 255.0, b / 255.0, a / 255.0]


def quant(f):
    if OPTS['wbits'] is None:
        return f
    s = float(1 << OPTS['wbits'])
    if OPTS['round'] == 'nearest':
        return math.floor(f * s + 0.5) / s
    return math.floor(f * s) / s


def addr(i, size, mode):
    """a texel index after addressing, or -1 for the border"""
    if mode == WRAP:
        return i % size
    if mode == MIRROR:
        p = i % (2 * size)
        return p if p < size else 2 * size - 1 - p
    if mode == MIRRORONCE:
        if i < 0:
            i = -i - 1
        return min(i, size - 1)
    if mode == BORDER:
        return i if 0 <= i < size else -1
    return min(max(i, 0), size - 1)


def srgb(c):
    return c / 12.92 if c <= 0.04045 else ((c + 0.055) / 1.055) ** 2.4


def fetch(d, lev, x, y, z, bc):
    if x < 0 or y < 0 or z < 0:
        return bc
    L = d['levels'][lev]
    t = L['t'][(z * L['h'] + y) * L['w'] + x]
    if d['states'].get(S_SRGB, 0):
        t = [srgb(t[0]), srgb(t[1]), srgb(t[2]), t[3]]
    return t


def cube_face(c):
    cx, cy, cz = c[0], c[1], c[2]
    ax, ay, az = abs(cx), abs(cy), abs(cz)
    if az >= ax and az >= ay:
        f = 4 if cz >= 0 else 5
        sc = cx if cz >= 0 else -cx
        tc = -cy
        ma = az
    elif ay >= ax:
        f = 2 if cy >= 0 else 3
        sc = cx
        tc = cz if cy >= 0 else -cz
        ma = ay
    elif True:
        f = 0 if cx >= 0 else 1
        sc = -cz if cx >= 0 else cz
        tc = -cy
        ma = ax
    elif ay >= az:
        f = 2 if cy >= 0 else 3
        sc = cx
        tc = cz if cy >= 0 else -cz
        ma = ay
    else:
        f = 4 if cz >= 0 else 5
        sc = cx if cz >= 0 else -cx
        tc = -cy
        ma = az
    if ma == 0:
        ma = 1
    return f, (sc / ma + 1) * 0.5, (tc / ma + 1) * 0.5


def sample_level(d, lev, c, filt):
    st = d['states']
    L = d['levels'][lev]
    bc = border_color(st.get(S_BORDER, 0))
    kind = d['kind']
    if kind == 'cube':
        face, u, v = cube_face(c)
        coords = [(u, L['w'], CLAMP), (v, L['h'], CLAMP)]
        zfix = face
    else:
        modes = [st.get(S_ADDRU, WRAP), st.get(S_ADDRV, WRAP), st.get(S_ADDRW, WRAP)]
        coords = [(c[0], L['w'], modes[0]), (c[1], L['h'], modes[1])]
        if kind == 'volume':
            coords.append((c[2], L['d'], modes[2]))
        zfix = 0

    axes = []
    for (u, size, mode) in coords:
        if filt == POINT:
            # address the coordinate then pick the texel
            if mode == WRAP:
                uu = u - math.floor(u)
            elif mode == MIRROR:
                t = u - 2 * math.floor(u * 0.5)
                uu = 2 - t if t > 1 else t
            elif mode == MIRRORONCE:
                uu = min(abs(u), 1.0)
            elif mode == BORDER:
                uu = u
            else:
                uu = min(max(u, 0.0), 1.0)
            i = math.floor(uu * size)
            if mode == BORDER and (i < 0 or i >= size):
                axes.append([(-1, 1.0)])
            else:
                axes.append([(min(max(i, 0), size - 1), 1.0)])
        else:
            p = u * size - 0.5
            i = math.floor(p)
            f = quant(p - i)
            axes.append([(addr(i, size, mode), 1 - f), (addr(i + 1, size, mode), f)])

    while len(axes) < 3:
        axes.append([(zfix, 1.0)])

    out = [0.0] * 4
    for (x, wx) in axes[0]:
        for (y, wy) in axes[1]:
            for (z, wz) in axes[2]:
                w = wx * wy * wz
                if OPTS.get('prodq', 0):
                    w = math.floor(w * 256 + 0.5) / 256.0
                if w == 0:
                    continue
                t = fetch(d, lev, x, y, z, bc) if not (x < 0 or y < 0 or z < 0) else bc
                for k in range(4):
                    out[k] += w * t[k]
    return out


def model(d, c):
    st = d['states']
    mag = st.get(S_MAG, POINT)
    mn = st.get(S_MIN, POINT)
    mip = st.get(S_MIP, NONE)
    bias = f32(0)
    if S_BIAS in st:
        import struct
        bias = struct.unpack('f', struct.pack('I', st[S_BIAS]))[0]
    maxmip = st.get(S_MAXMIP, 0)
    n = len(d['levels'])
    lod = c[3] + bias
    if OPTS.get('lodq', 'floor') == 'floor':
        lod = math.floor(lod * 256) / 256.0
    base = min(maxmip, n - 1)

    if mip == NONE:
        filt = mag if max(lod, float(base)) <= 0 else mn
        if filt == ANISO:
            filt = LINEAR
        return sample_level(d, 0, c, filt)

    lodc = min(max(lod, float(base)), float(n - 1))
    filt = mag if lodc <= 0 else mn
    if filt == ANISO:
        filt = LINEAR
    if mip == POINT:
        lev = int(math.floor(lodc + 0.5)) if OPTS['pointmip'] == 'round' else int(math.ceil(lodc - 0.5))
        lev = min(max(lev, base), n - 1)
        return sample_level(d, lev, c, filt)

    l0 = int(math.floor(lodc))
    f = lodc - l0
    l1 = min(l0 + 1, n - 1)
    a = sample_level(d, l0, c, filt)
    if f == 0:
        return a
    b = sample_level(d, l1, c, filt)
    return [a[k] * (1 - f) + b[k] * f for k in range(4)]


def check(path, verbose=3):
    d = load(path)
    # each channel's range over the texture (and the border colour), to judge errors by
    lo = [1e30] * 4
    hi = [-1e30] * 4
    vals = [t for L in d['levels'] for t in L['t']] + [border_color(d['states'].get(S_BORDER, 0))]
    for t in vals:
        for k in range(4):
            lo[k] = min(lo[k], t[k])
            hi[k] = max(hi[k], t[k])
    rng = [max(hi[k] - lo[k], 1e-6) for k in range(4)]
    bad = 0
    worst = 0.0
    shown = 0
    for c, got in d['samples']:
        exp = model(d, c)
        e = max(abs(exp[k] - got[k]) / rng[k] for k in range(4))
        worst = max(worst, e)
        if e > TOL:
            bad += 1
            if shown < verbose:
                shown += 1
                print('   ', ['%.4g' % x for x in c], 'got', ['%.5g' % x for x in got], 'exp',
                      ['%.5g' % x for x in exp])
    return bad, len(d['samples']), worst


TOL = 1.0 / 128


if __name__ == '__main__':
    pdir, dev = sys.argv[1], sys.argv[2]
    for a in sys.argv[3:]:
        if '=' in a:
            k, v = a.split('=')
            OPTS[k] = None if v == 'None' else (int(v) if v.isdigit() else v)
    names = [a for a in sys.argv[3:] if '=' not in a]
    files = sorted(glob.glob(os.path.join(pdir, dev + '_*.txt')))
    total_bad = 0
    for f in files:
        n = os.path.basename(f)[len(dev) + 1:-4]
        if names and not any(x in n for x in names):
            continue
        bad, cnt, worst = check(f)
        total_bad += bad
        print('%-34s %5d/%5d wrong, worst rel %.3g' % (n, bad, cnt, worst))
    print('total wrong', total_bad)
