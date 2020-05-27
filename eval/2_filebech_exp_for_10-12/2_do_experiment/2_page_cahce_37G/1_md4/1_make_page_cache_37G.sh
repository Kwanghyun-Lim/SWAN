#/bin/bash


./disable_randomize.sh
free -h
sync; echo 3 > /proc/sys/vm/drop_caches
free -h
stress --vm 1 --vm-bytes 15G --vm-keep
free -h
