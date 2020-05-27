#!/bin/bash

DEV=/dev/mapper/dmsrc-vol

time trace_replay 32 32 exch.txt 3600 30 $DEV ../../Traces/exch/fast2012-0.csv
