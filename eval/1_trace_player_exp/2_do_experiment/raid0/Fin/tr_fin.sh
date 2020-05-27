#!/bin/bash

DEV=/dev/md0

time trace_replay 32 32 md0-fin-test.txt 1800 30 $DEV /home/dell/Traces/fin/financial_trace.txt
