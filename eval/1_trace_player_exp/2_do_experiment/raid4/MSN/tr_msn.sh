#!/bin/bash

DEV=/dev/md4

time trace_replay 32 32 msn.txt 1200 30 $DEV /home/dell/Traces/MSN/fast-1.csv
