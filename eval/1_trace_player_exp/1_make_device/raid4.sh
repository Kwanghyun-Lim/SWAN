#!/bin/bash

SSD0=/dev/sdb
SSD1=/dev/sdc
SSD2=/dev/sdd
SSD3=/dev/sde
SSD4=/dev/sdf
SSD5=/dev/sdg
SSD6=/dev/sdh

yes|mdadm --create /dev/md4 --assume-clean --level=4 --raid-devices=7 $SSD0 $SSD1 $SSD2 $SSD3 $SSD4 $SSD5 $SSD6

#mkfs.ext4 /dev/md4
#mount -t ext4 /dev/md4 /mnt/md4
