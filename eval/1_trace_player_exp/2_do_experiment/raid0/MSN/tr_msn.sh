#!/bin/bash

DEV=/dev/md0

time trace_replay 32 32 msn.txt 1800 30 $DEV /home/dell/Traces/MSN/fast-1.csv
