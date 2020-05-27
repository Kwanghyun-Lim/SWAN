#!/bin/bash

echo " ===== Performance result ====="

echo " ===== exch ====="
cat exch*.txt | grep "Avg lat"
cat exch*.txt | grep "Latency min"
cat exch*.txt | grep "Bandwidth"

echo " ===== fin ====="
cat *fin.txt | grep "Avg lat"
cat *fin.txt | grep "Latency min"
cat *fin.txt | grep "Bandwidth"

echo " ===== hm ====="
cat *hm_0.txt | grep "Avg lat"
cat *hm_0.txt | grep "Latency min"
cat *hm_0.txt | grep "Bandwidth"

echo " ===== prn ====="
cat *prn_1.txt | grep "Avg lat"
cat *prn_1.txt | grep "Latency min"
cat *prn_1.txt | grep "Bandwidth"

echo " ===== proj ====="
cat *proj_0.txt | grep "Avg lat"
cat *proj_0.txt | grep "Latency min"
cat *proj_0.txt | grep "Bandwidth"

echo " ===== prxy ====="
cat *prxy_1.txt | grep "Avg lat"
cat *prxy_1.txt | grep "Latency min"
cat *prxy_1.txt | grep "Bandwidth"

echo " ===== rsrch ====="
cat *rsrch_0.txt | grep "Avg lat"
cat *rsrch_0.txt | grep "Latency min"
cat *rsrch_0.txt | grep "Bandwidth"
