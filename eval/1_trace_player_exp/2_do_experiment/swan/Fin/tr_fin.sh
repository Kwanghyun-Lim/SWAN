#!/bin/bash

DEV=/dev/mapper/dmsrc-vol

time trace_replay 32 32 fin-3r3c.txt 1800 30 $DEV /home/dell/Traces/fin/financial_trace.txt
