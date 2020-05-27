#!/bin/bash

DEV=/dev/mapper/dmsrc-vol

time trace_replay 32 32 rsrch_0-3r3c.txt 1200 30 $DEV /home/dell/Traces/rsrch_0_scale900g_align4k.trace
