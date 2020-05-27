#!/bin/bash

python dmesg2plt.py $1 normal_io > ./tmp/tmp0
python dmesg2plt.py $1 gc_io > ./tmp/tmp1
#python iostat2plt.py $1 sdd3 > ./tmp/tmp2
#python iostat2plt.py $1 sdd4 > ./tmp/tmp3

gnuplot iocnt.plt
