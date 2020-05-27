#!/bin/bash

DEV=/dev/mapper/dmsrc-vol

time trace_replay 32 32 msn.txt 1200 30 $DEV ../../Traces/MSN/fast-1.csv
