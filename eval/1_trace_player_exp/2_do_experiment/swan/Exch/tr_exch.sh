#!/bin/bash

DEV=/dev/mapper/dmsrc-vol

time trace_replay 32 32 exch-3r3c.txt 1800 30 $DEV /home/dell/Traces/exch/fast2012-0.csv
