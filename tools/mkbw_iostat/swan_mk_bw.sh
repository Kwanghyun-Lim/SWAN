#!/bin/bash

python iostat2plt.py $1 dm-0 > ./tmp/tmp0
#python iostat2plt.py $1 sdb > ./tmp/tmp1
#python iostat2plt.py $1 sdc > ./tmp/tmp2
#python iostat2plt.py $1 sdd > ./tmp/tmp3
#python iostat2plt.py $1 sde > ./tmp/tmp4

gnuplot swan_bw.plt
