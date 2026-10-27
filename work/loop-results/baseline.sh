#!/bin/bash
# baseline.sh <name> <init.pb.gz> -- classic whole-tree SPR to plateau (same budget style)
set -u
export LD_LIBRARY_PATH=~/install/gcc-trunk/lib64
R=/home/ogi/matsen/larch2-wric-10datasets/build
L=$R/bin/larch2
W=$R/wric_tension_map
NAME=$1; INIT=$2
D=$(dirname $INIT)/base-$NAME
mkdir -p $D && cd $D
T0=$(date +%s.%N)
$L --dag-pb $INIT -o base_out.pb.gz -n 100 --max-moves 50 --seed 1 --patience 3 2> spr.log
T1=$(date +%s.%N)
FIN=$(grep -oE "final: parsimony=[0-9]+|iter [0-9]+: parsimony=[0-9]+" spr.log | tail -3 | head -1)
ITERS=$(grep -cE "iter [0-9]+: parsimony" spr.log)
$R/wric_dag_binarize base_out.pb.gz base_bin.pb.gz 2>/dev/null
SC=$($W tension base_bin.pb.gz base_bin.pb.gz 0 2>&1 | grep -oP "witness total score = \K[0-9]+")
echo "==== $NAME classic-SPR: iters=$ITERS last='$FIN' tension=$SC wall=$(echo "$T1-$T0"|bc)s"
