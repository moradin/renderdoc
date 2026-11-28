# prints the native stage counts for draws at chosen probes, and the first failing stage
import sys
NAMES = ['rasterise', 'culled', 'clip plane', 'scissor', 'sample mask', 'shader', 'depth', 'stencil']
ref = sys.argv[1]
recs = []
with open(ref) as f:
    for line in f:
        p = line.rstrip('\n').split('\t')
        if p[0] == 'E':
            recs.append({'batch': p[1], 'marker': p[2], 'kind': p[3], 'probes': {}})
        else:
            recs[-1]['probes'][(int(p[1]), int(p[2]))] = p[7]

want = {
    'Cull Front': [(170, 160)],
    'Scissor Fail': [(106, 248)],
    'Background': [(150, 250), (170, 140)],
    'Simple Test': [(170, 140), (170, 160)],
}
cols = ['Alpha Test FF', 'Alpha Test', 'Texkill ps_1_1', 'Texkill ps_2_0', 'Two Sided Stencil', 'Clip Plane',
        'Sample Mask 0', 'Sample Mask 5']
for i, c in enumerate(cols):
    want[c] = [(26 + 20 * i, 268), (26 + 20 * i, 282)]

for r in recs:
    if r['kind'] != 'draw' or r['marker'] not in want:
        continue
    for pr in want[r['marker']]:
        st = [int(v) for v in r['probes'][pr].split(',')]
        reason = 'not listed' if st[0] == 0 else next((NAMES[i] for i in range(1, 8) if st[i] == 0), 'passed')
        print('%-12s %-18s %-10s %-32s %s' % (r['batch'], r['marker'], pr, st, reason))
