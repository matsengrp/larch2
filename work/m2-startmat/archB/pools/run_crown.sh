#!/bin/bash
export LD_LIBRARY_PATH=~/install/gcc-trunk/lib64
L=/home/ogi/matsen/larch2-wric-10datasets/build/bin/larch2
cd /tmp/v0
G=$(grep -h "^# part " m23/rp1/resp3full/part_08.spr.pb.resp | awk '{print $3}')
n=$($L --dag-pb $G -o /tmp/v0/m23/archb/pools/p08.div.pb.gz --diverse-sample 16 --diverse-pool 160 --diverse-newick /tmp/v0/m23/archb/pools/p08.div.nwk 2>&1 | grep -oP "Wrote \K[0-9]+")
echo "crown: $n distinct"
