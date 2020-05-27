#!/bin/bash

# $1: device name ex. /dev/sdc

# Trace all events
#blktrace -d $1 -o - | blkparse -i -

# Trace issued events
blktrace -d $1 -a issue -o - | blkparse -i -

# Trace masked events
#blktrace -d $1 -A barrier issue -o - | blkparse -i -
