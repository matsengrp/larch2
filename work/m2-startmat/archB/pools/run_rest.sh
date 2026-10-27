#!/bin/bash
export LD_LIBRARY_PATH=~/install/gcc-trunk/lib64
L=/home/ogi/matsen/larch2-wric-10datasets/build/bin/larch2
cd /tmp/v0
for j in 09 11 12 13 14 15 16 17 18 19 20 21 22 23 24 25; do
  p=$(printf %02d $j)
  G=$(grep -h "^# part " m23/rp1/resp3full/part_$p.spr.pb.resp | awk '{print $3}')
  n=$($L --dag-pb $G -o /tmp/v0/m23/archb/pools/p$p.div.pb.gz --diverse-sample 16 --diverse-newick /tmp/v0/m23/archb/pools/p$p.div.nwk 2>&1 | grep -oP "Wrote \K[0-9]+")
  echo "slot $p: $n distinct"
done
