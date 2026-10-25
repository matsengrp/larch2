import subprocess, re, random, hashlib
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

def to_nwk(n):
    return n[0] if not n[1] else '(' + ','.join(to_nwk(c) for c in n[1]) + ')' + n[0]

def collect_edges(n, acc, parent=None):
    # internal nodes with >=2 children: candidate NNI sites
    if n[1]:
        acc.append(n)
        for c in n[1]: collect_edges(c, acc, n)

def nni(root, rng):
    nodes = []
    collect_edges(root, nodes)
    for _ in range(8):  # 8 NNI moves per kick
        a = rng.choice(nodes)
        if len(a[1]) < 2: continue
        i, j = rng.sample(range(len(a[1])), 2)
        ci, cj = a[1][i], a[1][j]
        # swap a random child of ci with a random child of cj
        if not ci[1] or not cj[1]: continue
        x = rng.randrange(len(ci[1])); y = rng.randrange(len(cj[1]))
        ci[1][x], cj[1][y] = cj[1][y], ci[1][x]

def run(cmd, **kw):
    return subprocess.run(cmd, capture_output=True, text=True, **kw)

targets_set = None
sub0line = open('m23/sub0.nwk').readline().strip().rstrip(';')
template = open('m23/t1_template.nwk').read().strip().rstrip(';')

def context_score(sub_nwk_str, tag):
    full = template.replace('@@SUB@@', sub_nwk_str)
    open(f'm23/k_full_{tag}.nwk','w').write(full + ';\n')
    r = run([f'{R}/wric_tension_map','megatree','fromnewick',f'm23/k_full_{tag}.nwk',
             'm21/cand/t1.pb.gz', f'm23/k_full_{tag}.pb.gz'])
    if r.returncode: return ('ERR', r.stderr.strip()[:100])
    r = run([f'{R}/wric_tension_map','scaffold','pin','scaffold_bin.pb.gz',
             f'm23/k_full_{tag}.pb.gz','npart/fold/ids/part_','npart/fold/resp/part_',
             '24','npart/fold/fold_lb3.profile'])
    m = re.search(r'total = (\d+)', r.stderr)
    return (int(m.group(1)) if m else 'NOPRINT', '')

rng = random.Random(1234)
results = []
for kick in range(14):
    root = parse_line(sub0line)
    nni(root, rng)
    open(f'm23/kick_{kick}.nwk','w').write(to_nwk(root) + ';\n')
    r = run([f'{R}/wric_tension_map','megatree','fromnewick',f'm23/kick_{kick}.nwk',
             'm21/cand/t1.pb.gz', f'm23/kick_{kick}.pb.gz'])
    if r.returncode:
        print(f'kick{kick}: fromnewick ERR {r.stderr.strip()[:80]}', flush=True); continue
    # isolated reopt: SPR on the kicked subtree
    r = run([f'{R}/bin/larch2','--dag-pb',f'm23/kick_{kick}.pb.gz','-o',f'm23/kick_{kick}_opt.pb.gz',
             '-n','1','--max-moves','30','--seed',str(kick),'--validate'], timeout=600)
    # materialize one parsimony-optimal tree of the reopt DAG
    r = run([f'{R}/bin/larch2','--dag-pb',f'm23/kick_{kick}_opt.pb.gz','-o','/dev/null','-n','1',
             '--max-moves','0','--sample-method','parsimony','--diverse-sample','3',
             '--diverse-newick',f'm23/kick_{kick}_cand.nwk','--seed',str(kick),'--validate'], timeout=600)
    try:
        cands = [l.strip().rstrip(';') for l in open(f'm23/kick_{kick}_cand.nwk') if l.strip()]
    except FileNotFoundError:
        cands = []
    for ci, c in enumerate(cands[:3]):
        sc, err = context_score(c, f'{kick}_{ci}')
        results.append((sc, kick, ci))
        print(f'kick{kick} cand{ci}: context = {sc} {err}', flush=True)
print('DONE. best:', min((x for x in results if isinstance(x[0], int)), default='none'), flush=True)
