#!/bin/bash

DEV=/dev/mapper/dmsrc-vol

time trace_replay 32 32 msn-3r3c.txt 1800 30 $DEV /home/dell/Traces/MSN/fast-1.csv
