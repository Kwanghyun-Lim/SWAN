#!/bin/bash

umount /mnt/md4
mdadm --stop /dev/md4
