#!/bin/bash

./disable_randomize.sh

free -h
sync; echo 3 > /proc/sys/vm/drop_caches
free -h

DEV=/dev/mapper/dmsrc-vol

time trace_replay 32 32 fin.txt 1200 30 $DEV /root/Prj/Traces/fin/financial_trace.txt
