#!/bin/bash
# enrich.sh <name> -- per-part 3-seed SPR + plain chart + chart-spr-search, min per part, refold
set -u
export LD_LIBRARY_PATH=~/install/gcc-trunk/lib64
R=/home/ogi/matsen/larch2-wric-10datasets/build
L=$R/bin/larch2
DG=$R/bin/dagutil
W=$R/wric_tension_map
NAME=$1
D=/tmp/v0/tk/$NAME
cd $D
NP=$(cat nparts)
mkdir -p en/{spr,wit,srch}
BEST=""
for i in $(seq 0 $((NP-1))); do
  p=$(printf %02d $i)
  best=999999; bestw=""
  for s in 1 2 3; do
    $L --dag-pb bin/part_$p.bin.pb.gz -o en/spr/part_${p}_s$s.pb.gz -n 1 --max-moves 50 --seed $s 2> en/spr/t_${p}_s$s.txt
    INC=$(grep -oE "iter 1: parsimony=[0-9]+" en/spr/t_${p}_s$s.txt | head -1 | grep -oE "[0-9]+$")
    $R/wric_dag_binarize en/spr/part_${p}_s$s.pb.gz en/spr/part_${p}_s$s.sbin.pb.gz 2>/dev/null
    # (a) plain chart on the seed grammar
    $L --dag-pb en/spr/part_${p}_s$s.sbin.pb.gz -o en/wit/part_${p}_s$s.wit.pb.gz --trim-mode chart-bnb -n 0 --chart-bnb-score-only --chart-bnb-upper-bound $INC --chart-bnb-class-compress on --chart-bnb-profile-dedup on --wric-lazy-chart on --chart-bnb-trim-application optimal-topology-materialize --chart-bnb-max-exact-topologies 1 2> en/wit/t_${p}_s$s.txt
    SC=$(grep -oP "bnb_optimum: \K[0-9]+" en/wit/t_${p}_s$s.txt | head -1)
    [ -z "$SC" ] && SC=$INC
    if [ "$SC" -lt "$best" ]; then best=$SC; bestw=en/wit/part_${p}_s$s.wit.pb.gz; fi
# (b) moved to monolithic scale (campaign: ~927s/seed)
  done
  echo "part $p: best=$best"
  BEST="$BEST $best"
  cp $bestw wit/part_$p.wit.pb.gz
done
# refold
cp scaffold_bin.pb.gz fold2_cur.pb.gz
for i in $(seq 0 $((NP-1))); do
  p=$(printf %02d $i)
  mkdir -p fold2/g_$p
  $R/wric_subtree graft fold2_cur.pb.gz ids/part_$p.ids.txt fold2/g_$p wit/part_$p.wit.pb.gz 2>/dev/null
  cp fold2/g_$p/graft_0.pb.gz fold2_cur.pb.gz
done
U2=$($W tension fold2_cur.pb.gz fold2_cur.pb.gz 0 2>&1 | grep -oP "witness total score = \K[0-9]+")
echo "==== $NAME enriched: per-part[$BEST ] U=$U2"
