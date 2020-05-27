#!/bin/bash

DEV=/dev/md0

time trace_replay 32 32 md0-prxy_1.txt 1200 30 $DEV /home/dell/Traces/prxy_1.part1_scale900g_align4k.trace
