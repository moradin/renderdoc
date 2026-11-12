import re,sys
for line in open(sys.argv[1]):
    m = re.search(r'Log: (\w+) LOD (\d+) MAXMIP (\d+) MIPF (\d) bias ([\d.]+):(.*)', line)
    if not m: 
        if 'differ between' in line or 'vertex texture' in line: print(line.split('Log: ')[1].strip())
        continue
    what,l,mx,f,b,rest = m.groups()
    if sys.argv[2] != what: continue
    vals = rest.split()
    out=[]
    for v in vals:
        r,g = [int(x,16) for x in v.split('/')]
        lv = (r-16)/32
        out.append('%4.2f%s' % (lv, 'm' if g in (0x7f,0x80) else ' '))
    print('L%s M%s F%s b%s |' % (l,mx,f,b[0]), ' '.join(out))
