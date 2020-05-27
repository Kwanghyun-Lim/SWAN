#!/bin/bash

DEV=/dev/md4

time trace_replay 32 32 fin.txt 1200 30 $DEV /home/dell/Traces/fin/financial_trace.txt
