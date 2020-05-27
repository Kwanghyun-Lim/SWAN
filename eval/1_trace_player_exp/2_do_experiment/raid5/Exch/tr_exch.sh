#!/bin/bash

DEV=/dev/md5

time trace_replay 32 32 exch.txt 1200 30 $DEV /home/dell/Traces/exch/fast2012-0.csv
