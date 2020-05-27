#!/bin/bash

/mnt/workload2/ycsb-0.15.0/bin/ycsb load rocksdb -s -P /mnt/workload2/ycsb-0.15.0/workloads/workloada -p rocksdb.dir=/mnt/workload2/ycsb_load/a-4k

/mnt/workload2/ycsb-0.15.0/bin/ycsb load rocksdb -s -P /mnt/workload2/ycsb-0.15.0/workloads/workloadb -p rocksdb.dir=/mnt/workload2/ycsb_load/b-4k

/mnt/workload2/ycsb-0.15.0/bin/ycsb load rocksdb -s -P /mnt/workload2/ycsb-0.15.0/workloads/workloadc -p rocksdb.dir=/mnt/workload2/ycsb_load/c-4k

/mnt/workload2/ycsb-0.15.0/bin/ycsb load rocksdb -s -P /mnt/workload2/ycsb-0.15.0/workloads/workloadb -p rocksdb.dir=/mnt/workload2/ycsb_load/d-4k
