#!/bin/bash

DEV=/dev/md0

time trace_replay 32 32 md0-hm_0.txt 1200 30 $DEV /home/dell/Traces/hm_0_scale900g_align4k.trace
