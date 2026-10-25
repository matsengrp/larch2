import subprocess, sys, hashlib
R = "/home/ogi/matsen/larch2-wric-10datasets/build"

def parse_line(line):
    pos = 0
    def parse():
        nonlocal pos
        if line[pos] == '(':
            pos += 1
            kids = [parse()]
            while line[pos] == ',':
                pos += 1
                kids.append(parse())
            assert line[pos] == ')'; pos += 1
            st = pos
            while pos < len(line) and line[pos] not in ',():;':
                pos += 1
            return ('', kids) if pos == st else (line[st:pos], kids)
        st = pos
        while pos < len(line) and line[pos] not in ',():;':
            pos += 1
        return (line[st:pos], [])
    return parse()

def taxa(n):
    return [n[0]] if not n[1] else sum((taxa(c) for c in n[1]), [])

def to_nwk(n):
    return n[0] if not n[1] else '(' + ','.join(to_nwk(c) for c in n[1]) + ')' + n[0]

def find_clade(n, tset):
    if set(taxa(n)) == tset: return n
    for c in n[1]:
        r = find_clade(c, tset)
        if r is not None: return r
    return None

targets = set(x.strip() for x in open('m23/t9126.ids.txt') if x.strip())
template = open('m23/t1_template.nwk').read().strip()  # contains @@SUB@@

def context_score(sub_nwk_str, tag):
    full = template.rstrip(';').replace('@@SUB@@', sub_nwk_str)
    open(f'm23/cand_full_{tag}.nwk','w').write(full + ';\n')
    r = subprocess.run([f'{R}/wric_tension_map','megatree','fromnewick',f'm23/cand_full_{tag}.nwk',
                        'm21/cand/t1.pb.gz', f'm23/cand_full_{tag}.pb.gz'], capture_output=True, text=True)
    if r.returncode: return ('ERR', r.stderr.strip()[:120])
    r = subprocess.run([f'{R}/wric_tension_map','scaffold','pin','scaffold_bin.pb.gz',
                        f'm23/cand_full_{tag}.pb.gz','npart/fold/ids/part_','npart/fold/resp/part_',
                        '24','npart/fold/fold_lb3.profile'], capture_output=True, text=True)
    tot = [l for l in (r.stderr+r.stderr).splitlines() if 'total =' in l]
    import re
    m = re.search(r'total = (\d+)', r.stderr)
    return (int(m.group(1)) if m else 'NOPRINT', '')

# baseline: t1's own subtree
sub0 = open('m23/sub0.nwk').read().strip().rstrip(';')
base = context_score(sub0, 'base')
print('BASE t1 subtree in context:', base, flush=True)

# A) region variants from the 24 diverse full trees
seen = {hashlib.md5(sub0.encode()).hexdigest()}
lines = [l.strip() for l in open('m23/../m21/r8_div.nwk') if l.strip()]
best = (99999, 'none')
nscored = 0
for i, l in enumerate(lines):
    try:
        root = parse_line(l)
        sub = find_clade(root, targets)
    except Exception as e:
        print(f'div{i}: parse fail {e}', flush=True); continue
    if sub is None:
        print(f'div{i}: clade not exact, skip', flush=True); continue
    s = to_nwk(sub)
    h = hashlib.md5(s.encode()).hexdigest()
    if h in seen:
        print(f'div{i}: duplicate of seen, skip', flush=True); continue
    seen.add(h)
    sc, err = context_score(s, f'div{i}')
    nscored += 1
    if isinstance(sc, int) and sc < best[0]: best = (sc, f'div{i}')
    print(f'div{i}: context = {sc} {err}', flush=True)
print('BEST from diverse pool:', best, 'scored:', nscored, flush=True)
