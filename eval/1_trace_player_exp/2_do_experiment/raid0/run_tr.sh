#!/bin/bash

echo " ===== SSD ====="

#now=$(date)
#echo "$now"
#echo "tr_exch.sh /dev/vdd"
#./tr_exch.sh /dev/vdd exch_ssd.txt

now=$(date)
echo "$now"
#echo "tr_msn.sh"
#./MSN/tr_msn.sh

#echo "tr_exch.sh"
#./Exch/tr_exch.sh

#echo "tr_fin.sh"
#./Fin/tr_fin.sh

echo "tr_hm.sh"
./hm/tr_hm.sh

echo "tr_proj.sh"
./proj/tr_proj.sh

echo "tr_prn.sh"
./prn/tr_prn.sh

echo "tr_rsrch.sh"
./rsrch/tr_rsrch.sh

echo "tr_prxy.sh"
./prxy/tr_prxy.sh
