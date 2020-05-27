#!/bin/bash

SSD0=/dev/sdb1
SSD1=/dev/sdc1
SSD2=/dev/sdd1
SSD3=/dev/sde1
SSD4=/dev/sdf1
SSD5=/dev/sdg1

yes|mdadm --create /dev/md5 --assume-clean --level=5 --raid-devices=6 $SSD0 $SSD1 $SSD2 $SSD3 $SSD4 $SSD5

mkfs.ext4 /dev/md5
mount -t ext4 /dev/md5 /mnt/md5
