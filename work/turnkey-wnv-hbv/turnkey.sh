#!/bin/bash
# turnkey.sh <name> <tree.pb> <refseq.txt>  -- full partitioned line + monolithic arm
set -u
export LD_LIBRARY_PATH=~/install/gcc-trunk/lib64
R=/home/ogi/matsen/larch2-wric-10datasets/build
L=$R/bin/larch2
W=$R/wric_tension_map
NAME=$1; TREE=$2; REF=$3
D=/tmp/v0/tk/$NAME
rm -rf $D && mkdir -p $D/{bin,spr,wit,fold,resp}
cd $D

# 0. load + normalize
$L --tree-pb "$TREE" --refseq "$REF" -o probe.pb.gz -n 0 --validate 2> load.txt || $L --dag-pb "$TREE" -o probe.pb.gz -n 0 2> load.txt
NL=$($R/bin/dagutil --dag-pb probe.pb.gz --force-no-vcf --dag-info 2>/dev/null | grep -oP "leaves: \K[0-9]+")
FIXED=$($R/bin/dagutil --dag-pb probe.pb.gz --force-no-vcf --dag-info 2>/dev/null | grep -oP "parsimony_min: score:\K[0-9]+")

# 1. comb
$R/wric_binarize probe.pb.gz scaffold_bin.pb.gz scaffold.ids 2> binz.txt
COMB=$($W tension scaffold_bin.pb.gz scaffold_bin.pb.gz 0 2>&1 | grep -oP "witness total score = \K[0-9]+")

# 2. carve (polytomic original)
T0=$(date +%s.%N)
$W megatree tonewick probe.pb.gz poly.nwk 2>/dev/null
python3 - <<'PY'
import sys, os
sys.setrecursionlimit(60000)
s = open("poly.nwk").read().strip().rstrip(";")
pos = 0
def subtree():
    global pos
    if s[pos] == "(":
        pos += 1
        kids = [subtree()]
        while s[pos] == ",":
            pos += 1
            kids.append(subtree())
        pos += 1; st = pos
        while pos < len(s) and s[pos] not in ",();": pos += 1
        return (kids, s[st:pos])
    st = pos
    while pos < len(s) and s[pos] not in ",();": pos += 1
    return ([], s[st:pos])
t = subtree()
def ser(n): return n[1] if not n[0] else "(" + ",".join(ser(c) for c in n[0]) + ")" + n[1]
def leaves(n):
    if not n[0]: return {n[1]}
    out = set()
    for c in n[0]: out |= leaves(c)
    return out
removed, parts = set(), []
while True:
    best, bestsz = None, 0
    stack = [t]
    while stack:
        x = stack.pop()
        L = leaves(x); n = len(L)
        if not (L & removed) and bestsz < n <= 280 and n >= 30:
            best, bestsz = x, n
        stack.extend(x[0])
    if best is None: break
    parts.append(best); removed |= leaves(best)
os.makedirs("parts", exist_ok=True); os.makedirs("ids", exist_ok=True)
for i, p in enumerate(parts):
    open(f"parts/part_{i}.nwk", "w").write(ser(p) + ";")
    open(f"ids/part_{i:02d}.ids.txt", "w").write("\n".join(sorted(leaves(p))) + "\n")
open("frozen.ids.txt", "w").write("\n".join(sorted(leaves(t) - removed)) + "\n")
open("nparts", "w").write(str(len(parts)))
print("carve:", [len(leaves(p)) for p in parts], "frozen:", len(leaves(t) - removed))
PY
NP=$(cat nparts)

# 3. per-part SPR + chart + graft
cp scaffold_bin.pb.gz fold/cur.pb.gz
for i in $(seq 0 $((NP-1))); do
  p=$(printf %02d $i)
  $W megatree fromnewick parts/part_$i.nwk probe.pb.gz bin/part_$p.poly.pb.gz 2>/dev/null
  $R/wric_binarize bin/part_$p.poly.pb.gz bin/part_$p.bin.pb.gz ids_$p.txt 2>/dev/null
  $L --dag-pb bin/part_$p.bin.pb.gz -o spr/part_$p.spr.pb.gz -n 1 --max-moves 50 --seed 1 2> spr/t_$p.txt
  INC=$(grep -oE "iter 1: parsimony=[0-9]+" spr/t_$p.txt | head -1 | grep -oE "[0-9]+$")
  $R/wric_dag_binarize spr/part_$p.spr.pb.gz spr/part_$p.sbin.pb.gz 2>/dev/null
  $L --dag-pb spr/part_$p.sbin.pb.gz -o wit/part_$p.wit.pb.gz --trim-mode chart-bnb -n 0 --chart-bnb-score-only --chart-bnb-upper-bound $INC --chart-bnb-class-compress on --chart-bnb-profile-dedup on --wric-lazy-chart on --chart-bnb-trim-application optimal-topology-materialize --chart-bnb-max-exact-topologies 1 2> wit/t_$p.txt
  mkdir -p fold/g_$p
  $R/wric_subtree graft fold/cur.pb.gz ids/part_$p.ids.txt fold/g_$p wit/part_$p.wit.pb.gz 2>/dev/null
  cp fold/g_$p/graft_0.pb.gz fold/cur.pb.gz
done
T1=$(date +%s.%N)
U=$($W tension fold/cur.pb.gz fold/cur.pb.gz 0 2>&1 | grep -oP "witness total score = \K[0-9]+")

# 4. certificate
ARGS=""
for i in $(seq 0 $((NP-1))); do p=$(printf %02d $i); ARGS="$ARGS spr/part_$p.sbin.pb.gz"; done
mkdir -p resp2
$W response scaffold_bin.pb.gz resp/ $ARGS 2>/dev/null
for i in $(seq 0 $((NP-1))); do p=$(printf %02d $i); cp resp/part_$p.sbin.pb.resp resp2/part_$p.spr.pb.resp; done
printf '#\n' > empty.mask
$W scaffold lb scaffold_bin.pb.gz fold/cur.pb.gz ids/part_ resp2/part_ $NP pi.profile 0 empty.mask 2> lb.txt
T2=$(date +%s.%N)
LB=$(grep -oP "total = \K[0-9]+" lb.txt | head -1)
$W scaffold wit scaffold_bin.pb.gz fold/cur.pb.gz ids/part_ wit/part_ $NP - $U 2> wassert.txt
WOK=$(grep -c "OK" wassert.txt)

# 5. monolithic arm
M0=$(date +%s.%N)
$L --dag-pb scaffold_bin.pb.gz -o mono_spr.pb.gz -n 1 --max-moves 50 --seed 1 2> mono_t.txt
MINC=$(grep -oE "iter 1: parsimony=[0-9]+" mono_t.txt | head -1 | grep -oE "[0-9]+$")
$R/wric_dag_binarize mono_spr.pb.gz mono_sbin.pb.gz 2>/dev/null
$L --dag-pb mono_sbin.pb.gz -o mono_wit.pb.gz --trim-mode chart-bnb -n 0 --chart-bnb-score-only --chart-bnb-upper-bound $MINC --chart-bnb-class-compress on --chart-bnb-profile-dedup on --wric-lazy-chart on --chart-bnb-trim-application optimal-topology-materialize --chart-bnb-max-exact-topologies 1 2> mono_w.txt
MOPT=$(grep -oP "bnb_optimum: \K[0-9]+" mono_w.txt | head -1)
M1=$(date +%s.%N)

echo "==== $NAME ($NL leaves)"
echo "fixed=$FIXED comb=$COMB | partitioned(n=$NP): pipeline $(echo "$T1-$T0"|bc)s cert $(echo "$T2-$T1"|bc)s -> U=$U LB=$LB assertOK=$WOK | monolithic: $(echo "$M1-$M0"|bc)s inc=$MINC chart=$MOPT"
