#!/bin/bash

umount /mnt/md0
mdadm --stop /dev/md0
