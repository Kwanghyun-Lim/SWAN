#!/bin/bash

DIR="/mnt/lograid"
SCHEME="lograid"

/mnt/workload2/ycsb-0.15.0/bin/ycsb load rocksdb -s -P /mnt/workload2/ycsb-0.15.0/workloads/workloada -p rocksdb.dir=${DIR}/ycsb-a > /mnt/workloads/result/${SCHEME}/load/load_result_a 2> /mnt/workloads/result/${SCHEME}/load/load_log_a 

#/mnt/workload2/ycsb-0.15.0/bin/ycsb load rocksdb -s -P /mnt/workload2/ycsb-0.15.0/workloads/workloadb -p rocksdb.dir=/mnt/workload2/ycsb_load/b-4k

#/mnt/workload2/ycsb-0.15.0/bin/ycsb load rocksdb -s -P /mnt/workload2/ycsb-0.15.0/workloads/workloadc -p rocksdb.dir=/mnt/workload2/ycsb_load/c-4k

#/mnt/workload2/ycsb-0.15.0/bin/ycsb load rocksdb -s -P /mnt/workload2/ycsb-0.15.0/workloads/workloadb -p rocksdb.dir=/mnt/workload2/ycsb_load/d-4k
