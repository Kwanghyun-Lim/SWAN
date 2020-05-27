#!/bin/bash

python iostat2plt.py $1 dm-0 > ./tmp/tmp0
#python iostat2plt.py $1 sdd2 > ./tmp/tmp1
#python iostat2plt.py $1 sdd3 > ./tmp/tmp2
#python iostat2plt.py $1 sdd4 > ./tmp/tmp3

gnuplot lograid_bw.plt
