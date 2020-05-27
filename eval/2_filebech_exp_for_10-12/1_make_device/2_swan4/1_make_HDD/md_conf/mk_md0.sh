#!/bin/bash

SSD0=/dev/sdb
SSD1=/dev/sdc
SSD2=/dev/sdd
SSD3=/dev/sde
SSD4=/dev/sdf
SSD5=/dev/sdg
SSD6=/dev/sdh
SSD7=/dev/sdi

NVM0=/dev/nvme0n1p1
NVM1=/dev/nvme1n1p1
NVM2=/dev/nvme2n1
NVM3=/dev/nvme3n1

#yes|mdadm --create /dev/md0 --assume-clean --level=0 --raid-devices=6 $SSD0 $SSD1 $SSD2 $SSD3 $SSD4 $SSD5
#yes|mdadm --create /dev/md0 --assume-clean --level=0 --raid-devices=3 $SSD0 $SSD1 $SSD2
#yes|mdadm --create /dev/md0 --assume-clean --level=0 --raid-devices=4 $SSD4 $SSD5 $SSD6 $SSD7
yes|mdadm --create /dev/md0 --assume-clean --level=0 --raid-devices=2 $NVM0 $NVM1
#mkfs.ext4 /dev/md0
#mount -t ext4 /dev/md0 /mnt/md0
