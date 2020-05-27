#!/bin/bash

./md_conf/clear_cache.sh

NVM0=/dev/nvme0n1p1
NVM1=/dev/nvme1n1p1
yes|mdadm --create /dev/md0 --assume-clean --level=0 --raid-devices=2 $NVM0 $NVM1
