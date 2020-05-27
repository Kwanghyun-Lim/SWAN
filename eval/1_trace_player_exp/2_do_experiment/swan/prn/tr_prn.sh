#!/bin/bash

DEV=/dev/mapper/dmsrc-vol

time trace_replay 32 32 prn_1-3r3c.txt 1200 30 $DEV /home/dell/Traces/prn_1_scale900g_align4k.trace
