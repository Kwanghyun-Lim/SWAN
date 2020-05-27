#!/bin/bash

DEV=/dev/md0

time trace_replay 32 32 exch-md0.txt 1800 30 $DEV /home/dell/Traces/exch/fast2012-0.csv
