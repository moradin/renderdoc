"""Summarises the probe's g_<op>.txt files: error by input range against the C library, and what
the GPU does with special inputs (zero, infinity, NaN, denormals). Usage: analyse_g.py <dir>"""
import math
import os
import struct
import sys

d = sys.argv[1]


def f32(x):
    return struct.unpack('<f', struct.pack('<f', x))[0]


def bits(x):
    return struct.unpack('<I', struct.pack('<f', x))[0]


def isden(x):
    # the files print 9 significant digits, so round back to float32 before comparing
    return x != 0 and not math.isnan(x) and abs(f32(x)) < 2.0 ** -126


def num(v):
    v = v.replace("-nan(ind)", "nan").replace("nan(ind)", "nan").replace("-nan", "nan")
    return float(v)


def parse(op):
    rows = []
    with open(os.path.join(d, 'g_%s.txt' % op)) as f:
        next(f)
        for line in f:
            parts = [p.strip() for p in line.split('|')]
            inp = [num(v) for v in parts[0].split()]
            gpu = parts[2].split()
            g = [num(gpu[0]), num(gpu[1]), num(gpu[2])]
            refs = []
            for p in parts[3:]:
                v = p.split()
                refs.append((num(v[0]), int(v[2])))
            rows.append((inp, g, refs))
    return rows


def ulps(a, b):
    if math.isnan(a) and math.isnan(b):
        return 0
    if math.isnan(a) or math.isnan(b) or math.isinf(a) or math.isinf(b):
        return 0 if a == b else None

    def o(x):
        i = struct.unpack('<i', struct.pack('<f', x))[0]
        return i if i >= 0 else -(i & 0x7fffffff)
    return abs(o(a) - o(b))


out = open(os.path.join(d, 'g_analysis.txt'), 'w')


def P(*a):
    print(*a, file=out)


def special(x):
    return math.isinf(x) or math.isnan(x) or isden(x)


def fmt(x):
    if math.isnan(x):
        return 'NaN'
    if isden(x):
        return '%.9g(den)' % x
    return '%.9g' % x


ranges = {
    'rcp': [(1.17549435e-38, 1e38, 'all normal')],
    'rsq': [(1.17549435e-38, 1e38, 'all normal')],
    'log': [(0.5, 2, '|x| in 0.5..2 (result near 0)'), (1.17549435e-38, 0.5, '|x| < 0.5'), (2, 1e38, '|x| > 2')],
    'exp': [(0, 1, '|x| < 1'), (1, 126, '1 <= |x| < 126'), (126, 200, '|x| >= 126')],
    'sincos': [(0, 3.14159265, '|x| <= pi'), (3.14159265, 100.0001, 'pi < |x| <= 100'), (100.0001, 1e30, '|x| > 100')],
    'pow': [(0, 1e38, 'all')],
}

for op in ['rcp', 'rsq', 'log', 'exp', 'pow', 'sincos']:
    rows = parse(op)
    P('== %s (%d inputs)' % (op, len(rows)))
    for lo, hi, name in ranges[op]:
        for ri, refname in ([(0, 'cos'), (1, 'sin')] if op == 'sincos' else [(0, 'C library')]):
            us = []
            absmax = 0.0
            relmax = 0.0
            for inp, g, refs in rows:
                x = inp[0]
                if special(x) or not (lo <= abs(x) < hi) or (op == 'pow' and special(inp[1])):
                    continue
                gv = g[1] if (op == 'sincos' and ri == 1) else g[0]
                r = refs[ri][0]
                if special(r) or special(gv):
                    continue
                u = ulps(gv, r)
                if u is None:
                    continue
                us.append(u)
                absmax = max(absmax, abs(gv - r))
                if r != 0:
                    relmax = max(relmax, abs(gv - r) / abs(r))
            if not us:
                continue
            us.sort()
            n = len(us)
            P('  %s, against %s: %d inputs, exact %d, <=1 ulp %d, <=2 %d, median %d, max %d ulps, '
              'max abs error %.3g, max relative error %.3g' %
              (name, refname, n, sum(1 for u in us if u == 0), sum(1 for u in us if u <= 1),
               sum(1 for u in us if u <= 2), us[n // 2], us[-1], absmax, relmax))
    # special inputs: distinct ones from the fixed list
    seen = set()
    # denormal inputs: does the GPU give what it gives for a zero of the same sign?
    zero = {}
    for inp, g, refs in rows:
        if inp[0] == 0 and (op != 'pow' or inp[1] == 1.0 or True):
            zero.setdefault((math.copysign(1, inp[0]), bits(inp[1]) if op == 'pow' else 0), g)
    nden = asz = 0
    for inp, g, refs in rows:
        x = inp[0]
        if not isden(x):
            continue
        k = (math.copysign(1, x), bits(inp[1]) if op == 'pow' else 0)
        if k not in zero:
            continue
        nden += 1
        z = zero[k]
        if bits(g[0]) == bits(z[0]) and (op != 'sincos' or bits(g[1]) == bits(z[1])):
            asz += 1
    P('  denormal x: %d of %d give the same result as a zero of the same sign' % (asz, nden))
    P('  special inputs (x [y]: gpu | C library):')
    for inp, g, refs in rows:
        x, y = inp[0], inp[1]
        if isden(x) and x not in (1.40129846e-45, f32(1.17549421e-38)):
            continue
        key = (bits(x), bits(y) if op == 'pow' else 0)
        if key in seen:
            continue
        interesting = x in (0.0, 1.0, -1.0) or special(x) or (op == 'pow' and (special(y) or y in (0.0,)))
        if op == 'exp' and abs(x) in (126.0, 127.0, 128.0, 129.0, 149.0, 150.0):
            interesting = True
        if not interesting:
            continue
        seen.add(key)
        if op == 'sincos':
            P('    %s: cos %s sin %s | %s %s' % (fmt(x), fmt(g[0]), fmt(g[1]), fmt(refs[0][0]), fmt(refs[1][0])))
        elif op == 'pow':
            P('    %s %s: %s | %s' % (fmt(x), fmt(y), fmt(g[0]), fmt(refs[0][0])))
        else:
            sx = '-0' if (x == 0 and math.copysign(1, x) < 0) else fmt(x)
            sg = '-0' if (g[0] == 0 and math.copysign(1, g[0]) < 0) else fmt(g[0])
            sr = '-0' if (refs[0][0] == 0 and math.copysign(1, refs[0][0]) < 0) else fmt(refs[0][0])
            P('    %s: %s | %s' % (sx, sg, sr))

# the other ops: just the special cases that differ from the reference that matched otherwise
best = {'mul': 0, 'add': 0, 'mad': 1, 'mul_add': 1, 'min': 0, 'max': 0, 'cmp': 0, 'frc': 0,
        'dp3': 1, 'dp4': 1, 'dp2add': 3, 'nrm': 0, 'lrp': 1, 'mul_pp': 0}
for op, ri in best.items():
    rows = parse(op)
    diffs = []
    for inp, g, refs in rows:
        r = refs[ri][0]
        gv = g[0]
        same = (bits(gv) == bits(r)) or (math.isnan(gv) and math.isnan(r))
        if not same:
            diffs.append((inp, gv, r))
    P('== %s: %d of %d differ from reference %d' % (op, len(diffs), len(rows), ri + 1))
    shown = set()
    for inp, gv, r in diffs:
        def cls(v):
            if math.isnan(v):
                return 'nan'
            if math.isinf(v):
                return 'inf'
            if isden(v):
                return 'den'
            if v == 0:
                return '-0' if math.copysign(1, v) < 0 else '0'
            return 'n'
        key = (tuple(cls(v) for v in inp[:3]), cls(gv), cls(r))
        if key in shown:
            continue
        shown.add(key)
        def s(v):
            if v == 0 and math.copysign(1, v) < 0:
                return '-0'
            return fmt(v)
        P('    in %s: gpu %s, ref %s' % (' '.join(s(v) for v in inp), s(gv), s(r)))
        if len(shown) > 24:
            P('    ...')
            break

out.close()
print(open(os.path.join(d, 'g_analysis.txt')).read())
