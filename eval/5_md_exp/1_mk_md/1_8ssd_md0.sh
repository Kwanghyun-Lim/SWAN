#!/bin/bash

SSD0=/dev/sdb
SSD1=/dev/sdc
SSD2=/dev/sdd
SSD3=/dev/sde

SSD4=/dev/sdf
SSD5=/dev/sdg
SSD6=/dev/sdh
SSD7=/dev/sdi

yes|mdadm --create /dev/md0 --assume-clean --level=0 --raid-devices=8 $SSD0 $SSD1 $SSD2 $SSD3 $SSD4 $SSD5 $SSD6 $SSD7

